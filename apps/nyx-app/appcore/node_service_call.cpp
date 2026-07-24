#include "node_service.hpp"

#include "nyx/call_media.hpp"
#include "nyx/nat.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace nyx_app {

namespace {

constexpr auto kCallAnnounceInterval = std::chrono::milliseconds(400);
constexpr int kCallAnnounceBursts = 20;
constexpr auto kCallSignalRetryInterval = std::chrono::milliseconds(350);

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

void NodeService::emit_call_media(nyx::CallMediaType type, const nyx::ByteBuffer& payload,
                                  const nyx::UserId& from) {
  CallMediaCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_call_media_;
  }
  if (cb) cb(type, payload, from);
}

bool NodeService::note_inbound_call_media(const nyx::UserId& from,
                                          nyx::CallMediaType type, uint32_t seq) {
  std::lock_guard lock(call_mutex_);
  for (const auto& entry : call_media_dedupe_) {
    if (entry.from == from && entry.type == static_cast<uint8_t>(type) &&
        entry.seq == seq) {
      return false;
    }
  }
  auto& entry = call_media_dedupe_[static_cast<std::size_t>(call_media_dedupe_i_)];
  entry.from = from;
  entry.type = static_cast<uint8_t>(type);
  entry.seq = seq;
  call_media_dedupe_i_ =
      (call_media_dedupe_i_ + 1) % static_cast<int>(call_media_dedupe_.size());
  const auto now = std::chrono::steady_clock::now();
  if (type == nyx::CallMediaType::Opus) call_inbound_opus_ = now;
  if (type == nyx::CallMediaType::Video) call_inbound_video_ = now;
  return true;
}

void NodeService::pump_pending_call_signals(const std::shared_ptr<NetSession>& session) {
  if (!session) return;
  std::vector<nyx::ByteBuffer> due;
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard lock(call_mutex_);
    for (auto it = pending_call_signals_.begin(); it != pending_call_signals_.end();) {
      if (now >= it->expires) {
        it = pending_call_signals_.erase(it);
        continue;
      }
      if (it->session_id == session->id && now >= it->next_send) {
        due.push_back(it->wire);
        it->next_send = now + kCallSignalRetryInterval;
      }
      ++it;
    }
  }
  for (const auto& wire : due) send_call_frame_on_session(session, wire);
}

bool NodeService::call_media_peer_allowed(const nyx::UserId& peer) const {
  if (call_relays_.empty()) return true;
  const auto self = load_profile().public_key;
  const bool self_relay =
      std::find(call_relays_.begin(), call_relays_.end(), self) != call_relays_.end();
  const bool peer_relay =
      std::find(call_relays_.begin(), call_relays_.end(), peer) != call_relays_.end();
  if (self_relay && peer_relay) return true;

  if (!self_relay) {
    const auto assigned = nyx::call_relay_targets(self, call_relays_);
    return std::find(assigned.begin(), assigned.end(), peer) != assigned.end();
  }
  const auto assigned = nyx::call_relay_targets(peer, call_relays_);
  return std::find(assigned.begin(), assigned.end(), self) != assigned.end();
}

void NodeService::apply_call_topology() {
  std::shared_ptr<nyx::CallMesh> mesh;
  std::set<nyx::UserId> allowed;
  {
    std::lock_guard lock(call_mutex_);
    mesh = call_mesh_;
    if (!mesh || call_relays_.empty()) return;
    for (const auto& peer : call_participants_) {
      if (peer != load_profile().public_key && call_media_peer_allowed(peer)) {
        allowed.insert(peer);
      }
    }
  }
  mesh->retain_peers(allowed);
}

void NodeService::announce_relay_candidate() {
  nyx::CallRelayCandidateMessage candidate;
  std::string sid;
  {
    std::lock_guard lock(call_mutex_);
    if (call_.state != nyx::CallState::Active ||
        call_.scope != nyx::CallScope::Field) {
      return;
    }
    candidate.call_id = call_.call_id;
    candidate.user_id = load_profile().public_key;
    candidate.score = static_cast<uint16_t>(
        std::min<uint32_t>(1000, call_relay_score_.load() + (call_is_host_ ? 200 : 0)));
    call_relay_candidates_[candidate.user_id] = candidate.score;
    sid = call_session_id_;
  }
  if (auto session = find_session(sid)) {
    send_call_frame_on_session(session, candidate.encode());
  }
  recompute_relay_set();
}

void NodeService::recompute_relay_set() {
  nyx::CallRelaySetMessage message;
  std::string sid;
  bool changed = false;
  {
    std::lock_guard lock(call_mutex_);
    if (!call_is_host_ || call_.state != nyx::CallState::Active ||
        call_.scope != nyx::CallScope::Field) {
      return;
    }
    std::vector<std::pair<nyx::UserId, uint16_t>> ranked;
    for (const auto& id : call_participants_) {
      const auto score = call_relay_candidates_.find(id);
      ranked.emplace_back(id, score == call_relay_candidates_.end() ? 0 : score->second);
    }
    std::vector<nyx::UserId> relays =
        nyx::select_call_relays(std::move(ranked), call_participants_.size());
    if (relays != call_relays_) {
      call_relays_ = relays;
      ++call_relay_epoch_;
      changed = true;
    }
    message.call_id = call_.call_id;
    message.epoch = call_relay_epoch_;
    message.relays = call_relays_;
    sid = call_session_id_;
  }
  if (!changed) return;
  if (auto session = find_session(sid)) {
    send_call_frame_on_session(session, message.encode());
  }
  apply_call_topology();
  maybe_send_field_intros();
}

void NodeService::handle_field_mesh_media(const nyx::UserId& from, nyx::ByteBuffer raw) {
  auto frame = nyx::CallMediaFrame::decode(raw);
  if (!frame) return;
  if (from != nyx::UserId{} && frame->hop_count == 0 &&
      frame->origin != nyx::UserId{} && frame->origin != from) {
    return;
  }
  const nyx::UserId origin = frame->origin == nyx::UserId{} ? from : frame->origin;
  if (!note_inbound_call_media(origin, frame->type, frame->seq)) return;

  std::shared_ptr<nyx::CallMesh> mesh;
  bool relay = false;
  bool forward = false;
  bool deliver = true;
  {
    std::lock_guard lock(call_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (frame->type == nyx::CallMediaType::Opus) {
      call_speaker_levels_[origin] = {frame->audio_level, now};
      nyx::UserId best{};
      uint8_t best_level = 8;
      for (auto it = call_speaker_levels_.begin(); it != call_speaker_levels_.end();) {
        if (now - it->second.second > std::chrono::milliseconds(800)) {
          it = call_speaker_levels_.erase(it);
          continue;
        }
        if (it->second.first > best_level) {
          best_level = it->second.first;
          best = it->first;
        }
        ++it;
      }
      const auto current = call_speaker_levels_.find(call_dominant_speaker_);
      if (best == nyx::UserId{}) {
        call_dominant_speaker_ = {};
      } else if (current == call_speaker_levels_.end() ||
          current->second.first + 12 < best_level) {
        call_dominant_speaker_ = best;
      }
    }
    if (frame->type == nyx::CallMediaType::Video &&
        call_dominant_speaker_ != nyx::UserId{} &&
        origin != call_dominant_speaker_) {
      deliver = false;
    }
    const auto self = load_profile().public_key;
    relay = std::find(call_relays_.begin(), call_relays_.end(), self) !=
            call_relays_.end();
    forward = relay && frame->hop_count < 2 &&
              (frame->type != nyx::CallMediaType::Video ||
               origin == call_dominant_speaker_);
    mesh = call_mesh_;
  }
  if (forward && mesh) {
    frame->origin = origin;
    ++frame->hop_count;
    mesh->send_realtime_except(from, frame->encode());
  }
  if (deliver) emit_call_media(frame->type, frame->payload, origin);
}

void NodeService::stop_call_mesh() {
  call_mesh_need_start_.store(false);
  call_mesh_need_announce_.store(false);
  call_mesh_pending_peers_.clear();
  call_mesh_announce_burst_ = 0;
  call_mesh_last_announce_ = {};
  call_relay_candidates_.clear();
  call_relays_.clear();
  call_relay_epoch_ = 0;
  call_speaker_levels_.clear();
  call_dominant_speaker_ = {};
  call_media_dedupe_ = {};
  call_media_dedupe_i_ = 0;
  if (call_mesh_) {
    call_mesh_->stop();
    call_mesh_.reset();
  }
}

void NodeService::request_call_mesh_start() { call_mesh_need_start_.store(true); }
void NodeService::request_call_mesh_announce() { call_mesh_need_announce_.store(true); }

void NodeService::queue_mesh_peer(const nyx::CallPeerEndpoint& peer) {
  if (peer.port == 0 || peer.host.empty()) return;
  if (!call_media_peer_allowed(peer.user_id)) return;
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
  mesh->set_on_realtime([this](const nyx::UserId& from, nyx::ByteBuffer raw) {
    handle_field_mesh_media(from, std::move(raw));
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

void NodeService::pump_field_hub_media(
    const std::shared_ptr<NetSession>& session,
    const std::function<void(const nyx::UserId& from, nyx::ByteBuffer)>& handle_raw) {
  if (!session) return;
  if (session->group_hub) {
    session->group_hub->relay_realtime(handle_raw);
    return;
  }
  if (session->connection) {
    nyx::ByteBuffer raw;
    while (session->connection->recv_realtime(raw)) handle_raw({}, std::move(raw));
  }
}

bool NodeService::send_call_media(nyx::CallMediaType type, const nyx::ByteBuffer& payload,
                                  uint8_t audio_level) {
  if (payload.empty() || payload.size() > nyx::kMaxCallMediaPayload) return false;
  std::string sid;
  uint32_t seq = 0;
  nyx::CallScope scope = nyx::CallScope::Direct;
  {
    std::lock_guard lock(call_mutex_);
    if (call_.state != nyx::CallState::Active) return false;
    sid = call_session_id_;
    seq = call_media_seq_++;
    scope = call_.scope;
  }
  auto session = find_session(sid);
  if (!session) return false;

  nyx::CallMediaFrame frame;
  frame.type = type;
  frame.seq = seq;
  frame.origin = load_profile().public_key;
  frame.audio_level = audio_level;
  frame.payload = payload;
  const nyx::ByteBuffer wire = frame.encode();

  if (scope == nyx::CallScope::Direct || scope == nyx::CallScope::Field) {
    if (scope == nyx::CallScope::Direct && !session->connection) return false;
    std::lock_guard lock(session->call_media_outbound_mutex);
    constexpr std::size_t kMaxQueuedMedia = 384;
    while (session->call_media_outbound.size() >= kMaxQueuedMedia) {
      auto it = std::find_if(
          session->call_media_outbound.begin(), session->call_media_outbound.end(),
          [](const nyx::ByteBuffer& queued) {
            return !queued.empty() &&
                   queued[0] == static_cast<uint8_t>(nyx::CallMediaType::Video);
          });
      if (it != session->call_media_outbound.end())
        session->call_media_outbound.erase(it);
      else
        session->call_media_outbound.pop_front();
    }
    session->call_media_outbound.push_back(wire);
    return true;
  }
  return false;
}

void NodeService::pump_call_realtime(const std::shared_ptr<NetSession>& session) {
  if (!session) return;
  pump_pending_call_signals(session);
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

  auto handle_raw = [this](const nyx::UserId& from, nyx::ByteBuffer raw) {
    handle_field_mesh_media(from, std::move(raw));
  };

  std::deque<nyx::ByteBuffer> outbound;
  {
    std::lock_guard lock(session->call_media_outbound_mutex);
    constexpr std::size_t kMaxSendPerPump = 128;
    for (std::size_t i = 0;
         i < kMaxSendPerPump && !session->call_media_outbound.empty(); ++i) {
      auto opus = std::find_if(
          session->call_media_outbound.begin(), session->call_media_outbound.end(),
          [](const nyx::ByteBuffer& queued) {
            return !queued.empty() &&
                   queued[0] == static_cast<uint8_t>(nyx::CallMediaType::Opus);
          });
      if (opus != session->call_media_outbound.end()) {
        outbound.push_back(std::move(*opus));
        session->call_media_outbound.erase(opus);
      } else {
        outbound.push_back(std::move(session->call_media_outbound.front()));
        session->call_media_outbound.pop_front();
      }
    }
  }

  if (scope == nyx::CallScope::Direct && session->connection) {
    for (const auto& packet : outbound) session->connection->send_realtime(packet);
    // Drain all, deliver Opus before Video so mic audio is not starved by JPEG frags.
    std::vector<nyx::ByteBuffer> opus_q;
    std::vector<nyx::ByteBuffer> video_q;
    nyx::ByteBuffer raw;
    while (session->connection->recv_realtime(raw)) {
      auto frame = nyx::CallMediaFrame::decode(raw);
      if (!frame) continue;
      if (frame->type == nyx::CallMediaType::Opus)
        opus_q.push_back(std::move(raw));
      else
        video_q.push_back(std::move(raw));
    }
    for (auto& p : opus_q) handle_raw({}, std::move(p));
    for (auto& p : video_q) handle_raw({}, std::move(p));
  }

  if (scope == nyx::CallScope::Field) {
    for (const auto& packet : outbound) {
      const auto frame = nyx::CallMediaFrame::decode(packet);
      bool mesh_sent = false;
      if (mesh && mesh->active() && mesh->established_count() > 0) {
        mesh_sent = frame && frame->type == nyx::CallMediaType::Video
                        ? mesh->send_realtime_video(packet)
                        : mesh->send_realtime(packet);
      }
      if (!mesh_sent) {
        if (session->group_hub)
          session->group_hub->send_realtime_all(packet);
        else if (session->connection)
          session->connection->send_realtime(packet);
      }
    }
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
  return call_.scope == nyx::CallScope::Field && !call_.idle();
}

bool NodeService::call_mic_muted() const {
  std::lock_guard lock(call_mutex_);
  return call_.local_mic_muted;
}

void NodeService::set_call_mic_muted(bool muted) {
  std::lock_guard lock(call_mutex_);
  call_.local_mic_muted = muted;
}

bool NodeService::call_camera_on() const {
  std::lock_guard lock(call_mutex_);
  return call_.local_camera_on;
}

void NodeService::set_call_camera_on(bool on) {
  std::lock_guard lock(call_mutex_);
  call_.local_camera_on = on;
}

bool NodeService::call_is_host() const {
  std::lock_guard lock(call_mutex_);
  return call_is_host_;
}

std::vector<nyx::UserId> NodeService::call_participants() const {
  std::lock_guard lock(call_mutex_);
  return {call_participants_.begin(), call_participants_.end()};
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
        [this, session](const nyx::UserId& from, const nyx::ByteBuffer& frame) {
          handle_incoming_call_frame(session, frame, from);
        });
  }
}

void NodeService::handle_incoming_call_frame(const std::shared_ptr<NetSession>& session,
                                             const nyx::ByteBuffer& frame,
                                             const nyx::UserId& from) {
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
      if (call_.state == nyx::CallState::Active) {
        busy_rej.call_id = inv->call_id;
        busy_rej.reason = nyx::CallRejectReason::Busy;
        send_busy = true;
      } else {
        // Replace stale Incoming/Outgoing/Ringing/Ended so callback after hangup works.
        if (!call_.idle()) {
          stop_call_mesh();
          call_is_host_ = false;
          call_.reset();
          call_session_id_.clear();
          call_title_.clear();
          call_participants_.clear();
        }
        if (call_.on_invite(*inv)) {
          call_session_id_ = session->id;
          call_title_ = session->title;
          call_is_host_ = false;
          call_participants_.clear();
          call_participants_.insert(load_profile().public_key);
          if (from != nyx::UserId{}) call_participants_.insert(from);
          accepted = true;
        }
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
      if (ok && field && from != nyx::UserId{}) call_participants_.insert(from);
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
        call_participants_.clear();
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
      // Only end the call that this Hangup names. Retried hangups from a previous
      // call must not wipe a fresh Incoming/Outgoing with a new call_id.
      const bool field_guest_leave =
          call_.scope == nyx::CallScope::Field &&
          hang->reason != nyx::CallHangupReason::HubClosed && from != nyx::UserId{};
      if (field_guest_leave) {
        call_participants_.erase(from);
        if (call_mesh_) call_mesh_->remove_peer(from);
        ok = call_participants_.size() <= 1;
      } else {
        ok = call_.on_hangup(*hang);
      }
      if (ok) {
        stop_call_mesh();
        call_is_host_ = false;
        call_.reset();
        call_session_id_.clear();
        call_title_.clear();
        call_media_seq_ = 0;
        call_participants_.clear();
      }
    }
    if (ok) {
      emit_call_changed();
      emit_status("звонок завершён");
    }
    return;
  }

  if (auto upd = nyx::CallUpdateMessage::decode(frame)) {
    std::lock_guard lock(call_mutex_);
    call_.on_update(*upd);
    return;
  }

  if (auto intro = nyx::CallPeerIntroMessage::decode(frame)) {
    {
      std::lock_guard lock(call_mutex_);
      if (call_.call_id != intro->call_id || call_.idle()) return;
      call_participants_.insert(intro->peer.user_id);
      queue_mesh_peer(intro->peer);
    }
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
      return;
    }
    {
      std::lock_guard lock(call_mutex_);
      if (call_.call_id != roster->call_id || call_.idle()) return;
      call_participants_.clear();
      call_participants_.insert(load_profile().public_key);
      call_participants_.insert(roster->participants.begin(), roster->participants.end());
    }
    return;
  }

  if (auto candidate = nyx::CallRelayCandidateMessage::decode(frame)) {
    {
      std::lock_guard lock(call_mutex_);
      if (call_.call_id != candidate->call_id || !call_is_host_ ||
          call_participants_.find(candidate->user_id) == call_participants_.end()) {
        return;
      }
      call_relay_candidates_[candidate->user_id] = candidate->score;
    }
    recompute_relay_set();
    return;
  }

  if (auto relay_set = nyx::CallRelaySetMessage::decode(frame)) {
    {
      std::lock_guard lock(call_mutex_);
      if (call_.call_id != relay_set->call_id ||
          relay_set->epoch < call_relay_epoch_) {
        return;
      }
      call_relay_epoch_ = relay_set->epoch;
      call_relays_ = relay_set->relays;
    }
    apply_call_topology();
    request_call_mesh_announce();
    return;
  }

  if (auto gone = nyx::CallPeerGoneMessage::decode(frame)) {
    bool end_call = false;
    nyx::CallLeaveAckMessage ack;
    ack.call_id = gone->call_id;
    ack.user_id = gone->user_id;
    send_call_frame_on_session(session, ack.encode());
    {
      std::lock_guard lock(call_mutex_);
      if (call_.call_id != gone->call_id && !call_.idle()) {
        // Ignore peer-gone for a different call.
      } else if (!call_.idle()) {
        if (call_mesh_ && call_.call_id == gone->call_id) {
          call_mesh_->remove_peer(gone->user_id);
        }
        call_participants_.erase(gone->user_id);
        call_relay_candidates_.erase(gone->user_id);
        call_speaker_levels_.erase(gone->user_id);
        call_.on_peer_leave(gone->call_id);
        // Direct calls are 1:1 — peer leave means the call is over.
        // Field: end when mesh is empty (last peer left).
        const bool direct = call_.scope == nyx::CallScope::Direct;
        if (direct || call_participants_.size() <= 1) {
          stop_call_mesh();
          call_is_host_ = false;
          call_.reset();
          call_session_id_.clear();
          call_title_.clear();
          call_media_seq_ = 0;
          call_participants_.clear();
          end_call = true;
        }
      }
    }
    if (end_call) {
      emit_call_changed();
      emit_status("звонок завершён");
    }
    if (!end_call) recompute_relay_set();
    return;
  }

  if (auto ack = nyx::CallLeaveAckMessage::decode(frame)) {
    const auto self = load_profile().public_key;
    if (ack->user_id != self) return;
    std::lock_guard lock(call_mutex_);
    pending_call_signals_.erase(
        std::remove_if(pending_call_signals_.begin(), pending_call_signals_.end(),
                       [&](const PendingCallSignal& pending) {
                         auto gone = nyx::CallPeerGoneMessage::decode(pending.wire);
                         return gone && gone->call_id == ack->call_id &&
                                gone->user_id == ack->user_id;
                       }),
        pending_call_signals_.end());
    return;
  }
}

void NodeService::maybe_send_field_intros() {
  std::string sid;
  nyx::CallId call_id{};
  std::vector<nyx::UserId> participants;
  {
    std::lock_guard lock(call_mutex_);
    if (call_.state != nyx::CallState::Active || call_.scope != nyx::CallScope::Field) return;
    call_id = call_.call_id;
    sid = call_session_id_;
    participants.assign(call_participants_.begin(), call_participants_.end());
  }
  auto session = find_session(sid);
  if (!session || !session->group_hub) return;

  const std::size_t n = participants.size();
  if (n > nyx::kMaxCallParticipants) {
    emit_status("лимит участников звонка: 20");
    return;
  }

  session->group_hub->distribute_call_mesh_intros(call_id, participants);
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
      emit_status("нет права открывать комнату");
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
    // Ghost Incoming/Ended after lost hangup must not block callback.
    if (!call_.idle() && call_.state != nyx::CallState::Active) {
      stop_call_mesh();
      call_is_host_ = false;
      call_.reset();
      call_session_id_.clear();
      call_title_.clear();
      call_media_seq_ = 0;
      call_participants_.clear();
    }
    if (scope == nyx::CallScope::Field) {
      if (!call_.open_field_room(mode, target)) {
        emit_status("звонок уже идёт");
        return false;
      }
      call_is_host_ = true;
      call_participants_.clear();
      call_participants_.insert(load_profile().public_key);
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
    inv.sdp_lite = "nyx-call/2;av1;room";
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
      call_participants_.clear();
    }
    emit_status("не удалось открыть комнату");
    emit_call_changed();
    return false;
  }

  if (scope == nyx::CallScope::Field) {
    request_call_mesh_start();
    maybe_send_field_intros();
    request_call_mesh_announce();
    announce_relay_candidate();
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
    acc.sdp_lite = "nyx-call/2;av1;room";
    sid = call_session_id_;
    scope = call_.scope;
    call_is_host_ = false;
    if (scope == nyx::CallScope::Field) {
      call_participants_.insert(load_profile().public_key);
    }
  }
  auto session = find_session(sid);
  if (!session || !send_call_frame_on_session(session, acc.encode())) {
    {
      std::lock_guard lock(call_mutex_);
      stop_call_mesh();
      call_is_host_ = false;
      call_.reset();
      call_session_id_.clear();
      call_title_.clear();
      call_media_seq_ = 0;
      call_participants_.clear();
    }
    emit_status("не удалось войти в комнату");
    emit_call_changed();
    return false;
  }
  emit_call_changed();
  emit_status("на линии");
  if (scope == nyx::CallScope::Field) {
    request_call_mesh_start();
    request_call_mesh_announce();
    announce_relay_candidate();
  }
  return true;
}

bool NodeService::reject_call() {
  nyx::CallRejectMessage rej;
  std::string sid;
  {
    std::lock_guard lock(call_mutex_);
    if (call_.state != nyx::CallState::Incoming) return false;
    rej.call_id = call_.call_id;
    rej.reason = nyx::CallRejectReason::Declined;
    sid = call_session_id_;
    call_.reject();
    stop_call_mesh();
    call_is_host_ = false;
    call_.reset();
    call_session_id_.clear();
    call_title_.clear();
    call_participants_.clear();
  }
  if (auto session = find_session(sid)) send_call_frame_on_session(session, rej.encode());
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
  nyx::CallId call_id{};
  {
    std::lock_guard lock(call_mutex_);
    if (call_.idle()) return false;
    hang.call_id = call_.call_id;
    hang.reason =
        call_.scope == nyx::CallScope::Field && call_is_host_
            ? nyx::CallHangupReason::HubClosed
            : nyx::CallHangupReason::Normal;
    gone.call_id = call_.call_id;
    call_id = call_.call_id;
    sid = call_session_id_;
    is_host = call_is_host_;
    field = call_.scope == nyx::CallScope::Field;
  }
  self = load_profile().public_key;
  gone.user_id = self;

  // Keep leave signaling alive briefly after local media has stopped.
  if (auto session = find_session(sid)) {
    const nyx::ByteBuffer wire =
        (field && !is_host) ? gone.encode() : hang.encode();
    send_call_frame_on_session(session, wire);
    if (field) {
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard lock(call_mutex_);
      pending_call_signals_.push_back(
          PendingCallSignal{sid, wire, now + kCallSignalRetryInterval,
                            now + std::chrono::seconds(5)});
    }
  }

  {
    std::lock_guard lock(call_mutex_);
    if (!call_.idle() && call_.call_id == call_id) {
      call_.hangup();
      stop_call_mesh();
      call_is_host_ = false;
      call_.reset();
      call_session_id_.clear();
      call_title_.clear();
      call_media_seq_ = 0;
      call_participants_.clear();
    }
  }
  emit_call_changed();
  emit_status(field && is_host ? "комната закрыта" : "звонок завершён");
  return true;
}

}  // namespace nyx_app
