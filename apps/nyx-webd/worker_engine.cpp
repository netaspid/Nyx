#include "worker_engine.hpp"
#include "json_util.hpp"

#include "node_service.hpp"

#include "nyx/account_store.hpp"
#include "nyx/call_media.hpp"
#include "nyx/call_session.hpp"
#include "nyx/conversation.hpp"
#include "nyx/file_access.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/json_text.hpp"
#include "nyx/chat_id.hpp"
#include "nyx/message_store.hpp"
#include "nyx/paths.hpp"
#include "nyx/profile_meta.hpp"
#include "nyx/util.hpp"

#include <sstream>
#include <utility>

namespace nyx_web {
namespace {

std::string bytes_to_b64(const uint8_t* data, std::size_t len) {
  static const char* kTbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (std::size_t i = 0; i < len; i += 3) {
    const uint32_t n = (uint32_t(data[i]) << 16) |
                       ((i + 1 < len ? uint32_t(data[i + 1]) : 0u) << 8) |
                       (i + 2 < len ? uint32_t(data[i + 2]) : 0u);
    out.push_back(kTbl[(n >> 18) & 63]);
    out.push_back(kTbl[(n >> 12) & 63]);
    out.push_back(i + 1 < len ? kTbl[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < len ? kTbl[n & 63] : '=');
  }
  return out;
}

std::optional<std::vector<uint8_t>> b64_to_bytes(const std::string& s) {
  auto dec = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<uint8_t> out;
  int val = 0, valb = -8;
  for (char c : s) {
    if (c == '=') break;
    int d = dec(c);
    if (d < 0) continue;
    val = (val << 6) + d;
    valb += 6;
    if (valb >= 0) {
      out.push_back(uint8_t((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}


std::string call_state_str(nyx::CallState st) {
  switch (st) {
  case nyx::CallState::Incoming:
    return "incoming";
  case nyx::CallState::Outgoing:
    return "outgoing";
  case nyx::CallState::Ringing:
    return "ringing";
  case nyx::CallState::Active:
    return "active";
  case nyx::CallState::Ended:
    return "ended";
  default:
    return "idle";
  }
}

std::string ok_result(const std::string& id, const std::string& result_json) {
  return "{\"type\":\"rpc_result\",\"id\":" + json_str(id) + ",\"ok\":true,\"result\":" +
         result_json + "}";
}

std::string err_result(const std::string& id, const std::string& msg) {
  return "{\"type\":\"rpc_result\",\"id\":" + json_str(id) + ",\"ok\":false,\"error\":" +
         json_str(msg) + "}";
}

std::string event_msg(const std::string& name, const std::string& payload) {
  return "{\"type\":\"event\",\"name\":" + json_str(name) + ",\"payload\":" + payload + "}";
}

} // namespace

struct WorkerEngine::Impl {
  std::string data_root;
  nyx_app::NodeService svc;
  EventSink sink;
  std::mutex sink_mutex;
  bool unlocked = false;
  std::string account_id;
  std::string nickname;

  void emit(const std::string& ev) {
    EventSink s;
    {
      std::lock_guard lock(sink_mutex);
      s = sink;
    }
    if (s)
      s(ev);
  }

  void wire_callbacks() {
    svc.set_on_status([this](const std::string& t) {
      emit(event_msg("status", json_str(t)));
    });
    svc.set_on_message([this](const nyx_app::UiMessage& m) {
      std::ostringstream ss;
      ss << "{\"messageId\":" << m.message_id << ",\"ts\":" << m.timestamp_ms
         << ",\"author\":" << json_str(m.author) << ",\"authorId\":" << json_str(m.author_user_id)
         << ",\"text\":" << json_str(m.text) << ",\"outgoing\":" << (m.outgoing ? "true" : "false")
         << ",\"delivery\":" << json_str(m.delivery) << ",\"sessionId\":" << json_str(m.session_id)
         << ",\"chatKey\":" << json_str(m.chat_key) << "}";
      emit(event_msg("message", ss.str()));
    });
    svc.set_on_sessions_changed([this]() { emit(event_msg("sessionsChanged", "{}")); });
    svc.set_on_chat_ready([this](const std::string& sid,
                                 const std::string& title,
                                 const std::string& label,
                                 nyx::ConversationKind kind,
                                 const std::string& ref) {
      std::ostringstream ss;
      ss << "{\"sessionId\":" << json_str(sid) << ",\"title\":" << json_str(title)
         << ",\"connectionLabel\":" << json_str(label) << ",\"kind\":" << static_cast<int>(kind)
         << ",\"refId\":" << json_str(ref) << "}";
      emit(event_msg("chatReady", ss.str()));
    });
    svc.set_on_session_ended([this](const std::string& sid) {
      emit(event_msg("sessionEnded", "{\"sessionId\":" + json_str(sid) + "}"));
    });
    svc.set_on_invite_token([this](const std::string& tok) {
      const std::string prefix = tok.size() > 8 ? tok.substr(0, 8) : tok;
      emit(event_msg("inviteToken", json_str(prefix)));
    });
    svc.set_on_lan_peers([this](const std::vector<nyx::LanPeer>& peers) {
      std::ostringstream ss;
      ss << "[";
      for (std::size_t i = 0; i < peers.size(); ++i) {
        if (i)
          ss << ',';
        ss << "{\"host\":" << json_str(peers[i].host) << ",\"port\":" << peers[i].port
           << ",\"instance\":" << json_str(peers[i].instance)
           << ",\"userIdShort\":" << json_str(peers[i].user_id_short) << "}";
      }
      ss << "]";
      emit(event_msg("lanPeers", ss.str()));
    });
    svc.set_on_group_created([this](const std::string& gid, const std::string& invite) {
      emit(event_msg("groupCreated",
                     "{\"groupId\":" + json_str(gid) + ",\"invite\":" + json_str(invite) + "}"));
    });
    svc.set_on_group_meta_changed([this]() { emit(event_msg("groupMetaChanged", "{}")); });
    svc.set_on_file_progress([this](const std::string& label, int pct) {
      emit(event_msg("fileProgress",
                     "{\"label\":" + json_str(label) + ",\"percent\":" + std::to_string(pct) + "}"));
    });
    svc.set_on_remote_files([this](const std::vector<nyx::FileEntry>&) {
      emit(event_msg("remoteFilesChanged", "{}"));
    });
    svc.set_on_transfer_queue_changed([this]() { emit(event_msg("transferQueueChanged", "{}")); });
    svc.set_on_file_access_sync([this]() { emit(event_msg("fileAccessChanged", "{}")); });
    svc.set_on_call_changed([this]() {
      std::ostringstream ss;
      ss << "{\"state\":" << json_str(call_state_str(svc.call_state()))
         << ",\"title\":" << json_str(svc.call_title())
         << ",\"video\":" << (svc.call_mode() == nyx::CallMode::AudioVideo ? "true" : "false")
         << ",\"micMuted\":" << (svc.call_mic_muted() ? "true" : "false")
         << ",\"cameraOn\":" << (svc.call_camera_on() ? "true" : "false")
         << ",\"fieldRoom\":" << (svc.call_is_field_room() ? "true" : "false")
         << ",\"callId\":" << json_str(svc.call_id_hex()) << ",\"participants\":[";
      const auto parts = svc.call_participants();
      for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i)
          ss << ',';
        ss << json_str(nyx::to_hex(parts[i].data(), parts[i].size()));
      }
      ss << "]}";
      emit(event_msg("callChanged", ss.str()));
    });
    svc.set_on_call_media([this](nyx::CallMediaType type,
                                 const nyx::ByteBuffer& payload,
                                 const nyx::UserId& from) {
      std::ostringstream ss;
      ss << "{\"mediaType\":" << static_cast<int>(type) << ",\"from\":"
         << json_str(nyx::to_hex(from.data(), from.size()))
         << ",\"payloadBase64\":" << json_str(bytes_to_b64(payload.data(), payload.size())) << "}";
      emit(event_msg("callMedia", ss.str()));
    });
    svc.set_on_avatars_changed([this]() { emit(event_msg("avatarsChanged", "{}")); });
  }

  std::string list_accounts_json() {
    auto accounts = nyx::list_accounts();
    std::ostringstream ss;
    ss << "[";
    for (std::size_t i = 0; i < accounts.size(); ++i) {
      if (i)
        ss << ',';
      const auto& a = accounts[i];
      ss << "{\"id\":" << json_str(a.id) << ",\"nickname\":" << json_str(a.nickname)
         << ",\"locked\":" << (a.locked ? "true" : "false")
         << ",\"hasRecovery\":" << (a.has_recovery ? "true" : "false")
         << ",\"rememberActive\":" << (a.remember_active ? "true" : "false") << "}";
    }
    ss << "]";
    return ss.str();
  }

  std::string conversations_json() {
    if (!unlocked)
      return "[]";
    const auto list = nyx::list_conversations(svc.profile().user_id());
    std::ostringstream ss;
    ss << "[";
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (i)
        ss << ',';
      const auto& c = list[i];
      const std::string ref =
          c.kind == nyx::ConversationKind::Group ? c.group_id_hex : c.peer_id_hex;
      ss << "{\"key\":" << json_str(c.key) << ",\"title\":" << json_str(c.title)
         << ",\"kind\":" << static_cast<int>(c.kind) << ",\"refId\":" << json_str(ref)
         << ",\"lastPreview\":" << json_str(c.preview) << ",\"ts\":" << c.timestamp_ms << "}";
    }
    ss << "]";
    return ss.str();
  }

  std::string groups_json() {
    const auto groups = svc.list_groups();
    std::ostringstream ss;
    ss << "[";
    for (std::size_t i = 0; i < groups.size(); ++i) {
      if (i)
        ss << ',';
      const auto& g = groups[i];
      ss << "{\"id\":" << json_str(nyx::GroupStore::group_id_hex(g.id))
         << ",\"name\":" << json_str(g.name)
         << ",\"invite\":" << json_str(nyx::GroupStore::invite_hex(g.invite_token))
         << ",\"description\":" << json_str(g.description)
         << ",\"direction\":" << json_str(g.direction) << ",\"tags\":" << json_str(g.tags)
         << ",\"publicListed\":"
         << (g.visibility == nyx::GroupVisibility::PublicListed ? "true" : "false") << "}";
    }
    ss << "]";
    return ss.str();
  }
};

WorkerEngine::WorkerEngine(std::string data_root) : impl_(std::make_unique<Impl>()) {
  impl_->data_root = std::move(data_root);
  nyx::set_base_data_root(impl_->data_root);
  nyx::ensure_data_dir();
  impl_->wire_callbacks();
  impl_->svc.load_network_config();
}

WorkerEngine::~WorkerEngine() {
  if (impl_->unlocked) {
    impl_->svc.stop();
    nyx::lock_session(false);
  }
}

void WorkerEngine::set_event_sink(EventSink sink) {
  std::lock_guard lock(impl_->sink_mutex);
  impl_->sink = std::move(sink);
}

std::string WorkerEngine::handle_rpc(const std::string& raw_json) {
  const auto id = json_get_string(raw_json, "id").value_or("");
  const auto op = json_get_string(raw_json, "op").value_or("");
  const std::string args = json_obj_get(raw_json, "args");
  const std::string& a = args.empty() ? raw_json : args;

  try {
    if (op == "health")
      return ok_result(id, "{\"service\":\"nyx-web-worker\"}");

    if (op == "listAccounts")
      return ok_result(id, impl_->list_accounts_json());

    if (op == "createAccount") {
      const auto nick = json_get_string(a, "nickname").value_or("");
      const auto pass = json_get_string(a, "password").value_or("");
      std::string phrase;
      std::string err;
      nyx::AccountMeta meta;
      if (!nyx::create_account(nick, pass, &phrase, &meta, &err))
        return err_result(id, err.empty() ? "create failed" : err);
      return ok_result(id,
                       "{\"accountId\":" + json_str(meta.id) + ",\"recoveryPhrase\":" +
                           json_str(phrase) + "}");
    }

    if (op == "unlockAccount") {
      const auto acc = json_get_string(a, "accountId").value_or("");
      const auto pass = json_get_string(a, "password").value_or("");
      const bool remember = json_get_bool(a, "rememberMe").value_or(false);
      if (impl_->unlocked) {
        impl_->svc.stop();
        nyx::lock_session(false);
        impl_->unlocked = false;
      }
      nyx::Profile profile;
      std::string err;
      if (!nyx::unlock_account(acc, pass, remember, &profile, &err))
        return err_result(id, err.empty() ? "unlock failed" : err);
      impl_->svc.set_nickname(profile.nickname);
      impl_->svc.reload_account_data();
      impl_->svc.start_dm_inbox();
      impl_->unlocked = true;
      impl_->account_id = acc;
      impl_->nickname = profile.nickname;
      return ok_result(id,
                       "{\"accountId\":" + json_str(acc) + ",\"nickname\":" +
                           json_str(profile.nickname) + ",\"userId\":" +
                           json_str(nyx::to_hex(profile.user_id().data(), profile.user_id().size())) +
                           "}");
    }

    if (op == "tryUnlockRemembered") {
      const auto acc = json_get_string(a, "accountId").value_or("");
      if (impl_->unlocked) {
        impl_->svc.stop();
        nyx::lock_session(false);
        impl_->unlocked = false;
      }
      nyx::Profile profile;
      std::string err;
      if (!nyx::try_unlock_remembered(acc, &profile, &err))
        return err_result(id, err.empty() ? "remember unlock failed" : err);
      impl_->svc.set_nickname(profile.nickname);
      impl_->svc.reload_account_data();
      impl_->svc.start_dm_inbox();
      impl_->unlocked = true;
      impl_->account_id = acc;
      impl_->nickname = profile.nickname;
      return ok_result(id,
                       "{\"accountId\":" + json_str(acc) + ",\"nickname\":" +
                           json_str(profile.nickname) + "}");
    }

    if (op == "signOut") {
      if (impl_->unlocked) {
        impl_->svc.stop();
        nyx::lock_session(false);
        impl_->unlocked = false;
        impl_->account_id.clear();
      }
      return ok_result(id, "true");
    }

    if (op == "confirmRecoveryPhraseSaved") {
      return ok_result(id, "true");
    }

    if (op == "sessionInfo") {
      return ok_result(id,
                       "{\"unlocked\":" + std::string(impl_->unlocked ? "true" : "false") +
                           ",\"accountId\":" + json_str(impl_->account_id) + ",\"nickname\":" +
                           json_str(impl_->nickname) + ",\"busy\":" +
                           (impl_->svc.busy() ? "true" : "false") + ",\"listening\":" +
                           (impl_->svc.is_listening() ? "true" : "false") + "}");
    }

    if (!impl_->unlocked && op != "listAccounts" && op != "health")
      return err_result(id, "not unlocked");

    if (op == "getNetworkConfig") {
      return ok_result(id,
                       "{\"rendezvousList\":" + json_str(impl_->svc.rendezvous_list_string()) +
                           ",\"discoveryMode\":" +
                           std::to_string(static_cast<int>(impl_->svc.network_config().mode)) + "}");
    }

    if (op == "saveNetworkSettings") {
      const auto list = json_get_string(a, "rendezvousList").value_or("");
      const auto mode = json_get_int(a, "discoveryMode").value_or(0);
      if (!list.empty() && !impl_->svc.set_rendezvous_list(list))
        return err_result(id, "bad rendezvous list");
      impl_->svc.set_discovery_mode(static_cast<int>(mode));
      if (!impl_->svc.save_network_config())
        return err_result(id, "save failed");
      return ok_result(id, "true");
    }

    if (op == "connectToken") {
      const auto tok = json_get_string(a, "token").value_or("");
      if (!impl_->svc.start_connect_token(tok))
        return err_result(id, "connect failed");
      return ok_result(id, "true");
    }

    if (op == "startDmInbox") {
      if (!impl_->svc.start_dm_inbox())
        return err_result(id, "inbox failed");
      return ok_result(id, "true");
    }

    if (op == "disconnectSession") {
      const auto sid = json_get_string(a, "sessionId").value_or("");
      if (sid.empty())
        impl_->svc.stop();
      else
        impl_->svc.stop_session(sid);
      return ok_result(id, "true");
    }

    if (op == "sendMessage") {
      const auto text = json_get_string(a, "text").value_or("");
      const auto sid = json_get_string(a, "sessionId").value_or("");
      if (!impl_->svc.send_message(text, sid))
        return err_result(id, "send failed");
      return ok_result(id, "true");
    }

    if (op == "listConversations")
      return ok_result(id, impl_->conversations_json());

    if (op == "listGroups")
      return ok_result(id, impl_->groups_json());

    if (op == "createGroup") {
      const auto name = json_get_string(a, "name").value_or("");
      if (!impl_->svc.create_group(name))
        return err_result(id, "create group failed");
      return ok_result(id, "true");
    }

    if (op == "startFieldHub") {
      const auto gid = json_get_string(a, "groupId").value_or("");
      if (!impl_->svc.start_group_hub(gid))
        return err_result(id, "hub failed");
      return ok_result(id, "true");
    }

    if (op == "joinField") {
      const auto inv = json_get_string(a, "invite").value_or("");
      if (!impl_->svc.start_group_join(inv))
        return err_result(id, "join failed");
      return ok_result(id, "true");
    }

    if (op == "loadHistory") {
      const auto kind = json_get_int(a, "kind").value_or(0);
      const auto ref = json_get_string(a, "refId").value_or("");
      std::string path;
      if (kind == static_cast<int>(nyx::ConversationKind::Group)) {
        nyx::GroupId gid {};
        if (!nyx::GroupStore::group_id_from_hex(ref, gid))
          return err_result(id, "bad group id");
        path = nyx::MessageStore::path_for_group(gid);
      } else {
        path = nyx::data_dir() + "/chats/" + ref + ".jsonl";
      }
      nyx::MessageStore store(path);
      const auto recent = store.recent(200);
      std::ostringstream ss;
      ss << "[";
      for (std::size_t i = 0; i < recent.size(); ++i) {
        if (i)
          ss << ',';
        const auto& m = recent[i];
        ss << "{\"id\":" << m.id << ",\"ts\":" << m.timestamp_ms << ",\"author\":" << json_str(m.author)
           << ",\"authorId\":" << json_str(m.author_id_hex) << ",\"text\":" << json_str(m.text)
           << ",\"outgoing\":" << (m.outgoing ? "true" : "false") << "}";
      }
      ss << "]";
      return ok_result(id, ss.str());
    }

    if (op == "refreshLanPeers") {
      if (!impl_->svc.scan_lan_peers(1200))
        return err_result(id, "lan scan failed");
      return ok_result(id, "true");
    }

    if (op == "listLocalFiles") {
      const auto scope = json_get_string(a, "scopeGroupId").value_or("");
      const auto files = impl_->svc.local_files_for_scope(scope);
      std::ostringstream ss;
      ss << "[";
      for (std::size_t i = 0; i < files.size(); ++i) {
        if (i)
          ss << ',';
        const auto& f = files[i];
        ss << "{\"hash\":" << json_str(nyx::to_hex(f.hash.data(), f.hash.size()))
           << ",\"name\":" << json_str(f.relative_path) << ",\"size\":" << f.size
           << ",\"mime\":" << json_str(f.mime) << ",\"root\":" << json_str(f.root_path)
           << ",\"directory\":" << (f.is_directory() ? "true" : "false") << "}";
      }
      ss << "]";
      return ok_result(id, ss.str());
    }

    if (op == "listRemoteFiles") {
      const auto files = impl_->svc.remote_files();
      std::ostringstream ss;
      ss << "[";
      for (std::size_t i = 0; i < files.size(); ++i) {
        if (i)
          ss << ',';
        const auto& f = files[i];
        ss << "{\"hash\":" << json_str(nyx::to_hex(f.hash.data(), f.hash.size()))
           << ",\"name\":" << json_str(f.relative_path) << ",\"size\":" << f.size
           << ",\"mime\":" << json_str(f.mime) << ",\"root\":" << json_str(f.root_path)
           << ",\"directory\":" << (f.is_directory() ? "true" : "false") << "}";
      }
      ss << "]";
      return ok_result(id, ss.str());
    }

    if (op == "requestRemoteFiles") {
      const auto root = json_get_string(a, "rootPath").value_or("");
      const auto rel = json_get_string(a, "relativePath").value_or("");
      const auto scope = json_get_string(a, "scopeGroupId").value_or("");
      bool ok = scope.empty() ? impl_->svc.request_remote_files_at(root, rel)
                              : impl_->svc.request_remote_files_at(scope, root, rel);
      if (!ok)
        return err_result(id, "request failed");
      return ok_result(id, "true");
    }

    if (op == "downloadFile") {
      const auto hash = json_get_string(a, "hash").value_or("");
      const auto dest = json_get_string(a, "destPath").value_or("");
      if (!impl_->svc.download_file(hash, dest))
        return err_result(id, "download failed");
      return ok_result(id, "true");
    }

    if (op == "sendFile") {
      const auto path = json_get_string(a, "pathOrHash").value_or("");
      if (!impl_->svc.send_file(path))
        return err_result(id, "send file failed");
      return ok_result(id, "true");
    }

    if (op == "addIndexedFolder") {
      const auto path = json_get_string(a, "path").value_or("");
      const auto scope = json_get_string(a, "scopeGroupId").value_or("");
      if (!impl_->svc.index_folder(path, scope))
        return err_result(id, "index failed");
      return ok_result(id, "true");
    }

    if (op == "removeIndexedFolder") {
      const auto path = json_get_string(a, "path").value_or("");
      const auto scope = json_get_string(a, "scopeGroupId").value_or("");
      if (!impl_->svc.remove_share_root(path, scope))
        return err_result(id, "remove failed");
      return ok_result(id, "true");
    }

    if (op == "transferQueue") {
      const auto q = impl_->svc.transfer_queue();
      std::ostringstream ss;
      ss << "[";
      for (std::size_t i = 0; i < q.size(); ++i) {
        if (i)
          ss << ',';
        const auto& it = q[i];
        ss << "{\"hash\":" << json_str(it.hash_hex) << ",\"dest\":" << json_str(it.dest_path)
           << ",\"state\":" << json_str(it.state) << ",\"progress\":" << it.progress
           << ",\"paused\":" << (it.paused ? "true" : "false") << ",\"direction\":"
           << json_str(it.direction) << "}";
      }
      ss << "]";
      return ok_result(id, ss.str());
    }

    if (op == "fileBlobPath") {
      const auto hash = json_get_string(a, "hash").value_or("");
      auto entry = impl_->svc.find_file_object(hash);
      if (!entry)
        return err_result(id, "not found");
      const std::string path = entry->root_path + "/" + entry->relative_path;
      return ok_result(id, json_str(path));
    }

    if (op == "getProfileMeta") {
      nyx::ProfileMeta meta;
      nyx::load_profile_meta(meta);
      return ok_result(id,
                       "{\"bio\":" + json_str(meta.bio) + ",\"interests\":" +
                           json_str(meta.interests) + ",\"availability\":" +
                           json_str(nyx::availability_to_string(meta.availability)) + "}");
    }

    if (op == "setProfileMeta") {
      nyx::ProfileMeta meta;
      nyx::load_profile_meta(meta);
      if (auto bio = json_get_string(a, "bio"))
        meta.bio = *bio;
      if (auto interests = json_get_string(a, "interests"))
        meta.interests = *interests;
      if (auto avail = json_get_string(a, "availability"))
        meta.availability = nyx::availability_from_string(*avail);
      if (!nyx::save_profile_meta(meta))
        return err_result(id, "save meta failed");
      return ok_result(id, "true");
    }

    if (op == "listContacts") {
      nyx::ContactBook book(nyx::default_contacts_path());
      book.load();
      const auto all = book.contacts();
      std::ostringstream ss;
      ss << "[";
      for (std::size_t i = 0; i < all.size(); ++i) {
        if (i)
          ss << ',';
        ss << "{\"userId\":" << json_str(nyx::to_hex(all[i].user_id.data(), all[i].user_id.size()))
           << ",\"nickname\":" << json_str(all[i].nickname) << "}";
      }
      ss << "]";
      return ok_result(id, ss.str());
    }

    if (op == "startCall") {
      const bool video = json_get_bool(a, "video").value_or(false);
      const auto sid = json_get_string(a, "sessionId").value_or("");
      if (!impl_->svc.start_call(video, sid))
        return err_result(id, "start call failed");
      return ok_result(id, "true");
    }
    if (op == "acceptCall") {
      if (!impl_->svc.accept_call())
        return err_result(id, "accept failed");
      return ok_result(id, "true");
    }
    if (op == "rejectCall") {
      if (!impl_->svc.reject_call())
        return err_result(id, "reject failed");
      return ok_result(id, "true");
    }
    if (op == "hangupCall") {
      if (!impl_->svc.hangup_call())
        return err_result(id, "hangup failed");
      return ok_result(id, "true");
    }
    if (op == "setCallMicMuted") {
      impl_->svc.set_call_mic_muted(json_get_bool(a, "muted").value_or(false));
      return ok_result(id, "true");
    }
    if (op == "setCallCameraOn") {
      impl_->svc.set_call_camera_on(json_get_bool(a, "on").value_or(false));
      return ok_result(id, "true");
    }
    if (op == "getCallState") {
      std::ostringstream ss;
      ss << "{\"state\":" << json_str(call_state_str(impl_->svc.call_state()))
         << ",\"canStart\":" << (impl_->svc.can_start_call() ? "true" : "false") << "}";
      return ok_result(id, ss.str());
    }

    if (op == "sendCallMedia") {
      const auto type = json_get_int(a, "mediaType").value_or(0);
      const auto b64 = json_get_string(a, "payloadBase64").value_or("");
      const auto level = static_cast<uint8_t>(json_get_int(a, "audioLevel").value_or(0));
      auto bytes = b64_to_bytes(b64);
      if (!bytes)
        return err_result(id, "bad base64");
      nyx::ByteBuffer buf(bytes->begin(), bytes->end());
      if (!impl_->svc.send_call_media(static_cast<nyx::CallMediaType>(type), buf, level))
        return err_result(id, "send media failed");
      return ok_result(id, "true");
    }

    return err_result(id, "unknown op: " + op);
  } catch (const std::exception& ex) {
    return err_result(id, ex.what());
  }
}

} // namespace nyx_web
