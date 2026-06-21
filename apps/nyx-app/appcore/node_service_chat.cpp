#include "node_service.hpp"

#include "direct_chat_loop.hpp"

#include <algorithm>

#include "nyx/app.hpp"
#include "nyx/chat_service.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

namespace nyx_app {

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

  chat_->set_on_message(
      [this](const nyx::ChatMessage& msg, bool outgoing) { emit_message(msg, outgoing); });
  chat_->set_on_event([this](const std::string& text) { emit_status(text); });
  files_->set_on_event([this](const std::string& text) { emit_status(text); });
  files_->set_on_progress([this](const nyx::FileHash& hash, uint64_t done, uint64_t total) {
    FileProgressCallback cb;
    {
      std::lock_guard lock(cb_mutex_);
      cb = on_file_progress_;
    }
    if (!cb || total == 0) return;
    const int pct = static_cast<int>((done * 100) / total);
    cb(nyx::hash_hex(hash).substr(0, 8) + "…", pct);
  });

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

bool NodeService::index_folder(const std::string& path) {
  const nyx::GroupId* scope_ptr = nullptr;
  const bool scoped = !std::all_of(share_scope_group_.begin(), share_scope_group_.end(),
                                   [](uint8_t b) { return b == 0; });
  if (scoped) scope_ptr = &share_scope_group_;

  if (!file_index_.add_root(path, scope_ptr)) {
    emit_status("не удалось проиндексировать: " + path);
    return false;
  }
  if (files_) files_->set_share_scope(share_scope_group_);
  const auto visible = file_index_.entries_for_session(share_scope_group_);
  const std::string scope_label = scoped ? " (поле)" : " (личка)";
  emit_status("индекс" + scope_label + ": " + std::to_string(visible.size()) + " файлов");
  return true;
}

bool NodeService::request_remote_files() {
  return files_ && files_->request_list();
}

bool NodeService::download_file(const std::string& hash_hex) {
  return files_ && files_->request_file(hash_hex);
}

bool NodeService::send_file(const std::string& path_or_hash) {
  return files_ && files_->send_file(path_or_hash);
}

std::vector<nyx::StoredMessage> NodeService::chat_history(std::size_t count) const {
  if (chat_) return chat_->history(count);
  if (group_hub_) return group_hub_->store().recent(count);
  return {};
}

}  // namespace nyx_app
