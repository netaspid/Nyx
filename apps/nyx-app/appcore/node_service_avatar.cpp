#include "node_service.hpp"

#include "nyx/avatar_proto.hpp"
#include "nyx/avatar_store.hpp"
#include "nyx/util.hpp"

#include <algorithm>

namespace nyx_app {

namespace {

void send_avatar_file(nyx::Connection& conn, const nyx::FileHash& hash,
                      const nyx::ByteBuffer& data, const std::string& mime) {
  nyx::AvatarOffer offer;
  offer.hash = hash;
  offer.size = data.size();
  offer.mime = mime.empty() ? "image/jpeg" : mime;
  conn.send_payload(nyx::kBulkStream, offer.encode());

  uint32_t index = 0;
  for (std::size_t off = 0; off < data.size(); off += nyx::kAvatarChunkSize) {
    nyx::AvatarChunk chunk;
    chunk.hash = hash;
    chunk.index = index++;
    const std::size_t n = std::min(nyx::kAvatarChunkSize, data.size() - off);
    chunk.data.assign(data.begin() + static_cast<std::ptrdiff_t>(off),
                     data.begin() + static_cast<std::ptrdiff_t>(off + n));
    conn.send_payload(nyx::kBulkStream, chunk.encode());
  }
  nyx::AvatarDone done;
  done.hash = hash;
  conn.send_payload(nyx::kBulkStream, done.encode());
}

}  // namespace

void NodeService::request_missing_avatars(nyx::Connection& conn, const nyx::UserId& peer,
                                          const std::vector<nyx::FileHash>& hashes) {
  nyx::AvatarStore store;
  store.load();
  for (const auto& h : hashes) {
    if (store.has_peer_photo(peer, h)) continue;
    nyx::AvatarRequest req;
    req.hash = h;
    conn.send_payload(nyx::kBulkStream, req.encode());
  }
}

void NodeService::sync_avatars_after_hello(const std::shared_ptr<NetSession>& session,
                                          const nyx::HelloMessage& peer) {
  if (!session || !session->connection || !peer.has_profile_meta) return;
  if (peer.profile_meta.photo_hashes.empty()) return;
  session->avatar_peer = peer.public_key;
  request_missing_avatars(*session->connection, peer.public_key,
                          peer.profile_meta.photo_hashes);
}

bool NodeService::handle_avatar_bulk(const std::shared_ptr<NetSession>& session,
                                     const nyx::ByteBuffer& payload) {
  if (!session || !session->connection || !nyx::is_avatar_frame(payload)) return false;

  if (auto req = nyx::AvatarRequest::decode(payload)) {
    nyx::AvatarStore store;
    store.load();
    nyx::ByteBuffer data;
    if (!store.read_bytes(req->hash, data)) {
      nyx::AvatarDeny deny;
      deny.hash = req->hash;
      deny.reason = "нет фото";
      session->connection->send_payload(nyx::kBulkStream, deny.encode());
      return true;
    }
    std::string mime = "image/jpeg";
    for (const auto& e : store.photos()) {
      if (e.hash == req->hash) {
        mime = e.mime;
        break;
      }
    }
    send_avatar_file(*session->connection, req->hash, data, mime);
    return true;
  }

  if (auto offer = nyx::AvatarOffer::decode(payload)) {
    if (offer->size == 0 || offer->size > nyx::kMaxAvatarBytes) return true;
    session->avatar_rx = NetSession::AvatarRx{};
    session->avatar_rx->hash = offer->hash;
    session->avatar_rx->size = offer->size;
    session->avatar_rx->mime = offer->mime;
    session->avatar_rx->data.clear();
    session->avatar_rx->data.reserve(static_cast<std::size_t>(offer->size));
    return true;
  }

  if (auto chunk = nyx::AvatarChunk::decode(payload)) {
    if (!session->avatar_rx || session->avatar_rx->hash != chunk->hash) return true;
    session->avatar_rx->data.insert(session->avatar_rx->data.end(), chunk->data.begin(),
                                    chunk->data.end());
    return true;
  }

  if (auto done = nyx::AvatarDone::decode(payload)) {
    if (!session->avatar_rx || session->avatar_rx->hash != done->hash) return true;
    if (session->avatar_rx->data.size() != session->avatar_rx->size) {
      session->avatar_rx.reset();
      return true;
    }
    nyx::AvatarStore store;
    store.load();
    const nyx::UserId peer = session->avatar_peer;
    const bool peer_ok = std::any_of(peer.begin(), peer.end(), [](uint8_t b) { return b != 0; });
    if (peer_ok) {
      store.cache_peer_photo(peer, done->hash, session->avatar_rx->data,
                             session->avatar_rx->mime);
      SessionsChangedCallback cb;
      {
        std::lock_guard lock(cb_mutex_);
        cb = on_avatars_changed_;
      }
      if (cb) cb();
    }
    session->avatar_rx.reset();
    return true;
  }

  if (nyx::AvatarDeny::decode(payload)) return true;
  return false;
}

}  // namespace nyx_app
