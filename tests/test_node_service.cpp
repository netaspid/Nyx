#include "node_service.hpp"

#include "nyx/file_index.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/paths.hpp"
#include "nyx/session_intent.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

static void test_node_service_profile() {
  nyx_app::NodeService svc;
  svc.set_nickname("GuiTest");
  const auto profile = svc.profile();
  assert(profile.nickname == "GuiTest");
  assert(!nyx::short_user_id(profile.user_id()).empty());
  std::cout << "node service profile ok\n";
}

static void test_node_service_create_group() {
  nyx_app::NodeService svc;
  svc.set_nickname("GroupOwner");

  std::string got_id;
  std::string got_invite;
  svc.set_on_group_created([&](const std::string& gid, const std::string& invite) {
    got_id = gid;
    got_invite = invite;
  });

  assert(svc.create_group("Test Field"));
  assert(got_id.size() == 64);
  assert(got_invite.size() == 64);

  const auto groups = svc.list_groups();
  assert(!groups.empty());
  bool found = false;
  for (const auto& g : groups) {
    if (nyx::GroupStore::group_id_hex(g.id) == got_id)
      found = true;
  }
  assert(found);
  std::cout << "node service create group ok\n";
}

static void test_node_service_callbacks() {
  nyx_app::NodeService svc;
  assert(!svc.create_group(""));
  std::cout << "node service callbacks ok\n";
}

static void test_chat_media_import_directory() {
  nyx_app::NodeService svc;
  svc.set_nickname("MediaTest");
  const std::string source = "test_voice_message.m4a";
  {
    std::ofstream out(source, std::ios::binary | std::ios::trunc);
    out << "voice-payload";
  }
  auto entry = svc.import_file_object(source,
                                      "voice-message.m4a",
                                      "audio/mp4",
                                      {},
                                      {},
                                      "Медиа/Test Chat (12345678)/Голосовые сообщения");
  assert(entry);
  assert(entry->relative_path.find("Медиа/") == 0);
  assert(entry->mime == "audio/mp4");
  std::filesystem::remove(source);
  std::cout << "chat media import directory ok\n";
}

static void test_session_intent_store() {
  const std::string path = "test_session_intents.json";
  {
    nyx::SessionIntentStore store(path);
    nyx::SessionIntent intent;
    intent.key = "group:abcd";
    intent.kind = nyx::SessionIntentKind::GroupHub;
    intent.ref_id_hex = "abcd";
    intent.enabled = true;
    store.upsert(intent);
    assert(store.save());
  }
  {
    nyx::SessionIntentStore store(path);
    assert(store.load());
    assert(store.is_enabled("group:abcd"));
    store.disable("group:abcd");
    assert(!store.is_enabled("group:abcd"));
    assert(!store.is_enabled("group:unknown"));
    assert(store.save());
  }
  {
    nyx::SessionIntentStore store(path);
    assert(store.load());
    assert(!store.is_enabled("group:abcd"));
    assert(!store.is_enabled("dm:missing"));
  }
  std::remove(path.c_str());
  std::cout << "session intent store ok\n";
}

static void test_multi_session_hubs_parallel() {
  nyx_app::NodeService svc;
  svc.set_nickname("MultiOwner");
  assert(svc.create_group("FieldA"));
  assert(svc.create_group("FieldB"));
  const auto groups = svc.list_groups();
  assert(groups.size() >= 2);
  const std::string a = nyx::GroupStore::group_id_hex(groups[0].id);
  const std::string b = nyx::GroupStore::group_id_hex(groups[1].id);
  assert(svc.start_group_hub(a));
  assert(svc.start_group_hub(b));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  bool has_a = false;
  bool has_b = false;
  for (const auto& s : svc.list_sessions()) {
    if (s.id == nyx_app::make_group_session_id(a))
      has_a = true;
    if (s.id == nyx_app::make_group_session_id(b))
      has_b = true;
  }
  assert(has_a && has_b);
  svc.stop_session(nyx_app::make_group_session_id(a));
  svc.stop();
  std::cout << "multi session hubs parallel ok\n";
}

int main() {
  const std::string test_data_root = "test_nyx_appcore_data";
  std::filesystem::remove_all(test_data_root);
  nyx::set_base_data_root(test_data_root);
  assert(nyx::ensure_data_dir());
  test_node_service_profile();
  test_node_service_create_group();
  test_node_service_callbacks();
  test_chat_media_import_directory();
  test_session_intent_store();
  test_multi_session_hubs_parallel();
  nyx::set_base_data_root({});
  std::filesystem::remove_all(test_data_root);
  std::cout << "node service tests passed\n";
  return 0;
}
