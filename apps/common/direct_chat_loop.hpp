#pragma once

/** Shared ChatService + FileTransferService loop (GUI and CLI). */

#include "nyx/chat_service.hpp"
#include "nyx/connection.hpp"
#include "nyx/file_transfer.hpp"

#include <functional>

namespace nyx_app {

/** Runs tick/pump/recv while should_continue() and chat.connected().
 *  on_bulk: returning true means the frame is handled (e.g. avatar); files is skipped.
 */
void pump_direct_chat(nyx::ChatService& chat,
                      nyx::FileTransferService& files,
                      nyx::Connection& connection,
                      const std::function<bool()>& should_continue,
                      const std::function<void()>& on_user_stop = {},
                      const std::function<void()>& on_tick = {},
                      const std::function<bool(const nyx::ByteBuffer&)>& on_bulk = {});

} // namespace nyx_app
