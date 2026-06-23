#pragma once

/** @file file_transfer.hpp
 *  Передача файлов по kBulkStream (фаза 4).
 */

#include "nyx/blob_store.hpp"
#include "nyx/connection.hpp"
#include "nyx/file_index.hpp"
#include "nyx/file_proto.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nyx {

/** Отправка и приём файлов поверх Connection. */
class FileTransferService {
 public:
  using EventCallback = std::function<void(const std::string& text)>;
  using ProgressCallback =
      std::function<void(const FileHash& hash, uint64_t done, uint64_t total)>;

  FileTransferService(Connection& connection, FileIndex& index,
                      std::string download_dir);

  /** Область share: zero = личка, иначе group_id поля. */
  void set_share_scope(const GroupId& group_id) { share_scope_ = group_id; }
  const GroupId& share_scope() const { return share_scope_; }

  /** Обработка payload с kBulkStream. */
  void handle_bulk(const ByteBuffer& payload);

  /** Отправляет следующий чанк, если идёт исходящая передача. */
  void pump();

  /** Запрос списка файлов у peer. */
  bool request_list();

  /** Запрос актуальной политики ACL у hub. */
  bool request_policy();

  /** Запрос файла по hex-хешу; dest_path — полный путь сохранения (необязательно). */
  bool request_file(const std::string& hash_hex, const std::string& dest_path = {});

  /** Проактивная отправка локального файла (из индекса или по пути). */
  bool send_file(const std::string& path_or_hash_hex);

  /** Ответ на ListReq (вызывается из handle_bulk автоматически). */
  void respond_list();

  /** Публикует на peer свой индекс поля (IndexPush). */
  bool push_field_index(const std::vector<FileEntry>& entries,
                        const std::vector<std::string>& root_paths = {});

  void set_on_event(EventCallback cb) { on_event_ = std::move(cb); }
  void set_on_progress(ProgressCallback cb) { on_progress_ = std::move(cb); }
  /** Вызывается после получения ListResp от peer. */
  void set_on_remote_list(std::function<void(const std::vector<FileEntry>&)> cb) {
    on_remote_list_ = std::move(cb);
  }

  const std::vector<FileEntry>& remote_list() const { return remote_list_; }

  /** Копия remote_list_ (потокобезопасно для UI). */
  std::vector<FileEntry> remote_list_snapshot() const;

  /** Идёт исходящая/входящая передача или ожидание Offer. */
  bool busy() const;

 private:
  bool send_bulk(const ByteBuffer& payload);
  void emit_event(const std::string& text);
  void emit_progress(const FileHash& hash, uint64_t done, uint64_t total);
  void flush_deferred();
  void start_outgoing(const FileEntry& entry);
  void send_next_chunk();
  void finish_outgoing();
  void handle_offer(const FileOffer& offer);
  void handle_chunk(const FileChunk& chunk);
  void handle_complete(const FileComplete& complete);
  void handle_deny(const FileDeny& deny);
  void handle_request(const FileRequest& req);
  void reset_incoming();

  Connection& connection_;
  FileIndex& index_;
  std::string download_dir_;
  GroupId share_scope_{};

  struct OutgoingState {
    FileEntry entry;
    BlobReader reader;
    uint64_t offset = 0;

    OutgoingState(FileEntry e, BlobReader r, uint64_t off)
        : entry(std::move(e)), reader(std::move(r)), offset(off) {}
  };
  struct IncomingState {
    FileOffer offer;
    BlobWriter writer;
    uint64_t received = 0;
    std::string dest_path;

    IncomingState(FileOffer o, BlobWriter w, uint64_t rec, std::string dest)
        : offer(std::move(o)), writer(std::move(w)), received(rec), dest_path(std::move(dest)) {}
  };

  mutable std::mutex mutex_;
  std::optional<OutgoingState> outgoing_;
  std::optional<IncomingState> incoming_;
  /** Запрос отправлен, ждём Offer или Deny. */
  std::optional<FileHash> awaiting_offer_;
  /** hash_hex → полный путь, выбранный до запроса. */
  std::unordered_map<std::string, std::string> pending_dest_paths_;
  std::vector<FileEntry> remote_list_;
  std::vector<std::function<void()>> deferred_callbacks_;

  EventCallback on_event_;
  ProgressCallback on_progress_;
  std::function<void(const std::vector<FileEntry>&)> on_remote_list_;
};

}  // namespace nyx
