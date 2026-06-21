# Архитектура Nyx

Версия документа: 0.1 (соответствует коду прототипа транспорта).

## Целевая картина (MVP и дальше)

```
┌─────────────────────────────────────────────────────────────┐
│  nyx-app (Qt/QML): чаты, файлы, группы — Telegram-like    │
├─────────────────────────────────────────────────────────────┤
│  AppCore: NodeService, модели для UI, без протокола в QML  │
├─────────────────────────────────────────────────────────────┤
│  CLI nyx-node (отладка) · прикладной слой messaging/files  │
├─────────────────────────────────────────────────────────────┤
│  Идентичность: ключи + отображаемые никнеймы, контакты      │
├─────────────────────────────────────────────────────────────┤
│  Индекс файлов: сканирование, метаданные, выдача по запросу │
├─────────────────────────────────────────────────────────────┤
│  Connection + Multiplexer (потоки: control, data, bulk)     │
├─────────────────────────────────────────────────────────────┤
│  Crypto (Noise handshake + Session)                         │
├─────────────────────────────────────────────────────────────┤
│  ReliableSession (ARQ, фрагментация)                        │
├─────────────────────────────────────────────────────────────┤
│  Proto (Frame, rendezvous, control messages)                │
├─────────────────────────────────────────────────────────────┤
│  UdpSocket + NAT (rendezvous client, hole punch)            │
└─────────────────────────────────────────────────────────────┘
```

Сейчас реализованы нижние шесть уровней, личный мессенджер 1:1 (CLI) и идентичность. **Поля (группы)** — фаза 5, обязательны для MVP; см. раздел ниже.

## Поля (группы) — целевая модель MVP

Поле — центральная сущность MVP наряду с личкой и файлами:

```
                    ┌─────────────┐
                    │  Rendezvous │
                    └──────┬──────┘
           invite (1:1)    │    group invite
                ┌──────────┼──────────┐
                ▼          ▼          ▼
            Узел A    Создатель    Узел C
            (личка)   поля «X»      (join)
                │          │          │
                │     fan-out Msg     │
                └──────────┴──────────┘
                      участники поля
```

| Компонент | Личка (сейчас) | Поле (фаза 5) |
|-----------|----------------|---------------|
| Подключение | `listen` / `connect --token` | `group create` / `group join --token` |
| История | `chats/<peer_id>.jsonl` | `groups/<group_id>.jsonl` |
| Файлы | между двумя peer (фаза 4) | политика привязана к `GroupId` |
| Топология P2P | один Connection | star через создателя (MVP) |

Модули: `include/nyx/groups/` (план), `GroupId`, roster, group invite; messaging маршрутизирует по `ChatId`.

## Принципы

1. **Анонимность сессии.** Handshake на эфемерных ключах; invite token не равен identity пользователя.
2. **Разделение слоёв.** Каждый модуль знает только о соседе ниже. Прикладной код не собирает кадры вручную.
3. **Потоки вместо множества соединений.** Один UDP + Noise session, несколько logical stream.
4. **Документация рядом с кодом.** Публичный API в `include/nyx/` с комментариями; архитектура и сценарии — в `docs/`.
5. **Читаемость на каждом этапе.** После каждой фазы roadmap — рефакторинг имён, файлов, границ модулей (см. ROADMAP.md).

## Структура репозитория

```
game.ru/
├── apps/
│   ├── nyx-app/           GUI (Qt 6 QML) + appcore/NodeService
│   ├── nyx-node/          CLI узла (listen / connect)
│   └── nyx-rendezvous/    Bootstrap-сервер токенов
├── include/nyx/           Публичные заголовки библиотеки
├── src/                    Реализации модулей
├── tests/                  Минимальные unit/integration тесты
├── cmake/                  Вспомогательные файлы сборки (noise-c, MinGW)
├── docs/                   Документация (этот каталог)
└── CMakeLists.txt
```

Зависимость: **noise-c** (FetchContent), C++17, Winsock2 на Windows.

## Модули библиотеки

### types.hpp

Общие константы и перечисления: `PacketType`, `StreamType`, `ConnectionState`, размеры заголовков.

### proto.hpp / proto.cpp

**Wire format.** Структуры `Frame`, `EndpointHint`, `RendezvousMessage`, `ControlMessage`. Кодирование little-endian, магия `Nyx`.

Не зависит от сети и криптографии.

### util.hpp / util.cpp

CRC32, hex, чтение/запись целых LE, `random_bytes`.

### udp.hpp / udp.cpp

Блокирующий кроссплатформенный UDP: `bind`, `send_to`, `recv_from` с опциональным timeout.

### crypto.hpp / crypto.cpp

- `HandshakeDriver` — пошаговый Noise XX (prologue `Nyxv1`).
- `Session` — encrypt/decrypt после split.

Обёртка над noise-c; указатели на состояние opaque в заголовке.

### transport.hpp / transport.cpp

`ReliableSession`: selective repeat, окно, фрагментация по MTU, сборка сообщений, ACK/SACK.

На проводе — кадры типа `Data` и `Ack`.

### mux.hpp / mux.cpp

`Multiplexer`: поток 0 = control (Ping/Pong, Open/Close stream), пользовательские потоки с очередями.

Формат внутри шифрованной нагрузки: `u32 stream_id` + bytes.

### connection.hpp / connection.cpp + rendezvous.cpp

- `RendezvousClient` — register/lookup.
- `make_hint` (connection), `hole_punch`, STUN (`nat.hpp`).
- `Connection` — orchestration: handshake, encrypt, mux, reliable, recv_stream.
- Фаза 1: `drive()` (keep-alive ping ~15 с, таймаут peer 45 с), `send_text()`, `peer_alive()`.
- Константа `kChatStream = 1` в `types.hpp` — текстовый echo в CLI.

Единая точка входа для приложения.

## Диаграмма: установка соединения

```
  Узел A (listen)              Rendezvous           Узел B (connect)
       |                            |                      |
       |--- Register(token,hint) --->|                      |
       |                            |                      |
       |                            |<-- Lookup(token) ----|
       |                            |--- Response(hint) -->|
       |                            |                      |
       |<=========== hole punch / HandshakeInit ============|
       |<=========== HandshakeResp =========================|
       |<=========== HandshakeFinish =======================|
       |                            |                      |
       |<=========== encrypted streams (Ping, data) =======>|
```

## Диаграмма: стек данных (установленное соединение)

```
send_stream(stream_id, data)
    │
    ├─► mux_.send        → [stream_id|payload]
    ├─► session_.encrypt → ciphertext
    ├─► reliable_.send   → [Frame Data × N fragments]
    └─► socket_.send_to  → UDP

recv path:
UDP → reliable_.recv_wire → poll_recv → decrypt → mux_.push/recv
```

## Приложения

### nyx-rendezvous

Один UDP socket, map `hex(token) → {hint, expires}`. Обработка Register и Lookup. Без TLS и без авторизации операторов.

### nyx-node

Разделён на модули:

| Файл | Роль |
|------|------|
| `main.cpp` | Точка входа |
| `cli.hpp/cpp` | Парсинг argv, сообщения об ошибках на русском |
| `node_session.hpp/cpp` | `run_listen`, `run_connect`, `run_chat_session` |

**Прикладной слой v0 (фаза 1):** после handshake узел не завершается. Фоновый поток вызывает `Connection::drive()` (keep-alive ping, таймаут peer). Основной поток читает stdin и шлёт строки через `Connection::send_text(kChatStream, ...)`. Входящие на потоке 1 выводятся как `peer> ...`.

Бизнес-логики мессенджера (история, никнеймы, формат сообщений) пока нет — только сырой UTF-8 echo.

## Безопасность (текущее состояние)

| Аспект | Статус |
|--------|--------|
| Конфиденциальность payload после handshake | Да (ChaCha20-Poly1305) |
| Аутентичность peer в сессии | Да (Noise) |
| Долгоживущая identity | Нет |
| Метаданные rendezvous | Видны оператору bootstrap |
| Forward secrecy между сессиями | Да (новые ephemeral ключи) |
| Rekey по объёму/времени | ✓ `ControlKind::Rekey`, 1 GB / 24 h |

## Точки расширения (заложены в дизайн)

- Новые `PacketType` и `ControlKind` без смены магии версии 1.
- `StreamType`: Bulk для файлов, Realtime для голоса/видео позже.
- Отдельный модуль identity поверх Connection (не в proto).
- FileIndex как служба с API «запросить метаданные / chunk» поверх Bulk stream.

## Связанные документы

- [APPLICATION.md](APPLICATION.md) — сценарии запуска
- [protocol.md](protocol.md) — байтовая спецификация
- [ROADMAP.md](ROADMAP.md) — план до MVP
- [GLOSSARY.md](GLOSSARY.md) — термины
