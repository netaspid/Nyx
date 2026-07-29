#include "node_service.hpp"

#include "direct_chat_loop.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include "nyx/app.hpp"
#include "nyx/chat_service.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/file_proto.hpp"
#include "nyx/group.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

namespace nyx_app {
namespace {
std::mutex transfer_queue_file_mutex;
}

nyx::GroupId NodeService::scope_from_hex(const std::string& scope_group_id_hex) const {
  nyx::GroupId scope {};
  if (!scope_group_id_hex.empty()) {
    nyx::GroupStore::group_id_from_hex(scope_group_id_hex, scope);
  }
  return scope;
}

void NodeService::wire_file_transfer(const std::shared_ptr<NetSession>& session,
                                     nyx::FileTransferService& files) {
  files.set_on_event([this](const std::string& text) { emit_status(text); });
  files.set_on_progress([this, session](const nyx::FileHash& hash, uint64_t done, uint64_t total) {
    {
      std::lock_guard lock(session->download_mutex);
      const std::string hex = nyx::hash_hex(hash);
      for (auto& item : session->download_queue) {
        if (item.hash_hex != hex)
          continue;
        item.state = "active";
        item.progress = total == 0 ? 0 : static_cast<int>((done * 100) / total);
        break;
      }
    }
    save_download_queue(session);
    emit_transfer_queue_changed();
    FileProgressCallback cb;
    {
      std::lock_guard lock(cb_mutex_);
      cb = on_file_progress_;
    }
    if (!cb || total == 0)
      return;
    const int pct = static_cast<int>((done * 100) / total);
    cb(nyx::hash_hex(hash).substr(0, 8) + "…", pct);
  });
  files.set_on_complete([this, session](const nyx::FileHash& hash,
                                        bool success,
                                        const std::string&,
                                        const std::string& error) {
    const std::string hex = nyx::hash_hex(hash);
    {
      std::lock_guard lock(session->download_mutex);
      auto it = std::find_if(session->download_queue.begin(),
                             session->download_queue.end(),
                             [&](const FileDownloadRequest& item) { return item.hash_hex == hex; });
      if (it != session->download_queue.end()) {
        if (success) {
          session->download_queue.erase(it);
        } else {
          it->state = "failed";
          it->error = error;
        }
      }
    }
    save_download_queue(session);
    emit_transfer_queue_changed();
  });
  files.set_on_remote_list([this](const std::vector<nyx::FileEntry>& list) {
    RemoteFilesCallback cb;
    {
      std::lock_guard lock(cb_mutex_);
      cb = on_remote_files_;
    }
    if (cb)
      cb(list);
  });
  (void)session;
}

void NodeService::run_direct_chat(std::shared_ptr<NetSession> session,
                                  std::unique_ptr<nyx::Connection> connection,
                                  const nyx::Profile& profile,
                                  bool incoming,
                                  ConnectionVia via) {
  if (!session || !connection)
    return;
  session->kind = SessionKind::Direct;
  session->state.store(SessionState::Connecting);
  session->share_scope = {};
  session->connection = std::move(connection);
  set_mode(NodeMode::ChatDirect);

  const std::string peer_host = session->connection->peer_host();
  const std::string endpoint = peer_host + ':' + std::to_string(session->connection->peer_port());
  emit_status(incoming ? "входящее соединение " + endpoint : "соединение с " + endpoint);

  nyx::HelloMessage peer_hello;
  if (!nyx::exchange_hello(*session->connection, profile, peer_hello, 10, [session]() {
        return session->running.load();
      })) {
    emit_status("не удалось поздороваться с собеседником");
    finish_session(session, SessionState::Offline);
    return;
  }

  nyx::remember_contact(peer_hello);
  sync_avatars_after_hello(session, peer_hello);
  const std::string peer_hex =
      nyx::to_hex(peer_hello.public_key.data(), peer_hello.public_key.size());
  const std::string final_id = make_dm_session_id(peer_hex);

  {
    std::lock_guard lock(sessions_mutex_);
    if (session->id != final_id) {
      sessions_.erase(session->id);
      session->id = final_id;
      sessions_[final_id] = session;
    }
    // Do not steal active from another open chat.
    if (active_session_id_.empty() || active_session_id_ == final_id ||
        active_session_id_.rfind("dm:pending:", 0) == 0 ||
        active_session_id_.rfind("dm:incoming:", 0) == 0) {
      active_session_id_ = final_id;
    }
    session->ref_id_hex = peer_hex;
  }

  emit_status(peer_hello.nickname + " в сети (id: " + nyx::short_user_id(peer_hello.public_key) +
              ")");
  if (!peer_host.empty())
    nyx::add_discovery_unicast_target(peer_host);
  emit_chat_ready(
      session, peer_hello.nickname, via, peer_host, nyx::ConversationKind::Direct, peer_hex);

  std::string invite_for_intent;
  if (peer_hello.has_dm_inbox_token) {
    invite_for_intent =
        nyx::to_hex(peer_hello.dm_inbox_token.data(), peer_hello.dm_inbox_token.size());
  } else if (via == ConnectionVia::LanDirect && !peer_host.empty()) {
    // Only as last resort when Hello had no inbox token. Prefer beacon re-browse on
    // reconnect — the port here is the peer's current socket, which may rebind.
    invite_for_intent =
        "lan://" + peer_host + ":" + std::to_string(session->connection->peer_port());
  }
  remember_intent_for_session(session, invite_for_intent);

  nyx::ChatService::PeerInfo peer;
  peer.user_id = peer_hello.public_key;
  peer.nickname = peer_hello.nickname;

  session->chat = std::make_unique<nyx::ChatService>(*session->connection, profile, peer);
  session->files = std::make_unique<nyx::FileTransferService>(
      *session->connection, file_index_, nyx::default_downloads_dir());
  session->files->set_share_scope(session->share_scope);
  wire_file_transfer(session, *session->files);
  session->files->announce_capabilities();
  load_download_queue(session);

  session->chat->set_on_message([this, session](const nyx::ChatMessage& msg, bool outgoing) {
    emit_message(session, msg, outgoing, outgoing ? "pending" : "");
  });
  session->chat->set_on_delivery([this, session](uint64_t message_id, nyx::DeliveryStatus status) {
    emit_delivery(session, message_id, status == nyx::DeliveryStatus::Delivered);
  });
  session->chat->set_on_event([this](const std::string& text) { emit_status(text); });
  wire_call_handlers(session);

  const auto recent = session->chat->history(30);
  for (const auto& stored : recent) {
    nyx::ChatMessage msg;
    msg.id = stored.id;
    msg.timestamp_ms = stored.timestamp_ms;
    msg.author = stored.author;
    msg.text = stored.text;
    emit_message(session, msg, stored.outgoing, stored.outgoing ? "delivered" : "");
  }

  pump_direct_chat(
      *session->chat,
      *session->files,
      *session->connection,
      [session]() { return session->running.load(); },
      [session]() {
        if (session->chat)
          session->chat->send_bye("пользователь вышел");
      },
      [this, session]() {
        drain_file_download_queue(session);
        pump_call_realtime(session);
      },
      [this, session](const nyx::ByteBuffer& payload) {
        return handle_avatar_bulk(session, payload);
      });

  finish_session(session, SessionState::Disconnected);
  emit_status("сессия завершена");
}

bool NodeService::send_message(const std::string& text, const std::string& session_id) {
  auto session = session_id.empty() ? active_session() : find_session(session_id);
  if (!session || session->state.load() != SessionState::Live)
    return false;
  if (session->chat)
    return session->chat->send_message(text);
  if (session->group_hub)
    return session->group_hub->send_message(text);
  if (session->group_member)
    return session->group_member->send_message(text);
  return false;
}

bool NodeService::send_bye(const std::string& reason) {
  auto session = active_session();
  if (!session || !session->chat)
    return false;
  return session->chat->send_bye(reason);
}

bool NodeService::index_folder(const std::string& path, const std::string& scope_group_id_hex) {
  const nyx::GroupId scope = scope_from_hex(scope_group_id_hex);
  const nyx::GroupId* scope_ptr = nullptr;
  if (!std::all_of(scope.begin(), scope.end(), [](uint8_t b) { return b == 0; })) {
    scope_ptr = &scope;
  }

  std::error_code ec;
  const std::string norm = nyx::normalize_utf8_path(path);
  const auto fs_path = nyx::path_from_utf8(norm);
  if (!std::filesystem::exists(fs_path, ec)) {
    emit_status("папка не найдена: " + path);
    return false;
  }
  if (!std::filesystem::is_directory(fs_path, ec)) {
    emit_status("не папка: " + path);
    return false;
  }

  FileIndexProgressCallback progress_cb;
  {
    std::lock_guard lock(cb_mutex_);
    progress_cb = on_file_index_progress_;
  }

  nyx::FileIndex::ScanProgressFn progress_fn;
  if (progress_cb) {
    progress_fn = [progress_cb](const std::string& file_path, int count, bool finished) {
      progress_cb(file_path, count, finished);
    };
  }

  if (!file_index_.add_root(norm, scope_ptr, progress_fn)) {
    emit_status("не удалось проиндексировать: " + norm);
    return false;
  }

  if (auto session = active_session()) {
    if (session->files)
      session->files->set_share_scope(session->share_scope);
  }
  const int file_count = file_index_.count_in_root(norm, scope);
  const bool scoped = scope_ptr != nullptr;
  emit_status("индекс" + std::string(scoped ? " (поле)" : " (личка)") + ": " +
              std::to_string(file_count) + " файлов в папке");
  if (scoped)
    publish_field_index();
  return true;
}

bool NodeService::remove_share_root(const std::string& path,
                                    const std::string& scope_group_id_hex) {
  const nyx::GroupId scope = scope_from_hex(scope_group_id_hex);
  const nyx::GroupId* scope_ptr = nullptr;
  if (!std::all_of(scope.begin(), scope.end(), [](uint8_t b) { return b == 0; })) {
    scope_ptr = &scope;
  }
  const std::string norm = nyx::normalize_utf8_path(path);
  if (!file_index_.remove_root(norm, scope_ptr)) {
    emit_status("папка не в индексе: " + norm);
    return false;
  }
  emit_status("папка убрана из индекса");
  // Always publish on a scoped removal (even an empty index resets the hub).
  if (scope_ptr)
    publish_field_index();
  // Reset the owner local Resources cache so the UI drops ghost entries.
  hub_remote_catalog_.clear();
  return true;
}

bool NodeService::can_request_remote_files() const {
  return !file_exchange_session_id({}).empty();
}

std::string NodeService::file_exchange_session_id(const std::string& scope_group_id_hex) const {
  if (!scope_group_id_hex.empty()) {
    const std::string key = make_group_session_id(scope_group_id_hex);
    auto s = find_session(key);
    if (s && s->state.load() == SessionState::Live && (s->files || s->group_hub)) {
      return key;
    }
  }
  auto session = active_session();
  if (session && session->state.load() == SessionState::Live &&
      (session->files || session->group_hub)) {
    return session->id;
  }
  return {};
}

std::string NodeService::file_exchange_hint() const {
  auto session = active_session();
  if (session && session->group_hub) {
    return "Hub поля: каталог и скачивание своего индекса доступны. Файлы участников — через "
           "клиент участника.";
  }
  if (session && session->files)
    return {};
  if (live_session_count() == 0) {
    return "Подключитесь к чату или полю — файлы доступны в активной сессии.";
  }
  return "Обмен файлами недоступен в текущем режиме. Выберите поле в списке чатов (в поле).";
}

std::vector<nyx::FileEntry> NodeService::remote_files() const {
  const std::string sid = file_exchange_session_id({});
  auto session = sid.empty() ? active_session() : find_session(sid);
  if (!session)
    return {};
  if (session->files)
    return session->files->remote_list_snapshot();
  if (session->group_hub) {
    if (!hub_remote_catalog_.empty())
      return hub_remote_catalog_;
    const auto profile = load_profile();
    return session->group_hub->catalog_for(profile.user_id());
  }
  return {};
}

bool NodeService::request_remote_files_at(const std::string& root_path,
                                          const std::string& parent_rel) {
  return request_remote_files_at({}, root_path, parent_rel);
}

bool NodeService::request_remote_files_at(const std::string& scope_group_id_hex,
                                          const std::string& root_path,
                                          const std::string& parent_rel) {
  std::string scope_hex = scope_group_id_hex;
  if (scope_hex.empty()) {
    if (auto active = active_session()) {
      if (active->kind == SessionKind::GroupHub || active->kind == SessionKind::GroupMember) {
        scope_hex = active->ref_id_hex;
      }
    }
  }
  if (scope_hex.empty()) {
    std::lock_guard lock(sessions_mutex_);
    if (active_session_id_.rfind("group:", 0) == 0 && active_session_id_.size() > 6) {
      scope_hex = active_session_id_.substr(6);
    }
  }

  const std::string sid = file_exchange_session_id(scope_hex);
  auto session = sid.empty() ? active_session() : find_session(sid);
  if (!session) {
    emit_status(file_exchange_hint());
    return false;
  }
  set_active_session(session->id);

  if (session->group_hub) {
    const auto profile = load_profile();
    std::vector<nyx::FileEntry> entries;
    if (root_path.empty()) {
      entries = session->group_hub->catalog_for(profile.user_id());
      hub_remote_catalog_ = entries;
    } else {
      entries = session->group_hub->catalog_level_for(profile.user_id(), root_path, parent_rel);
      const std::string normalized_root = nyx::normalize_utf8_path(root_path);
      const std::string normalized_parent = parent_rel;
      const std::string prefix =
          normalized_parent.empty() ? std::string {} : normalized_parent + "/";
      // Snapshot of this level: drop previous children, keep root marker.
      hub_remote_catalog_.erase(
          std::remove_if(hub_remote_catalog_.begin(),
                         hub_remote_catalog_.end(),
                         [&](const nyx::FileEntry& existing) {
                           if (nyx::normalize_utf8_path(existing.root_path) != normalized_root) {
                             return false;
                           }
                           if (normalized_parent.empty()) {
                             const std::string root_name =
                                 nyx::path_to_utf8(nyx::path_from_utf8(normalized_root).filename());
                             if (existing.is_directory() &&
                                 (existing.relative_path == root_name ||
                                  existing.relative_path.rfind("участник:", 0) == 0)) {
                               return false;
                             }
                             return true;
                           }
                           return existing.relative_path == normalized_parent ||
                                  existing.relative_path.rfind(prefix, 0) == 0;
                         }),
          hub_remote_catalog_.end());
      for (auto& e : entries) {
        hub_remote_catalog_.push_back(std::move(e));
      }
    }
    RemoteFilesCallback cb;
    {
      std::lock_guard lock(cb_mutex_);
      cb = on_remote_files_;
    }
    if (cb)
      cb(hub_remote_catalog_);
    emit_status("каталог поля: " + std::to_string(hub_remote_catalog_.size()) + " объектов");
    return true;
  }
  if (!session->files) {
    emit_status(file_exchange_hint());
    return false;
  }
  if (root_path.empty()) {
    if (!session->files->request_list()) {
      emit_status("не удалось запросить список файлов");
      return false;
    }
  } else if (!session->files->request_list(root_path, parent_rel)) {
    emit_status("не удалось запросить уровень каталога");
    return false;
  }
  return true;
}

std::vector<nyx::FileEntry>
NodeService::local_files_for_scope(const std::string& scope_group_id_hex) const {
  return file_index_.listing_for_session(scope_from_hex(scope_group_id_hex));
}

int NodeService::file_count_in_root(const std::string& root_path,
                                    const std::string& scope_group_id_hex) const {
  return file_index_.count_in_root(root_path, scope_from_hex(scope_group_id_hex));
}

bool NodeService::rescan_share_root(const std::string& path,
                                    const std::string& scope_group_id_hex) {
  const nyx::GroupId scope = scope_from_hex(scope_group_id_hex);
  const nyx::GroupId* scope_ptr = nullptr;
  if (!std::all_of(scope.begin(), scope.end(), [](uint8_t b) { return b == 0; })) {
    scope_ptr = &scope;
  }
  const std::string norm = nyx::normalize_utf8_path(path);

  FileIndexProgressCallback progress_cb;
  {
    std::lock_guard lock(cb_mutex_);
    progress_cb = on_file_index_progress_;
  }
  nyx::FileIndex::ScanProgressFn progress_fn;
  if (progress_cb) {
    progress_fn = [progress_cb](const std::string& file_path, int count, bool finished) {
      progress_cb(file_path, count, finished);
    };
  }

  if (!file_index_.rescan_root(norm, scope_ptr, progress_fn)) {
    emit_status("не удалось переиндексировать: " + norm);
    return false;
  }
  const int file_count = file_index_.count_in_root(norm, scope);
  emit_status("переиндексировано: " + std::to_string(file_count) + " файлов");
  if (scope_ptr)
    publish_field_index();
  return true;
}

std::vector<nyx::ShareRoot>
NodeService::share_roots_for_scope(const std::string& scope_group_id_hex) const {
  return file_index_.roots_for_session(scope_from_hex(scope_group_id_hex));
}

bool NodeService::request_file_access_policy() {
  auto session = active_session();
  if (!session || !session->files)
    return false;
  return session->files->request_policy();
}

void NodeService::emit_transfer_queue_changed() {
  TransferQueueCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_transfer_queue_changed_;
  }
  if (cb)
    cb();
}

void NodeService::save_download_queue(const std::shared_ptr<NetSession>& session) const {
  if (!session)
    return;
  std::lock_guard persistence_lock(transfer_queue_file_mutex);
  const std::string path = nyx::data_dir() + "/transfer_queue.txt";
  std::vector<std::string> preserved;
  {
    std::ifstream existing(nyx::path_from_utf8(path), std::ios::binary);
    std::string line;
    while (std::getline(existing, line)) {
      std::istringstream parser(line);
      std::string stored_session;
      if (!(parser >> std::quoted(stored_session)) || stored_session != session->id) {
        preserved.push_back(std::move(line));
      }
    }
  }
  const std::string temporary = path + ".tmp";
  std::ofstream out(nyx::path_from_utf8(temporary), std::ios::binary | std::ios::trunc);
  if (!out)
    return;
  for (const auto& line : preserved)
    out << line << '\n';
  std::lock_guard lock(session->download_mutex);
  for (const auto& item : session->download_queue) {
    out << std::quoted(session->id) << ' ' << std::quoted(item.hash_hex) << ' '
        << std::quoted(item.dest_path) << ' '
        << std::quoted(item.state == "active" ? "queued" : item.state) << ' ' << item.progress
        << ' ' << item.paused << ' ' << std::quoted(item.error) << '\n';
  }
  out.close();
  std::error_code ec;
  std::filesystem::remove(nyx::path_from_utf8(path), ec);
  ec.clear();
  std::filesystem::rename(nyx::path_from_utf8(temporary), nyx::path_from_utf8(path), ec);
}

void NodeService::load_download_queue(const std::shared_ptr<NetSession>& session) {
  if (!session)
    return;
  std::lock_guard persistence_lock(transfer_queue_file_mutex);
  std::ifstream in(nyx::path_from_utf8(nyx::data_dir() + "/transfer_queue.txt"), std::ios::binary);
  if (!in)
    return;
  std::deque<FileDownloadRequest> restored;
  std::string session_id;
  FileDownloadRequest item;
  while (in >> std::quoted(session_id) >> std::quoted(item.hash_hex) >>
         std::quoted(item.dest_path) >> std::quoted(item.state) >> item.progress >> item.paused >>
         std::quoted(item.error)) {
    if (session_id != session->id)
      continue;
    if (item.state == "active")
      item.state = "queued";
    restored.push_back(item);
  }
  if (restored.empty())
    return;
  {
    std::lock_guard lock(session->download_mutex);
    if (session->download_queue.empty()) {
      session->download_queue = std::move(restored);
    }
  }
  emit_transfer_queue_changed();
}

std::vector<NodeService::TransferQueueItem> NodeService::transfer_queue() const {
  std::vector<TransferQueueItem> out;
  auto session = active_session();
  if (!session)
    return out;
  {
    std::lock_guard lock(session->download_mutex);
    out.reserve(session->download_queue.size() + 4);
    for (const auto& item : session->download_queue) {
      out.push_back({item.hash_hex,
                     item.dest_path,
                     item.state,
                     item.progress,
                     item.paused,
                     item.error,
                     "download"});
    }
  }
  if (session->files) {
    for (const auto& [hash, name] : session->files->outgoing_queue_snapshot()) {
      out.push_back({hash, name, "active", 0, false, {}, "upload"});
    }
  }
  return out;
}

bool NodeService::pause_transfer(const std::string& hash_hex, bool paused) {
  auto session = active_session();
  if (!session)
    return false;
  bool found = false;
  {
    std::lock_guard lock(session->download_mutex);
    for (auto& item : session->download_queue) {
      if (item.hash_hex != hash_hex)
        continue;
      item.paused = paused;
      item.state = paused ? "paused" : "queued";
      item.error.clear();
      found = true;
      break;
    }
  }
  if (!found)
    return false;
  if (paused && session->files)
    session->files->cancel(hash_hex);
  save_download_queue(session);
  emit_transfer_queue_changed();
  return true;
}

bool NodeService::cancel_transfer(const std::string& hash_hex) {
  auto session = active_session();
  if (!session)
    return false;
  if (session->files)
    session->files->cancel(hash_hex);
  bool removed = false;
  {
    std::lock_guard lock(session->download_mutex);
    const auto old_size = session->download_queue.size();
    session->download_queue.erase(
        std::remove_if(session->download_queue.begin(),
                       session->download_queue.end(),
                       [&](const FileDownloadRequest& item) { return item.hash_hex == hash_hex; }),
        session->download_queue.end());
    removed = session->download_queue.size() != old_size;
  }
  save_download_queue(session);
  emit_transfer_queue_changed();
  return removed;
}

bool NodeService::retry_transfer(const std::string& hash_hex) {
  return pause_transfer(hash_hex, false);
}

bool NodeService::move_transfer(const std::string& hash_hex, int delta) {
  auto session = active_session();
  if (!session || delta == 0)
    return false;
  bool moved = false;
  {
    std::lock_guard lock(session->download_mutex);
    auto it =
        std::find_if(session->download_queue.begin(),
                     session->download_queue.end(),
                     [&](const FileDownloadRequest& item) { return item.hash_hex == hash_hex; });
    if (it == session->download_queue.end() || it->state == "active") {
      return false;
    }
    const auto index = std::distance(session->download_queue.begin(), it);
    const auto target = index + (delta < 0 ? -1 : 1);
    if (target < 0 || target >= static_cast<decltype(target)>(session->download_queue.size())) {
      return false;
    }
    std::iter_swap(it, session->download_queue.begin() + target);
    moved = true;
  }
  if (moved) {
    save_download_queue(session);
    emit_transfer_queue_changed();
  }
  return moved;
}

bool NodeService::download_file(const std::string& hash_hex,
                                const std::string& dest_path,
                                const std::string& session_id) {
  if (hash_hex.empty() || dest_path.empty())
    return false;
  auto session = session_id.empty() ? active_session() : find_session(session_id);
  if (!session || (!session->files && !session->group_hub))
    return false;
  {
    std::lock_guard lock(session->download_mutex);
    for (const auto& item : session->download_queue) {
      if (item.hash_hex == hash_hex)
        return true;
    }
    session->download_queue.push_back(FileDownloadRequest {hash_hex, dest_path});
  }
  save_download_queue(session);
  emit_transfer_queue_changed();
  return true;
}

void NodeService::drain_file_download_queue(const std::shared_ptr<NetSession>& session) {
  if (!session)
    return;
  if (session->files && session->files->busy())
    return;

  FileDownloadRequest next;
  std::size_t next_index = 0;
  {
    std::lock_guard lock(session->download_mutex);
    if (session->download_queue.empty())
      return;
    bool found = false;
    for (std::size_t i = 0; i < session->download_queue.size(); ++i) {
      auto& item = session->download_queue[i];
      if (item.paused || item.state == "paused" || item.state == "failed" ||
          item.state == "active") {
        continue;
      }
      next = item;
      next_index = i;
      found = true;
      break;
    }
    if (!found)
      return;
    if (next_index != 0) {
      auto it = session->download_queue.begin() + static_cast<std::ptrdiff_t>(next_index);
      std::rotate(session->download_queue.begin(), it, it + 1);
    }
  }

  if (session->group_hub && !session->files) {
    nyx::FileHash hash {};
    if (!nyx::hash_from_hex(next.hash_hex, hash)) {
      {
        std::lock_guard lock(session->download_mutex);
        if (!session->download_queue.empty() &&
            session->download_queue.front().hash_hex == next.hash_hex) {
          session->download_queue.pop_front();
        }
      }
      save_download_queue(session);
      emit_transfer_queue_changed();
      emit_status("неверный hash файла");
      return;
    }
    std::string saved;
    if (session->group_hub->download_local_file(hash, next.dest_path, &saved)) {
      {
        std::lock_guard lock(session->download_mutex);
        if (!session->download_queue.empty() &&
            session->download_queue.front().hash_hex == next.hash_hex) {
          session->download_queue.pop_front();
        }
      }
      save_download_queue(session);
      emit_transfer_queue_changed();
      emit_status("файл сохранён: " + saved);
      return;
    }
    if (session->group_hub->provider_transfer_busy(hash))
      return;
    if (session->group_hub->request_file_from_provider(hash, next.dest_path)) {
      {
        std::lock_guard lock(session->download_mutex);
        if (!session->download_queue.empty() &&
            session->download_queue.front().hash_hex == next.hash_hex) {
          session->download_queue.front().state = "active";
          session->download_queue.front().progress = 0;
          session->download_queue.front().error.clear();
        }
      }
      save_download_queue(session);
      emit_transfer_queue_changed();
      emit_status("запрос файла у участника поля");
      return;
    }
    {
      std::lock_guard lock(session->download_mutex);
      if (!session->download_queue.empty() &&
          session->download_queue.front().hash_hex == next.hash_hex) {
        session->download_queue.front().state = "failed";
        session->download_queue.front().error = "нет источников";
      }
    }
    save_download_queue(session);
    emit_transfer_queue_changed();
    emit_status("нет доступных источников файла");
    return;
  }

  if (!session->files)
    return;

  if (!session->files->request_file(next.hash_hex, next.dest_path)) {
    if (session->files->busy())
      return;
    {
      std::lock_guard lock(session->download_mutex);
      if (!session->download_queue.empty() &&
          session->download_queue.front().hash_hex == next.hash_hex) {
        session->download_queue.front().state = "failed";
        session->download_queue.front().error = "не удалось запросить файл";
      }
    }
    save_download_queue(session);
    emit_transfer_queue_changed();
    emit_status("не удалось запросить файл");
    return;
  }

  {
    std::lock_guard lock(session->download_mutex);
    if (!session->download_queue.empty() &&
        session->download_queue.front().hash_hex == next.hash_hex) {
      session->download_queue.front().state = "active";
      session->download_queue.front().progress = 0;
    }
  }
  save_download_queue(session);
  emit_transfer_queue_changed();
  const std::string short_hash =
      next.hash_hex.size() > 8 ? next.hash_hex.substr(0, 8) + "…" : next.hash_hex;
  emit_status("запрос файла " + short_hash);
}

void NodeService::try_pump_download_queue() {
  if (auto session = active_session())
    drain_file_download_queue(session);
}

std::size_t NodeService::enqueue_folder_downloads(const std::string& root_path,
                                                  const std::string& folder_rel,
                                                  const std::string& dest_dir) {
  if (dest_dir.empty())
    return 0;
  auto session = active_session();
  if (!session || (!session->files && !session->group_hub))
    return 0;

  const std::string root_norm = nyx::normalize_grant_root(root_path);
  std::string folder = folder_rel;
  for (char& c : folder) {
    if (c == '\\')
      c = '/';
  }
  while (!folder.empty() && folder.front() == '/')
    folder.erase(folder.begin());
  while (!folder.empty() && folder.back() == '/')
    folder.pop_back();

  struct QueuedFile {
    std::string hash_hex;
    std::string dest_path;
  };
  std::vector<QueuedFile> items;
  for (const auto& e : remote_files()) {
    if (e.is_directory())
      continue;
    if (nyx::normalize_grant_root(e.root_path) != root_norm)
      continue;
    std::string rel = e.relative_path;
    for (char& c : rel) {
      if (c == '\\')
        c = '/';
    }
    if (!folder.empty()) {
      if (rel.size() <= folder.size())
        continue;
      if (rel.compare(0, folder.size(), folder) != 0)
        continue;
      if (rel[folder.size()] != '/')
        continue;
      rel = rel.substr(folder.size() + 1);
    }
    std::string dest = dest_dir;
    if (!dest.empty() && dest.back() != '/' && dest.back() != '\\')
      dest += '/';
    dest += rel;
    items.push_back(QueuedFile {nyx::hash_hex(e.hash), std::move(dest)});
  }
  if (items.empty())
    return 0;

  const std::size_t total = items.size();
  {
    std::lock_guard lock(session->download_mutex);
    for (auto& item : items) {
      const bool exists =
          std::any_of(session->download_queue.begin(),
                      session->download_queue.end(),
                      [&](const FileDownloadRequest& q) { return q.hash_hex == item.hash_hex; });
      if (exists)
        continue;
      session->download_queue.push_back(
          FileDownloadRequest {std::move(item.hash_hex), std::move(item.dest_path)});
    }
  }
  save_download_queue(session);
  emit_transfer_queue_changed();
  return total;
}

bool NodeService::send_file(const std::string& path_or_hash) {
  auto session = active_session();
  if (!session || !session->files) {
    emit_status(file_exchange_hint());
    return false;
  }
  return session->files->send_file(path_or_hash);
}

std::optional<nyx::FileEntry> NodeService::import_file_object(const std::string& path,
                                                              const std::string& display_name,
                                                              const std::string& mime,
                                                              const std::string& scope_group_id_hex,
                                                              const std::string& owner_user_id_hex,
                                                              const std::string& relative_dir) {
  nyx::UserId owner {};
  const nyx::UserId* owner_ptr = nullptr;
  if (!owner_user_id_hex.empty()) {
    std::vector<uint8_t> bytes;
    if (nyx::from_hex(owner_user_id_hex, bytes) && bytes.size() == owner.size()) {
      std::memcpy(owner.data(), bytes.data(), owner.size());
      owner_ptr = &owner;
    }
  }
  if (!owner_ptr) {
    owner = load_profile().user_id();
    owner_ptr = &owner;
  }
  const auto imported = file_index_.import_file(
      path, display_name, mime, scope_from_hex(scope_group_id_hex), owner_ptr, relative_dir);
  if (imported && !scope_group_id_hex.empty())
    publish_field_index();
  return imported;
}

std::optional<nyx::FileEntry> NodeService::find_file_object(const std::string& hash_hex) const {
  auto entry = file_index_.find_by_hash_hex(hash_hex);
  if (!entry || entry->is_directory())
    return std::nullopt;
  std::error_code ec;
  const std::string abs = entry->absolute_path();
  if (abs.size() >= 5 && abs.compare(abs.size() - 5, 5, ".part") == 0) {
    return std::nullopt;
  }
  if (!std::filesystem::is_regular_file(nyx::path_from_utf8(abs), ec)) {
    return std::nullopt;
  }
  return entry;
}

} // namespace nyx_app
