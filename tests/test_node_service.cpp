#include "node_service.hpp"

#include "nyx/identity.hpp"
#include "nyx/group.hpp"

#include <cassert>
#include <iostream>

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
    if (nyx::GroupStore::group_id_hex(g.id) == got_id) found = true;
  }
  assert(found);
  std::cout << "node service create group ok\n";
}

static void test_node_service_callbacks() {
  nyx_app::NodeService svc;
  assert(!svc.create_group(""));
  std::cout << "node service callbacks ok\n";
}

int main() {
  test_node_service_profile();
  test_node_service_create_group();
  test_node_service_callbacks();
  std::cout << "node service tests passed\n";
  return 0;
}
