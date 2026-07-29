#pragma once

#include "nyx/connection.hpp"
#include "nyx/identity.hpp"

namespace nyx_node {

void run_chat_session(nyx::Connection& connection,
                      const nyx::Profile& profile,
                      bool incoming_connection);

}
