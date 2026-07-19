#pragma once

/** Общий цикл ChatService + FileTransferService (GUI и CLI). */

#include "nyx/chat_service.hpp"
#include "nyx/connection.hpp"
#include "nyx/file_transfer.hpp"

#include <functional>

namespace nyx_app {

/** Крутит tick/pump/recv пока should_continue() и chat.connected().
 *  on_bulk: если вернул true — кадр уже обработан (например avatar), files не зовём.
 */
void pump_direct_chat(nyx::ChatService& chat, nyx::FileTransferService& files,
                      nyx::Connection& connection,
                      const std::function<bool()>& should_continue,
                      const std::function<void()>& on_user_stop = {},
                      const std::function<void()>& on_tick = {},
                      const std::function<bool(const nyx::ByteBuffer&)>& on_bulk = {});

}  // namespace nyx_app
