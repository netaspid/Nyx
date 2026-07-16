#include "nyx/file_transfer.hpp"



#include "nyx/paths.hpp"

#include "nyx/util.hpp"



#include <algorithm>

#include <filesystem>



namespace nyx {



FileTransferService::FileTransferService(Connection& connection, FileIndex& index,

                                         std::string download_dir)

    : connection_(connection),

      index_(index),

      download_dir_(std::move(download_dir)) {

  ensure_data_dir();

  std::error_code ec;

  std::filesystem::create_directories(download_dir_, ec);

}



bool FileTransferService::busy() const {

  std::lock_guard<std::mutex> lock(mutex_);

  return outgoing_.has_value() || incoming_.has_value() || awaiting_offer_.has_value();

}



std::vector<FileEntry> FileTransferService::remote_list_snapshot() const {

  std::lock_guard<std::mutex> lock(mutex_);

  return remote_list_;

}



void FileTransferService::emit_event(const std::string& text) {

  EventCallback cb;

  {

    std::lock_guard<std::mutex> lock(mutex_);

    cb = on_event_;

  }

  if (cb) cb(text);

}



void FileTransferService::emit_progress(const FileHash& hash, uint64_t done, uint64_t total) {

  ProgressCallback cb;

  {

    std::lock_guard<std::mutex> lock(mutex_);

    cb = on_progress_;

  }

  if (cb) cb(hash, done, total);

}



void FileTransferService::flush_deferred() {

  std::vector<std::function<void()>> pending;

  {

    std::lock_guard<std::mutex> lock(mutex_);

    pending.swap(deferred_callbacks_);

  }

  for (auto& fn : pending) {

    if (fn) fn();

  }

}



bool FileTransferService::send_bulk(const ByteBuffer& payload) {

  return connection_.send_payload(kBulkStream, payload);

}



void FileTransferService::start_outgoing(const FileEntry& entry) {

  BlobReader reader(entry.absolute_path());

  if (!reader.open()) {

    FileDeny deny;

    deny.hash = entry.hash;

    deny.reason = "не удалось открыть файл";

    send_bulk(deny.encode());

    deferred_callbacks_.push_back(

        [this, path = entry.absolute_path()] { emit_event("не удалось открыть файл: " + path); });

    return;

  }



  outgoing_ = OutgoingState{entry, std::move(reader), 0};



  FileOffer offer;

  offer.hash = entry.hash;

  offer.size = entry.size;

  offer.name = entry.display_name();

  offer.mime = entry.mime;

  send_bulk(offer.encode());

  deferred_callbacks_.push_back([this, offer] {

    emit_event("отправка «" + offer.name + "» (" + std::to_string(offer.size) + " байт)");

  });

  send_next_chunk();

}



void FileTransferService::send_next_chunk() {

  if (!outgoing_) return;



  if (outgoing_->offset >= outgoing_->entry.size) {

    finish_outgoing();

    return;

  }



  ByteBuffer data;

  const std::size_t to_read = static_cast<std::size_t>(std::min<uint64_t>(

      kFileChunkSize, outgoing_->entry.size - outgoing_->offset));

  const std::size_t got =

      outgoing_->reader.read_at(outgoing_->offset, data, to_read);

  if (got == 0) {

    FileDeny deny;

    deny.hash = outgoing_->entry.hash;

    deny.reason = "ошибка чтения при отправке";

    send_bulk(deny.encode());

    const uint64_t off = outgoing_->offset;

    deferred_callbacks_.push_back(

        [this, off] { emit_event("ошибка чтения файла при offset " + std::to_string(off)); });

    outgoing_.reset();

    return;

  }



  FileChunk chunk;

  chunk.hash = outgoing_->entry.hash;

  chunk.offset = outgoing_->offset;

  chunk.data = std::move(data);

  if (!send_bulk(chunk.encode())) {

    return;

  }



  outgoing_->offset += got;

  const FileHash hash = outgoing_->entry.hash;

  const uint64_t done = outgoing_->offset;

  const uint64_t total = outgoing_->entry.size;

  deferred_callbacks_.push_back([this, hash, done, total] { emit_progress(hash, done, total); });



  if (outgoing_->offset >= outgoing_->entry.size) {

    finish_outgoing();

  }

}



void FileTransferService::finish_outgoing() {

  if (!outgoing_) return;

  FileComplete complete;

  complete.hash = outgoing_->entry.hash;

  complete.size = outgoing_->entry.size;

  send_bulk(complete.encode());

  const FileHash hash = complete.hash;

  deferred_callbacks_.push_back(

      [this, hash] { emit_event("файл отправлен: " + hash_hex(hash)); });

  outgoing_.reset();

}



void FileTransferService::reset_incoming() {

  if (incoming_) {

    incoming_->writer.close();

    incoming_.reset();

  }

}



void FileTransferService::handle_offer(const FileOffer& offer) {

  awaiting_offer_.reset();

  if (incoming_) {

    deferred_callbacks_.push_back(

        [this] { emit_event("уже идёт приём другого файла"); });

    return;

  }



  const std::string safe_name = offer.name.empty() ? hash_hex(offer.hash) : offer.name;

  std::string dest_path;
  const std::string hash_key = hash_hex(offer.hash);
  const auto pending = pending_dest_paths_.find(hash_key);
  if (pending != pending_dest_paths_.end()) {
    dest_path = pending->second;
    pending_dest_paths_.erase(pending);
  } else {
    dest_path = download_dir_ + "/" + safe_name;
  }

  {
    std::error_code ec;
    const auto fs_dest = path_from_utf8(dest_path);
    const auto parent = fs_dest.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, ec);
    }
  }

  BlobWriter writer(dest_path);

  if (!writer.open()) {

    deferred_callbacks_.push_back(

        [this, dest_path] { emit_event("не удалось создать файл: " + dest_path); });

    return;

  }



  incoming_ = IncomingState{offer, std::move(writer), 0, dest_path};

  deferred_callbacks_.push_back([this, offer] {

    emit_event("приём «" + offer.name + "» (" + std::to_string(offer.size) + " байт)");

  });

}



void FileTransferService::handle_chunk(const FileChunk& chunk) {

  if (!incoming_) return;

  if (chunk.hash != incoming_->offer.hash) return;



  if (!incoming_->writer.write_at(chunk.offset, chunk.data)) {

    deferred_callbacks_.push_back([this] { emit_event("ошибка записи чанка"); });

    reset_incoming();

    return;

  }

  incoming_->received = std::max(incoming_->received, chunk.offset + chunk.data.size());

  const FileHash hash = chunk.hash;

  const uint64_t done = incoming_->received;

  const uint64_t total = incoming_->offer.size;

  deferred_callbacks_.push_back([this, hash, done, total] { emit_progress(hash, done, total); });

}



void FileTransferService::handle_complete(const FileComplete& complete) {

  if (!incoming_) return;

  if (complete.hash != incoming_->offer.hash) return;



  incoming_->writer.close();



  const std::string dest_path = incoming_->dest_path;

  const uint64_t size = complete.size;

  FileHash verify{};

  if (!hash_file(dest_path, verify) || verify != complete.hash) {

    deferred_callbacks_.push_back([this] { emit_event("ошибка проверки hash после приёма"); });

    reset_incoming();

    return;

  }



  deferred_callbacks_.push_back([this, dest_path, size] {

    emit_event("файл сохранён: " + dest_path + " (" + std::to_string(size) + " байт)");

  });

  incoming_.reset();

}



void FileTransferService::handle_deny(const FileDeny& deny) {

  awaiting_offer_.reset();

  pending_dest_paths_.erase(hash_hex(deny.hash));

  reset_incoming();

  deferred_callbacks_.push_back(

      [this, reason = deny.reason] { emit_event("отказ: " + reason); });

}



void FileTransferService::handle_request(const FileRequest& req) {

  const auto entry = index_.find_for_session(req.hash, share_scope_);

  if (!entry) {

    FileDeny deny;

    deny.hash = req.hash;

    deny.reason = "файл не найден или недоступен в этой сессии";

    send_bulk(deny.encode());

    deferred_callbacks_.push_back([this, hash = req.hash] {

      emit_event("запрос файла " + hash_hex(hash) + " — не найден");

    });

    return;

  }

  if (outgoing_) {

    FileDeny deny;

    deny.hash = req.hash;

    deny.reason = "отправитель занят";

    send_bulk(deny.encode());

    return;

  }

  start_outgoing(*entry);

}



void FileTransferService::respond_list() {
  respond_list({}, {});
}

void FileTransferService::respond_list(const std::string& root_path,
                                       const std::string& parent_rel) {
  std::vector<FileEntry> entries;
  if (root_path.empty()) {
    for (const auto& e : index_.listing_for_session(share_scope_)) {
      if (e.is_directory()) entries.push_back(e);
    }
  } else {
    entries = index_.listing_at_root(root_path, parent_rel);
  }
  send_bulk(encode_list_response(entries));
}



void FileTransferService::handle_bulk(const ByteBuffer& payload) {

  if (payload.empty()) return;

  const auto kind = static_cast<FileKind>(payload[0]);



  {

    std::lock_guard<std::mutex> lock(mutex_);



    if (kind == FileKind::ListReq) {
      if (auto path = decode_list_request(payload)) {
        respond_list(path->first, path->second);
      } else {
        respond_list();
      }
      return;
    }

    if (kind == FileKind::ListResp) {
      if (auto list = decode_list_response(payload)) {
        if (merge_next_list_) {
          for (auto& e : *list) {
            const std::string hx = hash_hex(e.hash);
            bool found = false;
            for (auto& existing : remote_list_) {
              if (hash_hex(existing.hash) == hx) {
                existing = std::move(e);
                found = true;
                break;
              }
            }
            if (!found) remote_list_.push_back(std::move(e));
          }
          merge_next_list_ = false;
        } else {
          remote_list_ = std::move(*list);
        }
        const std::size_t count = remote_list_.size();
        deferred_callbacks_.push_back([this, count] {
          if (on_remote_list_) on_remote_list_(remote_list_);
          emit_event("получен список файлов: " + std::to_string(count) + " шт.");
        });
      }
      return;
    }

    if (auto req = FileRequest::decode(payload)) {

      handle_request(*req);

      return;

    }

    if (auto offer = FileOffer::decode(payload)) {

      handle_offer(*offer);

      return;

    }

    if (auto chunk = FileChunk::decode(payload)) {

      handle_chunk(*chunk);

      return;

    }

    if (auto complete = FileComplete::decode(payload)) {

      handle_complete(*complete);

      return;

    }

    if (auto deny = FileDeny::decode(payload)) {

      handle_deny(*deny);

    }

  }



  flush_deferred();

}



void FileTransferService::pump() {

  {

    std::lock_guard<std::mutex> lock(mutex_);

    if (outgoing_ && outgoing_->offset < outgoing_->entry.size) {

      send_next_chunk();

    }

  }

  flush_deferred();

}



bool FileTransferService::request_list() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    merge_next_list_ = false;
  }
  return send_bulk(encode_list_request());
}

bool FileTransferService::request_list(const std::string& root_path,
                                       const std::string& parent_rel) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    merge_next_list_ = !root_path.empty();
  }
  return send_bulk(encode_list_request(root_path, parent_rel));
}



bool FileTransferService::request_policy() {

  return send_bulk(encode_policy_request());

}



bool FileTransferService::request_file(const std::string& hash_hex,
                                       const std::string& dest_path) {

  FileHash hash{};

  if (!hash_from_hex(hash_hex, hash)) {

    emit_event("неверный hash (нужно 64 hex)");

    return false;

  }



  {

    std::lock_guard<std::mutex> lock(mutex_);

    if (outgoing_ || incoming_ || awaiting_offer_) {

      return false;

    }

    awaiting_offer_ = hash;

    if (!dest_path.empty()) {
      pending_dest_paths_[hash_hex] = dest_path;
    }

  }



  FileRequest req;

  req.hash = hash;

  if (!send_bulk(req.encode())) {

    std::lock_guard<std::mutex> lock(mutex_);

    awaiting_offer_.reset();

    pending_dest_paths_.erase(hash_hex);

    return false;

  }

  return true;

}



bool FileTransferService::send_file(const std::string& path_or_hash_hex) {

  bool ok = false;

  {

    std::lock_guard<std::mutex> lock(mutex_);

    if (outgoing_) {

      deferred_callbacks_.push_back([this] { emit_event("уже идёт отправка файла"); });

    } else {

      FileHash hash{};

      if (hash_from_hex(path_or_hash_hex, hash)) {

        if (auto entry = index_.find_for_session(hash, share_scope_)) {

          start_outgoing(*entry);

          ok = true;

        } else if (auto entry = index_.find_by_hash(hash)) {

          start_outgoing(*entry);

          ok = true;

        }

      }



      if (!ok) {

        std::error_code ec;

        const auto fs_path = path_from_utf8(path_or_hash_hex);

        if (!std::filesystem::exists(fs_path, ec)) {

          deferred_callbacks_.push_back([this, path = path_or_hash_hex] {

            emit_event("файл не найден: " + path);

          });

        } else {

          FileEntry entry;

          entry.root_path = path_to_utf8(fs_path.parent_path());

          entry.relative_path = path_to_utf8(fs_path.filename());

          entry.size = static_cast<uint64_t>(std::filesystem::file_size(fs_path, ec));

          entry.mime = FileIndex::guess_mime(path_or_hash_hex);

          if (!hash_file(path_or_hash_hex, entry.hash)) {

            deferred_callbacks_.push_back([this] { emit_event("не удалось вычислить hash"); });

          } else {

            start_outgoing(entry);

            ok = true;

          }

        }

      }

    }

  }

  flush_deferred();

  return ok;

}



bool FileTransferService::push_field_index(const std::vector<FileEntry>& entries,

                                           const std::vector<std::string>& root_paths) {

  return send_bulk(encode_index_push(entries, root_paths));

}



}  // namespace nyx

