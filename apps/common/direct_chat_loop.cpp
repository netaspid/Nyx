#include "direct_chat_loop.hpp"

#include <chrono>
#include <thread>

namespace nyx_app {

void pump_direct_chat(nyx::ChatService& chat, nyx::FileTransferService& files,
                      nyx::Connection& connection,
                      const std::function<bool()>& should_continue,
                      const std::function<void()>& on_user_stop,
                      const std::function<void()>& on_tick,
                      const std::function<bool(const nyx::ByteBuffer&)>& on_bulk) {
  while (should_continue() && chat.connected()) {
    if (on_tick) on_tick();
    chat.tick();
    files.pump();
    nyx::ByteBuffer payload;
    uint32_t stream_id = 0;
    while (chat.connected() && connection.recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kChatStream) {
        chat.handle_payload(payload);
      } else if (stream_id == nyx::kBulkStream) {
        if (on_bulk && on_bulk(payload)) continue;
        files.handle_bulk(payload);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (!should_continue() && chat.connected()) {
    if (on_user_stop) {
      on_user_stop();
    } else {
      chat.send_bye("пользователь вышел");
    }
  }
}

}  // namespace nyx_app
