#include "nyx/account_store.hpp"
#include "nyx/app.hpp"
#include "nyx/crypto.hpp"
#include "nyx/connection.hpp"
#include "nyx/identity.hpp"
#include "nyx/recovery_phrase.hpp"
#include "nyx/log.hpp"
#include "nyx/mdns.hpp"
#include "nyx/network_config.hpp"
#include "nyx/nat.hpp"
#include "nyx/conversation.hpp"
#include "nyx/message_store.hpp"
#include "nyx/chat_service.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/file_index.hpp"
#include "nyx/file_access.hpp"
#include "nyx/file_proto.hpp"
#include "nyx/file_transfer.hpp"
#include "nyx/group.hpp"
#include "nyx/group_hub.hpp"
#include "nyx/group_member.hpp"
#include "nyx/group_proto.hpp"
#include "nyx/messaging.hpp"
#include "nyx/avatar_proto.hpp"
#include "nyx/markdown_format.hpp"
#include "nyx/profile_meta.hpp"
#include "nyx/paths.hpp"
#include "nyx/proto.hpp"
#include "nyx/transport.hpp"
#include "nyx/util.hpp"

#include <atomic>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#endif

static void test_frame_roundtrip() {
  nyx::ByteBuffer payload = {1, 2, 3, 4, 5};
  auto frame = nyx::Frame::make(nyx::PacketType::Data, 1, 42, payload);
  auto wire = frame.encode();
  auto decoded = nyx::Frame::decode(wire.data(), wire.size());
  assert(decoded);
  assert(decoded->payload == payload);
  std::cout << "frame roundtrip ok\n";
}

static void test_noise_handshake() {
  nyx::HandshakeDriver initiator(nyx::HandshakeRole::Initiator);
  nyx::HandshakeDriver responder(nyx::HandshakeRole::Responder);

  auto m1 = initiator.step(nullptr);
  assert(m1);
  auto m2 = responder.step(&*m1);
  assert(m2);
  auto m3 = initiator.step(&*m2);
  assert(m3);
  responder.step(&*m3);
  initiator.step(nullptr);
  responder.step(nullptr);

  assert(initiator.complete());
  assert(responder.complete());

  auto sa = nyx::Session::from_handshake(initiator);
  auto sb = nyx::Session::from_handshake(responder);
  assert(sa && sb);

  auto ct = sa->encrypt({0x48, 0x69});
  assert(ct);
  auto pt = sb->decrypt(*ct);
  assert(pt && pt->size() == 2);
  std::cout << "noise handshake ok\n";
}

static void test_reliable() {
  nyx::ReliableSession a;
  nyx::ReliableSession b;
  nyx::ByteBuffer payload(4000, 0xAB);
  auto frames = a.send(0, payload);
  for (const auto& f : frames) {
    b.recv_wire(f);
    for (const auto& ack : b.make_ack_frames(0)) a.recv_wire(ack);
  }
  auto got = b.poll_recv();
  assert(got && got->size() == payload.size());
  std::cout << "reliable session ok\n";
}

static void test_rendezvous_client() {
  nyx::UdpSocket rv_sock;
  assert(rv_sock.bind("127.0.0.1", 0));
  const uint16_t rv_port = rv_sock.local_port();

  std::map<std::string, nyx::EndpointHint> registry;
  std::atomic<bool> done{false};

  std::thread rv_thread([&] {
    uint8_t buf[2048];
    while (!done.load()) {
      std::string from_host;
      uint16_t from_port = 0;
      auto pkt = rv_sock.recv_from(from_host, from_port, 100);
      if (!pkt) continue;

      auto frame = nyx::Frame::decode(pkt->data(), pkt->size());
      if (!frame) continue;

      if (frame->header.packet_type == nyx::PacketType::RendezvousRegister) {
        auto msg = nyx::RendezvousMessage::decode(frame->payload.data(),
                                                   frame->payload.size());
        if (!msg || msg->kind != nyx::RendezvousKind::Register) continue;
        registry[nyx::to_hex(msg->token.data(), msg->token.size())] = msg->hint;
      } else if (frame->header.packet_type == nyx::PacketType::RendezvousLookup) {
        auto msg = nyx::RendezvousMessage::decode(frame->payload.data(),
                                                   frame->payload.size());
        if (!msg || msg->kind != nyx::RendezvousKind::Lookup) continue;
        nyx::RendezvousMessage resp;
        const std::string key =
            nyx::to_hex(msg->token.data(), msg->token.size());
        if (registry.count(key)) {
          resp.kind = nyx::RendezvousKind::Response;
          resp.hint = registry[key];
        } else {
          resp.kind = nyx::RendezvousKind::NotFound;
        }
        auto payload = resp.encode();
        auto wire = nyx::Frame::make(nyx::PacketType::RendezvousResponse, 0, 0,
                                      payload)
                        .encode();
        rv_sock.send_to(wire, from_host, from_port);
        done.store(true);
      }
    }
  });

  nyx::UdpSocket listen_sock;
  assert(listen_sock.bind("0.0.0.0", 0));
  const uint16_t listen_port = listen_sock.local_port();

  nyx::InviteToken token{};
  nyx::random_bytes(token.data(), token.size());

  nyx::RendezvousClient register_client(std::move(listen_sock), "127.0.0.1", rv_port);
  assert(register_client.register_token(token));

  nyx::UdpSocket connect_sock;
  assert(connect_sock.bind("127.0.0.1", 0));
  nyx::RendezvousClient lookup_client(std::move(connect_sock), "127.0.0.1", rv_port);
  auto hint = lookup_client.lookup(token);
  done.store(true);
  rv_thread.join();

  assert(hint);
  assert(hint->port == listen_port);
  std::cout << "rendezvous client ok\n";
}

static void test_rendezvous_hint() {
  nyx::InviteToken token{};
  nyx::random_bytes(token.data(), token.size());
  const nyx::EndpointHint hint = nyx::make_hint("127.0.0.1", 62776);

  nyx::RendezvousMessage reg;
  reg.kind = nyx::RendezvousKind::Register;
  reg.token = token;
  reg.hint = hint;
  const auto reg_payload = reg.encode();
  const auto decoded_reg =
      nyx::RendezvousMessage::decode(reg_payload.data(), reg_payload.size());
  assert(decoded_reg);
  assert(decoded_reg->kind == nyx::RendezvousKind::Register);
  assert(decoded_reg->hint.port == 62776);
  assert(decoded_reg->hint.ip[12] == 127);

  const auto wire =
      nyx::Frame::make(nyx::PacketType::RendezvousRegister, 0, 0, reg_payload)
          .encode();
  const auto frame = nyx::Frame::decode(wire.data(), wire.size());
  assert(frame);
  const auto from_wire = nyx::RendezvousMessage::decode(frame->payload.data(),
                                                         frame->payload.size());
  assert(from_wire && from_wire->hint.port == 62776);
  std::cout << "rendezvous hint ok\n";
}

static bool is_handshake_datagram(const nyx::ByteBuffer& data) {
  return nyx::is_handshake_datagram(data);
}

static void test_node_flow() {
  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  assert(listen_sock.bind("0.0.0.0", 0));
  assert(connect_sock.bind("0.0.0.0", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread listener([&] {
    nyx::ByteBuffer first_pkt;
    std::string peer_host;
    uint16_t peer_port = 0;
    while (true) {
      auto pkt = listen_sock.recv_from(peer_host, peer_port, 1000);
      if (!pkt) continue;
      if (pkt->size() >= 10 &&
          std::memcmp(pkt->data(), "NYX-PUNCH", 10) == 0) {
        continue;
      }
      if (is_handshake_datagram(*pkt)) {
        first_pkt = std::move(*pkt);
        break;
      }
    }
    server = nyx::Connection::accept_responder(
        std::move(listen_sock), peer_host, peer_port, &first_pkt);
  });

  nyx::hole_punch(connect_sock, nyx::make_hint("127.0.0.1", listen_port));
  auto client = nyx::Connection::connect_initiator(
      std::move(connect_sock), "127.0.0.1", listen_port);
  listener.join();

  assert(client);
  assert(server);
  std::cout << "node flow ok\n";
}

static void test_udp_connection() {
  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  assert(listen_sock.bind("127.0.0.1", 0));
  assert(connect_sock.bind("127.0.0.1", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread accept_thread([&] {
    std::string host;
    uint16_t port = 0;
    auto pkt = listen_sock.recv_from(host, port, 5000);
    assert(pkt);
    server = nyx::Connection::accept_responder(
        std::move(listen_sock), host, port, &*pkt);
  });

  auto client = nyx::Connection::connect_initiator(
      std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();

  assert(client);
  assert(server);
  std::cout << "udp connection ok\n";
}

static void test_chat_echo() {
  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  assert(listen_sock.bind("127.0.0.1", 0));
  assert(connect_sock.bind("127.0.0.1", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread accept_thread([&] {
    std::string host;
    uint16_t port = 0;
    auto packet = listen_sock.recv_from(host, port, 5000);
    assert(packet);
    server = nyx::Connection::accept_responder(
        std::move(listen_sock), host, port, &*packet);
  });

  auto client = nyx::Connection::connect_initiator(
      std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  assert(client && server);

  const std::string message = "hello nyx";
  assert(client->send_payload(nyx::kChatStream, nyx::encode_text_message(message)));

  nyx::ByteBuffer received;
  uint32_t stream_id = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    server->drive();
    if (server->recv_stream(stream_id, received)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  assert(stream_id == nyx::kChatStream);
  auto decoded = nyx::decode_text_message(received);
  assert(decoded && *decoded == message);
  std::cout << "chat echo ok\n";
}

static std::optional<nyx::Connection> loopback_connect(
    std::optional<nyx::Connection>& server_out) {
  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  if (!listen_sock.bind("127.0.0.1", 0)) return std::nullopt;
  if (!connect_sock.bind("127.0.0.1", 0)) return std::nullopt;
  const uint16_t listen_port = listen_sock.local_port();

  std::thread accept_thread([&] {
    std::string host;
    uint16_t port = 0;
    auto packet = listen_sock.recv_from(host, port, 5000);
    if (!packet) return;
    server_out = nyx::Connection::accept_responder(
        std::move(listen_sock), host, port, &*packet);
  });

  auto client = nyx::Connection::connect_initiator(
      std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  if (!client || !server_out) return std::nullopt;
  return client;
}

static void pump_both(nyx::Connection& a, nyx::Connection& b, int rounds = 30) {
  for (int i = 0; i < rounds; ++i) {
    a.drive();
    b.drive();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

static void test_session_rekey() {
  nyx::set_session_rekey_byte_limit(4096);
  std::optional<nyx::Connection> server;
  auto client = loopback_connect(server);
  assert(client && server);
  assert(client->session_rekey_epoch() == 0);

  const std::string chunk(512, 'R');
  for (int i = 0; i < 30; ++i) {
    assert(client->send_payload(nyx::kChatStream, nyx::encode_text_message(chunk)));
    pump_both(*client, *server, 40);
    if (client->session_rekey_epoch() >= 1) break;
  }

  assert(client->session_rekey_epoch() >= 1);
  assert(server->session_rekey_epoch() >= 1);

  const std::string after = "after-rekey-ok";
  assert(client->send_payload(nyx::kChatStream, nyx::encode_text_message(after)));

  nyx::ByteBuffer received;
  uint32_t stream_id = 0;
  bool got = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    pump_both(*client, *server, 5);
    while (server->recv_stream(stream_id, received)) {
      if (stream_id != nyx::kChatStream) continue;
      auto decoded = nyx::decode_text_message(received);
      if (decoded && *decoded == after) got = true;
    }
    if (got) break;
  }
  assert(got);
  nyx::set_session_rekey_byte_limit(0);
  std::cout << "session rekey ok (epoch " << client->session_rekey_epoch() << ")\n";
}

static void test_hello_roundtrip() {
  nyx::HelloMessage msg;
  msg.nickname = "Alice";
  nyx::random_bytes(msg.public_key.data(), msg.public_key.size());
  msg.capabilities = 1;
  auto wire = msg.encode();
  auto decoded = nyx::HelloMessage::decode(wire);
  assert(decoded);
  assert(decoded->nickname == msg.nickname);
  assert(decoded->public_key == msg.public_key);
  assert(decoded->capabilities == msg.capabilities);
  std::cout << "hello roundtrip ok\n";
}

static void test_profile_save_load() {
  const std::string path = "test_profile_phase2.json";
  nyx::Profile original = nyx::generate_profile("test-user");
  assert(nyx::save_profile(path, original));
  nyx::Profile loaded;
  assert(nyx::load_profile(path, loaded));
  assert(loaded.nickname == original.nickname);
  assert(loaded.public_key == original.public_key);
  assert(loaded.secret_key == original.secret_key);
  std::remove(path.c_str());
  std::cout << "profile save load ok\n";
}

static void test_hello_exchange() {
  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  assert(listen_sock.bind("127.0.0.1", 0));
  assert(connect_sock.bind("127.0.0.1", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread accept_thread([&] {
    std::string host;
    uint16_t port = 0;
    auto packet = listen_sock.recv_from(host, port, 5000);
    assert(packet);
    server = nyx::Connection::accept_responder(
        std::move(listen_sock), host, port, &*packet);
  });

  auto client = nyx::Connection::connect_initiator(
      std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  assert(client && server);

  nyx::Profile alice = nyx::generate_profile("Alice");
  nyx::Profile bob = nyx::generate_profile("Bob");

  nyx::HelloMessage hello_a;
  hello_a.public_key = alice.public_key;
  hello_a.nickname = alice.nickname;
  nyx::HelloMessage hello_b;
  hello_b.public_key = bob.public_key;
  hello_b.nickname = bob.nickname;

  assert(client->send_payload(nyx::kChatStream, hello_b.encode()));
  assert(server->send_payload(nyx::kChatStream, hello_a.encode()));

  nyx::HelloMessage got_on_server;
  nyx::HelloMessage got_on_client;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline &&
         (got_on_server.nickname.empty() || got_on_client.nickname.empty())) {
    client->drive();
    server->drive();
    nyx::ByteBuffer payload;
    uint32_t stream_id = 0;
    while (server->recv_stream(stream_id, payload)) {
      if (auto h = nyx::decode_hello_message(payload)) got_on_server = *h;
    }
    while (client->recv_stream(stream_id, payload)) {
      if (auto h = nyx::decode_hello_message(payload)) got_on_client = *h;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  assert(got_on_server.nickname == "Bob");
  assert(got_on_client.nickname == "Alice");
  std::cout << "hello exchange ok\n";
}

static void test_chat_message_roundtrip() {
  nyx::ChatMessage msg;
  msg.id = 42;
  msg.timestamp_ms = 1'700'000'000'000ULL;
  msg.author = "Alice";
  msg.text = "hello phase 3";
  nyx::random_bytes(msg.author_id.data(), msg.author_id.size());
  nyx::UserId peer{};
  nyx::random_bytes(peer.data(), peer.size());
  msg.chat_id = nyx::dm_chat_id(msg.author_id, peer);

  auto wire = msg.encode();
  assert(wire[0] == static_cast<uint8_t>(nyx::ChatKind::MsgV2));
  auto decoded = nyx::ChatMessage::decode(wire);
  assert(decoded);
  assert(decoded->id == msg.id);
  assert(decoded->timestamp_ms == msg.timestamp_ms);
  assert(decoded->author == msg.author);
  assert(decoded->text == msg.text);
  assert(decoded->author_id == msg.author_id);
  assert(decoded->chat_id == msg.chat_id);
  std::cout << "chat message roundtrip ok\n";
}

static void test_dm_chat_id() {
  nyx::UserId a{};
  nyx::UserId b{};
  nyx::random_bytes(a.data(), a.size());
  nyx::random_bytes(b.data(), b.size());
  assert(nyx::dm_chat_id(a, b) == nyx::dm_chat_id(b, a));
  std::cout << "dm chat id ok\n";
}

static void test_message_store() {
  const std::string path = "test_chat_history.jsonl";
  std::remove(path.c_str());

  nyx::MessageStore store(path);
  nyx::StoredMessage msg;
  msg.id = 1;
  msg.timestamp_ms = nyx::now_ms();
  msg.author = "Bob";
  msg.text = "stored";
  msg.outgoing = true;
  assert(store.append(msg));

  nyx::MessageStore reload(path);
  const auto recent = reload.recent(10);
  assert(recent.size() == 1);
  assert(recent[0].text == "stored");
  std::remove(path.c_str());
  std::cout << "message store ok\n";
}

static void test_chat_msg_exchange() {
  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  assert(listen_sock.bind("127.0.0.1", 0));
  assert(connect_sock.bind("127.0.0.1", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread accept_thread([&] {
    std::string host;
    uint16_t port = 0;
    auto packet = listen_sock.recv_from(host, port, 5000);
    assert(packet);
    server = nyx::Connection::accept_responder(
        std::move(listen_sock), host, port, &*packet);
  });

  auto client = nyx::Connection::connect_initiator(
      std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  assert(client && server);

  nyx::Profile sender = nyx::generate_profile("Sender");
  nyx::UserId peer{};
  nyx::random_bytes(peer.data(), peer.size());
  nyx::ChatMessage out;
  out.id = nyx::next_message_id();
  out.timestamp_ms = nyx::now_ms();
  out.author_id = sender.public_key;
  out.author = sender.nickname;
  out.chat_id = nyx::dm_chat_id(sender.public_key, peer);
  out.text = "phase3";
  assert(client->send_payload(nyx::kChatStream, out.encode()));

  nyx::ChatMessage received;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    server->drive();
    nyx::ByteBuffer payload;
    uint32_t stream_id = 0;
    if (server->recv_stream(stream_id, payload)) {
      if (auto decoded = nyx::ChatMessage::decode(payload)) {
        received = *decoded;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  assert(received.text == "phase3");
  assert(received.author == "Sender");
  std::cout << "chat msg exchange ok\n";
}

static std::optional<nyx::Connection> accept_loopback(nyx::UdpSocket& listen_sock) {
  std::string host;
  uint16_t port = 0;
  auto packet = listen_sock.recv_from(host, port, 5000);
  if (!packet) return std::nullopt;
  return nyx::Connection::accept_responder(std::move(listen_sock), host, port,
                                            &*packet);
}

static void test_ten_messages_roundtrip() {
  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  assert(listen_sock.bind("127.0.0.1", 0));
  assert(connect_sock.bind("127.0.0.1", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread accept_thread([&] {
    server = accept_loopback(listen_sock);
  });

  auto client = nyx::Connection::connect_initiator(
      std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  assert(client && server);

  nyx::Profile alice = nyx::generate_profile("Alice");
  nyx::Profile bob = nyx::generate_profile("Bob");

  nyx::ChatService::PeerInfo peer_for_alice{bob.public_key, bob.nickname};
  nyx::ChatService::PeerInfo peer_for_bob{alice.public_key, alice.nickname};
  nyx::ChatService chat_a(*client, alice, peer_for_alice);
  nyx::ChatService chat_b(*server, bob, peer_for_bob);

  std::vector<std::string> got_on_b;

  chat_b.set_on_message([&](const nyx::ChatMessage& msg, bool outgoing) {
    if (!outgoing) got_on_b.push_back(msg.text);
  });

  for (int i = 0; i < 10; ++i) {
    const std::string text = "msg-" + std::to_string(i);
    assert(chat_a.send_message(text));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      chat_a.tick();
      chat_b.tick();
      nyx::ByteBuffer payload;
      uint32_t sid = 0;
      while (client->recv_stream(sid, payload)) {
        if (sid == nyx::kChatStream) chat_a.handle_payload(payload);
      }
      while (server->recv_stream(sid, payload)) {
        if (sid == nyx::kChatStream) chat_b.handle_payload(payload);
      }
      if (static_cast<int>(got_on_b.size()) > i) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  assert(got_on_b.size() == 10);
  for (int i = 0; i < 10; ++i) {
    assert(got_on_b[static_cast<std::size_t>(i)] == "msg-" + std::to_string(i));
  }
  std::cout << "ten messages roundtrip ok\n";
}

static void test_file_index_three() {
  const std::string dir = "test_index_dir";
  std::filesystem::remove_all(dir);
  std::remove(nyx::FileIndex::index_path().c_str());
  std::filesystem::create_directories(dir);
  std::ofstream(dir + "/a.txt") << "aaa";
  std::ofstream(dir + "/b.txt") << "bbb";
  std::ofstream(dir + "/c.txt") << "ccc";

  nyx::FileIndex index;
  assert(index.add_root(dir));
  assert(index.entries().size() == 3);

  std::filesystem::create_directories(dir + "/sub");
  std::ofstream(dir + "/sub/nested.txt") << "n";
  assert(index.rescan_root(dir));
  const auto level = nyx::FileIndex::listing_level(index.entries_for_session({}), dir, "");
  assert(level.size() >= 2);
  bool has_sub = false;
  for (const auto& e : level) {
    if (e.is_directory() && e.relative_path == "sub") has_sub = true;
  }
  assert(has_sub);

  // Кэш уровня с peer: маркеры папок + leaf-имена файлов (как после старого ListResp).
  // Повторный listing_level не должен выкидывать ни папки, ни файлы.
  {
    std::vector<nyx::FileEntry> wire_level;
    for (const auto& e : level) {
      nyx::FileEntry w = e;
      if (!w.is_directory()) {
        // Имитация старого бага на проводе: только leaf.
        w.relative_path = w.leaf_name();
      }
      wire_level.push_back(std::move(w));
    }
    const auto again = nyx::FileIndex::listing_level(wire_level, dir, "");
    bool has_sub_again = false;
    int files_again = 0;
    for (const auto& e : again) {
      if (e.is_directory() && e.relative_path == "sub") has_sub_again = true;
      if (!e.is_directory()) ++files_again;
    }
    assert(has_sub_again);
    assert(files_again >= 3);

    // Уровень внутри sub: leaf-файл nested.txt не должен пропасть.
    std::vector<nyx::FileEntry> nested_wire;
    for (const auto& e : index.entries_for_session({})) {
      if (e.relative_path.find("sub/") == 0) {
        nyx::FileEntry w = e;
        w.relative_path = w.leaf_name();
        nested_wire.push_back(std::move(w));
      }
    }
    nyx::FileEntry sub_marker;
    sub_marker.root_path = nyx::normalize_utf8_path(dir);
    sub_marker.relative_path = "sub";
    sub_marker.mime = "application/x-nyx-directory";
    nested_wire.push_back(sub_marker);
    const auto nested_level =
        nyx::FileIndex::listing_level(nested_wire, dir, "sub");
    bool has_nested_file = false;
    for (const auto& e : nested_level) {
      if (!e.is_directory() && e.leaf_name() == "nested.txt") has_nested_file = true;
    }
    assert(has_nested_file);
  }

  assert(index.remove_root(dir));
  assert(index.entries().empty());
  assert(index.share_roots().empty());
  assert(index.listing_for_session({}).empty());

  // Повторное добавление после удаления не должно ломаться.
  assert(index.add_root(dir));
  assert(index.entries().size() == 4);
  assert(index.remove_root(dir));

  std::filesystem::remove_all(dir);
  std::remove(nyx::FileIndex::index_path().c_str());
  std::cout << "file index three ok\n";
}

static void test_list_response_size_cap() {
  std::vector<nyx::FileEntry> entries;
  entries.reserve(2000);
  for (int i = 0; i < 2000; ++i) {
    nyx::FileEntry e;
    e.root_path = "C:/very/long/share/root/path/for/noise/limit/testing/thrust";
    e.relative_path = "pkg_" + std::to_string(i) + "/nested/name.js";
    e.mime = "application/javascript";
    e.size = static_cast<uint64_t>(i);
    e.hash = nyx::hash_bytes(reinterpret_cast<const uint8_t*>(e.relative_path.data()),
                             e.relative_path.size());
    if (i % 5 == 0) e.mime = "application/x-nyx-directory";
    entries.push_back(std::move(e));
  }
  const auto wire = nyx::encode_list_response(entries);
  assert(wire.size() <= 48000);
  assert(wire.size() > 3);
  auto decoded = nyx::decode_list_response(wire);
  assert(decoded);
  assert(!decoded->empty());
  assert(decoded->size() < entries.size());
  // Папки кодируются раньше файлов — в урезанном ответе должны быть directory-маркеры.
  bool has_dir = false;
  for (const auto& e : *decoded) {
    if (e.is_directory()) {
      has_dir = true;
      break;
    }
  }
  assert(has_dir);
  std::cout << "list response size cap ok (" << decoded->size() << " entries, "
            << wire.size() << " bytes)\n";
}

#ifdef _WIN32
static void test_file_index_unicode() {
  const std::wstring wdir = L"test_index_unicode";
  const std::wstring wfile = wdir + L"\\" + L"\u043A\u043F\u044B\u0432\u0430.txt";
  std::filesystem::remove_all(wdir);
  std::remove(nyx::FileIndex::index_path().c_str());
  std::filesystem::create_directories(wdir);
  {
    std::ofstream out(std::filesystem::path(wfile), std::ios::binary);
    out << "unicode ok";
  }

  const std::string dir = nyx::path_to_utf8(wdir);
  nyx::FileIndex index;
  assert(index.add_root(dir));
  assert(index.entries().size() == 1);
  assert(index.entries()[0].relative_path == nyx::path_to_utf8(std::filesystem::path(L"\u043A\u043F\u044B\u0432\u0430.txt")));

  nyx::FileHash hash{};
  assert(nyx::hash_file(index.entries()[0].absolute_path(), hash));
  assert(hash == index.entries()[0].hash);

  std::filesystem::remove_all(wdir);
  std::remove(nyx::FileIndex::index_path().c_str());
  std::cout << "file index unicode ok\n";
}
#endif

static void test_file_transfer_1mb() {
  const std::string src_dir = "test_xfer_src";
  const std::string dl_dir = "test_xfer_dl";
  std::filesystem::remove_all(src_dir);
  std::filesystem::remove_all(dl_dir);
  std::remove(nyx::FileIndex::index_path().c_str());
  std::filesystem::create_directories(src_dir);
  std::filesystem::create_directories(dl_dir);

  const std::string src_path = src_dir + "/payload.bin";
  {
    std::ofstream out(src_path, std::ios::binary);
    std::vector<char> chunk(8192);
    for (int i = 0; i < 128; ++i) {
      std::fill(chunk.begin(), chunk.end(), static_cast<char>(i));
      out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
  }

  nyx::FileHash expected{};
  assert(nyx::hash_file(src_path, expected));

  nyx::FileIndex server_index;
  assert(server_index.add_root(src_dir));

  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  assert(listen_sock.bind("127.0.0.1", 0));
  assert(connect_sock.bind("127.0.0.1", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread accept_thread([&] {
    std::string host;
    uint16_t port = 0;
    auto packet = listen_sock.recv_from(host, port, 5000);
    assert(packet);
    server = nyx::Connection::accept_responder(
        std::move(listen_sock), host, port, &*packet);
  });

  auto client = nyx::Connection::connect_initiator(
      std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  assert(client && server);

  nyx::FileIndex client_index;
  nyx::FileTransferService fs_server(*server, server_index, dl_dir + "/srv");
  nyx::FileTransferService fs_client(*client, client_index, dl_dir);

  assert(fs_client.request_file(nyx::hash_hex(expected)));

  bool done = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && !done) {
    client->drive();
    server->drive();
    fs_server.pump();
    fs_client.pump();

    nyx::ByteBuffer payload;
    uint32_t stream_id = 0;
    while (client->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kBulkStream) fs_client.handle_bulk(payload);
    }
    while (server->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kBulkStream) fs_server.handle_bulk(payload);
    }

    const std::string dest = dl_dir + "/payload.bin";
    std::error_code ec;
    if (std::filesystem::exists(dest, ec)) {
      nyx::FileHash got{};
      if (nyx::hash_file(dest, got) && got == expected) done = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  assert(done);
  std::filesystem::remove_all(src_dir);
  std::filesystem::remove_all(dl_dir);
  std::remove(nyx::FileIndex::index_path().c_str());
  std::cout << "file transfer 1mb ok\n";
}

static bool test_exchange_hello(nyx::Connection& connection, const nyx::Profile& profile) {
  nyx::HelloMessage hello;
  hello.public_key = profile.public_key;
  hello.nickname = profile.nickname;
  if (!connection.send_payload(nyx::kChatStream, hello.encode())) return false;

  for (int i = 0; i < 500; ++i) {
    connection.drive();
    nyx::ByteBuffer payload;
    uint32_t stream_id = 0;
    while (connection.recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kChatStream && nyx::decode_hello_message(payload)) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

static void test_group_chat_id() {
  nyx::GroupId gid{};
  nyx::random_bytes(gid.data(), gid.size());
  assert(nyx::group_chat_id(gid) != nyx::ChatId{});
  std::cout << "group chat id ok\n";
}

static void test_group_three_members() {
  std::remove(nyx::GroupStore::store_path().c_str());

  const std::string alice_dir = nyx::data_root() + "/test_group3_alice";
  const std::string bob_dir = nyx::data_root() + "/test_group3_bob";
  const std::string charlie_dir = nyx::data_root() + "/test_group3_charlie";
  std::filesystem::remove_all(alice_dir);
  std::filesystem::remove_all(bob_dir);
  std::filesystem::remove_all(charlie_dir);
  std::filesystem::create_directories(alice_dir);
  std::filesystem::create_directories(bob_dir);
  std::filesystem::create_directories(charlie_dir);

  nyx::set_account_data_dir(alice_dir);
  nyx::Profile alice = nyx::generate_profile("Alice");
  nyx::Profile bob = nyx::generate_profile("Bob");
  nyx::Profile charlie = nyx::generate_profile("Charlie");

  nyx::GroupStore store;
  const nyx::GroupRecord group = store.create("test-field", alice.user_id(), "Alice");

  nyx::UdpSocket hub_sock;
  assert(hub_sock.bind("127.0.0.1", 0));
  const uint16_t hub_port = hub_sock.local_port();

  nyx::GroupHub hub(hub_sock, alice, group);

  std::atomic<bool> hub_running{true};
  std::thread hub_thread([&] {
    while (hub_running.load()) {
      hub.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  auto connect_member = [&](const nyx::Profile& profile) {
    (void)profile;
    nyx::UdpSocket sock;
    assert(sock.bind("127.0.0.1", 0));
    return nyx::Connection::connect_initiator(std::move(sock), "127.0.0.1", hub_port);
  };

  auto bob_conn = connect_member(bob);
  auto charlie_conn = connect_member(charlie);
  assert(bob_conn && charlie_conn);

  assert(test_exchange_hello(*bob_conn, bob));
  assert(test_exchange_hello(*charlie_conn, charlie));

  nyx::GroupId zero{};
  // У каждого участника свой data_dir — иначе общий groups/*.jsonl ломает дедуп id.
  nyx::set_account_data_dir(bob_dir);
  nyx::GroupMemberService bob_svc(*bob_conn, bob, zero, "");
  nyx::set_account_data_dir(charlie_dir);
  nyx::GroupMemberService charlie_svc(*charlie_conn, charlie, zero, "");

  std::vector<std::string> bob_got;
  std::vector<std::string> charlie_got;
  bob_svc.set_on_message([&](const nyx::ChatMessage& m, bool outgoing) {
    if (!outgoing) bob_got.push_back(m.text);
  });
  charlie_svc.set_on_message([&](const nyx::ChatMessage& m, bool outgoing) {
    if (!outgoing) charlie_got.push_back(m.text);
  });

  nyx::set_account_data_dir(bob_dir);
  assert(bob_svc.join(15000));
  nyx::set_account_data_dir(charlie_dir);
  assert(charlie_svc.join(15000));

  nyx::set_account_data_dir(bob_dir);
  assert(bob_svc.send_message("from-bob"));
  nyx::set_account_data_dir(charlie_dir);
  assert(charlie_svc.send_message("from-charlie"));
  hub.send_message("from-alice");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline) {
    bob_conn->drive();
    charlie_conn->drive();
    nyx::ByteBuffer payload;
    uint32_t stream_id = 0;
    while (bob_conn->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kChatStream) bob_svc.handle_payload(payload);
    }
    while (charlie_conn->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kChatStream) charlie_svc.handle_payload(payload);
    }
    if (bob_got.size() >= 2 && charlie_got.size() >= 2) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  hub_running.store(false);
  hub_thread.join();

  assert(bob_got.size() >= 2);
  assert(charlie_got.size() >= 2);
  assert(std::find(bob_got.begin(), bob_got.end(), "from-charlie") != bob_got.end());
  assert(std::find(bob_got.begin(), bob_got.end(), "from-alice") != bob_got.end());
  assert(std::find(charlie_got.begin(), charlie_got.end(), "from-bob") != charlie_got.end());
  assert(std::find(charlie_got.begin(), charlie_got.end(), "from-alice") != charlie_got.end());

  nyx::clear_account_data_dir();
  std::remove(nyx::GroupStore::store_path().c_str());
  std::filesystem::remove_all(alice_dir);
  std::filesystem::remove_all(bob_dir);
  std::filesystem::remove_all(charlie_dir);
  std::cout << "group three members ok\n";
}

static void test_mdns_beacon_roundtrip() {
  const nyx::Profile profile = nyx::generate_profile("Test Peer");

  nyx::UdpSocket sock;
  assert(sock.bind("0.0.0.0", 0));
  assert(nyx::MdnsLan::send_announcement(sock, profile, sock.local_port(), "192.168.1.42"));

  nyx::ByteBuffer wire;
  wire.insert(wire.end(), {'N', 'Y', 'X', '1'});
  nyx::write_u16_le(wire, sock.local_port());
  const std::string instance = "Test-Peer";
  const std::string id_short = nyx::short_user_id(profile.user_id());
  nyx::write_u16_le(wire, static_cast<uint16_t>(instance.size()));
  nyx::write_u16_le(wire, static_cast<uint16_t>(id_short.size()));
  nyx::write_u32_le(wire, 0x2a01a8c0);  // 192.168.1.42 LE
  wire.insert(wire.end(), instance.begin(), instance.end());
  wire.insert(wire.end(), id_short.begin(), id_short.end());

  const auto parsed = nyx::MdnsLan::parse_beacon(wire, "10.0.0.1");
  assert(parsed);
  assert(parsed->instance == instance);
  assert(parsed->user_id_short == id_short);
  assert(parsed->port == sock.local_port());
  assert(parsed->host == "192.168.1.42");
  std::cout << "mdns beacon roundtrip ok\n";
}

static void test_mdns_browse_receives_beacon() {
  const nyx::Profile profile = nyx::generate_profile("LanBrowse");

  nyx::UdpSocket advert;
  assert(advert.bind("0.0.0.0", 0));

  std::atomic<bool> stop{false};
  std::thread sender([&]() {
    while (!stop.load()) {
      nyx::MdnsLan::send_announcement(advert, profile, advert.local_port(), "192.168.50.10");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  nyx::UdpSocket browse;
  assert(nyx::MdnsLan::setup_socket(browse));
  const auto peers = nyx::MdnsLan::browse(browse, 1500);
  stop.store(true);
  sender.join();

  assert(!peers.empty());
  assert(peers.front().instance == "LanBrowse");
  assert(peers.front().port == advert.local_port());
  assert(peers.front().host == "192.168.50.10");
  std::cout << "mdns browse receives beacon ok\n";
}

static void test_guess_lan_ipv4() {
  const auto ip = nyx::guess_lan_ipv4();
  assert(!ip.empty());
  std::cout << "guess lan ipv4 ok (" << ip << ")\n";
}

static void test_is_lan_ipv4() {
  assert(nyx::is_lan_ipv4("127.0.0.1"));
  assert(nyx::is_lan_ipv4("192.168.1.5"));
  assert(nyx::is_lan_ipv4("10.0.0.1"));
  assert(!nyx::is_lan_ipv4("8.8.8.8"));
  std::cout << "is lan ipv4 ok\n";
}

static void test_group_member_persistence() {
  std::remove(nyx::GroupStore::store_path().c_str());
  nyx::Profile owner = nyx::generate_profile("Owner");
  nyx::GroupStore store;
  auto group = store.create("Q", owner.user_id(), owner.nickname);
  nyx::UserId member{};
  nyx::random_bytes(member.data(), member.size());
  group.members.push_back({member, "Test", nyx::GroupRole::Member});
  assert(store.upsert(group));

  nyx::GroupStore reloaded;
  assert(reloaded.load());
  const auto found = reloaded.find(group.id);
  assert(found);
  assert(found->members.size() == 2);
  bool has_test = false;
  bool has_owner = false;
  for (const auto& m : found->members) {
    if (m.user_id == member) has_test = true;
    if (m.role == nyx::GroupRole::Owner) has_owner = true;
  }
  assert(has_test);
  assert(has_owner);
  std::cout << "group member persistence ok\n";
}

static void test_profile_meta_photos_wire() {
  nyx::ProfileMeta meta;
  meta.bio = "hi";
  meta.interests = "x";
  meta.availability = nyx::Availability::Away;
  meta.updated_ms = 42;
  nyx::FileHash h{};
  h[0] = 0xab;
  h[31] = 0xcd;
  meta.photo_hashes.push_back(h);
  nyx::ByteBuffer wire;
  nyx::append_profile_meta_wire(wire, meta);
  nyx::ProfileMeta out;
  std::size_t off = 0;
  assert(nyx::read_profile_meta_wire(wire, off, out));
  assert(out.bio == "hi");
  assert(out.availability == nyx::Availability::Away);
  assert(out.updated_ms == 42);
  assert(out.photo_hashes.size() == 1);
  assert(out.photo_hashes[0] == h);

  // Старый кадр без хвоста фото.
  nyx::ByteBuffer legacy;
  nyx::write_u16_le(legacy, 2);
  legacy.push_back('o');
  legacy.push_back('k');
  nyx::write_u16_le(legacy, 0);
  legacy.push_back(0);
  nyx::ProfileMeta legacy_out;
  std::size_t loff = 0;
  assert(nyx::read_profile_meta_wire(legacy, loff, legacy_out));
  assert(legacy_out.bio == "ok");
  assert(legacy_out.photo_hashes.empty());
  std::cout << "profile meta photos wire ok\n";
}

static void test_avatar_proto_roundtrip() {
  nyx::FileHash h{};
  h[0] = 1;
  nyx::AvatarRequest req;
  req.hash = h;
  const auto req_w = req.encode();
  assert(nyx::is_avatar_frame(req_w));
  assert(!nyx::ByeMessage::decode(req_w));
  auto req_d = nyx::AvatarRequest::decode(req_w);
  assert(req_d && req_d->hash == h);

  nyx::AvatarOffer offer;
  offer.hash = h;
  offer.size = 3;
  offer.mime = "image/jpeg";
  auto offer_d = nyx::AvatarOffer::decode(offer.encode());
  assert(offer_d && offer_d->size == 3);

  nyx::AvatarChunk chunk;
  chunk.hash = h;
  chunk.index = 0;
  chunk.data = {9, 8, 7};
  auto chunk_d = nyx::AvatarChunk::decode(chunk.encode());
  assert(chunk_d && chunk_d->data.size() == 3 && chunk_d->data[0] == 9);

  nyx::AvatarDone done;
  done.hash = h;
  assert(nyx::AvatarDone::decode(done.encode()));
  std::cout << "avatar proto roundtrip ok\n";
}

static void test_markdown_to_html() {
  const auto bold = nyx::markdown_to_html("**hi**");
  assert(bold.find("<b>hi</b>") != std::string::npos);

  const auto under = nyx::markdown_to_html("__u__");
  assert(under.find("<u>u</u>") != std::string::npos);

  const auto strike = nyx::markdown_to_html("~~x~~");
  assert(strike.find("<s>x</s>") != std::string::npos);

  const auto spoil = nyx::markdown_to_html("||secret||");
  assert(spoil.find("nyx-spoiler:0") != std::string::npos);
  assert(spoil.find("secret") != std::string::npos);

  const auto revealed = nyx::markdown_to_html("||secret||", {0});
  assert(revealed.find("nyx-spoiler:") == std::string::npos);
  assert(revealed.find("<span") != std::string::npos);

  const auto code = nyx::markdown_to_html("`a<b>`");
  assert(code.find("<code") != std::string::npos);
  assert(code.find("&lt;") != std::string::npos);

  const auto link = nyx::markdown_to_html("[t](https://example.com)");
  assert(link.find("href=\"https://example.com\"") != std::string::npos);

  const auto quote = nyx::markdown_to_html("> hi");
  assert(quote.find("<blockquote") != std::string::npos);
  assert(quote.find("hi") != std::string::npos);

  const auto h1 = nyx::markdown_to_html("# Title");
  assert(h1.find("Title") != std::string::npos);
  assert(h1.find("font-weight:700") != std::string::npos);

  const auto ul = nyx::markdown_to_html("- one\n- two");
  assert(ul.find("<ul") != std::string::npos);
  assert(ul.find("<li>one</li>") != std::string::npos);

  const auto table = nyx::table_to_html("| a | b |\n| --- | --- |\n| 1 | 2 |");
  assert(table.find("<table") != std::string::npos);
  assert(table.find("<th") != std::string::npos);

  const auto math = nyx::formula_to_html("a^2 + \\pi");
  assert(math.find("<sup>2</sup>") != std::string::npos);
  assert(math.find("π") != std::string::npos);

  const auto mention = nyx::markdown_to_html(
      "[@bob](nyx-user:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef)");
  assert(mention.find("nyx-user:") != std::string::npos);

  assert(nyx::normalize_me_message("/me jumps") == "nyx-me:jumps");
  assert(nyx::is_action_message("nyx-me:jumps"));
  assert(nyx::action_message_body("nyx-me:jumps") == "jumps");

  const auto blocks = nyx::parse_markdown_blocks(
      "hi\n\n![pic](nyx-media:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef)\n\n$$x^2$$");
  assert(blocks.size() >= 3);
  bool saw_media = false;
  bool saw_formula = false;
  for (const auto& b : blocks) {
    if (b.type == nyx::MdBlockType::Media) saw_media = true;
    if (b.type == nyx::MdBlockType::Formula) saw_formula = true;
  }
  assert(saw_media && saw_formula);

  std::cout << "markdown to html ok\n";
}

static void test_group_meta_message() {
  nyx::GroupMetaMessage msg;
  msg.description = "desc";
  msg.direction = "dir";
  msg.tags = "a, b";
  msg.visibility = nyx::GroupVisibility::PublicListed;
  const auto wire = msg.encode();
  assert(nyx::is_group_frame(wire));
  assert(wire[0] != static_cast<uint8_t>(nyx::ChatKind::Bye));
  assert(!nyx::ByeMessage::decode(wire));
  const auto decoded = nyx::GroupMetaMessage::decode(wire);
  assert(decoded);
  assert(decoded->description == "desc");
  assert(decoded->direction == "dir");
  assert(decoded->tags == "a, b");
  assert(decoded->visibility == nyx::GroupVisibility::PublicListed);

  // Пустая мета тоже не должна читаться как Bye.
  nyx::GroupMetaMessage empty;
  const auto empty_wire = empty.encode();
  assert(!nyx::ByeMessage::decode(empty_wire));
  assert(nyx::GroupMetaMessage::decode(empty_wire));

  nyx::ByeMessage bye;
  bye.reason = "эфир закрыт";
  const auto bye_wire = bye.encode();
  assert(nyx::ByeMessage::decode(bye_wire));
  assert(!nyx::GroupMetaMessage::decode(bye_wire));
  std::cout << "group meta message ok\n";
}

static void test_file_access_roles() {
  std::remove(nyx::FileAccessStore::store_path().c_str());
  nyx::GroupId gid{};
  nyx::random_bytes(gid.data(), gid.size());
  nyx::GroupRecord group;
  group.id = gid;
  group.name = "test-field";
  nyx::UserId owner{};
  nyx::random_bytes(owner.data(), owner.size());
  group.owner_id = owner;
  nyx::GroupMemberRecord om;
  om.user_id = owner;
  om.nickname = "owner";
  om.role = nyx::GroupRole::Owner;
  group.members.push_back(om);

  nyx::FileAccessStore store;
  auto& policy = store.ensure_policy(gid, group);
  assert(policy.roles.size() >= 3);
  assert(store.has_permission(store.permissions_for(gid, owner),
                              nyx::FilePermission::ManageRoles));

  nyx::UserId member{};
  nyx::random_bytes(member.data(), member.size());
  assert(store.set_member_role(gid, member, nyx::FileAccessStore::role_id_viewer()));
  assert(store.has_permission(store.permissions_for(gid, member), nyx::FilePermission::List));
  assert(!store.has_permission(store.permissions_for(gid, member), nyx::FilePermission::Upload));

  nyx::FilePermissionPreset preset;
  preset.id = "preset_test";
  preset.name = "Редактор";
  preset.permissions = static_cast<uint32_t>(nyx::FilePermission::List) |
                       static_cast<uint32_t>(nyx::FilePermission::Upload);
  assert(store.upsert_permission_preset(gid, preset));

  nyx::FileRole custom;
  custom.id = "role_editor";
  custom.name = "Редактор";
  custom.permissions = preset.permissions;
  assert(store.upsert_role(gid, custom));

  assert(store.save());

  nyx::FileAccessStore reloaded;
  assert(reloaded.load());
  const auto* loaded = reloaded.find_policy(gid);
  assert(loaded);
  assert(loaded->permission_presets.size() == 1);
  assert(loaded->permission_presets[0].id == "preset_test");
  assert(loaded->roles.size() >= 4);
  bool found_custom = false;
  for (const auto& role : loaded->roles) {
    if (role.id == "role_editor") {
      found_custom = true;
      assert(role.permissions == preset.permissions);
    }
  }
  assert(found_custom);

  assert(store.set_path_member_role(gid, "C:/Share", "StratumD", member,
                                    nyx::FileAccessStore::role_id_viewer()));
  assert(store.save());
  nyx::FileAccessStore grants_reloaded;
  assert(grants_reloaded.load());
  const auto* gp = grants_reloaded.find_policy(gid);
  assert(gp);
  assert(gp->root_grants.size() == 1);
  assert(gp->root_grants[0].relative_path == "StratumD");

  assert(store.set_root_member_role(gid, "C:/Share", member,
                                    nyx::FileAccessStore::role_id_viewer()));
  assert(store.save());
  const auto* synced = store.find_policy(gid);
  assert(synced);
  const auto wire = nyx::encode_policy_push(*synced);
  const auto decoded = nyx::decode_policy_push(wire);
  assert(decoded);
  assert(decoded->root_grants.size() == 2);

  assert(store.set_path_member_role(gid, "C:/Share", "StratumD", member, "role_editor"));
  const std::string indexed_root = "C:/Share/StratumD";
  assert(store.has_permission(store.permissions_for(gid, member, indexed_root, "documentation"),
                              nyx::FilePermission::Upload));
  assert(store.has_permission(store.permissions_for(gid, member, indexed_root, "installer"),
                              nyx::FilePermission::Upload));
  assert(!store.has_permission(store.permissions_for(gid, member, indexed_root, "documentation"),
                               nyx::FilePermission::Download));

  std::cout << "file access roles ok\n";
}

static void test_share_policy() {
  std::remove(nyx::FileIndex::index_path().c_str());
  const std::string personal_dir = "test_share_personal";
  const std::string group_dir = "test_share_group";
  const std::string dl_dir = "test_share_dl";
  std::filesystem::remove_all(personal_dir);
  std::filesystem::remove_all(group_dir);
  std::filesystem::remove_all(dl_dir);
  std::filesystem::create_directories(personal_dir);
  std::filesystem::create_directories(group_dir);
  std::filesystem::create_directories(dl_dir);
  std::ofstream(personal_dir + "/personal.txt") << "personal-only";
  std::ofstream(group_dir + "/group.txt") << "group-only";

  nyx::GroupId gid{};
  nyx::random_bytes(gid.data(), gid.size());
  nyx::GroupId zero{};

  nyx::FileIndex index;
  assert(index.add_root(personal_dir));
  assert(index.add_root(group_dir, &gid));
  assert(index.entries().size() == 2);
  assert(index.entries_for_session(zero).size() == 1);
  assert(index.entries_for_session(gid).size() == 1);
  assert(index.entries_for_session(zero)[0].display_name() == "personal.txt");
  assert(index.entries_for_session(gid)[0].display_name() == "group.txt");

  const auto personal_hash = index.entries_for_session(zero)[0].hash;
  const auto group_hash = index.entries_for_session(gid)[0].hash;
  assert(!index.find_for_session(personal_hash, gid));
  assert(!index.find_for_session(group_hash, zero));
  assert(index.find_for_session(group_hash, gid));

  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  assert(listen_sock.bind("127.0.0.1", 0));
  assert(connect_sock.bind("127.0.0.1", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread accept_thread([&] {
    std::string host;
    uint16_t port = 0;
    auto packet = listen_sock.recv_from(host, port, 5000);
    assert(packet);
    server = nyx::Connection::accept_responder(
        std::move(listen_sock), host, port, &*packet);
  });

  auto client = nyx::Connection::connect_initiator(
      std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  assert(client && server);

  nyx::FileIndex client_index;
  nyx::FileTransferService fs_server(*server, index, dl_dir + "/srv");
  nyx::FileTransferService fs_client(*client, client_index, dl_dir);
  fs_server.set_share_scope(gid);

  assert(fs_client.request_file(nyx::hash_hex(personal_hash)));
  const auto deny_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deny_deadline) {
    client->drive();
    server->drive();
    fs_server.pump();
    fs_client.pump();
    nyx::ByteBuffer payload;
    uint32_t stream_id = 0;
    while (client->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kBulkStream) fs_client.handle_bulk(payload);
    }
    while (server->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kBulkStream) fs_server.handle_bulk(payload);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::error_code ec;
  assert(!std::filesystem::exists(dl_dir + "/personal.txt", ec));

  assert(fs_client.request_file(nyx::hash_hex(group_hash)));
  bool got_group = false;
  const auto ok_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < ok_deadline && !got_group) {
    client->drive();
    server->drive();
    fs_server.pump();
    fs_client.pump();
    nyx::ByteBuffer payload;
    uint32_t stream_id = 0;
    while (client->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kBulkStream) fs_client.handle_bulk(payload);
    }
    while (server->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kBulkStream) fs_server.handle_bulk(payload);
    }
    nyx::FileHash verify{};
    if (nyx::hash_file(dl_dir + "/group.txt", verify) && verify == group_hash) {
      got_group = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  assert(got_group);

  std::filesystem::remove_all(personal_dir);
  std::filesystem::remove_all(group_dir);
  std::filesystem::remove_all(dl_dir);
  std::remove(nyx::FileIndex::index_path().c_str());
  std::cout << "share policy ok\n";
}

static void test_conversation_list() {
  const std::string contacts_path = nyx::default_contacts_path();
  const std::string contacts_backup = contacts_path + ".bak";
  std::ifstream probe(contacts_path);
  if (probe.good()) {
    probe.close();
    std::remove(contacts_backup.c_str());
    std::rename(contacts_path.c_str(), contacts_backup.c_str());
  }

  nyx::Profile self = nyx::generate_profile("conv-self");
  nyx::Profile peer = nyx::generate_profile("conv-peer");
  nyx::ContactBook book(contacts_path);
  nyx::Contact c;
  c.user_id = peer.user_id();
  c.nickname = peer.nickname;
  c.last_seen_ms = nyx::now_ms();
  book.upsert(std::move(c));
  book.save();

  const auto cid = nyx::dm_chat_id(self.user_id(), peer.user_id());
  const std::string chat_path = nyx::MessageStore::path_for_chat(cid);
  nyx::MessageStore store(chat_path);
  nyx::StoredMessage msg;
  msg.id = 1;
  msg.timestamp_ms = nyx::now_ms();
  msg.chat_id_hex = nyx::chat_id_hex(cid);
  msg.author = peer.nickname;
  msg.text = "preview text";
  msg.outgoing = false;
  store.append(msg);

  const auto list = nyx::list_conversations(self.user_id());
  bool found = false;
  for (const auto& item : list) {
    if (item.title == "conv-peer") {
      assert(item.preview == "preview text");
      found = true;
      break;
    }
  }
  assert(found);

  std::remove(contacts_path.c_str());
  std::remove(chat_path.c_str());
  if (std::ifstream(contacts_backup).good()) {
    std::rename(contacts_backup.c_str(), contacts_path.c_str());
  }
  std::cout << "conversation list ok\n";
}

static void test_reconnect_flow() {
  auto run_session = [](nyx::Connection& a, nyx::Connection& b, const std::string& text) {
    assert(a.send_payload(nyx::kChatStream, nyx::encode_text_message(text)));
    nyx::ByteBuffer payload;
    uint32_t sid = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      a.drive();
      b.drive();
      if (b.recv_stream(sid, payload)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto decoded = nyx::decode_text_message(payload);
    assert(decoded && *decoded == text);
  };

  for (int round = 0; round < 2; ++round) {
    nyx::UdpSocket listen_sock;
    nyx::UdpSocket connect_sock;
    assert(listen_sock.bind("127.0.0.1", 0));
    assert(connect_sock.bind("127.0.0.1", 0));
    const uint16_t listen_port = listen_sock.local_port();

    std::optional<nyx::Connection> server;
    std::thread accept_thread([&] {
      std::string host;
      uint16_t port = 0;
      auto packet = listen_sock.recv_from(host, port, 5000);
      assert(packet);
      server = nyx::Connection::accept_responder(
          std::move(listen_sock), host, port, &*packet);
    });

    auto client = nyx::Connection::connect_initiator(
        std::move(connect_sock), "127.0.0.1", listen_port);
    accept_thread.join();
    assert(client && server);
    run_session(*client, *server, "round-" + std::to_string(round));
  }
  std::cout << "reconnect flow ok\n";
}

static void test_network_config_roundtrip() {
  const std::string path = nyx::NetworkConfig::config_path();
  const std::string backup = path + ".bak";
  std::ifstream probe(path);
  if (probe.good()) {
    probe.close();
    std::remove(backup.c_str());
    std::rename(path.c_str(), backup.c_str());
  }

  nyx::NetworkConfig cfg;
  cfg.mode = nyx::DiscoveryMode::Internet;
  nyx::RendezvousServer a{"10.0.0.1", 3478, "test"};
  cfg.rendezvous_servers = {a};
  assert(cfg.save());

  nyx::NetworkConfig loaded;
  assert(loaded.load());
  assert(loaded.mode == nyx::DiscoveryMode::Internet);
  assert(loaded.rendezvous_servers.size() == 1);
  assert(loaded.rendezvous_servers[0].host == "10.0.0.1");

  std::remove(path.c_str());
  if (std::ifstream(backup).good()) std::rename(backup.c_str(), path.c_str());
  std::cout << "network config roundtrip ok\n";
}

static void test_file_log() {
  nyx::log_init();
  nyx::log_info("test log line");
  const auto path = nyx::default_log_path();
  assert(!path.empty());
  std::ifstream in(path);
  assert(in.good());
  std::cout << "file log ok\n";
}

static void test_recovery_phrase_roundtrip() {
  const std::string phrase = nyx::generate_recovery_phrase();
  assert(!phrase.empty());
  std::string normalized;
  assert(nyx::normalize_recovery_phrase(phrase, &normalized));
  assert(normalized == phrase);
  assert(nyx::split_recovery_words(normalized).size() == 12);

  std::string upper;
  upper.reserve(phrase.size());
  for (unsigned char c : phrase) upper.push_back(static_cast<char>(std::toupper(c)));
  assert(nyx::normalize_recovery_phrase(upper, &normalized));
  assert(normalized == phrase);

  std::string err;
  assert(!nyx::normalize_recovery_phrase("not a valid phrase here at all xx", &normalized, &err));
  std::cout << "recovery phrase roundtrip ok\n";
}

static void test_account_recovery_and_remember() {
  const std::string tmp = "test_nyx_auth_root";
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);

#ifdef _WIN32
  const char* prev = std::getenv("APPDATA");
  const std::string prev_appdata = prev ? prev : "";
  _putenv_s("APPDATA", tmp.c_str());
#else
  const char* prev = std::getenv("HOME");
  const std::string prev_home = prev ? prev : "";
  setenv("HOME", tmp.c_str(), 1);
#endif

  std::string phrase;
  std::string err;
  assert(nyx::create_account("AuthTest", "password123", &phrase, nullptr, &err));
  assert(!phrase.empty());
  const std::string account_id = nyx::active_account_id();
  assert(!account_id.empty());
  assert(nyx::account_has_recovery(account_id));
  assert(nyx::enable_remember_me(&err));
  assert(nyx::account_remember_active(account_id));

  nyx::lock_session(false);
  assert(nyx::active_account_id().empty());
  assert(nyx::try_unlock_remembered(account_id, nullptr, &err));
  assert(nyx::active_account_id() == account_id);

  nyx::lock_session(true);
  assert(!nyx::account_remember_active(account_id));
  assert(nyx::reset_password_with_recovery(account_id, phrase, "newpass1234", &err));
  assert(nyx::unlock_account(account_id, "newpass1234", false, nullptr, &err));
  nyx::lock_session(true);

#ifdef _WIN32
  if (prev_appdata.empty()) _putenv_s("APPDATA", "");
  else _putenv_s("APPDATA", prev_appdata.c_str());
#else
  if (prev_home.empty()) unsetenv("HOME");
  else setenv("HOME", prev_home.c_str(), 1);
#endif
  std::filesystem::remove_all(tmp);
  std::cout << "account recovery and remember ok\n";
}

int main() {
  test_frame_roundtrip();
  test_noise_handshake();
  test_reliable();
  test_rendezvous_hint();
  test_rendezvous_client();
  test_node_flow();
  test_udp_connection();
  test_chat_echo();
  test_session_rekey();
  test_hello_roundtrip();
  test_profile_save_load();
  test_hello_exchange();
  test_chat_message_roundtrip();
  test_dm_chat_id();
  test_message_store();
  test_chat_msg_exchange();
  test_ten_messages_roundtrip();
  test_file_index_three();
  test_list_response_size_cap();
#ifdef _WIN32
  test_file_index_unicode();
#endif
  test_file_transfer_1mb();
  test_group_member_persistence();
  test_profile_meta_photos_wire();
  test_avatar_proto_roundtrip();
  test_markdown_to_html();
  test_group_meta_message();
  test_file_access_roles();
  test_share_policy();
  test_conversation_list();
  test_reconnect_flow();
  test_network_config_roundtrip();
  test_group_chat_id();
  test_group_three_members();
  test_mdns_beacon_roundtrip();
  test_mdns_browse_receives_beacon();
  test_guess_lan_ipv4();
  test_is_lan_ipv4();
  test_file_log();
  test_recovery_phrase_roundtrip();
  test_account_recovery_and_remember();
  std::cout << "all tests passed\n";
  return 0;
}
