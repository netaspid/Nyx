#include "node_service.hpp"

#include "connect_via_hint.hpp"

#include "nyx/app.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/mdns.hpp"
#include "nyx/nat.hpp"
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
    emit_status("только владелец может открыть эфир");
    finish_session(session, SessionState::Offline);
    return;
  }

  nyx::UdpSocket socket;
  if (!socket.bind("0.0.0.0", 0)) {
    emit_status("не удалось открыть сетевой порт");
    finish_session(session, SessionState::Offline);
    return;
  }

  std::string rendezvous_host;
  uint16_t rendezvous_port = 0;
  const bool need_rv = network_config_.mode != nyx::DiscoveryMode::LanOnly;
  if (need_rv && !parse_rendezvous(rendezvous_host, rendezvous_port)) {
    emit_status("неверный адрес rendezvous");
    finish_session(session, SessionState::Offline);
    return;
  }

  nyx::RendezvousPool rv(std::move(socket));
  if (need_rv) {
    rv.set_servers(network_config_.rendezvous_servers);
  }
  bool rv_ok = true;
  if (need_rv) {
    rv_ok = rv.register_token(group->invite_token);
    if (!rv_ok) {
      emit_status("rendezvous не отвечает — эфир всё равно открыт в LAN");
    }
  } else {
    emit_status("эфир в режиме LAN-only");
  }

  emit_status("эфир «" + group->name + "», invite: " +
              nyx::GroupStore::invite_hex(group->invite_token));
  session->ref_id_hex = nyx::GroupStore::group_id_hex(group->id);
  session->share_scope = group_id;
  remember_intent_for_session(session, nyx::GroupStore::invite_hex(group->invite_token));

  // GroupHub до chat_ready: иначе Live без send_message → «Не удалось отправить».
  session->group_hub = std::make_unique<nyx::GroupHub>(rv.socket(), profile, *group);
  session->group_hub->attach_files(file_index_, group_id, &file_access_);
  session->group_hub->set_on_message([this, session](const nyx::ChatMessage& msg, bool outgoing) {
    emit_message(session, msg, outgoing, outgoing ? "pending" : "");
  });
  session->group_hub->set_on_delivery(
      [this, session](uint64_t message_id, nyx::DeliveryStatus status) {
        emit_delivery(session, message_id, status == nyx::DeliveryStatus::Delivered);
      });
  session->group_hub->set_on_event([this](const std::string& text) { emit_status(text); });
  session->group_hub->set_on_file_progress(
      [this, session](const nyx::FileHash& hash, uint64_t done, uint64_t total) {
        {
          std::lock_guard lock(session->download_mutex);
          const std::string hex = nyx::hash_hex(hash);
          for (auto& item : session->download_queue) {
            if (item.hash_hex != hex) continue;
            item.state = "active";
            item.progress =
                total == 0 ? 0 : static_cast<int>((done * 100) / total);
            break;
          }
        }
        save_download_queue(session);
        emit_transfer_queue_changed();
      });
  session->group_hub->set_on_file_complete(
      [this, session](const nyx::FileHash& hash, bool success, const std::string&,
                      const std::string& error) {
        const std::string hex = nyx::hash_hex(hash);
        {
          std::lock_guard lock(session->download_mutex);
          auto it = std::find_if(
              session->download_queue.begin(), session->download_queue.end(),
              [&](const FileDownloadRequest& item) {
                return item.hash_hex == hex;
              });
          if (it != session->download_queue.end()) {
            if (success) {
              session->download_queue.erase(it);
            } else {
              it->state = "failed";
              it->error = error.empty() ? "нет источников" : error;
            }
          }
        }
        save_download_queue(session);
        emit_transfer_queue_changed();
      });
  load_download_queue(session);
  wire_call_handlers(session);
  sync_live_group_from_session(session);

  // Advertise hub on LAN so members can join when rendezvous UDP is blocked (VPN).
  if (network_config_.mode != nyx::DiscoveryMode::Internet) {
    nyx::Profile hub_profile = profile;
    hub_profile.nickname = profile.nickname + "-field";
    session->mdns = std::make_unique<nyx::MdnsLan>();
    const std::string lan_ip = nyx::guess_lan_ipv4();
    session->mdns->start_advertising(session->group_hub->socket(), hub_profile,
                                     session->group_hub->socket().local_port(), lan_ip);
    emit_status(std::string("эфир LAN ") + lan_ip + ':' +
                std::to_string(session->group_hub->socket().local_port()) +
                (rv_ok ? "" : " (без rendezvous)"));
  }

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
    pump_call_realtime(session);
    sync_live_group_from_session(session);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  session->group_hub->notify_shutdown("эфир закрыт");
  // Дать UDP Bye уйти до unregister.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  nyx::unregister_token_on(session->group_hub->socket(), rv_servers, hub_invite);
  clear_live_group_snapshot(group_id);
  finish_session(session, SessionState::Disconnected);
  emit_status("эфир закрыт");
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
    emit_status("не удалось открыть сетевой порт");
    bind_group_session_key();
    finish_session(session, SessionState::Offline);
    return;
  }

  std::string rendezvous_host;
  uint16_t rendezvous_port = 0;
  const bool have_rv = parse_rendezvous(rendezvous_host, rendezvous_port);
  if (!have_rv && network_config_.mode == nyx::DiscoveryMode::Internet) {
    emit_status("неверный rendezvous");
    bind_group_session_key();
    finish_session(session, SessionState::Offline);
    return;
  }

  nyx::RendezvousPool rv(std::move(socket));
  if (have_rv) {
    rv.set_servers(network_config_.rendezvous_servers);
  }

  auto connect_with_lookup = [&](nyx::RendezvousPool& pool) -> bool {
    auto hint = pool.lookup(token);
    if (!hint) return false;
    emit_status("подключение к эфиру " + hint->host_string() + ':' +
                std::to_string(hint->port) + "...");
    auto result = connect_via_rendezvous_hint(pool.socket(), *hint);
    if (!result.connection) return false;
    session->connection = std::make_unique<nyx::Connection>(std::move(*result.connection));
    return true;
  };

  auto connect_lan_direct = [&](const std::string& host, uint16_t port) -> bool {
    emit_status("LAN: подключение к эфиру " + host + ':' + std::to_string(port) + "...");
    nyx::UdpSocket dial;
    if (!dial.bind("0.0.0.0", 0)) return false;
    auto conn = nyx::Connection::connect_initiator(std::move(dial), host, port);
    if (!conn) return false;
    session->connection = std::make_unique<nyx::Connection>(std::move(*conn));
    return true;
  };

  const std::string owner_short =
      group ? nyx::short_user_id(group->owner_id) : std::string{};

  auto try_lan_hubs = [&](int browse_ms) -> bool {
    emit_status("эфир через LAN…");
    const auto peers = browse_lan_peers(browse_ms);
    std::vector<nyx::LanPeer> hubs;
    for (const auto& p : peers) {
      if (p.instance.size() < 6 ||
          p.instance.compare(p.instance.size() - 6, 6, "-field") != 0) {
        continue;
      }
      // Prefer the owner of this field when roster is known.
      if (!owner_short.empty() && p.user_id_short != owner_short) continue;
      hubs.push_back(p);
    }
    // If owner filter emptied the list (old beacon / mismatch), still try any *-field.
    if (hubs.empty()) {
      for (const auto& p : peers) {
        if (p.instance.size() >= 6 &&
            p.instance.compare(p.instance.size() - 6, 6, "-field") == 0) {
          hubs.push_back(p);
        }
      }
    }
    for (const auto& p : hubs) {
      if (!session->running.load()) break;
      if (p.host.empty() || p.port == 0) continue;
      if (connect_lan_direct(p.host, p.port)) return true;
      session->connection.reset();
    }
    return false;
  };

  bool connected = false;
  // Same Wi‑Fi first: rendezvous UDP often dead behind VPN, and LAN is faster to fail/succeed.
  if (network_config_.mode != nyx::DiscoveryMode::Internet) {
    connected = try_lan_hubs(3200);
  }

  if (!connected && have_rv && network_config_.mode != nyx::DiscoveryMode::LanOnly) {
    for (int attempt = 1; attempt <= 2 && session->running.load(); ++attempt) {
      if (attempt > 1) {
        emit_status("повтор lookup (" + std::to_string(attempt) + "/2)…");
        std::this_thread::sleep_for(std::chrono::milliseconds(400 * attempt));
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
  }

  if (!connected && network_config_.mode != nyx::DiscoveryMode::Internet) {
    connected = try_lan_hubs(3500);
  }

  if (!connected || !session->connection) {
    emit_status("эфир не найден — владелец online в той же Wi‑Fi / тот же rendezvous?");
    bind_group_session_key();
    finish_session(session, SessionState::Offline);
    return;
  }

  const auto profile = load_profile();

  nyx::HelloMessage peer_hello;
  if (!nyx::exchange_hello(*session->connection, profile, peer_hello, 10,
                           [session]() { return session->running.load(); })) {
    emit_status("не удалось поздороваться с владельцем эфира");
    bind_group_session_key();
    finish_session(session, SessionState::Offline);
    return;
  }
  nyx::remember_contact(peer_hello);
  sync_avatars_after_hello(session, peer_hello);

  nyx::GroupId gid{};
  if (group) gid = group->id;
  session->share_scope = gid;

  session->group_member = std::make_unique<nyx::GroupMemberService>(
      *session->connection, profile, gid, group_name);
  session->group_member->set_on_message(
      [this, session](const nyx::ChatMessage& msg, bool outgoing) {
        emit_message(session, msg, outgoing, outgoing ? "pending" : "");
      });
  session->group_member->set_on_delivery(
      [this, session](uint64_t message_id, nyx::DeliveryStatus status) {
        emit_delivery(session, message_id, status == nyx::DeliveryStatus::Delivered);
      });
  session->group_member->set_on_event([this](const std::string& text) { emit_status(text); });
  session->group_member->set_on_meta([this, session]() {
    sync_live_group_from_session(session);
    SessionsChangedCallback cb;
    {
      std::lock_guard lock(cb_mutex_);
      cb = on_group_meta_changed_;
    }
    if (cb) cb();
  });
  wire_call_handlers(session);

  if (!session->group_member->join()) {
    emit_status("не удалось войти в эфир");
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
    rec.description = view.description;
    rec.direction = view.direction;
    rec.tags = view.tags;
    rec.visibility = view.visibility;
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
  session->files->announce_capabilities();
  load_download_queue(session);
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
        if (handle_avatar_bulk(session, payload)) {
          // avatar
        } else if (!try_apply_file_access_policy(payload) && session->files) {
          session->files->handle_bulk(payload);
        }
      }
    }
    pump_call_realtime(session);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  clear_live_group_snapshot(session->share_scope);
  const bool user_stopped = !session->running.load();
  finish_session(session, user_stopped ? SessionState::Disconnected : SessionState::Offline);
  emit_status(user_stopped ? "выход из поля" : "поле недоступно (владелец офлайн)");
}

}  // namespace nyx_app
