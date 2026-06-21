# Nyx Protocol Specification v1

## Overview

Nyx is an anonymous peer-to-peer protocol over UDP. Nodes have no persistent identity;
each session uses ephemeral X25519 keys exchanged via the Noise XX handshake pattern.

## Wire Frame Format

All multi-byte integers are **little-endian**.

```
Offset  Size  Field
0       4     Magic (0x444E4554, ASCII "Nyx")
4       1     Version (currently 1)
5       1     PacketType
6       2     Flags
8       4     StreamID
12      4     SeqNum
16      2     PayloadLength
18      N     Payload (0..65535 bytes)
18+N    4     CRC32 (optional, if FLAG_CRC set)
```

### Flags

| Bit | Name        | Description                    |
|-----|-------------|--------------------------------|
| 0   | FLAG_CRC    | CRC32 appended after payload   |
| 1   | FLAG_COMPRESS | Payload is zstd-compressed   Pending |
| 2   | FLAG_FRAGMENT | Payload is a fragment        |

### Packet Types

| Value | Name               | Layer      |
|-------|--------------------|------------|
| 0x01  | HandshakeInit      | Crypto     |
| 0x02  | HandshakeResp      | Crypto     |
| 0x03  | HandshakeFinish    | Crypto     |
| 0x10  | Data               | Transport  |
| 0x11  | Ack                | Transport  |
| 0x12  | Nack               | Transport  |
| 0x20  | KeepAlive          | Connection |
| 0x30  | RendezvousRegister | NAT        |
| 0x31  | RendezvousLookup   | NAT        |
| 0x32  | RendezvousResponse | NAT        |

## Noise Handshake

- Pattern: **Noise_XX_25519_ChaChaPoly_BLAKE2s**
- Prologue: `"Nyxv1"`
- No static keys; both sides use ephemeral keys only
- After handshake, transport messages carry ChaCha20-Poly1305 encrypted payloads

## Stream Multiplexing

- Stream 0 is reserved for **Control** (Ping/Pong, open/close)
- Stream IDs are allocated by the opener (odd/even convention optional)
- Per-stream flow control via credit window (default 256 KB)

### Control Messages (Stream 0)

| Type | Payload        |
|------|----------------|
| 0x01 | Ping (8-byte nonce) |
| 0x02 | Pong (echo nonce)   |
| 0x03 | OpenStream (stream_id, stream_type) |
| 0x04 | CloseStream (stream_id) |

## Rendezvous

1. Host A generates random 32-byte `invite_token`
2. Host A sends `RendezvousRegister { token, endpoint_hint }` to bootstrap
3. Host B sends `RendezvousLookup { token }`
4. Bootstrap returns `RendezvousResponse { endpoint_hint }` (TTL 5 min)
5. Both hosts perform UDP hole punch + Noise handshake

`endpoint_hint` is opaque bytes (IP + port + nonce), not tied to identity.

## Reliability

- Selective Repeat ARQ over UDP
- Initial MTU: 1200 bytes
- RTO = SRTT + 4 × RTTVAR (RFC 6298)
- Congestion window: BBR-lite (bandwidth × min_rtt estimate)

## Rekey

Session keys rotate after **1 GB** transferred or **24 hours**, whichever comes first.
Both peers exchange `ControlKind::Rekey` with monotonic epoch; new keys derived via
SHA-256(handshake_hash ‖ epoch ‖ `"Nyx-tx"` / `"Nyx-rx"`) с учётом роли initiator/responder.
