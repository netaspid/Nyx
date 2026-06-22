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

void NodeService::run_group_hub(std::string group_id_hex) {
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
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  nyx::GroupStore store;
  store.load();
  auto group = store.find(group_id);
  if (!group) {
    emit_status("поле не найдено");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  const auto profile = load_profile();
  if (group->owner_id != profile.user_id()) {
    emit_status("только владелец может запустить hub");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  nyx::UdpSocket socket;
  if (!socket.bind("0.0.0.0", 0)) {
    emit_status("bind failed");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  std::string rendezvous_host;
  uint16_t rendezvous_port = 0;
  if (!parse_rendezvous(rendezvous_host, rendezvous_port)) {
    emit_status("неверный rendezvous");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  nyx::RendezvousPool rv(std::move(socket));
  rv.set_servers(network_config_.rendezvous_servers);
  if (!rv.register_token(group->invite_token)) {
    emit_status("register invite failed");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  emit_status("hub «" + group->name + "», invite: " +
              nyx::GroupStore::invite_hex(group->invite_token));
  emit_chat_ready(group->name, ConnectionVia::Group, {}, nyx::ConversationKind::Group,
                  nyx::GroupStore::group_id_hex(group->id));

  share_scope_group_ = group_id;
  group_hub_ = std::make_unique<nyx::GroupHub>(rv.socket(), profile, *group);
  group_hub_->attach_files(file_index_, group_id, &file_access_);
  group_hub_->set_on_message(
      [this](const nyx::ChatMessage& msg, bool outgoing) { emit_message(msg, outgoing); });
  group_hub_->set_on_event([this](const std::string& text) { emit_status(text); });

  const nyx::InviteToken hub_invite = group->invite_token;
  const auto rv_servers = network_config_.rendezvous_servers;
  const auto refresh_interval =
      std::chrono::seconds(network_config_.register_refresh_sec);
  auto last_register = std::chrono::steady_clock::now();

  while (running_.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_register >= refresh_interval) {
      nyx::register_token_on(group_hub_->socket(), rv_servers, hub_invite);
      last_register = now;
    }
    group_hub_->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  nyx::unregister_token_on(group_hub_->socket(), rv_servers, hub_invite);
  group_hub_.reset();
  busy_.store(false);
  set_mode(NodeMode::Idle);
  emit_session_ended();
  emit_status("hub остановлен");
}

void NodeService::run_group_join(std::string invite_hex) {
  set_mode(NodeMode::GroupMember);
  nyx::InviteToken token{};
  if (!nyx::GroupStore::invite_from_hex(invite_hex, token)) {
    emit_status("неверный invite");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  nyx::GroupStore store;
  store.load();
  auto group = store.find_by_invite(token);
  std::string group_name = group ? group->name : "поле";

  nyx::UdpSocket socket;
  if (!socket.bind("0.0.0.0", 0)) {
    emit_status("bind failed");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  std::string rendezvous_host;
  uint16_t rendezvous_port = 0;
  if (!parse_rendezvous(rendezvous_host, rendezvous_port)) {
    emit_status("неверный rendezvous");
    busy_.store(false);
    set_mode(NodeMode::Idle);
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
    connection_ = std::make_unique<nyx::Connection>(std::move(*result.connection));
    return true;
  };

  if (!connect_with_lookup(rv)) {
    emit_status("lookup failed — hub запущен?");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  if (!connection_) {
    emit_status("handshake failed — hub перезапустите на стороне создателя");
    nyx::UdpSocket retry_socket;
    if (retry_socket.bind("0.0.0.0", 0)) {
      nyx::RendezvousPool retry_pool(std::move(retry_socket));
      retry_pool.set_servers(network_config_.rendezvous_servers);
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      if (!connect_with_lookup(retry_pool)) {
        emit_status("handshake failed");
      }
    } else {
      emit_status("handshake failed");
    }
    if (!connection_) {
      busy_.store(false);
      set_mode(NodeMode::Idle);
      return;
    }
  }

  const auto profile = load_profile();

  nyx::HelloMessage peer_hello;
  if (!nyx::exchange_hello(*connection_, profile, peer_hello, 10,
                            [this]() { return running_.load(); })) {
    emit_status("Hello timeout");
    connection_.reset();
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  nyx::GroupId gid{};
  if (group) gid = group->id;
  share_scope_group_ = gid;

  group_member_ = std::make_unique<nyx::GroupMemberService>(
      *connection_, profile, gid, group_name);
  group_member_->set_on_message(
      [this](const nyx::ChatMessage& msg, bool outgoing) { emit_message(msg, outgoing); });
  group_member_->set_on_event([this](const std::string& text) { emit_status(text); });

  if (!group_member_->join()) {
    emit_status("join failed");
    group_member_.reset();
    connection_.reset();
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  {
    const auto& view = group_member_->view();
    share_scope_group_ = view.id;
    nyx::GroupStore store;
    store.load();
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
    store.upsert(rec);
    store.save();
    group_name = view.name;
  }

  emit_status("в поле «" + group_name + "»");
  emit_chat_ready(group_name, ConnectionVia::Group, {}, nyx::ConversationKind::Group,
                  nyx::GroupStore::group_id_hex(share_scope_group_));

  files_ = std::make_unique<nyx::FileTransferService>(
      *connection_, file_index_, nyx::default_downloads_dir());
  files_->set_share_scope(share_scope_group_);
  wire_file_transfer(*files_);
  publish_field_index();

  while (running_.load() && group_member_->joined()) {
    group_member_->tick();
    connection_->drive();
    if (files_) files_->pump();
    nyx::ByteBuffer payload;
    uint32_t stream_id = 0;
    while (connection_->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kChatStream) {
        group_member_->handle_payload(payload);
      } else if (stream_id == nyx::kBulkStream && files_) {
        files_->handle_bulk(payload);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  files_.reset();

  group_member_.reset();
  connection_.reset();
  busy_.store(false);
  set_mode(NodeMode::Idle);
  emit_session_ended();
  emit_status("выход из поля");
}

}  // namespace nyx_app
