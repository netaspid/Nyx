#include "nyx/file_transfer.hpp"



#include "nyx/paths.hpp"

#include "nyx/util.hpp"



#include <algorithm>

#include <filesystem>

#include <fstream>



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



void FileTransferService::start_outgoing(const FileEntry& entry,
                                         uint64_t offset) {

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



  outgoing_ =
      OutgoingState{entry, std::move(reader), std::min(offset, entry.size)};



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
    if (!pending_outgoing_.empty()) {
      FileEntry next = pending_outgoing_.front();
      pending_outgoing_.pop_front();
      start_outgoing(next);
    }

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
  if (!pending_outgoing_.empty()) {
    FileEntry next = pending_outgoing_.front();
    pending_outgoing_.pop_front();
    start_outgoing(next);
  }

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
  const auto resume_it = pending_resume_offsets_.find(hash_key);
  uint64_t resume_offset =
      resume_it == pending_resume_offsets_.end() ? 0 : resume_it->second;
  pending_resume_offsets_.erase(hash_key);
  if (resume_offset > offer.size) resume_offset = 0;
  const std::string part_path = dest_path + ".part";

  {
    std::error_code ec;
    const auto fs_dest = path_from_utf8(dest_path);
    const auto parent = fs_dest.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, ec);
    }
  }

  BlobWriter writer(part_path);

  if (!writer.open(resume_offset == 0)) {

    deferred_callbacks_.push_back(

        [this, dest_path] { emit_event("не удалось создать файл: " + dest_path); });
    if (on_complete_) {
      deferred_callbacks_.push_back([this, hash = offer.hash, dest_path] {
        on_complete_(hash, false, dest_path,
                     "не удалось создать partial-файл");
      });
    }

    return;

  }



  incoming_ = IncomingState{offer, std::move(writer), resume_offset,
                            dest_path, part_path};

  deferred_callbacks_.push_back([this, offer] {

    emit_event("приём «" + offer.name + "» (" + std::to_string(offer.size) + " байт)");

  });

}



void FileTransferService::handle_chunk(const FileChunk& chunk) {

  if (!incoming_) return;

  if (chunk.hash != incoming_->offer.hash) return;



  if (!incoming_->writer.write_at(chunk.offset, chunk.data)) {

    deferred_callbacks_.push_back([this] { emit_event("ошибка записи чанка"); });
    if (on_complete_) {
      const auto failed_hash = incoming_->offer.hash;
      const auto failed_path = incoming_->dest_path;
      deferred_callbacks_.push_back([this, failed_hash, failed_path] {
        on_complete_(failed_hash, false, failed_path,
                     "ошибка записи чанка");
      });
    }

    reset_incoming();

    return;

  }

  incoming_->received = std::max(incoming_->received, chunk.offset + chunk.data.size());

  {
    std::ofstream meta(path_from_utf8(incoming_->part_path + ".meta"),
                       std::ios::binary | std::ios::trunc);
    if (meta) {
      meta << hash_hex(chunk.hash) << ' ' << incoming_->received << '\n';
    }
  }

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
  const std::string part_path = incoming_->part_path;

  const uint64_t size = complete.size;

  FileHash verify{};

  if (!hash_file(part_path, verify) || verify != complete.hash) {

    deferred_callbacks_.push_back([this] { emit_event("ошибка проверки hash после приёма"); });
    std::error_code remove_ec;
    std::filesystem::remove(path_from_utf8(part_path), remove_ec);
    if (on_complete_) {
      deferred_callbacks_.push_back([this, hash = complete.hash, dest_path] {
        on_complete_(hash, false, dest_path, "ошибка проверки hash");
      });
    }

    reset_incoming();

    return;

  }

  std::error_code move_ec;
  std::filesystem::remove(path_from_utf8(dest_path), move_ec);
  move_ec.clear();
  std::filesystem::rename(path_from_utf8(part_path),
                          path_from_utf8(dest_path), move_ec);
  {
    std::error_code meta_ec;
    std::filesystem::remove(path_from_utf8(part_path + ".meta"), meta_ec);
  }
  if (move_ec) {
    deferred_callbacks_.push_back(
        [this] { emit_event("не удалось завершить partial-файл"); });
    if (on_complete_) {
      deferred_callbacks_.push_back([this, hash = complete.hash, dest_path] {
        on_complete_(hash, false, dest_path,
                     "не удалось завершить partial-файл");
      });
    }
    reset_incoming();
    return;
  }

  std::string relative_dir;
  {
    const auto root =
        path_from_utf8(FileIndex::library_root_path(share_scope_));
    const auto dest = path_from_utf8(dest_path);
    std::error_code rel_ec;
    const auto rel = std::filesystem::relative(dest.parent_path(), root, rel_ec);
    if (!rel_ec && !rel.empty() && !rel.is_absolute() &&
        std::find(rel.begin(), rel.end(), std::filesystem::path("..")) ==
            rel.end()) {
      relative_dir = path_to_utf8(rel);
    }
  }
  const auto cached =
      index_.adopt_file(dest_path, complete.hash, incoming_->offer.name,
                        incoming_->offer.mime, share_scope_, nullptr,
                        relative_dir);
  if (!cached) {
    deferred_callbacks_.push_back(
        [this] { emit_event("файл получен, но не добавлен в локальный cache"); });
  }


  deferred_callbacks_.push_back([this, dest_path, size] {

    emit_event("файл сохранён: " + dest_path + " (" + std::to_string(size) + " байт)");

  });
  if (on_complete_) {
    deferred_callbacks_.push_back([this, hash = complete.hash, dest_path] {
      on_complete_(hash, true, dest_path, {});
    });
  }

  incoming_.reset();

}



void FileTransferService::handle_deny(const FileDeny& deny) {

  awaiting_offer_.reset();

  pending_dest_paths_.erase(hash_hex(deny.hash));
  pending_resume_offsets_.erase(hash_hex(deny.hash));

  reset_incoming();

  deferred_callbacks_.push_back(

      [this, reason = deny.reason] { emit_event("отказ: " + reason); });
  if (on_complete_) {
    deferred_callbacks_.push_back([this, hash = deny.hash, reason = deny.reason] {
      on_complete_(hash, false, {}, reason);
    });
  }

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
    entries = index_.listing_level_for_root(share_scope_, root_path, parent_rel);
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
        if (snapshot_level_list_) {
          const std::string root_norm = normalize_utf8_path(pending_list_root_);
          const std::string parent = pending_list_parent_;
          const std::string prefix =
              parent.empty() ? std::string{} : parent + "/";
          remote_list_.erase(
              std::remove_if(
                  remote_list_.begin(), remote_list_.end(),
                  [&](const FileEntry& existing) {
                    if (normalize_utf8_path(existing.root_path) != root_norm) {
                      return false;
                    }
                    if (parent.empty()) {
                      // Keep root directory marker; replace all children.
                      if (existing.is_directory()) {
                        const std::string root_leaf = path_to_utf8(
                            path_from_utf8(root_norm).filename());
                        if (existing.relative_path == root_leaf ||
                            existing.relative_path.rfind("участник:", 0) == 0) {
                          return false;
                        }
                      }
                      return true;
                    }
                    return existing.relative_path == parent ||
                           existing.relative_path.rfind(prefix, 0) == 0;
                  }),
              remote_list_.end());
          for (auto& e : *list) {
            remote_list_.push_back(std::move(e));
          }
          snapshot_level_list_ = false;
          pending_list_root_.clear();
          pending_list_parent_.clear();
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

    if (auto capabilities = FileCapabilities::decode(payload)) {
      peer_capabilities_ = capabilities->flags;
      return;
    }

    if (auto range = FileRangeRequest::decode(payload)) {
      const auto entry = index_.find_for_session(range->hash, share_scope_);
      if (!entry || outgoing_) {
        FileDeny deny;
        deny.hash = range->hash;
        deny.reason = entry ? "отправитель занят"
                            : "файл не найден или недоступен";
        send_bulk(deny.encode());
      } else {
        start_outgoing(*entry, range->offset);
      }
      return;
    }

    if (auto cancel = FileCancel::decode(payload)) {
      if (outgoing_ && outgoing_->entry.hash == cancel->hash) {
        outgoing_.reset();
      }
      if (incoming_ && incoming_->offer.hash == cancel->hash) {
        reset_incoming();
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
    snapshot_level_list_ = false;
    pending_list_root_.clear();
    pending_list_parent_.clear();
  }
  return send_bulk(encode_list_request());
}

bool FileTransferService::request_list(const std::string& root_path,
                                       const std::string& parent_rel) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_level_list_ = !root_path.empty();
    pending_list_root_ = root_path;
    pending_list_parent_ = parent_rel;
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
  const std::string canonical_hash = nyx::hash_hex(hash);
  uint64_t resume_offset = 0;
  bool use_resume = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outgoing_ || incoming_ || awaiting_offer_) {
      return false;
    }
    awaiting_offer_ = hash;
    if (!dest_path.empty()) {
      pending_dest_paths_[canonical_hash] = dest_path;
      if ((peer_capabilities_ & FileCapabilities::kResume) != 0) {
        std::error_code ec;
        const auto part_path = path_from_utf8(dest_path + ".part");
        const auto meta_path = path_from_utf8(dest_path + ".part.meta");
        if (std::filesystem::is_regular_file(part_path, ec)) {
          resume_offset =
              static_cast<uint64_t>(std::filesystem::file_size(part_path, ec));
          if (!ec && resume_offset > 0) {
            use_resume = true;
            std::ifstream meta(meta_path, std::ios::binary);
            if (meta) {
              std::string stored_hash;
              uint64_t stored_offset = 0;
              if (meta >> stored_hash >> stored_offset) {
                if (stored_hash != canonical_hash) {
                  use_resume = false;
                  resume_offset = 0;
                } else if (stored_offset > 0 &&
                           stored_offset <= resume_offset) {
                  resume_offset = stored_offset;
                }
              }
            }
          }
        }
      }
    }
    pending_resume_offsets_[canonical_hash] =
        use_resume ? resume_offset : 0;
  }
  ByteBuffer wire;
  if (use_resume) {
    FileRangeRequest req;
    req.hash = hash;
    req.offset = resume_offset;
    wire = req.encode();
  } else {
    FileRequest req;
    req.hash = hash;
    wire = req.encode();
  }
  if (!send_bulk(wire)) {
    std::lock_guard<std::mutex> lock(mutex_);
    awaiting_offer_.reset();
    pending_dest_paths_.erase(canonical_hash);
    pending_resume_offsets_.erase(canonical_hash);
    return false;
  }
  return true;
}



bool FileTransferService::send_file(const std::string& path_or_hash_hex) {

  bool ok = false;

  {

    std::lock_guard<std::mutex> lock(mutex_);
    auto queue_or_start = [&](const FileEntry& entry) {
      if (outgoing_) {
        const auto duplicate = std::any_of(
            pending_outgoing_.begin(), pending_outgoing_.end(),
            [&](const FileEntry& pending) {
              return pending.hash == entry.hash;
            });
        if (!duplicate) pending_outgoing_.push_back(entry);
      } else {
        start_outgoing(entry);
      }
      ok = true;
    };

      FileHash hash{};

      if (hash_from_hex(path_or_hash_hex, hash)) {

        if (auto entry = index_.find_for_session(hash, share_scope_)) {

          queue_or_start(*entry);

        } else if (auto entry = index_.find_by_hash(hash)) {

          queue_or_start(*entry);

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

            queue_or_start(entry);

          }

        }

      }

  }

  flush_deferred();

  return ok;

}

bool FileTransferService::announce_capabilities() {
  FileCapabilities caps;
  caps.flags = FileCapabilities::kResume | FileCapabilities::kCancel;
  caps.max_parallel = 1;
  return send_bulk(caps.encode());
}

bool FileTransferService::peer_supports_resume() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return (peer_capabilities_ & FileCapabilities::kResume) != 0;
}

std::vector<std::pair<std::string, std::string>>
FileTransferService::outgoing_queue_snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<std::string, std::string>> out;
  if (outgoing_) {
    out.emplace_back(hash_hex(outgoing_->entry.hash),
                     outgoing_->entry.display_name());
  }
  for (const auto& entry : pending_outgoing_) {
    out.emplace_back(hash_hex(entry.hash), entry.display_name());
  }
  return out;
}

bool FileTransferService::cancel(const std::string& hash_hex_value) {
  FileHash hash{};
  if (!hash_from_hex(hash_hex_value, hash)) return false;
  const std::string canonical_hash = hash_hex(hash);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outgoing_ && outgoing_->entry.hash == hash) outgoing_.reset();
    if (incoming_ && incoming_->offer.hash == hash) reset_incoming();
    if (awaiting_offer_ && *awaiting_offer_ == hash) awaiting_offer_.reset();
    pending_dest_paths_.erase(canonical_hash);
    pending_resume_offsets_.erase(canonical_hash);
  }
  FileCancel cancel;
  cancel.hash = hash;
  return send_bulk(cancel.encode());
}



bool FileTransferService::push_field_index(const std::vector<FileEntry>& entries,

                                           const std::vector<std::string>& root_paths) {

  return send_bulk(
      encode_index_push(entries, root_paths, ++index_revision_));

}



}  // namespace nyx

