#include "node_service.hpp"

#include "connect_via_hint.hpp"

#include "nyx/app.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/group.hpp"
#include "nyx/paths.hpp"
#include "nyx/rendezvous_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <thread>

namespace nyx_app {

void NodeService::run_group_hub(std::shared_ptr<NetSession> session, std::string group_id_hex) {
  set_mode(NodeMode::GroupHub);
  while (!group_id_hex.empty() && std::isspace(static_cast<unsigned char>(group_id_hex.front()))) {
    group_id_hex.erase(group_id_hex.begin());
  }
  while (!group_id_hex.empty() && std::isspace(static_cast<unsigned char>(group_id_hex.back()))) {
    group_id_hex.pop_back();
  }
  nyx::GroupId group_id{};
  if (!nyx::GroupStore::group_id_from_hex(group_id_hex, group_id)) {
    emit_status("неверный group_id");
    finish_session(session, SessionState::Offline);
    return;
  }

  nyx::GroupStore store;
  store.load();
  auto group = store.find(group_id);
  if (!group) {
    emit_status("поле не найдено");
    finish_session(session, SessionState::Offline);
    return;
  }

  const auto profile = load_profile();
  if (group->owner_id != profile.user_id()) {
    emit_status("только владелец может запустить hub");
    finish_session(session, SessionState::Offline);
    return;
  }

  nyx::UdpSocket socket;
  if (!socket.bind("0.0.0.0", 0)) {
    emit_status("bind failed");
    finish_session(session, SessionState::Offline);
    return;
  }

  std::string rendezvous_host;
  uint16_t rendezvous_port = 0;
  if (!parse_rendezvous(rendezvous_host, rendezvous_port)) {
    emit_status("неверный rendezvous");
    finish_session(session, SessionState::Offline);
    return;
  }

  nyx::RendezvousPool rv(std::move(socket));
  rv.set_servers(network_config_.rendezvous_servers);
  if (!rv.register_token(group->invite_token)) {
    emit_status("register invite failed");
    finish_session(session, SessionState::Offline);
    return;
  }

  emit_status("hub «" + group->name + "», invite: " +
              nyx::GroupStore::invite_hex(group->invite_token));
  session->ref_id_hex = nyx::GroupStore::group_id_hex(group->id);
  session->share_scope = group_id;
  remember_intent_for_session(session, nyx::GroupStore::invite_hex(group->invite_token));

  // GroupHub до chat_ready: иначе Live без send_message → «Не удалось отправить».
  session->group_hub = std::make_unique<nyx::GroupHub>(rv.socket(), profile, *group);
  session->group_hub->attach_files(file_index_, group_id, &file_access_);
  session->group_hub->set_on_message([this, session](const nyx::ChatMessage& msg, bool outgoing) {
    emit_message(session, msg, outgoing);
  });
  session->group_hub->set_on_event([this](const std::string& text) { emit_status(text); });
  sync_live_group_from_session(session);

  emit_chat_ready(session, group->name, ConnectionVia::Group, {}, nyx::ConversationKind::Group,
                  session->ref_id_hex);

  const nyx::InviteToken hub_invite = group->invite_token;
  const auto rv_servers = network_config_.rendezvous_servers;
  const auto refresh_interval =
      std::chrono::seconds(network_config_.register_refresh_sec);
  auto last_register = std::chrono::steady_clock::now();

  while (session->running.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_register >= refresh_interval) {
      nyx::register_token_on(session->group_hub->socket(), rv_servers, hub_invite);
      last_register = now;
    }
    drain_file_download_queue(session);
    session->group_hub->poll();
    sync_live_group_from_session(session);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  session->group_hub->notify_shutdown("hub остановлен");
  // Дать UDP Bye уйти до unregister.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  nyx::unregister_token_on(session->group_hub->socket(), rv_servers, hub_invite);
  clear_live_group_snapshot(group_id);
  finish_session(session, SessionState::Disconnected);
  emit_status("hub остановлен");
}

void NodeService::run_group_join(std::shared_ptr<NetSession> session, std::string invite_hex) {
  set_mode(NodeMode::GroupMember);
  nyx::InviteToken token{};
  if (!nyx::GroupStore::invite_from_hex(invite_hex, token)) {
    emit_status("неверный invite");
    finish_session(session, SessionState::Offline);
    return;
  }

  nyx::GroupStore store;
  store.load();
  auto group = store.find_by_invite(token);
  std::string group_name = group ? group->name : "поле";

  auto bind_group_session_key = [&]() {
    if (!group || !session) return;
    const std::string final_id =
        make_group_session_id(nyx::GroupStore::group_id_hex(group->id));
    std::lock_guard lock(sessions_mutex_);
    if (session->id == final_id) return;
    sessions_.erase(session->id);
    session->id = final_id;
    session->ref_id_hex = nyx::GroupStore::group_id_hex(group->id);
    sessions_[final_id] = session;
  };

  nyx::UdpSocket socket;
  if (!socket.bind("0.0.0.0", 0)) {
    emit_status("bind failed");
    bind_group_session_key();
    finish_session(session, SessionState::Offline);
    return;
  }

  std::string rendezvous_host;
  uint16_t rendezvous_port = 0;
  if (!parse_rendezvous(rendezvous_host, rendezvous_port)) {
    emit_status("неверный rendezvous");
    bind_group_session_key();
    finish_session(session, SessionState::Offline);
    return;
  }

  nyx::RendezvousPool rv(std::move(socket));
  rv.set_servers(network_config_.rendezvous_servers);

  auto connect_with_lookup = [&](nyx::RendezvousPool& pool) -> bool {
    auto hint = pool.lookup(token);
    if (!hint) return false;
    emit_status("подключение к hub " + hint->host_string() + ':' +
                std::to_string(hint->port) + "...");
    auto result = connect_via_rendezvous_hint(pool.socket(), *hint);
    if (!result.connection) return false;
    session->connection = std::make_unique<nyx::Connection>(std::move(*result.connection));
    return true;
  };

  bool connected = false;
  for (int attempt = 1; attempt <= 4 && session->running.load(); ++attempt) {
    if (attempt > 1) {
      emit_status("повтор lookup (" + std::to_string(attempt) + "/4)…");
      std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
      nyx::UdpSocket retry_socket;
      if (!retry_socket.bind("0.0.0.0", 0)) continue;
      nyx::RendezvousPool retry_pool(std::move(retry_socket));
      retry_pool.set_servers(network_config_.rendezvous_servers);
      if (connect_with_lookup(retry_pool)) {
        connected = true;
        break;
      }
    } else if (connect_with_lookup(rv)) {
      connected = true;
      break;
    }
  }

  if (!connected || !session->connection) {
    emit_status("lookup failed — hub запущен и тот же rendezvous?");
    bind_group_session_key();
    finish_session(session, SessionState::Offline);
    return;
  }

  const auto profile = load_profile();

  nyx::HelloMessage peer_hello;
  if (!nyx::exchange_hello(*session->connection, profile, peer_hello, 10,
                           [session]() { return session->running.load(); })) {
    emit_status("Hello timeout");
    bind_group_session_key();
    finish_session(session, SessionState::Offline);
    return;
  }

  nyx::GroupId gid{};
  if (group) gid = group->id;
  session->share_scope = gid;

  session->group_member = std::make_unique<nyx::GroupMemberService>(
      *session->connection, profile, gid, group_name);
  session->group_member->set_on_message(
      [this, session](const nyx::ChatMessage& msg, bool outgoing) {
        emit_message(session, msg, outgoing);
      });
  session->group_member->set_on_event([this](const std::string& text) { emit_status(text); });

  if (!session->group_member->join()) {
    emit_status("join failed");
    bind_group_session_key();
    finish_session(session, SessionState::Offline);
    return;
  }

  {
    const auto& view = session->group_member->view();
    session->share_scope = view.id;
    nyx::GroupStore store2;
    store2.load();
    nyx::GroupRecord rec;
    rec.id = view.id;
    rec.name = view.name;
    rec.invite_token = token;
    rec.members = view.members;
    for (const auto& m : view.members) {
      if (m.role == nyx::GroupRole::Owner) {
        rec.owner_id = m.user_id;
        break;
      }
    }
    store2.upsert(rec);
    store2.save();
    group_name = view.name;

    const std::string final_id = make_group_session_id(nyx::GroupStore::group_id_hex(view.id));
    {
      std::lock_guard lock(sessions_mutex_);
      const std::string old_id = session->id;
      if (session->id != final_id) {
        sessions_.erase(session->id);
        session->id = final_id;
        sessions_[final_id] = session;
      }
      if (active_session_id_.empty() || active_session_id_ == old_id ||
          active_session_id_ == final_id) {
        active_session_id_ = final_id;
      }
      session->ref_id_hex = nyx::GroupStore::group_id_hex(view.id);
    }
  }

  sync_live_group_from_session(session);
  remember_intent_for_session(session, invite_hex);

  emit_status("в поле «" + group_name + "»");
  emit_chat_ready(session, group_name, ConnectionVia::Group, {}, nyx::ConversationKind::Group,
                  session->ref_id_hex);

  session->files = std::make_unique<nyx::FileTransferService>(
      *session->connection, file_index_, nyx::default_downloads_dir());
  session->files->set_share_scope(session->share_scope);
  wire_file_transfer(session, *session->files);
  publish_field_index();
  request_file_access_policy();

  while (session->running.load() && session->group_member->joined()) {
    drain_file_download_queue(session);
    session->group_member->tick();
    if (!session->group_member->joined()) break;
    if (session->files) session->files->pump();
    nyx::ByteBuffer payload;
    uint32_t stream_id = 0;
    while (session->connection->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kChatStream) {
        session->group_member->handle_payload(payload);
        sync_live_group_from_session(session);
      } else if (stream_id == nyx::kBulkStream) {
        if (!try_apply_file_access_policy(payload) && session->files) {
          session->files->handle_bulk(payload);
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  clear_live_group_snapshot(session->share_scope);
  const bool user_stopped = !session->running.load();
  finish_session(session, user_stopped ? SessionState::Disconnected : SessionState::Offline);
  emit_status(user_stopped ? "выход из поля" : "поле недоступно (владелец офлайн)");
}

}  // namespace nyx_app
