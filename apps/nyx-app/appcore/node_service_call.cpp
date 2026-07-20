#include "node_service.hpp"

#include "nyx/call_media.hpp"
#include "nyx/nat.hpp"
#include "nyx/util.hpp"

#include <chrono>
#include <cstring>

namespace nyx_app {

namespace {

constexpr auto kCallAnnounceInterval = std::chrono::milliseconds(400);
constexpr int kCallAnnounceBursts = 20;

}  // namespace

void NodeService::set_on_call_changed(CallChangedCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_call_changed_ = std::move(cb);
}

void NodeService::set_on_call_media(CallMediaCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_call_media_ = std::move(cb);
}

void NodeService::emit_call_changed() {
  CallChangedCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_call_changed_;
  }
  if (cb) cb();
}

void NodeService::emit_call_media(nyx::CallMediaType type, const nyx::ByteBuffer& payload) {
  CallMediaCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_call_media_;
  }
  if (cb) cb(type, payload);
}

void NodeService::stop_call_mesh() {
  call_mesh_need_start_.store(false);
  call_mesh_need_announce_.store(false);
  call_mesh_pending_peers_.clear();
  call_mesh_announce_burst_ = 0;
  call_mesh_last_announce_ = {};
  if (call_mesh_) {
    call_mesh_->stop();
    call_mesh_.reset();
  }
}

void NodeService::request_call_mesh_start() { call_mesh_need_start_.store(true); }
void NodeService::request_call_mesh_announce() { call_mesh_need_announce_.store(true); }

void NodeService::queue_mesh_peer(const nyx::CallPeerEndpoint& peer) {
  if (peer.port == 0 || peer.host.empty()) return;
  for (auto& p : call_mesh_pending_peers_) {
    if (p.user_id == peer.user_id) {
      p = peer;
      return;
    }
  }
  call_mesh_pending_peers_.push_back(peer);
}

void NodeService::flush_pending_mesh_peers() {
  std::vector<nyx::CallPeerEndpoint> pending;
  std::shared_ptr<nyx::CallMesh> mesh;
  {
    std::lock_guard lock(call_mutex_);
    if (!call_mesh_ || !call_mesh_->active()) return;
    mesh = call_mesh_;
    pending.swap(call_mesh_pending_peers_);
  }
  for (const auto& p : pending) mesh->upsert_peer(p);
}

void NodeService::ensure_field_call_mesh() {
  nyx::CallId call_id{};
  bool already = false;
  {
    std::lock_guard lock(call_mutex_);
    if (call_.state != nyx::CallState::Active || call_.scope != nyx::CallScope::Field) return;
    call_id = call_.call_id;
    already = call_mesh_ && call_mesh_->active();
  }
  if (already) {
    flush_pending_mesh_peers();
    return;
  }

  const auto profile = load_profile();
  auto mesh = std::make_shared<nyx::CallMesh>();
  if (!mesh->start(call_id, profile.public_key)) {
    emit_status("не удалось открыть call-mesh сокет");
    return;
  }
  mesh->set_on_realtime([this](const nyx::UserId&, nyx::ByteBuffer raw) {
    auto frame = nyx::CallMediaFrame::decode(raw);
    if (!frame) return;
    emit_call_media(frame->type, frame->payload);
  });

  {
    std::lock_guard lock(call_mutex_);
    if (call_.state != nyx::CallState::Active || call_.scope != nyx::CallScope::Field) {
      mesh->stop();
      return;
    }
    if (call_mesh_ && call_mesh_->active()) {
      mesh->stop();
    } else {
      call_mesh_ = std::move(mesh);
      call_mesh_announce_burst_ = 0;
      call_mesh_last_announce_ = {};
    }
  }
  flush_pending_mesh_peers();
  request_call_mesh_announce();
}

void NodeService::announce_call_endpoint() {
  const auto profile = load_profile();
  std::shared_ptr<nyx::CallMesh> mesh;
  nyx::CallId call_id{};
  std::string sid;
  {
    std::lock_guard lock(call_mutex_);
    if (!call_mesh_ || !call_mesh_->active() || call_.state != nyx::CallState::Active) return;
    mesh = call_mesh_;
    call_id = call_.call_id;
    sid = call_session_id_;
    call_mesh_last_announce_ = std::chrono::steady_clock::now();
    ++call_mesh_announce_burst_;
  }

  nyx::CallEndpointMessage ep;
  ep.call_id = call_id;
  ep.self.user_id = profile.public_key;
  ep.self.host = mesh->local_host_guess();
  ep.self.port = mesh->local_port();

  if (auto session = find_session(sid)) {
    send_call_frame_on_session(session, ep.encode());
  }
}

void NodeService::pump_field_hub_media(const std::shared_ptr<NetSession>& session,
                                       const std::function<void(nyx::ByteBuffer)>& handle_raw) {
  if (!session) return;
  if (session->group_hub) {
    session->group_hub->relay_realtime(handle_raw);
    return;
  }
  if (session->connection) {
    nyx::ByteBuffer raw;
    while (session->connection->recv_realtime(raw)) handle_raw(std::move(raw));
  }
}

bool NodeService::send_call_media(nyx::CallMediaType type, const nyx::ByteBuffer& payload) {
  if (payload.empty() || payload.size() > nyx::kMaxCallMediaPayload) return false;
  std::string sid;
  uint32_t seq = 0;
  nyx::CallScope scope = nyx::CallScope::Direct;
  std::shared_ptr<nyx::CallMesh> mesh;
  {
    std::lock_guard lock(call_mutex_);
    if (call_.state != nyx::CallState::Active) return false;
    sid = call_session_id_;
    seq = call_media_seq_++;
    scope = call_.scope;
    mesh = call_mesh_;
  }
  auto session = find_session(sid);
  if (!session) return false;

  nyx::CallMediaFrame frame;
  frame.type = type;
  frame.seq = seq;
  frame.payload = payload;
  const nyx::ByteBuffer wire = frame.encode();

  if (scope == nyx::CallScope::Direct && session->connection) {
    return session->connection->send_realtime(wire);
  }
  if (scope == nyx::CallScope::Field) {
    bool sent = false;
    if (mesh && mesh->active() && mesh->established_count() > 0) {
      if (type == nyx::CallMediaType::Video)
        sent = mesh->send_realtime_video(wire);
      else
        sent = mesh->send_realtime(wire);
    }
    if (!sent) {
      if (session->group_hub) sent = session->group_hub->send_realtime_all(wire);
      else if (session->connection) sent = session->connection->send_realtime(wire);
    }
    return sent;
  }
  return false;
}

void NodeService::pump_call_realtime(const std::shared_ptr<NetSession>& session) {
  if (!session) return;
  bool active = false;
  std::string sid;
  nyx::CallScope scope = nyx::CallScope::Direct;
  std::shared_ptr<nyx::CallMesh> mesh;
  {
    std::lock_guard lock(call_mutex_);
    active = call_.state == nyx::CallState::Active;
    sid = call_session_id_;
    scope = call_.scope;
    mesh = call_mesh_;
  }
  if (!active || sid != session->id) return;

  if (call_mesh_need_start_.exchange(false)) ensure_field_call_mesh();
  if (call_mesh_need_announce_.exchange(false)) announce_call_endpoint();

  {
    std::lock_guard lock(call_mutex_);
    mesh = call_mesh_;
    if (scope == nyx::CallScope::Field && mesh && mesh->active() &&
        call_mesh_announce_burst_ < kCallAnnounceBursts) {
      const auto now = std::chrono::steady_clock::now();
      if (call_mesh_last_announce_.time_since_epoch().count() == 0 ||
          now - call_mesh_last_announce_ >= kCallAnnounceInterval) {
        call_mesh_need_announce_.store(true);
      }
    }
  }
  if (call_mesh_need_announce_.exchange(false)) announce_call_endpoint();
  flush_pending_mesh_peers();

  {
    std::lock_guard lock(call_mutex_);
    mesh = call_mesh_;
  }

  auto handle_raw = [this](nyx::ByteBuffer raw) {
    auto frame = nyx::CallMediaFrame::decode(raw);
    if (!frame) return;
    emit_call_media(frame->type, frame->payload);
  };

  if (scope == nyx::CallScope::Direct && session->connection) {
    nyx::ByteBuffer raw;
    while (session->connection->recv_realtime(raw)) handle_raw(std::move(raw));
  }

  if (scope == nyx::CallScope::Field) {
    if (mesh && mesh->active()) mesh->poll();
    pump_field_hub_media(session, handle_raw);
  }
}

nyx::CallState NodeService::call_state() const {
  std::lock_guard lock(call_mutex_);
  return call_.state;
}

nyx::CallMode NodeService::call_mode() const {
  std::lock_guard lock(call_mutex_);
  return call_.mode;
}

std::string NodeService::call_session_id() const {
  std::lock_guard lock(call_mutex_);
  return call_session_id_;
}

std::string NodeService::call_title() const {
  std::lock_guard lock(call_mutex_);
  return call_title_;
}

std::string NodeService::call_id_hex() const {
  std::lock_guard lock(call_mutex_);
  return nyx::call_id_hex(call_.call_id);
}

bool NodeService::call_is_field_room() const {
  std::lock_guard lock(call_mutex_);
  return call_.scope == nyx::CallScope::Field && call_.state == nyx::CallState::Active;
}

bool NodeService::call_is_host() const {
  std::lock_guard lock(call_mutex_);
  return call_is_host_;
}

nyx::GroupRole NodeService::local_field_role(const std::shared_ptr<NetSession>& session) const {
  if (!session) return nyx::GroupRole::Member;
  const auto profile = load_profile();
  if (session->group_hub) {
    return session->group_hub->role_of(profile.public_key);
  }
  if (session->group_member) {
    for (const auto& m : session->group_member->view().members) {
      if (m.user_id == profile.public_key) return m.role;
    }
  }
  if (!session->ref_id_hex.empty()) {
    nyx::GroupStore store;
    store.load();
    nyx::GroupId gid{};
    if (nyx::GroupStore::group_id_from_hex(session->ref_id_hex, gid)) {
      if (auto g = store.find(gid)) {
        if (g->owner_id == profile.public_key) return nyx::GroupRole::Owner;
        for (const auto& m : g->members) {
          if (m.user_id == profile.public_key) return m.role;
        }
      }
    }
  }
  return nyx::GroupRole::Member;
}

bool NodeService::can_start_call(const std::string& session_id) const {
  auto session = session_id.empty() ? active_session() : find_session(session_id);
  if (!session || session->state.load() != SessionState::Live) return false;
  if (session->kind == SessionKind::Direct || session->kind == SessionKind::DmInbox) return true;
  if (session->kind != SessionKind::GroupHub && session->kind != SessionKind::GroupMember) {
    return false;
  }
  return nyx::can_start_field_call(local_field_role(session));
}

bool NodeService::set_field_member_role(const std::string& group_id_hex,
                                        const std::string& user_id_hex,
                                        const std::string& role) {
  nyx::GroupId gid{};
  if (!nyx::GroupStore::group_id_from_hex(group_id_hex, gid)) return false;
  std::vector<uint8_t> raw;
  if (!nyx::from_hex(user_id_hex, raw) || raw.size() != nyx::kPublicKeySize) return false;
  nyx::UserId uid{};
  std::memcpy(uid.data(), raw.data(), nyx::kPublicKeySize);

  nyx::GroupRole gr = nyx::GroupRole::Member;
  if (role == "host")
    gr = nyx::GroupRole::Host;
  else if (role == "member")
    gr = nyx::GroupRole::Member;
  else
    return false;

  auto session = find_session("group:" + group_id_hex);
  if (!session) {
    for (const auto& info : list_sessions()) {
      if (info.ref_id_hex == group_id_hex && info.kind == SessionKind::GroupHub) {
        session = find_session(info.id);
        break;
      }
    }
  }
  if (session && session->group_hub) {
    return session->group_hub->set_member_role(uid, gr);
  }

  nyx::GroupStore store;
  store.load();
  auto g = store.find(gid);
  if (!g) return false;
  const auto profile = load_profile();
  if (g->owner_id != profile.public_key) return false;
  bool found = false;
  for (auto& m : g->members) {
    if (m.user_id == uid) {
      if (m.role == nyx::GroupRole::Owner) return false;
      m.role = gr;
      found = true;
      break;
    }
  }
  if (!found) return false;
  store.upsert(*g);
  return store.save();
}

bool NodeService::send_call_frame_on_session(const std::shared_ptr<NetSession>& session,
                                             const nyx::ByteBuffer& frame) {
  if (!session) return false;
  if (session->chat) return session->chat->send_call_frame(frame);
  if (session->group_member) return session->group_member->send_call_frame(frame);
  if (session->group_hub) return session->group_hub->send_call_frame(frame);
  return false;
}

void NodeService::wire_call_handlers(const std::shared_ptr<NetSession>& session) {
  if (!session) return;
  if (session->chat) {
    session->chat->set_on_call_frame([this, session](const nyx::ByteBuffer& frame) {
      handle_incoming_call_frame(session, frame);
    });
  }
  if (session->group_member) {
    session->group_member->set_on_call_frame([this, session](const nyx::ByteBuffer& frame) {
      handle_incoming_call_frame(session, frame);
    });
  }
  if (session->group_hub) {
    session->group_hub->set_on_call_frame(
        [this, session](const nyx::UserId&, const nyx::ByteBuffer& frame) {
          handle_incoming_call_frame(session, frame);
        });
  }
}

void NodeService::handle_incoming_call_frame(const std::shared_ptr<NetSession>& session,
                                             const nyx::ByteBuffer& frame) {
  if (!session || !nyx::is_call_frame(frame)) return;

  if (auto inv = nyx::CallInviteMessage::decode(frame)) {
    {
      std::lock_guard lock(call_mutex_);
      if (call_.state == nyx::CallState::Active && call_.call_id == inv->call_id) return;
    }

    nyx::CallRejectMessage busy_rej;
    bool send_busy = false;
    bool accepted = false;
    {
      std::lock_guard lock(call_mutex_);
      if (!call_.idle()) {
        busy_rej.call_id = inv->call_id;
        busy_rej.reason = nyx::CallRejectReason::Busy;
        send_busy = true;
      } else if (call_.on_invite(*inv)) {
        call_session_id_ = session->id;
        call_title_ = session->title;
        call_is_host_ = false;
        accepted = true;
      }
    }
    if (send_busy) {
      if (inv->scope == nyx::CallScope::Direct) {
        send_call_frame_on_session(session, busy_rej.encode());
      }
      return;
    }
    if (accepted) {
      if (inv->scope == nyx::CallScope::Direct) {
        nyx::CallRingingMessage ring;
        ring.call_id = inv->call_id;
        send_call_frame_on_session(session, ring.encode());
      }
      emit_call_changed();
      emit_status(inv->scope == nyx::CallScope::Field ? "комната в поле открыта"
                                                      : "входящий звонок");
    }
    return;
  }

  if (auto ring = nyx::CallRingingMessage::decode(frame)) {
    bool ok = false;
    {
      std::lock_guard lock(call_mutex_);
      ok = call_.on_ringing(*ring);
    }
    if (ok) emit_call_changed();
    return;
  }

  if (auto acc = nyx::CallAcceptMessage::decode(frame)) {
    bool ok = false;
    bool field = false;
    {
      std::lock_guard lock(call_mutex_);
      ok = call_.on_accept(*acc);
      field = call_.scope == nyx::CallScope::Field;
    }
    if (ok) {
      emit_call_changed();
      if (field) {
        emit_status("участник в комнате");
        request_call_mesh_start();
        maybe_send_field_intros();
        request_call_mesh_announce();
      } else {
        emit_status("звонок принят");
      }
    }
    return;
  }

  if (auto rej = nyx::CallRejectMessage::decode(frame)) {
    bool ok = false;
    {
      std::lock_guard lock(call_mutex_);
      ok = call_.on_reject(*rej);
      if (ok) {
        stop_call_mesh();
        call_is_host_ = false;
        call_.reset();
        call_session_id_.clear();
        call_title_.clear();
      }
    }
    if (ok) {
      emit_call_changed();
      emit_status(rej->reason == nyx::CallRejectReason::Unsupported
                      ? "нет права открывать комнату"
                      : "звонок отклонён");
    }
    return;
  }

  if (auto hang = nyx::CallHangupMessage::decode(frame)) {
    bool ok = false;
    {
      std::lock_guard lock(call_mutex_);
      ok = call_.on_hangup(*hang);
      if (ok) {
        stop_call_mesh();
        call_is_host_ = false;
        call_.reset();
        call_session_id_.clear();
        call_title_.clear();
      }
    }
    if (ok) {
      emit_call_changed();
      emit_status("комната закрыта");
    }
    return;
  }

  if (auto upd = nyx::CallUpdateMessage::decode(frame)) {
    std::lock_guard lock(call_mutex_);
    call_.on_update(*upd);
    return;
  }

  if (auto intro = nyx::CallPeerIntroMessage::decode(frame)) {
    (void)intro;
    request_call_mesh_start();
    request_call_mesh_announce();
    return;
  }

  if (auto ep = nyx::CallEndpointMessage::decode(frame)) {
    {
      std::lock_guard lock(call_mutex_);
      if (call_.call_id != ep->call_id) return;
      queue_mesh_peer(ep->self);
    }
    request_call_mesh_start();
    flush_pending_mesh_peers();
    return;
  }

  if (auto roster = nyx::CallRosterMessage::decode(frame)) {
    if (roster->participants.size() > nyx::kMaxCallParticipants) {
      emit_status("слишком много участников звонка");
    }
    return;
  }

  if (auto gone = nyx::CallPeerGoneMessage::decode(frame)) {
    std::lock_guard lock(call_mutex_);
    if (call_mesh_ && call_.call_id == gone->call_id) {
      call_mesh_->remove_peer(gone->user_id);
    }
    call_.on_peer_leave(gone->call_id);
    return;
  }
}

void NodeService::maybe_send_field_intros() {
  std::string sid;
  nyx::CallId call_id{};
  {
    std::lock_guard lock(call_mutex_);
    if (call_.state != nyx::CallState::Active || call_.scope != nyx::CallScope::Field) return;
    call_id = call_.call_id;
    sid = call_session_id_;
  }
  auto session = find_session(sid);
  if (!session || !session->group_hub) return;

  std::size_t n = 1;
  for (const auto& m : session->group_hub->members()) {
    if (m.joined) ++n;
  }
  if (n > nyx::kMaxCallParticipants) {
    emit_status("лимит участников звонка: 200");
    return;
  }

  session->group_hub->distribute_call_mesh_intros(call_id);
}

bool NodeService::start_call(bool video, const std::string& session_id) {
  auto session = session_id.empty() ? active_session() : find_session(session_id);
  if (!session) {
    emit_status("нет активной сессии для звонка");
    return false;
  }
  if (session->state.load() != SessionState::Live) {
    emit_status("сессия не на связи");
    return false;
  }

  const auto mode = video ? nyx::CallMode::AudioVideo : nyx::CallMode::Audio;
  const auto scope =
      session->kind == SessionKind::GroupHub || session->kind == SessionKind::GroupMember
          ? nyx::CallScope::Field
          : nyx::CallScope::Direct;

  if (scope == nyx::CallScope::Field) {
    if (!nyx::can_start_field_call(local_field_role(session))) {
      emit_status("нет права открывать комнату (нужна роль ведущего)");
      return false;
    }
  }

  nyx::UserId target{};
  if (session->chat) {
    target = session->chat->peer().user_id;
  } else if (!session->ref_id_hex.empty()) {
    std::vector<uint8_t> raw;
    if (nyx::from_hex(session->ref_id_hex, raw) && raw.size() == nyx::kPublicKeySize) {
      std::memcpy(target.data(), raw.data(), nyx::kPublicKeySize);
    }
  }

  nyx::CallInviteMessage inv;
  {
    std::lock_guard lock(call_mutex_);
    if (scope == nyx::CallScope::Field) {
      if (!call_.open_field_room(mode, target)) {
        emit_status("звонок уже идёт");
        return false;
      }
      call_is_host_ = true;
    } else {
      if (!call_.start_outgoing(mode, scope, target)) {
        emit_status("звонок уже идёт");
        return false;
      }
      call_is_host_ = true;
    }
    inv.call_id = call_.call_id;
    inv.mode = mode;
    inv.scope = scope;
    inv.group_or_peer = target;
    inv.sdp_lite = "nyx-call/1;av1;room";
    call_session_id_ = session->id;
    call_title_ = session->title;
  }

  if (!send_call_frame_on_session(session, inv.encode())) {
    {
      std::lock_guard lock(call_mutex_);
      stop_call_mesh();
      call_is_host_ = false;
      call_.reset();
      call_session_id_.clear();
      call_title_.clear();
    }
    emit_status("не удалось открыть комнату");
    emit_call_changed();
    return false;
  }

  if (scope == nyx::CallScope::Field) {
    request_call_mesh_start();
    maybe_send_field_intros();
    request_call_mesh_announce();
    emit_status(video ? "видеокомната открыта" : "аудиокомната открыта");
  } else {
    emit_status(video ? "видеовызов…" : "аудиовызов…");
  }
  emit_call_changed();
  return true;
}

bool NodeService::accept_call() {
  nyx::CallAcceptMessage acc;
  std::string sid;
  nyx::CallScope scope = nyx::CallScope::Direct;
  {
    std::lock_guard lock(call_mutex_);
    if (!call_.accept(call_.mode)) return false;
    acc.call_id = call_.call_id;
    acc.mode = call_.mode;
    acc.sdp_lite = "nyx-call/1;av1;room";
    sid = call_session_id_;
    scope = call_.scope;
    call_is_host_ = false;
  }
  auto session = find_session(sid);
  if (!session || !send_call_frame_on_session(session, acc.encode())) {
    emit_status("не удалось войти в комнату");
    return false;
  }
  emit_call_changed();
  emit_status("на линии");
  if (scope == nyx::CallScope::Field) {
    request_call_mesh_start();
    request_call_mesh_announce();
  }
  return true;
}

bool NodeService::reject_call() {
  nyx::CallRejectMessage rej;
  std::string sid;
  nyx::CallScope scope = nyx::CallScope::Direct;
  {
    std::lock_guard lock(call_mutex_);
    if (call_.state != nyx::CallState::Incoming) return false;
    rej.call_id = call_.call_id;
    rej.reason = nyx::CallRejectReason::Declined;
    sid = call_session_id_;
    scope = call_.scope;
    call_.reject();
    stop_call_mesh();
    call_is_host_ = false;
    call_.reset();
    call_session_id_.clear();
    call_title_.clear();
  }
  if (scope == nyx::CallScope::Direct) {
    if (auto session = find_session(sid)) send_call_frame_on_session(session, rej.encode());
  }
  emit_call_changed();
  return true;
}

bool NodeService::hangup_call() {
  nyx::CallHangupMessage hang;
  nyx::CallPeerGoneMessage gone;
  std::string sid;
  bool is_host = false;
  bool field = false;
  nyx::UserId self{};
  {
    std::lock_guard lock(call_mutex_);
    if (call_.idle()) return false;
    hang.call_id = call_.call_id;
    hang.reason = nyx::CallHangupReason::Normal;
    gone.call_id = call_.call_id;
    sid = call_session_id_;
    is_host = call_is_host_;
    field = call_.scope == nyx::CallScope::Field;
    call_.hangup();
    stop_call_mesh();
    call_is_host_ = false;
    call_.reset();
    call_session_id_.clear();
    call_title_.clear();
  }
  self = load_profile().public_key;
  gone.user_id = self;

  if (auto session = find_session(sid)) {
    if (field && !is_host) {
      send_call_frame_on_session(session, gone.encode());
    } else {
      send_call_frame_on_session(session, hang.encode());
    }
  }
  emit_call_changed();
  emit_status(field && is_host ? "комната закрыта" : "звонок завершён");
  return true;
}

}  // namespace nyx_app
