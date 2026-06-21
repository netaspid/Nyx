# Эволюция протокола Nyx

Ревью текущего состояния (v1) и план доведения до уровня production-grade P2P мессенджеров.

## Текущий стек (v1)

| Слой | Реализация | Оценка |
|------|------------|--------|
| Handshake | Noise_XX, ChaChaPoly, prologue `Nyxv1` | ✓ современная база |
| Транспорт | Selective Repeat ARQ, RTO RFC6298 | ✓ |
| NAT | Rendezvous + STUN hint + hole punch | ⚠ без TURN/relay |
| Мультиплекс | Stream 0 control, 1 chat, 2 bulk | ✓ |
| Rekey | 1 GB / 24h, SHA-256 KDF | ✓ |
| Identity | Ed25519 profile; ephemeral Noise keys per session | ⚠ нет binding static→Noise |

## Целевые SLO (фаза 11)

| Метрика | Цель | Сейчас |
|---------|------|--------|
| Разрыв сессии (30 дней) | ≤ 0.000001% | не измерено |
| Потеря приложенческих сообщений | ≤ 0.000001% | ARQ без FEC |
| Восстановление после обрыва | < 3 s | ручной reconnect |
| DPI/obfuscation | traffic indistinguishable | plaintext UDP framing |

> 0.000001% = **1 на 100 миллионов** событий. Достижимо только комбинацией: multipath, FEC, relay fallback, adaptive codec, formal monitoring.

## Фаза 10 — Rendezvous & Discovery (production)

- [x] Multi-rendezvous failover (`RendezvousPool`, `network.json`)
- [x] Register refresh каждые 120 s
- [x] Rate limit на сервере
- [x] VDS deploy (systemd, Docker, docs)
- [ ] Register ACK + signed register (HMAC operator key)
- [ ] TLS-over-UDP или QUIC bootstrap channel
- [ ] TURN relay для symmetric NAT (10% worst-case)
- [ ] Correct STUN mapped port (не local port)

## Фаза 11 — Надёжность «carrier-grade»

### 11.1 Transport hardening

1. **FEC (Reed-Solomon / RaptorQ)** поверх ARQ — восстановление без retransmit на потерях ≤15%.
2. **Multipath UDP** — 2+ сокета/пути, дублирование critical control.
3. **0-RTT session resume** — ticket после первого Noise, повторное подключение без полного XX.
4. **Adaptive framing** — MTU discovery, padding against traffic analysis.
5. **Connection supervisor** — exponential backoff reconnect, state sync from MessageStore.

### 11.2 DPI / censorship resistance

1. **Obfuscated framing** — убрать magic `Nyx` в cleartext; выглядеть как random/quic-like.
2. **Port hopping** — согласованный seed из handshake hash.
3. **Pluggable transports** — WebSocket/TLS443 fallback через relay (optional).
4. **Domain fronting / ECH** — для bootstrap HTTPS (post-MVP).

### 11.3 Protocol standards alignment

| Стандарт / RFC | Применение |
|----------------|------------|
| RFC 6347 (DTLS) | Сравнить с Noise-over-UDP; документировать отличия |
| RFC 8446 (TLS 1.3) | Для rendezvous control channel |
| RFC 5389 (STUN) | Полный STUN client, не только Google |
| RFC 5766 (TURN) | Relay fallback |
| RFC 6455 (WebSocket) | Optional transport |
| MLS (RFC 9420) | Групповое E2E (post-MVP, замена star hub) |

### 11.4 Identity & binding

- Static Ed25519 в Noise XX (вместо ephemeral-only)
- Invite token = HMAC(server_secret, pubkey) или signed capability
- EndpointHint nonce verification at connect

## Фаза 12 — Формальная верификация

- Model checking handshake (Noise Explorer)
- Fuzz: `tests/fuzz_frame.cpp`, `scripts/pen-test-rendezvous.ps1`
- External crypto audit (Trail of Bits / NCC) — перед public beta

## Roadmap приоритет (ближайшие спринты)

1. STUN mapped port fix + TURN minimal relay
2. Register ACK + network.json sync ✓
3. Session resume ticket
4. FEC on bulk stream (files)
5. Obfuscated wire header v2

См. [ROADMAP.md](ROADMAP.md), [SECURITY_AUDIT.md](SECURITY_AUDIT.md), [protocol.md](protocol.md).
