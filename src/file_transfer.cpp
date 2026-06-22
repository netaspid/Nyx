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
  return outgoing_.has_value() || incoming_.has_value();
}

bool FileTransferService::send_bulk(const ByteBuffer& payload) {
  return connection_.send_payload(kBulkStream, payload);
}

void FileTransferService::start_outgoing(const FileEntry& entry) {
  BlobReader reader(entry.absolute_path());
  if (!reader.open()) {
    if (on_event_) on_event_("не удалось открыть файл: " + entry.absolute_path());
    return;
  }

  outgoing_ = OutgoingState{entry, std::move(reader), 0};

  FileOffer offer;
  offer.hash = entry.hash;
  offer.size = entry.size;
  offer.name = entry.display_name();
  offer.mime = entry.mime;
  send_bulk(offer.encode());
  if (on_event_) {
    on_event_("отправка «" + offer.name + "» (" + std::to_string(offer.size) + " байт)");
  }
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
    if (on_event_) on_event_("ошибка чтения файла при offset " + std::to_string(outgoing_->offset));
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
  if (on_progress_) on_progress_(outgoing_->entry.hash, outgoing_->offset, outgoing_->entry.size);

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
  if (on_event_) {
    on_event_("файл отправлен: " + hash_hex(complete.hash));
  }
  outgoing_.reset();
}

void FileTransferService::handle_offer(const FileOffer& offer) {
  if (incoming_) {
    if (on_event_) on_event_("уже идёт приём другого файла");
    return;
  }

  const std::string safe_name = offer.name.empty() ? hash_hex(offer.hash) : offer.name;
  const std::string dest_path = download_dir_ + "/" + safe_name;
  BlobWriter writer(dest_path);
  if (!writer.open()) {
    if (on_event_) on_event_("не удалось создать файл: " + dest_path);
    return;
  }

  incoming_ = IncomingState{offer, std::move(writer), 0, dest_path};
  if (on_event_) {
    on_event_("приём «" + offer.name + "» (" + std::to_string(offer.size) + " байт)");
  }
}

void FileTransferService::handle_chunk(const FileChunk& chunk) {
  if (!incoming_) return;
  if (chunk.hash != incoming_->offer.hash) return;

  if (!incoming_->writer.write_at(chunk.offset, chunk.data)) {
    if (on_event_) on_event_("ошибка записи чанка");
    incoming_.reset();
    return;
  }
  incoming_->received = std::max(incoming_->received, chunk.offset + chunk.data.size());
  if (on_progress_) {
    on_progress_(chunk.hash, incoming_->received, incoming_->offer.size);
  }
}

void FileTransferService::handle_complete(const FileComplete& complete) {
  if (!incoming_) return;
  if (complete.hash != incoming_->offer.hash) return;

  incoming_->writer.close();

  FileHash verify{};
  if (!hash_file(incoming_->dest_path, verify) || verify != complete.hash) {
    if (on_event_) on_event_("ошибка проверки hash после приёма");
    incoming_.reset();
    return;
  }

  if (on_event_) {
    on_event_("файл сохранён: " + incoming_->dest_path + " (" +
              std::to_string(complete.size) + " байт)");
  }
  incoming_.reset();
}

void FileTransferService::handle_request(const FileRequest& req) {
  auto entry = index_.find_for_session(req.hash, share_scope_);
  if (!entry) {
    FileDeny deny;
    deny.hash = req.hash;
    deny.reason = "файл не найден или недоступен в этой сессии";
    send_bulk(deny.encode());
    if (on_event_) on_event_("запрос файла " + hash_hex(req.hash) + " — не найден");
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
  send_bulk(encode_list_response(index_.listing_for_session(share_scope_)));
}

void FileTransferService::handle_bulk(const ByteBuffer& payload) {
  if (payload.empty()) return;
  const auto kind = static_cast<FileKind>(payload[0]);

  std::lock_guard<std::mutex> lock(mutex_);

  if (kind == FileKind::ListReq) {
    respond_list();
    return;
  }
  if (kind == FileKind::ListResp) {
    if (auto list = decode_list_response(payload)) {
      remote_list_ = std::move(*list);
      if (on_remote_list_) on_remote_list_(remote_list_);
      if (on_event_) {
        on_event_("получен список файлов: " + std::to_string(remote_list_.size()) + " шт.");
      }
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
    if (on_event_) on_event_("отказ: " + deny->reason);
  }
}

void FileTransferService::pump() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (outgoing_ && outgoing_->offset < outgoing_->entry.size) {
    send_next_chunk();
  }
}

bool FileTransferService::request_list() {
  return send_bulk(encode_list_request());
}

bool FileTransferService::request_file(const std::string& hash_hex) {
  FileHash hash{};
  if (!hash_from_hex(hash_hex, hash)) {
    if (on_event_) on_event_("неверный hash (нужно 64 hex)");
    return false;
  }
  FileRequest req;
  req.hash = hash;
  return send_bulk(req.encode());
}

bool FileTransferService::send_file(const std::string& path_or_hash_hex) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (outgoing_) {
    if (on_event_) on_event_("уже идёт отправка файла");
    return false;
  }

  FileHash hash{};
  if (hash_from_hex(path_or_hash_hex, hash)) {
    if (auto entry = index_.find_for_session(hash, share_scope_)) {
      start_outgoing(*entry);
      return true;
    }
  }

  std::error_code ec;
  const auto fs_path = path_from_utf8(path_or_hash_hex);
  if (!std::filesystem::exists(fs_path, ec)) {
    if (on_event_) on_event_("файл не найден: " + path_or_hash_hex);
    return false;
  }

  FileEntry entry;
  entry.root_path = path_to_utf8(fs_path.parent_path());
  entry.relative_path = path_to_utf8(fs_path.filename());
  entry.size = static_cast<uint64_t>(std::filesystem::file_size(fs_path, ec));
  entry.mime = FileIndex::guess_mime(path_or_hash_hex);
  if (!hash_file(path_or_hash_hex, entry.hash)) {
    if (on_event_) on_event_("не удалось вычислить hash");
    return false;
  }
  start_outgoing(entry);
  return true;
}

bool FileTransferService::push_field_index(const std::vector<FileEntry>& entries,
                                           const std::vector<std::string>& root_paths) {
  return send_bulk(encode_index_push(entries, root_paths));
}

}  // namespace nyx
