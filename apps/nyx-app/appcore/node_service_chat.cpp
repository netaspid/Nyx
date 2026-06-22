#include "node_service.hpp"

#include "direct_chat_loop.hpp"

#include <algorithm>
#include <filesystem>

#include "nyx/app.hpp"
#include "nyx/chat_service.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/group.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

namespace nyx_app {

nyx::GroupId NodeService::scope_from_hex(const std::string& scope_group_id_hex) const {
  nyx::GroupId scope{};
  if (!scope_group_id_hex.empty()) {
    nyx::GroupStore::group_id_from_hex(scope_group_id_hex, scope);
  } else if (!std::all_of(share_scope_group_.begin(), share_scope_group_.end(),
                          [](uint8_t b) { return b == 0; })) {
    scope = share_scope_group_;
  }
  return scope;
}

void NodeService::wire_file_transfer(nyx::FileTransferService& files) {
  files.set_on_event([this](const std::string& text) { emit_status(text); });
  files.set_on_progress([this](const nyx::FileHash& hash, uint64_t done, uint64_t total) {
    FileProgressCallback cb;
    {
      std::lock_guard lock(cb_mutex_);
      cb = on_file_progress_;
    }
    if (!cb || total == 0) return;
    const int pct = static_cast<int>((done * 100) / total);
    cb(nyx::hash_hex(hash).substr(0, 8) + "…", pct);
  });
  files.set_on_remote_list([this](const std::vector<nyx::FileEntry>& list) {
    RemoteFilesCallback cb;
    {
      std::lock_guard lock(cb_mutex_);
      cb = on_remote_files_;
    }
    if (cb) cb(list);
  });
}

void NodeService::run_direct_chat(std::unique_ptr<nyx::Connection> connection,
                                  const nyx::Profile& profile, bool incoming,
                                  ConnectionVia via) {
  set_mode(NodeMode::ChatDirect);
  share_scope_group_ = {};
  connection_ = std::move(connection);

  const std::string peer_host = connection_->peer_host();
  const std::string endpoint = peer_host + ':' + std::to_string(connection_->peer_port());
  emit_status(incoming ? "входящее соединение " + endpoint
                       : "соединение с " + endpoint);

  nyx::HelloMessage peer_hello;
  if (!nyx::exchange_hello(*connection_, profile, peer_hello, 10,
                            [this]() { return running_.load(); })) {
    emit_status("Hello timeout");
    connection_.reset();
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  nyx::remember_contact(peer_hello);
  emit_status(peer_hello.nickname + " в сети (id: " +
              nyx::short_user_id(peer_hello.public_key) + ")");
  emit_chat_ready(peer_hello.nickname, via, peer_host, nyx::ConversationKind::Direct,
                  nyx::to_hex(peer_hello.public_key.data(), peer_hello.public_key.size()));

  nyx::ChatService::PeerInfo peer;
  peer.user_id = peer_hello.public_key;
  peer.nickname = peer_hello.nickname;

  chat_ = std::make_unique<nyx::ChatService>(*connection_, profile, peer);
  files_ = std::make_unique<nyx::FileTransferService>(
      *connection_, file_index_, nyx::default_downloads_dir());
  files_->set_share_scope(share_scope_group_);
  wire_file_transfer(*files_);

  chat_->set_on_message(
      [this](const nyx::ChatMessage& msg, bool outgoing) { emit_message(msg, outgoing); });
  chat_->set_on_event([this](const std::string& text) { emit_status(text); });

  const auto recent = chat_->history(30);
  for (const auto& stored : recent) {
    nyx::ChatMessage msg;
    msg.id = stored.id;
    msg.timestamp_ms = stored.timestamp_ms;
    msg.author = stored.author;
    msg.text = stored.text;
    emit_message(msg, stored.outgoing);
  }

  pump_direct_chat(*chat_, *files_, *connection_, [this]() { return running_.load(); },
                   [this]() { chat_->send_bye("пользователь вышел"); });

  chat_.reset();
  files_.reset();
  connection_.reset();
  busy_.store(false);
  set_mode(NodeMode::Idle);
  emit_session_ended();
  emit_status("сессия завершена");
}

bool NodeService::send_message(const std::string& text) {
  if (chat_) return chat_->send_message(text);
  if (group_hub_) return group_hub_->send_message(text);
  if (group_member_) return group_member_->send_message(text);
  return false;
}

bool NodeService::send_bye(const std::string& reason) {
  if (chat_) return chat_->send_bye(reason);
  return false;
}

bool NodeService::index_folder(const std::string& path,
                               const std::string& scope_group_id_hex) {
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

  if (files_) files_->set_share_scope(share_scope_group_);
  const int file_count = file_index_.count_in_root(norm);
  const bool scoped = scope_ptr != nullptr;
  emit_status("индекс" + std::string(scoped ? " (поле)" : " (личка)") + ": " +
              std::to_string(file_count) + " файлов в папке");
  if (scoped && (group_member_ || group_hub_)) publish_field_index();
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
  if (scope_ptr && (group_member_ || group_hub_)) publish_field_index();
  return true;
}

bool NodeService::can_request_remote_files() const {
  return files_ != nullptr;
}

std::string NodeService::file_exchange_hint() const {
  if (group_hub_) {
    return "Hub поля: ваши файлы доступны участникам. Запрос списка у peer недоступен в режиме hub.";
  }
  if (files_) return {};
  if (mode_.load() == NodeMode::Idle) {
    return "Подключитесь к чату или полю — файлы доступны в активной сессии.";
  }
  return "Обмен файлами недоступен в текущем режиме.";
}

std::vector<nyx::FileEntry> NodeService::local_files_for_scope(
    const std::string& scope_group_id_hex) const {
  return file_index_.listing_for_session(scope_from_hex(scope_group_id_hex));
}

int NodeService::file_count_in_root(const std::string& root_path) const {
  return file_index_.count_in_root(root_path);
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
  const int file_count = file_index_.count_in_root(norm);
  emit_status("переиндексировано: " + std::to_string(file_count) + " файлов");
  if (scope_ptr && (group_member_ || group_hub_)) publish_field_index();
  return true;
}

std::vector<nyx::ShareRoot> NodeService::share_roots_for_scope(
    const std::string& scope_group_id_hex) const {
  return file_index_.roots_for_session(scope_from_hex(scope_group_id_hex));
}

std::vector<nyx::FileEntry> NodeService::remote_files() const {
  if (!files_) return {};
  return files_->remote_list();
}

bool NodeService::request_remote_files() {
  if (!files_) {
    emit_status(file_exchange_hint());
    return false;
  }
  if (!files_->request_list()) {
    emit_status("не удалось запросить список файлов");
    return false;
  }
  return true;
}

bool NodeService::download_file(const std::string& hash_hex) {
  if (!files_) {
    emit_status(file_exchange_hint());
    return false;
  }
  if (!files_->request_file(hash_hex)) {
    emit_status("не удалось запросить файл");
    return false;
  }
  return true;
}

bool NodeService::send_file(const std::string& path_or_hash) {
  if (!files_) {
    emit_status(file_exchange_hint());
    return false;
  }
  if (!files_->send_file(path_or_hash)) {
    return false;
  }
  return true;
}

std::vector<nyx::StoredMessage> NodeService::chat_history(std::size_t count) const {
  if (chat_) return chat_->history(count);
  if (group_hub_) return group_hub_->store().recent(count);
  return {};
}

}  // namespace nyx_app
