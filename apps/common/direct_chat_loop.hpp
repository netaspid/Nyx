#pragma once

/** Общий цикл ChatService + FileTransferService (GUI и CLI). */

#include "nyx/chat_service.hpp"
#include "nyx/connection.hpp"
#include "nyx/file_transfer.hpp"

#include <functional>

namespace nyx_app {

/** Крутит tick/pump/recv пока should_continue() и chat.connected(). */
void pump_direct_chat(nyx::ChatService& chat, nyx::FileTransferService& files,
                      nyx::Connection& connection,
                      const std::function<bool()>& should_continue,
                      const std::function<void()>& on_user_stop = {});

}  // namespace nyx_app
