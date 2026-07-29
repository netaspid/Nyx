#pragma once

#include "nyx/blob_store.hpp"
#include "nyx/connection.hpp"
#include "nyx/file_index.hpp"
#include "nyx/file_proto.hpp"

#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nyx {

class FileTransferService {
public:
  using EventCallback = std::function<void(const std::string& text)>;
  using ProgressCallback = std::function<void(const FileHash& hash, uint64_t done, uint64_t total)>;
  using CompletionCallback = std::function<void(
      const FileHash& hash, bool success, const std::string& path, const std::string& error)>;

  FileTransferService(Connection& connection, FileIndex& index, std::string download_dir);


  void set_share_scope(const GroupId& group_id) { share_scope_ = group_id; }
  const GroupId& share_scope() const { return share_scope_; }


  void handle_bulk(const ByteBuffer& payload);


  void pump();


  bool request_list();
  bool request_list(const std::string& root_path, const std::string& parent_rel);


  bool request_policy();


  bool request_file(const std::string& hash_hex, const std::string& dest_path = {});


  bool send_file(const std::string& path_or_hash_hex);
  bool announce_capabilities();
  bool cancel(const std::string& hash_hex);
  bool peer_supports_resume() const;

  std::vector<std::pair<std::string, std::string>> outgoing_queue_snapshot() const;


  void respond_list();
  void respond_list(const std::string& root_path, const std::string& parent_rel);


  bool push_field_index(const std::vector<FileEntry>& entries,
                        const std::vector<std::string>& root_paths = {});

  void set_on_event(EventCallback cb) { on_event_ = std::move(cb); }
  void set_on_progress(ProgressCallback cb) { on_progress_ = std::move(cb); }
  void set_on_complete(CompletionCallback cb) { on_complete_ = std::move(cb); }

  void set_on_remote_list(std::function<void(const std::vector<FileEntry>&)> cb) {
    on_remote_list_ = std::move(cb);
  }


  std::vector<FileEntry> remote_list_snapshot() const;


  bool busy() const;

private:
  bool send_bulk(const ByteBuffer& payload);
  void emit_event(const std::string& text);
  void emit_progress(const FileHash& hash, uint64_t done, uint64_t total);
  void flush_deferred();
  void start_outgoing(const FileEntry& entry, uint64_t offset = 0);
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
  GroupId share_scope_ {};

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
    std::string part_path;

    IncomingState(FileOffer o, BlobWriter w, uint64_t rec, std::string dest, std::string part)
        : offer(std::move(o)), writer(std::move(w)), received(rec), dest_path(std::move(dest)),
          part_path(std::move(part)) {}
  };

  mutable std::mutex mutex_;
  std::optional<OutgoingState> outgoing_;
  std::deque<FileEntry> pending_outgoing_;
  std::optional<IncomingState> incoming_;

  std::optional<FileHash> awaiting_offer_;

  std::unordered_map<std::string, std::string> pending_dest_paths_;
  std::unordered_map<std::string, uint64_t> pending_resume_offsets_;
  std::vector<FileEntry> remote_list_;
  bool snapshot_level_list_ = false;
  std::string pending_list_root_;
  std::string pending_list_parent_;
  std::vector<std::function<void()>> deferred_callbacks_;

  EventCallback on_event_;
  ProgressCallback on_progress_;
  CompletionCallback on_complete_;
  std::function<void(const std::vector<FileEntry>&)> on_remote_list_;
  uint32_t peer_capabilities_ = 0;
  uint64_t index_revision_ = 0;
};

}
