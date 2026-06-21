#include "chat_session.hpp"

#include "cli_console.hpp"

#include "nyx/app.hpp"
#include "nyx/chat_service.hpp"
#include "nyx/identity.hpp"
#include "nyx/chat_id.hpp"
#include "nyx/file_index.hpp"
#include "nyx/file_transfer.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace nyx_node {

namespace {

std::vector<HistoryLine> to_history_lines(const std::vector<nyx::StoredMessage>& msgs) {
  std::vector<HistoryLine> lines;
  lines.reserve(msgs.size());
  for (const auto& msg : msgs) {
    HistoryLine line;
    line.timestamp_ms = msg.timestamp_ms;
    line.author = msg.author;
    line.text = msg.text;
    line.outgoing = msg.outgoing;
    lines.push_back(std::move(line));
  }
  return lines;
}

}  // namespace

void run_chat_session(nyx::Connection& connection, const nyx::Profile& profile,
                      bool incoming_connection) {
  CliConsole ui(profile.nickname);

  const std::string endpoint = connection.peer_host() + ':' +
                               std::to_string(connection.peer_port());
  if (incoming_connection) {
    ui.print_event("входящее соединение " + endpoint);
  } else {
    ui.print_event("соединение установлено с " + endpoint);
  }

  nyx::HelloMessage peer_hello;
  if (!nyx::exchange_hello(connection, profile, peer_hello)) {
    ui.print_event("таймаут обмена Hello");
    ui.print_event("не удалось обменяться профилем с собеседником");
    return;
  }

  nyx::remember_contact(peer_hello);
  ui.print_event(peer_hello.nickname + " в сети (id: " +
                 nyx::short_user_id(peer_hello.public_key) + ")");
  ui.print_header(peer_hello.nickname, nyx::short_user_id(peer_hello.public_key));

  nyx::ChatService::PeerInfo peer;
  peer.user_id = peer_hello.public_key;
  peer.nickname = peer_hello.nickname;

  nyx::ChatService chat(connection, profile, peer);

  nyx::FileIndex file_index;
  nyx::FileTransferService files(connection, file_index, nyx::default_downloads_dir());

  chat.set_on_message([&](const nyx::ChatMessage& msg, bool outgoing) {
    ui.print_message(msg.timestamp_ms, msg.author, msg.text, outgoing);
  });
  chat.set_on_delivery([&](uint64_t id, nyx::DeliveryStatus status) {
    if (status == nyx::DeliveryStatus::Delivered) {
      ui.print_event("доставлено #" + std::to_string(id));
    }
  });
  chat.set_on_event([&](const std::string& text) { ui.print_event(text); });

  files.set_on_event([&](const std::string& text) { ui.print_event(text); });
  files.set_on_progress([&](const nyx::FileHash& hash, uint64_t done, uint64_t total) {
    if (total == 0) return;
    const uint64_t pct = done * 100 / total;
    static uint64_t last_pct = 999;
    static std::string last_hash;
    const std::string hx = nyx::hash_hex(hash).substr(0, 8);
    if (pct != last_pct || hx != last_hash) {
      last_pct = pct;
      last_hash = hx;
      if (pct % 25 == 0 || done == total) {
        ui.print_event("файл " + hx + "… " + std::to_string(pct) + "%");
      }
    }
    if (done == total) last_pct = 999;
  });

  const auto recent = chat.history(20);
  if (!recent.empty()) {
    ui.print_event("загружена история (" + std::to_string(recent.size()) +
                   " сообщений)");
    ui.print_history(to_history_lines(recent));
  }

  std::atomic<bool> running{true};
  std::atomic<bool> user_quit{false};

  std::thread network_thread([&]() {
    while (running.load() && chat.connected()) {
      chat.tick();
      files.pump();
      nyx::ByteBuffer payload;
      uint32_t stream_id = 0;
      while (chat.connected() && connection.recv_stream(stream_id, payload)) {
        if (stream_id == nyx::kChatStream) {
          chat.handle_payload(payload);
        } else if (stream_id == nyx::kBulkStream) {
          files.handle_bulk(payload);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  ui.print_prompt();

  std::string line;
  while (running.load() && chat.connected() && std::getline(std::cin, line)) {
    if (line == "/quit" || line == "/exit") {
      user_quit.store(true);
      break;
    }
    if (line == "/help") {
      ui.print_help();
      ui.print_prompt();
      continue;
    }
    if (line == "/who") {
      ui.print_event("собеседник: " + peer.nickname + " (id: " +
                     nyx::to_hex(peer.user_id.data(), peer.user_id.size()) + ")");
      ui.print_event("chat_id: " + nyx::chat_id_hex(chat.chat_id()));
      ui.print_prompt();
      continue;
    }
    if (line == "/status") {
      ui.print_status(peer.nickname, endpoint, chat.connected());
      ui.print_event("ожидают доставки: " +
                     std::to_string(chat.outbox().pending_count()));
      ui.print_prompt();
      continue;
    }
    if (line.rfind("/history", 0) == 0) {
      std::size_t count = 20;
      if (line.size() > 9) {
        try {
          count = static_cast<std::size_t>(std::stoul(line.substr(9)));
        } catch (const std::exception&) {
        }
      }
      ui.print_history(to_history_lines(chat.history(count)));
      ui.print_prompt();
      continue;
    }
    if (line.rfind("/search ", 0) == 0) {
      const std::string query = line.substr(8);
      const auto hits = chat.search(query);
      if (hits.empty()) {
        ui.print_event("ничего не найдено по «" + query + "»");
      } else {
        ui.print_event("найдено " + std::to_string(hits.size()) + ":");
        ui.print_history(to_history_lines(hits));
      }
      ui.print_prompt();
      continue;
    }
    if (line.rfind("/send ", 0) == 0) {
      const std::string text = line.substr(6);
      if (text.empty()) {
        ui.print_event("использование: /send <текст>");
      } else if (!chat.send_message(text)) {
        ui.print_event("ошибка отправки");
        break;
      }
      ui.print_prompt();
      continue;
    }
    if (line.rfind("/index ", 0) == 0) {
      const std::string path = line.substr(7);
      if (file_index.add_root(path)) {
        ui.print_event("индекс обновлён: " + std::to_string(file_index.entries().size()) +
                       " файлов");
      } else {
        ui.print_event("не удалось проиндексировать: " + path);
      }
      ui.print_prompt();
      continue;
    }
    if (line == "/files") {
      const auto& entries = file_index.entries();
      if (entries.empty()) {
        ui.print_event("локальный индекс пуст (/index <папка>)");
      } else {
        ui.print_event("локальные файлы (" + std::to_string(entries.size()) + "):");
        for (const auto& e : entries) {
          ui.print_event("  " + nyx::hash_hex(e.hash).substr(0, 8) + "… " +
                         e.relative_path + " (" + std::to_string(e.size) + " b)");
        }
      }
      ui.print_prompt();
      continue;
    }
    if (line == "/remote") {
      files.request_list();
      ui.print_prompt();
      continue;
    }
    if (line.rfind("/get ", 0) == 0) {
      const std::string hash = line.substr(5);
      files.request_file(hash);
      ui.print_prompt();
      continue;
    }
    if (line.rfind("/sendfile ", 0) == 0) {
      const std::string path = line.substr(10);
      files.send_file(path);
      ui.print_prompt();
      continue;
    }
    if (line.empty()) {
      ui.print_prompt();
      continue;
    }
    if (line.rfind('/', 0) == 0) {
      ui.print_event("неизвестная команда: " + line + " (/help)");
      ui.print_prompt();
      continue;
    }

    if (!chat.send_message(line)) {
      ui.print_event("ошибка отправки — соединение, возможно, разорвано");
      break;
    }
    ui.print_prompt();
  }

  running.store(false);

  if (user_quit.load() && chat.connected()) {
    chat.send_bye("пользователь вышел");
    ui.print_event("вы отключились");
  }

  if (network_thread.joinable()) network_thread.join();

  if (!chat.connected() && !user_quit.load()) {
    ui.print_event("сессия с " + peer.nickname + " завершена");
  } else if (user_quit.load()) {
    ui.print_event("до встречи");
  }
}

}  // namespace nyx_node
