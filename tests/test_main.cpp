#include "nyx/account_store.hpp"
#include "nyx/app.hpp"
#include "nyx/avatar_proto.hpp"
#include "nyx/blob_store.hpp"
#include "nyx/call_av1.hpp"
#include "nyx/call_media.hpp"
#include "nyx/call_mesh.hpp"
#include "nyx/call_opus.hpp"
#include "nyx/call_proto.hpp"
#include "nyx/call_session.hpp"
#include "nyx/chat_service.hpp"
#include "nyx/connection.hpp"
#include "nyx/conversation.hpp"
#include "nyx/crypto.hpp"
#include "nyx/file_access.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/file_index.hpp"
#include "nyx/file_proto.hpp"
#include "nyx/file_transfer.hpp"
#include "nyx/group.hpp"
#include "nyx/group_hub.hpp"
#include "nyx/group_member.hpp"
#include "nyx/group_proto.hpp"
#include "nyx/identity.hpp"
#include "nyx/log.hpp"
#include "nyx/markdown_format.hpp"
#include "nyx/mdns.hpp"
#include "nyx/message_store.hpp"
#include "nyx/messaging.hpp"
#include "nyx/nat.hpp"
#include "nyx/network_config.hpp"
#include "nyx/paths.hpp"
#include "nyx/profile_meta.hpp"
#include "nyx/proto.hpp"
#include "nyx/recovery_phrase.hpp"
#include "nyx/transport.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>
#include <vector>

#undef NDEBUG
#include <cassert>

#ifdef _WIN32
#include <stdlib.h>
#endif

#define NYX_REQUIRE(cond)                                                                          \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::cerr << "REQUIRE failed: " #cond " @ " << __FILE__ << ":" << __LINE__ << std::endl;     \
      std::abort();                                                                                \
    }                                                                                              \
  } while (0)

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

  auto rt10 = sa->encrypt_realtime(10, {0x10});
  auto rt11 = sa->encrypt_realtime(11, {0x11});
  auto rt12 = sa->encrypt_realtime(12, {0x12});
  assert(rt10 && rt11 && rt12);

  auto pt12 = sb->decrypt_realtime(12, *rt12);
  auto pt10 = sb->decrypt_realtime(10, *rt10);
  assert(pt12 && *pt12 == nyx::ByteBuffer {0x12});
  assert(pt10 && *pt10 == nyx::ByteBuffer {0x10});
  std::cout << "noise handshake ok\n";
}

static void test_reliable() {
  nyx::ReliableSession a;
  nyx::ReliableSession b;
  nyx::ByteBuffer payload(4000, 0xAB);
  auto frames = a.send(0, payload);
  for (const auto& f : frames) {
    b.recv_wire(f);
    for (const auto& ack : b.make_ack_frames(0))
      a.recv_wire(ack);
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
  std::atomic<bool> done {false};

  std::thread rv_thread([&] {
    uint8_t buf[2048];
    while (!done.load()) {
      std::string from_host;
      uint16_t from_port = 0;
      auto pkt = rv_sock.recv_from(from_host, from_port, 100);
      if (!pkt)
        continue;

      auto frame = nyx::Frame::decode(pkt->data(), pkt->size());
      if (!frame)
        continue;

      if (frame->header.packet_type == nyx::PacketType::RendezvousRegister) {
        auto msg = nyx::RendezvousMessage::decode(frame->payload.data(), frame->payload.size());
        if (!msg || msg->kind != nyx::RendezvousKind::Register)
          continue;
        registry[nyx::to_hex(msg->token.data(), msg->token.size())] = msg->hint;
      } else if (frame->header.packet_type == nyx::PacketType::RendezvousLookup) {
        auto msg = nyx::RendezvousMessage::decode(frame->payload.data(), frame->payload.size());
        if (!msg || msg->kind != nyx::RendezvousKind::Lookup)
          continue;
        nyx::RendezvousMessage resp;
        const std::string key = nyx::to_hex(msg->token.data(), msg->token.size());
        if (registry.count(key)) {
          resp.kind = nyx::RendezvousKind::Response;
          resp.hint = registry[key];
        } else {
          resp.kind = nyx::RendezvousKind::NotFound;
        }
        auto payload = resp.encode();
        auto wire = nyx::Frame::make(nyx::PacketType::RendezvousResponse, 0, 0, payload).encode();
        rv_sock.send_to(wire, from_host, from_port);
        done.store(true);
      }
    }
  });

  nyx::UdpSocket listen_sock;
  assert(listen_sock.bind("0.0.0.0", 0));
  const uint16_t listen_port = listen_sock.local_port();

  nyx::InviteToken token {};
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
  nyx::InviteToken token {};
  nyx::random_bytes(token.data(), token.size());
  const nyx::EndpointHint hint = nyx::make_hint("127.0.0.1", 62776);

  nyx::RendezvousMessage reg;
  reg.kind = nyx::RendezvousKind::Register;
  reg.token = token;
  reg.hint = hint;
  const auto reg_payload = reg.encode();
  const auto decoded_reg = nyx::RendezvousMessage::decode(reg_payload.data(), reg_payload.size());
  assert(decoded_reg);
  assert(decoded_reg->kind == nyx::RendezvousKind::Register);
  assert(decoded_reg->hint.port == 62776);
  assert(decoded_reg->hint.ip[12] == 127);

  const auto wire =
      nyx::Frame::make(nyx::PacketType::RendezvousRegister, 0, 0, reg_payload).encode();
  const auto frame = nyx::Frame::decode(wire.data(), wire.size());
  assert(frame);
  const auto from_wire =
      nyx::RendezvousMessage::decode(frame->payload.data(), frame->payload.size());
  assert(from_wire && from_wire->hint.port == 62776);
  std::cout << "rendezvous hint ok\n";
}

static bool is_handshake_datagram(const nyx::ByteBuffer& data) {
  return nyx::is_handshake_datagram(data);
}

/** Accept одного Noise-handshake с дедлайном (без вечного while). */
static std::optional<nyx::Connection> accept_one(nyx::UdpSocket listen_sock, int timeout_sec = 12) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
  while (std::chrono::steady_clock::now() < deadline) {
    std::string host;
    uint16_t port = 0;
    auto pkt = listen_sock.recv_from(host, port, 200);
    if (!pkt)
      continue;
    if (nyx::is_punch_datagram(*pkt))
      continue;
    if (!is_handshake_datagram(*pkt))
      continue;
    return nyx::Connection::accept_responder(std::move(listen_sock), host, port, &*pkt);
  }
  return std::nullopt;
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline) {
      auto pkt = listen_sock.recv_from(peer_host, peer_port, 200);
      if (!pkt)
        continue;
      if (pkt->size() >= 10 && std::memcmp(pkt->data(), "NYX-PUNCH", 10) == 0) {
        continue;
      }
      if (is_handshake_datagram(*pkt)) {
        first_pkt = std::move(*pkt);
        server = nyx::Connection::accept_responder(
            std::move(listen_sock), peer_host, peer_port, &first_pkt);
        return;
      }
    }
  });

  nyx::hole_punch(connect_sock, nyx::make_hint("127.0.0.1", listen_port));
  auto client =
      nyx::Connection::connect_initiator(std::move(connect_sock), "127.0.0.1", listen_port);
  listener.join();

  assert(client);
  assert(server);
  std::cout << "node flow ok\n";
}

static void test_udp_connection() {
  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  NYX_REQUIRE(listen_sock.bind("127.0.0.1", 0));
  NYX_REQUIRE(connect_sock.bind("127.0.0.1", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread accept_thread([&] { server = accept_one(std::move(listen_sock)); });

  auto client =
      nyx::Connection::connect_initiator(std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();

  NYX_REQUIRE(client);
  NYX_REQUIRE(server);
  std::cout << "udp connection ok\n";
}

static void test_chat_echo() {
  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  NYX_REQUIRE(listen_sock.bind("127.0.0.1", 0));
  NYX_REQUIRE(connect_sock.bind("127.0.0.1", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread accept_thread([&] { server = accept_one(std::move(listen_sock)); });

  auto client =
      nyx::Connection::connect_initiator(std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  NYX_REQUIRE(client && server);

  const std::string message = "hello nyx";
  assert(client->send_payload(nyx::kChatStream, nyx::encode_text_message(message)));

  nyx::ByteBuffer received;
  uint32_t stream_id = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    server->drive();
    if (server->recv_stream(stream_id, received))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  assert(stream_id == nyx::kChatStream);
  auto decoded = nyx::decode_text_message(received);
  assert(decoded && *decoded == message);
  std::cout << "chat echo ok\n";
}

static void test_realtime_bidirectional() {
  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  NYX_REQUIRE(listen_sock.bind("127.0.0.1", 0));
  NYX_REQUIRE(connect_sock.bind("127.0.0.1", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread accept_thread([&] { server = accept_one(std::move(listen_sock)); });
  auto client =
      nyx::Connection::connect_initiator(std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  NYX_REQUIRE(client && server);

  const nyx::ByteBuffer to_server {0x01, 0x02, 0x03};
  const nyx::ByteBuffer to_client {0x04, 0x05, 0x06};
  NYX_REQUIRE(client->send_realtime(to_server));
  NYX_REQUIRE(server->send_realtime(to_client));

  nyx::ByteBuffer got_server;
  nyx::ByteBuffer got_client;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline &&
         (got_server.empty() || got_client.empty())) {
    client->drive();
    server->drive();
    if (got_server.empty())
      server->recv_realtime(got_server);
    if (got_client.empty())
      client->recv_realtime(got_client);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  NYX_REQUIRE(got_server == to_server);
  NYX_REQUIRE(got_client == to_client);
  std::cout << "realtime bidirectional ok\n";
}

static std::optional<nyx::Connection> loopback_connect(std::optional<nyx::Connection>& server_out) {
  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  if (!listen_sock.bind("127.0.0.1", 0))
    return std::nullopt;
  if (!connect_sock.bind("127.0.0.1", 0))
    return std::nullopt;
  const uint16_t listen_port = listen_sock.local_port();

  std::thread accept_thread([&] {
    std::string host;
    uint16_t port = 0;
    auto packet = listen_sock.recv_from(host, port, 5000);
    if (!packet)
      return;
    server_out = nyx::Connection::accept_responder(std::move(listen_sock), host, port, &*packet);
  });

  auto client =
      nyx::Connection::connect_initiator(std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  if (!client || !server_out)
    return std::nullopt;
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
    if (client->session_rekey_epoch() >= 1)
      break;
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
      if (stream_id != nyx::kChatStream)
        continue;
      auto decoded = nyx::decode_text_message(received);
      if (decoded && *decoded == after)
        got = true;
    }
    if (got)
      break;
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
  NYX_REQUIRE(listen_sock.bind("127.0.0.1", 0));
  NYX_REQUIRE(connect_sock.bind("127.0.0.1", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread accept_thread([&] { server = accept_one(std::move(listen_sock)); });

  auto client =
      nyx::Connection::connect_initiator(std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  NYX_REQUIRE(client && server);

  nyx::Profile alice = nyx::generate_profile("Alice");
  nyx::Profile bob = nyx::generate_profile("Bob");

  nyx::HelloMessage hello_a;
  hello_a.public_key = alice.public_key;
  hello_a.nickname = alice.nickname;
  nyx::HelloMessage hello_b;
  hello_b.public_key = bob.public_key;
  hello_b.nickname = bob.nickname;

  NYX_REQUIRE(client->send_payload(nyx::kChatStream, hello_b.encode()));
  NYX_REQUIRE(server->send_payload(nyx::kChatStream, hello_a.encode()));

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
      if (auto h = nyx::decode_hello_message(payload))
        got_on_server = *h;
    }
    while (client->recv_stream(stream_id, payload)) {
      if (auto h = nyx::decode_hello_message(payload))
        got_on_client = *h;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  NYX_REQUIRE(got_on_server.nickname == "Bob");
  NYX_REQUIRE(got_on_client.nickname == "Alice");
  std::cout << "hello exchange ok\n";
}

static void test_chat_message_roundtrip() {
  nyx::ChatMessage msg;
  msg.id = 42;
  msg.timestamp_ms = 1'700'000'000'000ULL;
  msg.author = "Alice";
  msg.text = "hello phase 3";
  nyx::random_bytes(msg.author_id.data(), msg.author_id.size());
  nyx::UserId peer {};
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
  nyx::UserId a {};
  nyx::UserId b {};
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
    server = nyx::Connection::accept_responder(std::move(listen_sock), host, port, &*packet);
  });

  auto client =
      nyx::Connection::connect_initiator(std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  assert(client && server);

  nyx::Profile sender = nyx::generate_profile("Sender");
  nyx::UserId peer {};
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
  if (!packet)
    return std::nullopt;
  return nyx::Connection::accept_responder(std::move(listen_sock), host, port, &*packet);
}

static void test_ten_messages_roundtrip() {
  nyx::UdpSocket listen_sock;
  nyx::UdpSocket connect_sock;
  assert(listen_sock.bind("127.0.0.1", 0));
  assert(connect_sock.bind("127.0.0.1", 0));
  const uint16_t listen_port = listen_sock.local_port();

  std::optional<nyx::Connection> server;
  std::thread accept_thread([&] { server = accept_loopback(listen_sock); });

  auto client =
      nyx::Connection::connect_initiator(std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  assert(client && server);

  nyx::Profile alice = nyx::generate_profile("Alice");
  nyx::Profile bob = nyx::generate_profile("Bob");

  nyx::ChatService::PeerInfo peer_for_alice {bob.public_key, bob.nickname};
  nyx::ChatService::PeerInfo peer_for_bob {alice.public_key, alice.nickname};
  nyx::ChatService chat_a(*client, alice, peer_for_alice);
  nyx::ChatService chat_b(*server, bob, peer_for_bob);

  std::vector<std::string> got_on_b;

  chat_b.set_on_message([&](const nyx::ChatMessage& msg, bool outgoing) {
    if (!outgoing)
      got_on_b.push_back(msg.text);
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
        if (sid == nyx::kChatStream)
          chat_a.handle_payload(payload);
      }
      while (server->recv_stream(sid, payload)) {
        if (sid == nyx::kChatStream)
          chat_b.handle_payload(payload);
      }
      if (static_cast<int>(got_on_b.size()) > i)
        break;
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
    if (e.is_directory() && e.relative_path == "sub")
      has_sub = true;
  }
  assert(has_sub);



  {
    std::vector<nyx::FileEntry> wire_level;
    for (const auto& e : level) {
      nyx::FileEntry w = e;
      if (!w.is_directory()) {

        w.relative_path = w.leaf_name();
      }
      wire_level.push_back(std::move(w));
    }
    const auto again = nyx::FileIndex::listing_level(wire_level, dir, "");
    bool has_sub_again = false;
    int files_again = 0;
    for (const auto& e : again) {
      if (e.is_directory() && e.relative_path == "sub")
        has_sub_again = true;
      if (!e.is_directory())
        ++files_again;
    }
    assert(has_sub_again);
    assert(files_again >= 3);


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
    const auto nested_level = nyx::FileIndex::listing_level(nested_wire, dir, "sub");
    bool has_nested_file = false;
    for (const auto& e : nested_level) {
      if (!e.is_directory() && e.leaf_name() == "nested.txt")
        has_nested_file = true;
    }
    assert(has_nested_file);
  }

  assert(index.remove_root(dir));
  assert(index.entries().empty());
  assert(index.share_roots().empty());
  assert(index.listing_for_session({}).empty());


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
    if (i % 5 == 0)
      e.mime = "application/x-nyx-directory";
    entries.push_back(std::move(e));
  }
  const auto wire = nyx::encode_list_response(entries);
  assert(wire.size() <= 48000);
  assert(wire.size() > 3);
  auto decoded = nyx::decode_list_response(wire);
  assert(decoded);
  assert(!decoded->empty());
  assert(decoded->size() < entries.size());

  bool has_dir = false;
  for (const auto& e : *decoded) {
    if (e.is_directory()) {
      has_dir = true;
      break;
    }
  }
  assert(has_dir);
  std::cout << "list response size cap ok (" << decoded->size() << " entries, " << wire.size()
            << " bytes)\n";
}

static void test_file_v2_and_scoped_index() {
  nyx::FileCapabilities capabilities;
  capabilities.flags = nyx::FileCapabilities::kResume | nyx::FileCapabilities::kCancel |
                       nyx::FileCapabilities::kMultiTransfer;
  capabilities.max_parallel = 2;
  const auto decoded_caps = nyx::FileCapabilities::decode(capabilities.encode());
  assert(decoded_caps);
  assert(decoded_caps->version == 2);
  assert(decoded_caps->flags == capabilities.flags);
  assert(decoded_caps->max_parallel == 2);
  assert(!nyx::FileCapabilities::decode(
      nyx::ByteBuffer {static_cast<uint8_t>(nyx::FileKind::Capabilities)}));

  nyx::FileRangeRequest range;
  range.hash = nyx::hash_bytes(reinterpret_cast<const uint8_t*>("range"), 5);
  range.offset = 123456;
  const auto decoded_range = nyx::FileRangeRequest::decode(range.encode());
  assert(decoded_range);
  assert(decoded_range->hash == range.hash);
  assert(decoded_range->offset == range.offset);
  auto truncated_range = range.encode();
  truncated_range.resize(40);
  assert(!nyx::FileRangeRequest::decode(truncated_range));

  nyx::FileCancel cancel;
  cancel.hash = range.hash;
  const auto decoded_cancel = nyx::FileCancel::decode(cancel.encode());
  assert(decoded_cancel && decoded_cancel->hash == cancel.hash);
  assert(!nyx::FileCancel::decode(nyx::ByteBuffer(32, 0)));

  const std::string dir = "test_scoped_index";
  std::filesystem::remove_all(dir);
  std::remove(nyx::FileIndex::index_path().c_str());
  std::filesystem::create_directories(dir);
  std::ofstream(dir + "/same.txt") << "scope isolation";
  nyx::GroupId group {};
  group[0] = 42;
  nyx::GroupId personal {};
  {
    nyx::FileIndex index;
    assert(index.add_root(dir));
    assert(index.add_root(dir, &group));
    assert(index.count_in_root(dir, personal) == 1);
    assert(index.count_in_root(dir, group) == 1);
    assert(index.remove_root(dir));
    assert(index.count_in_root(dir, personal) == 0);
    assert(index.count_in_root(dir, group) == 1);
    assert(index.find_for_session(index.entries_for_session(group)[0].hash, group));
  }
  std::filesystem::remove_all(dir);
  {
    nyx::FileIndex reloaded;
    assert(reloaded.load());
    assert(reloaded.share_roots().empty());
    assert(reloaded.entries().empty());
  }
  std::remove(nyx::FileIndex::index_path().c_str());

  const std::string partial = "test_resume_blob.part";
  std::filesystem::remove(partial);
  {
    nyx::BlobWriter writer(partial);
    assert(writer.open());
    assert(writer.write_at(0, nyx::ByteBuffer {'a', 'b', 'c'}));
  }
  {
    nyx::BlobWriter writer(partial);
    assert(writer.open(false));
    assert(writer.write_at(3, nyx::ByteBuffer {'d', 'e', 'f'}));
  }
  std::ifstream resumed(partial, std::ios::binary);
  std::string resumed_text((std::istreambuf_iterator<char>(resumed)),
                           std::istreambuf_iterator<char>());
  assert(resumed_text == "abcdef");
  resumed.close();
  std::filesystem::remove(partial);
  std::cout << "file v2, resume and scope isolation ok\n";
}

static void test_file_index_migration_and_objects() {
  const std::string dir = "test_index_migrate";
  std::filesystem::remove_all(dir);
  std::remove(nyx::FileIndex::index_path().c_str());
  std::filesystem::create_directories(dir);
  std::ofstream(dir + "/legacy.txt") << "legacy payload";


  {
    std::ofstream out(nyx::FileIndex::index_path(), std::ios::binary | std::ios::trunc);
    out << "{\"roots\":[{\"root\":\"" << dir << "\"}],"
        << "\"files\":[{\"hash\":\"";
  }
  nyx::FileHash hash {};
  assert(nyx::hash_file(dir + "/legacy.txt", hash));
  {
    std::ofstream out(nyx::FileIndex::index_path(), std::ios::binary | std::ios::trunc);
    out << "{\"roots\":[{\"root\":\"" << dir << "\"}],\"files\":[{"
        << "\"hash\":\"" << nyx::hash_hex(hash) << "\",\"size\":14,\"mtime\":0,"
        << "\"root\":\"" << dir << "\",\"rel\":\"legacy.txt\","
        << "\"mime\":\"text/plain\"}]}\n";
  }

  {
    nyx::FileIndex index;
    assert(index.load());
    assert(index.share_roots().size() == 1);
    assert(index.entries().size() == 1);
    assert(index.count_in_root(dir, {}) == 1);

    std::ifstream rewritten(nyx::FileIndex::index_path(), std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(rewritten)), std::istreambuf_iterator<char>());
    assert(json.find("\"schema_version\":2") != std::string::npos);
    assert(json.find("\"group\"") != std::string::npos);
  }


  std::filesystem::remove_all(dir);
  {
    nyx::FileIndex index;
    assert(index.load());
    assert(index.share_roots().empty());
    assert(index.entries().empty());
  }


  std::filesystem::create_directories(dir);
  std::ofstream(dir + "/obj.bin") << "object-bytes";
  assert(nyx::hash_file(dir + "/obj.bin", hash));
  {
    nyx::FileIndex index;
    nyx::GroupId group {};
    group[1] = 7;
    auto adopted =
        index.adopt_file(dir + "/obj.bin", hash, "obj.bin", "application/octet-stream", group);
    assert(adopted);
    assert(adopted->root_path.find("/library/") != std::string::npos ||
           adopted->root_path.find("\\library\\") != std::string::npos);
    assert(index.find_for_session(hash, group));
    assert(!index.find_for_session(hash, {}));
    const auto level = index.listing_at_root(adopted->root_path, {}, &group);
    assert(level.size() == 1);

    nyx::UserId owner {};
    owner[0] = 0xab;
    owner[1] = 0xcd;
    std::ofstream(dir + "/owned.bin") << "owned-bytes";
    nyx::FileHash owned_hash {};
    assert(nyx::hash_file(dir + "/owned.bin", owned_hash));
    auto owned = index.adopt_file(
        dir + "/owned.bin", owned_hash, "owned.bin", "application/octet-stream", group, &owner);
    assert(owned);
    assert(owned->relative_path.find(nyx::to_hex(owner.data(), owner.size())) == 0);
    assert(owned->owner_id == owner);
    const auto owner_level = index.listing_at_root(owned->root_path, {}, &group);

    assert(owner_level.size() >= 2);

    auto voice = index.adopt_file(dir + "/owned.bin",
                                  owned_hash,
                                  "voice-message.m4a",
                                  "audio/mp4",
                                  group,
                                  &owner,
                                  "Медиа/Test Chat (abcd1234)/Голосовые сообщения");
    assert(voice);
    assert(voice->owner_id == owner);
    assert(voice->relative_path.find("Медиа/") == 0);
    assert(voice->relative_path.find("Голосовые сообщения") != std::string::npos);

    auto circle = index.adopt_file(dir + "/owned.bin",
                                   owned_hash,
                                   "circle-message.mp4",
                                   "video/mp4",
                                   group,
                                   &owner,
                                   "Медиа/Other Chat (ef012345)/Видеокружки");
    assert(circle);
    const auto all = index.entries_for_session(group);
    int media_copies = 0;
    for (const auto& entry : all) {
      if (entry.hash == owned_hash)
        ++media_copies;
    }
    assert(media_copies == 3);
  }

  std::filesystem::remove_all(dir);
  std::remove(nyx::FileIndex::index_path().c_str());
  std::cout << "file index migration and objects ok\n";
}

static void test_file_catalog_snapshot_semantics() {

  std::vector<nyx::FileEntry> catalog;
  nyx::FileEntry root_a;
  root_a.root_path = "/share/a";
  root_a.relative_path = "a";
  root_a.mime = "application/x-nyx-directory";
  root_a.hash = nyx::hash_bytes(reinterpret_cast<const uint8_t*>("ra"), 2);
  catalog.push_back(root_a);

  nyx::FileEntry stale;
  stale.root_path = "/share/a";
  stale.relative_path = "gone.txt";
  stale.mime = "text/plain";
  stale.hash = nyx::hash_bytes(reinterpret_cast<const uint8_t*>("gone"), 4);
  catalog.push_back(stale);

  nyx::FileEntry other;
  other.root_path = "/share/b";
  other.relative_path = "b";
  other.mime = "application/x-nyx-directory";
  other.hash = nyx::hash_bytes(reinterpret_cast<const uint8_t*>("rb"), 2);
  catalog.push_back(other);

  const std::string root_norm = nyx::normalize_utf8_path("/share/a");
  catalog.erase(std::remove_if(catalog.begin(),
                               catalog.end(),
                               [&](const nyx::FileEntry& e) {
                                 if (nyx::normalize_utf8_path(e.root_path) != root_norm) {
                                   return false;
                                 }
                                 if (e.is_directory() && e.relative_path == "a") {
                                   return false;
                                 }
                                 return true;
                               }),
                catalog.end());
  nyx::FileEntry fresh;
  fresh.root_path = "/share/a";
  fresh.relative_path = "new.txt";
  fresh.mime = "text/plain";
  fresh.hash = nyx::hash_bytes(reinterpret_cast<const uint8_t*>("new"), 3);
  catalog.push_back(fresh);

  assert(catalog.size() == 3);
  bool saw_gone = false;
  bool saw_new = false;
  bool saw_b = false;
  for (const auto& e : catalog) {
    if (e.relative_path == "gone.txt")
      saw_gone = true;
    if (e.relative_path == "new.txt")
      saw_new = true;
    if (e.relative_path == "b")
      saw_b = true;
  }
  assert(!saw_gone && saw_new && saw_b);


  nyx::FileRequest req;
  req.hash = fresh.hash;
  const auto decoded = nyx::FileRequest::decode(req.encode());
  assert(decoded && decoded->hash == req.hash);

  assert(static_cast<nyx::FileKind>(req.encode()[0]) == nyx::FileKind::Request);

  std::cout << "file catalog snapshot and v1 request ok\n";
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
  assert(index.entries()[0].relative_path ==
         nyx::path_to_utf8(std::filesystem::path(L"\u043A\u043F\u044B\u0432\u0430.txt")));

  nyx::FileHash hash {};
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

  nyx::FileHash expected {};
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
    server = nyx::Connection::accept_responder(std::move(listen_sock), host, port, &*packet);
  });

  auto client =
      nyx::Connection::connect_initiator(std::move(connect_sock), "127.0.0.1", listen_port);
  accept_thread.join();
  assert(client && server);

  nyx::FileIndex client_index;
  nyx::FileTransferService fs_server(*server, server_index, dl_dir + "/srv");
  nyx::FileTransferService fs_client(*client, client_index, dl_dir);

  assert(fs_server.announce_capabilities());
  assert(fs_client.announce_capabilities());
  for (int i = 0; i < 100 && !fs_client.peer_supports_resume(); ++i) {
    client->drive();
    server->drive();
    nyx::ByteBuffer payload;
    uint32_t stream_id = 0;
    while (client->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kBulkStream)
        fs_client.handle_bulk(payload);
    }
    while (server->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kBulkStream)
        fs_server.handle_bulk(payload);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(fs_client.peer_supports_resume());
  const std::string dest = dl_dir + "/payload.bin";
  {
    std::ifstream source(src_path, std::ios::binary);
    std::ofstream partial(dest + ".part", std::ios::binary);
    std::vector<char> prefix(64 * 1024);
    source.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    partial.write(prefix.data(), source.gcount());
  }
  assert(fs_client.request_file(nyx::hash_hex(expected), dest));

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
      if (stream_id == nyx::kBulkStream)
        fs_client.handle_bulk(payload);
    }
    while (server->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kBulkStream)
        fs_server.handle_bulk(payload);
    }

    std::error_code ec;
    if (std::filesystem::exists(dest, ec)) {
      nyx::FileHash got {};
      if (nyx::hash_file(dest, got) && got == expected)
        done = true;
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
  if (!connection.send_payload(nyx::kChatStream, hello.encode()))
    return false;

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
  nyx::GroupId gid {};
  nyx::random_bytes(gid.data(), gid.size());
  assert(nyx::group_chat_id(gid) != nyx::ChatId {});
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

  std::atomic<bool> hub_running {true};
  std::atomic<bool> send_alice {false};
  std::thread hub_thread([&] {
    while (hub_running.load()) {
      hub.poll();
      if (send_alice.exchange(false))
        hub.send_message("from-alice");
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

  nyx::GroupId zero {};

  nyx::set_account_data_dir(bob_dir);
  nyx::GroupMemberService bob_svc(*bob_conn, bob, zero, "");
  nyx::set_account_data_dir(charlie_dir);
  nyx::GroupMemberService charlie_svc(*charlie_conn, charlie, zero, "");

  std::vector<std::string> bob_got;
  std::vector<std::string> charlie_got;
  bob_svc.set_on_message([&](const nyx::ChatMessage& m, bool outgoing) {
    if (!outgoing)
      bob_got.push_back(m.text);
  });
  charlie_svc.set_on_message([&](const nyx::ChatMessage& m, bool outgoing) {
    if (!outgoing)
      charlie_got.push_back(m.text);
  });

  nyx::set_account_data_dir(bob_dir);
  assert(bob_svc.join(15000));
  nyx::set_account_data_dir(charlie_dir);
  assert(charlie_svc.join(15000));

  nyx::set_account_data_dir(bob_dir);
  assert(bob_svc.send_message("from-bob"));
  nyx::set_account_data_dir(charlie_dir);
  assert(charlie_svc.send_message("from-charlie"));
  send_alice.store(true);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline) {
    bob_conn->drive();
    charlie_conn->drive();
    nyx::ByteBuffer payload;
    uint32_t stream_id = 0;
    while (bob_conn->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kChatStream)
        bob_svc.handle_payload(payload);
    }
    while (charlie_conn->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kChatStream)
        charlie_svc.handle_payload(payload);
    }
    if (bob_got.size() >= 2 && charlie_got.size() >= 2)
      break;
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
  nyx::write_u32_le(wire, 0x2a01a8c0);
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

  std::atomic<bool> stop {false};
  std::thread sender([&]() {
    while (!stop.load()) {
      nyx::MdnsLan::send_announcement(advert, profile, advert.local_port(), "192.168.50.10");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  nyx::UdpSocket browse;
  if (!nyx::MdnsLan::setup_socket(browse)) {
    stop.store(true);
    sender.join();
    std::cout << "mdns browse skipped (socket setup failed)\n";
    return;
  }
  const auto peers = nyx::MdnsLan::browse(browse, 1500);
  stop.store(true);
  sender.join();

  if (peers.empty()) {

    std::cout << "mdns browse skipped (no peers — multicast unavailable)\n";
    return;
  }
  const auto found = std::find_if(peers.begin(), peers.end(), [&](const nyx::LanPeer& peer) {
    return peer.instance == "LanBrowse" && peer.port == advert.local_port();
  });
  if (found == peers.end()) {
    std::cout << "mdns browse skipped (test beacon not observed)\n";
    return;
  }
  assert(found->host == "192.168.50.10");
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
  const std::string isolated = nyx::data_root() + "/test_group_persist";
  std::filesystem::remove_all(isolated);
  std::filesystem::create_directories(isolated);
  nyx::set_account_data_dir(isolated);
  std::remove(nyx::GroupStore::store_path().c_str());
  nyx::Profile owner = nyx::generate_profile("Owner");
  nyx::GroupStore store;
  auto group = store.create("Q", owner.user_id(), owner.nickname);
  nyx::UserId member {};
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
    if (m.user_id == member)
      has_test = true;
    if (m.role == nyx::GroupRole::Owner)
      has_owner = true;
  }
  assert(has_test);
  assert(has_owner);
  nyx::clear_account_data_dir();
  std::filesystem::remove_all(isolated);
  std::cout << "group member persistence ok\n";
}

static void test_profile_meta_photos_wire() {
  nyx::ProfileMeta meta;
  meta.bio = "hi";
  meta.interests = "x";
  meta.availability = nyx::Availability::Away;
  meta.updated_ms = 42;
  nyx::FileHash h {};
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
  nyx::FileHash h {};
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

static void test_control_message_roundtrip() {
  nyx::ControlMessage ping;
  ping.kind = nyx::ControlKind::Ping;
  ping.nonce = 0x1122334455667788ull;
  const auto ping_w = ping.encode();
  const auto ping_d = nyx::ControlMessage::decode(ping_w.data(), ping_w.size());
  assert(ping_d && ping_d->kind == nyx::ControlKind::Ping);
  assert(ping_d->nonce == ping.nonce);

  nyx::ControlMessage open;
  open.kind = nyx::ControlKind::OpenStream;
  open.stream_id = 7;
  open.stream_type = nyx::StreamType::Bulk;
  const auto open_w = open.encode();
  const auto open_d = nyx::ControlMessage::decode(open_w.data(), open_w.size());
  assert(open_d && open_d->kind == nyx::ControlKind::OpenStream);
  assert(open_d->stream_id == 7 && open_d->stream_type == nyx::StreamType::Bulk);

  assert(!nyx::ControlMessage::decode(nullptr, 0));
  const uint8_t junk[] = {0xFF};
  assert(!nyx::ControlMessage::decode(junk, sizeof(junk)));
  std::cout << "control message roundtrip ok\n";
}

static void test_group_join_roundtrip() {
  nyx::GroupId gid {};
  gid[0] = 0xAB;
  gid[31] = 0xCD;

  nyx::GroupJoinMessage join;
  join.group_id = gid;
  const auto join_w = join.encode();
  assert(nyx::is_group_frame(join_w));
  const auto join_d = nyx::GroupJoinMessage::decode(join_w);
  assert(join_d && join_d->group_id == gid);

  nyx::GroupJoinAckMessage ack;
  ack.accepted = true;
  ack.group_id = gid;
  ack.group_name = "The Field";
  nyx::GroupMemberRecord owner;
  owner.user_id[0] = 1;
  owner.nickname = "alice";
  owner.role = nyx::GroupRole::Owner;
  nyx::GroupMemberRecord member;
  member.user_id[0] = 2;
  member.nickname = "боб";
  ack.members = {owner, member};
  const auto ack_d = nyx::GroupJoinAckMessage::decode(ack.encode());
  assert(ack_d && ack_d->accepted && ack_d->group_id == gid);
  assert(ack_d->group_name == "The Field");
  assert(ack_d->members.size() == 2);
  assert(ack_d->members[0].nickname == "alice" && ack_d->members[0].role == nyx::GroupRole::Owner);
  assert(ack_d->members[1].nickname == "боб" && ack_d->members[1].role == nyx::GroupRole::Member);

  nyx::GroupJoinAckMessage deny;
  deny.accepted = false;
  deny.reason = "not invited";
  const auto deny_d = nyx::GroupJoinAckMessage::decode(deny.encode());
  assert(deny_d && !deny_d->accepted && deny_d->reason == "not invited");

  nyx::GroupMemberJoinedMessage joined;
  joined.member = member;
  const auto joined_d = nyx::GroupMemberJoinedMessage::decode(joined.encode());
  assert(joined_d && joined_d->member.user_id == member.user_id);
  assert(joined_d->member.nickname == member.nickname);

  assert(!nyx::GroupJoinMessage::decode({}));
  assert(!nyx::GroupJoinAckMessage::decode({0x02}));
  std::cout << "group join roundtrip ok\n";
}

static void test_file_transfer_proto_roundtrip() {
  nyx::FileHash h {};
  for (std::size_t i = 0; i < h.size(); ++i)
    h[i] = static_cast<uint8_t>(i);

  nyx::FileOffer offer;
  offer.hash = h;
  offer.size = 123456789ull;
  offer.name = "отчёт.pdf";
  offer.mime = "application/pdf";
  const auto offer_d = nyx::FileOffer::decode(offer.encode());
  assert(offer_d && offer_d->hash == h && offer_d->size == offer.size);
  assert(offer_d->name == offer.name && offer_d->mime == offer.mime);

  nyx::FileChunk chunk;
  chunk.hash = h;
  chunk.offset = 0xFFFF0000ull;
  chunk.data.assign(nyx::kFileChunkSize, 0x5A);
  const auto chunk_d = nyx::FileChunk::decode(chunk.encode());
  assert(chunk_d && chunk_d->hash == h && chunk_d->offset == chunk.offset);
  assert(chunk_d->data.size() == nyx::kFileChunkSize && chunk_d->data[0] == 0x5A);

  nyx::FileComplete complete;
  complete.hash = h;
  complete.size = 42;
  const auto complete_d = nyx::FileComplete::decode(complete.encode());
  assert(complete_d && complete_d->hash == h && complete_d->size == 42);

  nyx::FileDeny deny;
  deny.hash = h;
  deny.reason = "no permission";
  const auto deny_d = nyx::FileDeny::decode(deny.encode());
  assert(deny_d && deny_d->hash == h && deny_d->reason == "no permission");

  assert(!nyx::FileOffer::decode({}));
  assert(!nyx::FileChunk::decode({static_cast<uint8_t>(nyx::FileKind::Chunk)}));
  std::cout << "file transfer proto roundtrip ok\n";
}

static void test_avatar_deny_roundtrip() {
  nyx::FileHash h {};
  h[0] = 0x77;
  nyx::AvatarDeny deny;
  deny.hash = h;
  deny.reason = "not found";
  const auto wire = deny.encode();
  assert(nyx::is_avatar_frame(wire));
  const auto d = nyx::AvatarDeny::decode(wire);
  assert(d && d->hash == h && d->reason == "not found");
  assert(!nyx::AvatarDeny::decode({}));
  std::cout << "avatar deny roundtrip ok\n";
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
      "hi\n\n![pic](nyx-media:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef)"
      "\n\n$$x^2$$");
  assert(blocks.size() >= 3);
  bool saw_media = false;
  bool saw_formula = false;
  for (const auto& b : blocks) {
    if (b.type == nyx::MdBlockType::Media)
      saw_media = true;
    if (b.type == nyx::MdBlockType::Formula)
      saw_formula = true;
  }
  assert(saw_media && saw_formula);
  const auto file_blocks = nyx::parse_markdown_blocks(
      "[report.txt](nyx-file:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;size="
      "42;mime=text/plain)");
  assert(file_blocks.size() == 1);
  assert(file_blocks[0].type == nyx::MdBlockType::File);
  assert(file_blocks[0].caption == "report.txt");
  assert(file_blocks[0].mime == "text/plain");
  assert(file_blocks[0].size == 42);

  const auto circle_blocks = nyx::parse_markdown_blocks(
      "[circle-message.mp4](nyx-file:"
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;size=99;mime=video/mp4)");
  assert(circle_blocks.size() == 1);
  assert(circle_blocks[0].caption == "circle-message.mp4");
  assert(circle_blocks[0].mime == "video/mp4");

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

static void test_call_proto_roundtrip() {
  nyx::CallId id = nyx::generate_call_id();
  nyx::UserId peer {};
  peer[0] = 0x11;
  peer[31] = 0x22;

  nyx::CallInviteMessage inv;
  inv.call_id = id;
  inv.mode = nyx::CallMode::AudioVideo;
  inv.scope = nyx::CallScope::Field;
  inv.group_or_peer = peer;
  inv.sdp_lite = "v=nyx1";
  assert(nyx::is_call_frame(inv.encode()));
  assert(!nyx::ByeMessage::decode(inv.encode()));
  auto inv_d = nyx::CallInviteMessage::decode(inv.encode());
  assert(inv_d && inv_d->mode == nyx::CallMode::AudioVideo && inv_d->sdp_lite == "v=nyx1");
  assert(inv_d->group_or_peer == peer);

  nyx::CallRingingMessage ring;
  ring.call_id = id;
  assert(nyx::CallRingingMessage::decode(ring.encode()));

  nyx::CallAcceptMessage acc;
  acc.call_id = id;
  acc.mode = nyx::CallMode::Audio;
  acc.sdp_lite = "ok";
  assert(nyx::CallAcceptMessage::decode(acc.encode())->sdp_lite == "ok");

  nyx::CallRejectMessage rej;
  rej.call_id = id;
  rej.reason = nyx::CallRejectReason::Busy;
  assert(nyx::CallRejectMessage::decode(rej.encode())->reason == nyx::CallRejectReason::Busy);

  nyx::CallHangupMessage hang;
  hang.call_id = id;
  hang.reason = nyx::CallHangupReason::HubClosed;
  assert(nyx::CallHangupMessage::decode(hang.encode()));

  nyx::CallUpdateMessage upd;
  upd.call_id = id;
  upd.mic_muted = true;
  upd.camera_on = true;
  auto upd_d = nyx::CallUpdateMessage::decode(upd.encode());
  assert(upd_d && upd_d->mic_muted && upd_d->camera_on && !upd_d->screen_share);

  nyx::CallRosterMessage roster;
  roster.call_id = id;
  roster.participants.push_back(peer);
  assert(nyx::CallRosterMessage::decode(roster.encode())->participants.size() == 1);

  nyx::CallPeerIntroMessage intro;
  intro.call_id = id;
  intro.peer.user_id = peer;
  intro.peer.host = "10.0.0.2";
  intro.peer.port = 4040;
  auto intro_d = nyx::CallPeerIntroMessage::decode(intro.encode());
  assert(intro_d && intro_d->peer.host == "10.0.0.2" && intro_d->peer.port == 4040);

  nyx::CallEndpointMessage ep;
  ep.call_id = id;
  ep.self.user_id = peer;
  ep.self.host = "192.168.1.10";
  ep.self.port = 5050;
  auto ep_d = nyx::CallEndpointMessage::decode(ep.encode());
  assert(ep_d && ep_d->self.host == "192.168.1.10" && ep_d->self.port == 5050);
  assert(nyx::kMaxCallParticipants == 20);

  nyx::CallPeerGoneMessage gone;
  gone.call_id = id;
  gone.user_id = peer;
  assert(nyx::CallPeerGoneMessage::decode(gone.encode()));

  nyx::CallLeaveAckMessage leave_ack;
  leave_ack.call_id = id;
  leave_ack.user_id = peer;
  auto leave_ack_d = nyx::CallLeaveAckMessage::decode(leave_ack.encode());
  assert(leave_ack_d && leave_ack_d->user_id == peer);

  nyx::CallRelayCandidateMessage candidate;
  candidate.call_id = id;
  candidate.user_id = peer;
  candidate.score = 750;
  auto candidate_d = nyx::CallRelayCandidateMessage::decode(candidate.encode());
  assert(candidate_d && candidate_d->score == 750);

  nyx::CallRelaySetMessage relay_set;
  relay_set.call_id = id;
  relay_set.epoch = 3;
  relay_set.relays = {peer};
  auto relay_set_d = nyx::CallRelaySetMessage::decode(relay_set.encode());
  assert(relay_set_d && relay_set_d->epoch == 3 && relay_set_d->relays == relay_set.relays);

  const auto hex = nyx::call_id_hex(id);
  nyx::CallId back {};
  assert(nyx::call_id_from_hex(hex, back) && back == id);
  std::cout << "call proto roundtrip ok\n";
}

static void test_call_session_fsm() {
  nyx::CallSession a;
  nyx::UserId peer {};
  peer[0] = 7;
  assert(a.start_outgoing(nyx::CallMode::Audio, nyx::CallScope::Direct, peer));
  assert(a.state == nyx::CallState::Outgoing);

  nyx::CallRingingMessage ring;
  ring.call_id = a.call_id;
  assert(a.on_ringing(ring));
  assert(a.state == nyx::CallState::Ringing);

  nyx::CallAcceptMessage acc;
  acc.call_id = a.call_id;
  acc.mode = nyx::CallMode::AudioVideo;
  assert(a.on_accept(acc));
  assert(a.state == nyx::CallState::Active);
  assert(a.hangup());
  assert(a.state == nyx::CallState::Ended);

  nyx::CallSession b;
  nyx::CallInviteMessage inv;
  inv.call_id = nyx::generate_call_id();
  inv.mode = nyx::CallMode::Audio;
  inv.scope = nyx::CallScope::Direct;
  inv.group_or_peer = peer;
  assert(b.on_invite(inv));
  assert(b.state == nyx::CallState::Incoming);
  assert(b.accept(nyx::CallMode::Audio));
  assert(b.state == nyx::CallState::Active);

  nyx::CallSession busy;
  assert(busy.start_outgoing(nyx::CallMode::Audio, nyx::CallScope::Direct, peer));
  assert(!busy.on_invite(inv));
  std::cout << "call session fsm ok\n";
}

static void test_call_media_and_opus() {
  nyx::CallMediaFrame f;
  f.type = nyx::CallMediaType::Opus;
  f.seq = 42;
  f.payload = {1, 2, 3, 4};
  auto d = nyx::CallMediaFrame::decode(f.encode());
  assert(d && d->seq == 42 && d->payload.size() == 4);

  f.origin[0] = 0x42;
  f.hop_count = 1;
  f.audio_level = 99;
  d = nyx::CallMediaFrame::decode(f.encode());
  assert(d && d->origin == f.origin && d->hop_count == 1 && d->audio_level == 99 &&
         d->payload == f.payload);


  nyx::CallMediaFrame fat;
  fat.type = nyx::CallMediaType::Opus;
  fat.seq = 1;
  fat.origin[0] = 1;
  fat.payload.assign(nyx::kMaxCallMediaPayload, 0x7f);
  const auto fat_wire = fat.encode();
  assert(fat_wire.size() <= 1100);
  assert(fat_wire.size() == 1 + 4 + nyx::kPublicKeySize + 2 + nyx::kMaxCallMediaPayload);

  nyx::OpusEncoderWrap enc;
  nyx::OpusDecoderWrap dec;
  assert(enc.ok() && dec.ok());
  std::vector<int16_t> pcm(static_cast<std::size_t>(nyx::kCallAudioFrameSamples), 0);
  for (int i = 0; i < nyx::kCallAudioFrameSamples; ++i) {
    pcm[static_cast<std::size_t>(i)] =
        static_cast<int16_t>(3000 * std::sin(2.0 * 3.141592653589793 * 440.0 * i / 48000.0));
  }
  auto packet = enc.encode(pcm.data(), nyx::kCallAudioFrameSamples);
  assert(packet && !packet->empty());
  auto back = dec.decode(packet->data(), packet->size());
  assert(back && back->size() == static_cast<std::size_t>(nyx::kCallAudioFrameSamples));

  nyx::CallMediaFrame opus_frame;
  opus_frame.type = nyx::CallMediaType::Opus;
  opus_frame.seq = 9;
  opus_frame.payload = *packet;
  assert(opus_frame.encode().size() <= 1100);
  std::cout << "call media and opus ok\n";
}

static void test_call_av1_fragment() {
  nyx::ByteBuffer big(2500, 0xAB);
  auto frags = nyx::fragment_av1_frame(7, true, big, nyx::kMaxCallMediaPayload);
  assert(!frags.empty());
  nyx::CallVideoReassembler reasm;
  std::optional<nyx::CallVideoReassembler::Assembled> full;
  for (const auto& f : frags) {
    if (auto assembled = reasm.push(f))
      full = std::move(assembled);
  }
  assert(full && full->data.size() == big.size());
  assert(full->keyframe);
  assert(std::equal(full->data.begin(), full->data.end(), big.begin()));


  nyx::CallVideoReassembler fec_reasm;
  full.reset();
  for (std::size_t i = 0; i < frags.size(); ++i) {
    if (i == 2)
      continue;
    if (auto assembled = fec_reasm.push(frags[i]))
      full = std::move(assembled);
  }
  assert(full && full->data == big);

  nyx::Av1Encoder enc;
  nyx::Av1Decoder dec;
  if (!enc.ok() || !dec.ok()) {
    std::cout << "call av1 fragment ok (codec unavailable, frag only)\n";
    return;
  }
  const int w = nyx::kCallVideoWidth;
  const int h = nyx::kCallVideoHeight;
  std::vector<uint8_t> i420(static_cast<std::size_t>(w * h * 3 / 2), 128);
  std::fill(i420.begin(), i420.begin() + w * h, 96);
  auto encoded = enc.encode_i420(i420.data(), w, h, true);
  assert(encoded && !encoded->empty());
  auto decoded = dec.decode(encoded->data(), encoded->size());
  assert(decoded && decoded->width == w && decoded->height == h);
  const int y_size = w * h;
  const int uv_size = (w / 2) * (h / 2);
  for (int x = 32; x < w - 32; ++x) {
    assert(std::abs(static_cast<int>(decoded->i420[h / 2 * w + x]) - 96) < 16);
  }
  assert(std::abs(static_cast<int>(decoded->i420[y_size + uv_size / 2]) - 128) < 16);
  assert(std::abs(static_cast<int>(decoded->i420[y_size + uv_size + uv_size / 2]) - 128) < 16);
  std::cout << "call av1 fragment ok\n";
}

static void test_call_mesh_loopback() {
  nyx::UserId a {};
  nyx::UserId b {};
  a[0] = 1;
  b[0] = 2;
  const nyx::CallId id = nyx::generate_call_id();

  nyx::CallMesh ma;
  nyx::CallMesh mb;
  assert(ma.start(id, a));
  assert(mb.start(id, b));

  nyx::CallPeerEndpoint ea;
  ea.user_id = a;
  ea.host = "127.0.0.1";
  ea.port = ma.local_port();
  nyx::CallPeerEndpoint eb;
  eb.user_id = b;
  eb.host = "127.0.0.1";
  eb.port = mb.local_port();

  ma.upsert_peer(eb);
  mb.upsert_peer(ea);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (std::chrono::steady_clock::now() < deadline) {
    ma.poll();
    mb.poll();
    if (ma.established_count() >= 1 && mb.established_count() >= 1)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(ma.established_count() >= 1);
  assert(mb.established_count() >= 1);

  bool got = false;
  mb.set_on_realtime([&](const nyx::UserId&, nyx::ByteBuffer raw) {
    got = (raw.size() == 3 && raw[0] == 'n' && raw[1] == 'y' && raw[2] == 'x');
  });
  const nyx::ByteBuffer payload = {'n', 'y', 'x'};
  assert(ma.send_realtime(payload));
  const auto recv_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!got && std::chrono::steady_clock::now() < recv_deadline) {
    ma.poll();
    mb.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(got);
  std::cout << "call mesh loopback ok\n";
}

static void test_call_relay_topology_20() {
  std::vector<std::pair<nyx::UserId, uint16_t>> candidates;
  for (uint8_t i = 1; i <= 20; ++i) {
    nyx::UserId id {};
    id[0] = i;
    candidates.emplace_back(id, static_cast<uint16_t>(100 + i));
  }
  const auto relays = nyx::select_call_relays(candidates, 20);
  assert(relays.size() == 3);
  assert(relays[0][0] == 20 && relays[1][0] == 19 && relays[2][0] == 18);
  for (const auto& [leaf, _] : candidates) {
    const auto targets = nyx::call_relay_targets(leaf, relays);
    assert(targets.size() == 2);
    assert(targets[0] != targets[1]);
    assert(std::find(relays.begin(), relays.end(), targets[0]) != relays.end());
    assert(std::find(relays.begin(), relays.end(), targets[1]) != relays.end());
  }
  const auto again = nyx::select_call_relays(candidates, 20);
  assert(again == relays);
  std::cout << "call relay topology 20 ok\n";
}

static void test_file_access_roles() {
  std::remove(nyx::FileAccessStore::store_path().c_str());
  nyx::GroupId gid {};
  nyx::random_bytes(gid.data(), gid.size());
  nyx::GroupRecord group;
  group.id = gid;
  group.name = "test-field";
  nyx::UserId owner {};
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
  assert(store.has_permission(store.permissions_for(gid, owner), nyx::FilePermission::ManageRoles));

  nyx::UserId member {};
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

  assert(store.set_path_member_role(
      gid, "C:/Share", "StratumD", member, nyx::FileAccessStore::role_id_viewer()));
  assert(store.save());
  nyx::FileAccessStore grants_reloaded;
  assert(grants_reloaded.load());
  const auto* gp = grants_reloaded.find_policy(gid);
  assert(gp);
  assert(gp->root_grants.size() == 1);
  assert(gp->root_grants[0].relative_path == "StratumD");

  assert(
      store.set_root_member_role(gid, "C:/Share", member, nyx::FileAccessStore::role_id_viewer()));
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

  nyx::GroupId gid {};
  nyx::random_bytes(gid.data(), gid.size());
  nyx::GroupId zero {};

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
    server = nyx::Connection::accept_responder(std::move(listen_sock), host, port, &*packet);
  });

  auto client =
      nyx::Connection::connect_initiator(std::move(connect_sock), "127.0.0.1", listen_port);
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
      if (stream_id == nyx::kBulkStream)
        fs_client.handle_bulk(payload);
    }
    while (server->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kBulkStream)
        fs_server.handle_bulk(payload);
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
      if (stream_id == nyx::kBulkStream)
        fs_client.handle_bulk(payload);
    }
    while (server->recv_stream(stream_id, payload)) {
      if (stream_id == nyx::kBulkStream)
        fs_server.handle_bulk(payload);
    }
    nyx::FileHash verify {};
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
      if (b.recv_stream(sid, payload))
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto decoded = nyx::decode_text_message(payload);
    assert(decoded && *decoded == text);
  };

  for (int round = 0; round < 2; ++round) {
    nyx::UdpSocket listen_sock;
    nyx::UdpSocket connect_sock;
    NYX_REQUIRE(listen_sock.bind("127.0.0.1", 0));
    NYX_REQUIRE(connect_sock.bind("127.0.0.1", 0));
    const uint16_t listen_port = listen_sock.local_port();

    std::optional<nyx::Connection> server;
    std::thread accept_thread([&] { server = accept_one(std::move(listen_sock)); });

    auto client =
        nyx::Connection::connect_initiator(std::move(connect_sock), "127.0.0.1", listen_port);
    accept_thread.join();
    NYX_REQUIRE(client && server);
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
  nyx::RendezvousServer a {"10.0.0.1", 3478, "test"};
  cfg.rendezvous_servers = {a};
  assert(cfg.save());

  nyx::NetworkConfig loaded;
  assert(loaded.load());
  assert(loaded.mode == nyx::DiscoveryMode::Internet);
  assert(loaded.rendezvous_servers.size() == 1);
  assert(loaded.rendezvous_servers[0].host == "10.0.0.1");

  std::remove(path.c_str());
  if (std::ifstream(backup).good())
    std::rename(backup.c_str(), path.c_str());
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
  for (unsigned char c : phrase)
    upper.push_back(static_cast<char>(std::toupper(c)));
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
  if (prev_appdata.empty())
    _putenv_s("APPDATA", "");
  else
    _putenv_s("APPDATA", prev_appdata.c_str());
#else
  if (prev_home.empty())
    unsetenv("HOME");
  else
    setenv("HOME", prev_home.c_str(), 1);
#endif
  std::filesystem::remove_all(tmp);
  std::cout << "account recovery and remember ok\n";
}

int main() {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  const std::string test_data_root = "test_nyx_data";
  std::filesystem::remove_all(test_data_root);
  nyx::set_base_data_root(test_data_root);
  assert(nyx::ensure_data_dir());
  test_frame_roundtrip();
  test_noise_handshake();
  test_reliable();
  test_rendezvous_hint();
  test_rendezvous_client();
  test_node_flow();
  test_udp_connection();
  test_chat_echo();
  test_realtime_bidirectional();
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
  test_file_v2_and_scoped_index();
  test_file_index_migration_and_objects();
  test_file_catalog_snapshot_semantics();
#ifdef _WIN32
  test_file_index_unicode();
#endif
  test_call_media_and_opus();
  test_call_av1_fragment();
  test_group_member_persistence();
  test_profile_meta_photos_wire();
  test_avatar_proto_roundtrip();
  test_control_message_roundtrip();
  test_group_join_roundtrip();
  test_file_transfer_proto_roundtrip();
  test_avatar_deny_roundtrip();
  test_markdown_to_html();
  test_group_meta_message();
  test_call_proto_roundtrip();
  test_call_session_fsm();
  test_call_mesh_loopback();
  test_call_relay_topology_20();

  {
    nyx::CallSession host;
    nyx::UserId gid {};
    gid[0] = 1;
    assert(host.open_field_room(nyx::CallMode::Audio, gid));
    assert(host.state == nyx::CallState::Active);
    nyx::CallAcceptMessage join;
    join.call_id = host.call_id;
    join.mode = nyx::CallMode::Audio;
    assert(host.on_accept(join));
    assert(host.state == nyx::CallState::Active);
    nyx::CallRejectMessage rej;
    rej.call_id = host.call_id;
    assert(!host.on_reject(rej));
    assert(host.state == nyx::CallState::Active);
    assert(nyx::can_start_field_call(nyx::GroupRole::Owner));
    assert(nyx::can_start_field_call(nyx::GroupRole::Host));
    assert(nyx::can_start_field_call(nyx::GroupRole::Member));
  }
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
  test_file_transfer_1mb();
  nyx::set_base_data_root({});
  std::filesystem::remove_all(test_data_root);
  std::cout << "all tests passed\n";
  return 0;
}
