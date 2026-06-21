# Аудит безопасности Nyx (v1)

Дата: 2026-06-19. Scope: rendezvous, transport, crypto, identity, files, groups.

**Метод:** статический код-ревью + threat model. Pen-test сценарии — в конце.

## Threat model

| Актёр | Возможности |
|-------|-------------|
| Rendezvous operator | Видит token↔IP:port, может rate-limit/DDoS |
| Network attacker (MITM) | Подмена UDP, replay, flood |
| Malicious peer | Отправка файлов, group spam, handshake probe |
| Local attacker | Чтение profile.json, contacts, chat history |

## Сводка находок

| Severity | Location | Finding | Recommendation |
|----------|----------|---------|----------------|
| High | `apps/nyx-rendezvous/` | Нет аутентификации register/lookup; любой может flood | Rate limit ✓; добавить HMAC register |
| High | `src/nat.cpp` | STUN port ≠ mapped port → hole punch fail / wrong hint | Использовать XOR-MAPPED-ADDRESS port |
| Medium | `src/rendezvous.cpp` | Lookup доверяет только source IP сервера | Подпись ответа shared secret |
| Medium | `include/nyx/identity.hpp` | Profile keys plaintext JSON on disk | OS permissions ✓; optional OS keychain |
| Medium | `protocol.md` | Noise ephemeral-only — нет привязки к Ed25519 | Static key в Noise XX |
| Medium | `src/group_hub.cpp` | Hub relay без rate limit per member | Token bucket per connection |
| Low | `EndpointHint.nonce` | Не проверяется при connect | Verify в handshake prologue |
| Low | `src/file_transfer.cpp` | Path traversal в display name | Sanitize `../` в имени файла |
| Info | Rendezvous | Metadata visible to operator | Документировано; zero-knowledge bootstrap — фаза 10 |

## Криптография

**Сильные стороны:**
- Noise XX + ChaCha20-Poly1305
- Rekey с role-aware KDF
- SHA-256 file hashes

**Слабые стороны:**
- Нет forward secrecy между сессиями (новый ephemeral каждый раз — OK, но identity не в handshake)
- Нет formal audit

## Rendezvous pen-test сценарии

Автоматизируемые (`scripts/pen-test-rendezvous.ps1`):

1. **Flood register** — 10k register/s → ожидание rate limit
2. **Lookup brute** — random tokens → only NotFound, no crash
3. **Oversized UDP** — 64KB datagram → ignore
4. **Invalid frame magic** — drop silently

Ручные:

1. Token hijack — зарегистрировать чужой token до victim (race) → mitigated TTL + refresh
2. MITM lookup — подмена hint → mitigated после signed response

## Рекомендации до production

1. Deploy rendezvous на изолированном VDS с rate limit
2. Не хранить secrets в git; rotate operator HMAC key
3. Enable `%APPDATA%/Nyx/logs` rotation (уже есть)
4. Запланировать external crypto audit (фаза 12)
5. CI: добавить `pen-test-rendezvous` job (optional)

## Compliance note

Цель 0.000001% packet loss — **инженерный SLO**, не security control. Достигается transport layer (см. PROTOCOL_EVOLUTION.md), не шифрованием.

См. [DEPLOY_RENDEZVOUS.md](DEPLOY_RENDEZVOUS.md), [PROTOCOL_EVOLUTION.md](PROTOCOL_EVOLUTION.md).
