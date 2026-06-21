# Roadmap до MVP

Документ для кодинг-агента и для человека, который принимает результат. Каждый этап заканчивается **рабочим сценарием**, **минимальными тестами**, **рефакторингом на читаемость** и **обновлением docs/**.

## Цель MVP

Два компьютера в интернете или LAN:

1. Запускают приложение, видят себя под **никнеймом** (не обязательно совпадает с ключом).
2. Находят друг друга (invite link / discovery в пределах «поля»).
3. Обмениваются **текстовыми сообщениями** в личке.
4. Один пользователь **индексирует выбранные папки**, второй может **скачать файл** по запросу.
5. Можно **создать группу**, добавить участников, делиться файлами с политикой «только для этой группы».
6. Трафик между узлами **шифрован**; персональные данные не хранятся на rendezvous.
7. Основной способ работы для пользователя — **десктопное приложение с графическим интерфейсом** (стиль Telegram: чаты, иконки, тёмная/светлая тема). CLI остаётся для отладки и автоматизации.

Не входит в MVP: мобильные клиенты, видеозвонки, полная децентрализация rendezvous, формальный аудит криптографии.

---

## Поля (группы) — обязательная часть MVP

**Поле** — это групповой чат с несколькими участниками. Пользователь может **создать поле**, **пригласить новых участников** и общаться/делиться файлами **только в рамках этого поля**. Это не опциональная «фича на потом»: без полей MVP не считается выполненным (п. 5 цели выше).

| Сейчас (фазы 0–3) | Цель MVP (фазы 4–5) |
|-------------------|---------------------|
| Личка 1:1 по invite token | Личка + **групповые поля** |
| Один peer на Connection | Несколько участников в одном поле |
| История `%APPDATA%/Nyx/chats/<peer_id>.jsonl` | + история по `group_id`, политики файлов на поле |

**Модель для MVP (фаза 5):**

```
Создатель поля
    │
    ├─► GroupId (случайный id или hash от ключа создателя + nonce)
    ├─► Group invite token → rendezvous (как listen, но привязан к group)
    │
    ├─► Участник B: group join → handshake → Hello → в списке members
    ├─► Участник C: тот же invite → добавляется в members
    │
    └─► Сообщения: fan-out через создателя (star-topology) — допустимо для MVP
        Файлы: политика «эта папка видна только полю X»
```

**CLI (фаза 5):** `nyx-node group create`, `group invite`, `group join` — параллельно с GUI.

**Discovery «в пределах поля»** (п. 2 цели MVP): участники, уже состоящие в поле, видят друг друга в списке группы; новый человек попадает в поле только по **group invite**, не через глобальный каталог.

Текущий код (фаза 3) закладывает основу: `UserId`, `ChatMessage`, `MessageStore` — в фазе 5 к ним добавятся `GroupId`, `ChatId` (личка vs поле) и маршрутизация сообщений.

---

## Стратегия GUI

Графический интерфейс строится **поверх** библиотеки `Nyx`, без дублирования протокола в UI.

```
┌──────────────────────────────────────────┐
│  nyx-app (Qt 6 / QML) — Telegram-like   │  ← фазы 2–5 экраны, фаза 8 polish
├──────────────────────────────────────────┤
│  AppCore (C++): NodeService, ChatStore   │  ← тонкий слой между UI и Nyx
├──────────────────────────────────────────┤
│  libnyx (транспорт, messaging, files)   │
└──────────────────────────────────────────┘
```

**Стек (рекомендация для агента):** Qt 6, Qt Quick (QML), иконки SVG (Material Symbols / Phosphor / кастомный набор в `resources/icons/`). Сборка: отдельная цель CMake `nyx-app`, линкуется с `Nyx`.

**Принципы UI:**
- Внешний вид близок к Telegram: список чатов слева, переписка справа, пузыри сообщений, аватар с инициалами, статус «в сети» / «был(а)».
- Тема: светлая и тёмная, акцентный цвет настраивается (по умолчанию синий/teal).
- UI не блокирует сеть: `NodeService` в фоновом потоке, в QML только модели (`QAbstractListModel`).
- Каждая фаза backend добавляет **экран или панель** в GUI, а не откладывает UI «на конец».

**CLI vs GUI:** `nyx-node` остаётся; `nyx-app` — продукт для пользователя. Общая логика выносится в `apps/common/` или `src/app/`.

---

## Фаза 0 — Базовый транспорт (готово)

**Статус:** выполнено.

| Результат | Проверка человеком |
|-----------|-------------------|
| rendezvous + listen + connect + ping | Три терминала, вывод `connected` |
| nyx-tests зелёные | `.\build\nyx-tests.exe` |

**Рефакторинг (сделать при ближайшем касании):**
- [x] `.gitignore` для build и temp (`listen_out.txt`, `scratch_test.cpp`)
- [ ] Комментарии ко всем публичным функциям в `include/nyx/` (в процессе)
- [ ] Вынести `rendezvous.cpp` логику из connection.hpp циклической зависимости в `nat.hpp`
- [x] Удалить временные файлы из репозитория

---

## Фаза 1 — Стабильное соединение и CLI v2

**Статус:** backend ✓. **GUI:** `nyx-app` ✓ (listen, connect, чат, LAN, поля).

### GUI (заготовка)

- [x] Каркас `apps/nyx-app/`: окно, панели, лог
- [x] Подключение: token, listen, LAN browse
- [x] Чат с пузырями
- [x] Системный tray (сворачивание, меню, уведомления)

**Цель:** узел не падает после connect; можно отправить строку и увидеть её на другой стороне.

### Задачи агента

1. **Интерактивный режим** `nyx-node`: после connect/listen цикл ввода строки → отправка на stream 1 → вывод входящих. ✓
2. **Keep-alive:** периодический ping, таймаут «peer мёртв». ✓
3. **Обработка ошибок CLI:** понятные сообщения (lookup failed, handshake timeout, bind failed). ✓
4. **Тест:** интеграционный тест «две нити, echo одной строки» без ручного CLI. ✓

### Рефакторинг

- Разделить `apps/nyx-node/main.cpp` на `cli/` (parse args) и `node_session` (работа с Connection). ✓
- Имена без аббревиатур там, где длина не критична: `run_handshake` оставить, `hs` в новом коде не использовать.

### Документация

- Обновить APPLICATION.md: интерактивный режим.
- ARCHITECTURE.md: блок «Прикладной слой v0».

### Критерий приёмки (человек)

Два терминала: на одном `listen`, на другом `connect`, ввести «привет» на B, увидеть на A.

### GUI (заготовка)

- [x] Каркас `apps/nyx-app/`: окно, панели, лог
- [x] Экран подключения: token, listen, LAN
- [x] Чат (не заглушка)

---

## Фаза 2 — Идентичность и никнеймы

**Статус:** backend ✓. **GUI:** nickname/id ✓, onboarding ✓, настройки профиля ✓, last seen ✓.

**Цель:** у пользователя есть ключевая пара (долгоживущая или сессионная+профиль) и **отображаемый nickname**; контакты хранятся локально.

### Задачи агента

1. Модуль `identity/`: генерация ключей, хранение в файле профиля (путь OS-specific, права доступа). ✓
2. Структура `UserId` (публичный ключ или его hash) + `Nickname` (строка, уникальность локально). ✓
3. Протокол control или stream: обмен `Hello { user_id, nickname, capabilities }` после handshake. ✓
4. Локальная книга контактов: nickname, last seen, trust level (заготовка). ✓ (`last_seen_ms`)

### Рефакторинг

- Папка `include/nyx/identity.hpp`, `src/identity/` — не смешивать с proto. ✓
- Константы путей к профилю — один файл `paths.hpp`. ✓

### Тесты

- Roundtrip encode/decode Hello.
- Сохранение и загрузка профиля во временную директорию.

### Критерий приёмки

После connect в логе обе стороны видят nickname друг друга.

### GUI

- [x] **Онбординг:** первый запуск — nickname (`OnboardingDialog.qml`)
- [x] **Профиль:** смена nickname, id в настройках
- [x] **Контакты:** список в `chatList` + last seen
- [x] Design tokens v0: тёмная тема, радиус 12px (`ui/Theme.qml`)

---

## Фаза 3 — Мессенджер (личные сообщения)

**Статус:** backend + CLI + AppCore ✓. **GUI:** переписка 1:1 ✓; список чатов ✓.

**Цель:** текстовый чат 1:1 поверх stream Data, история локально. Код messaging готов к расширению на **поля** (фаза 5): `ChatId`, хранилище по id чата.

### Задачи агента

1. Формат сообщения: id, timestamp, author, text, **chat_id** (MsgV2). ✓
2. Очередь исходящих, ack доставки, retry (Outbox). ✓
3. Локальное хранилище JSON-lines: `%APPDATA%/Nyx/chats/<chat_id>.jsonl`. ✓
4. CLI: `/send`, `/history [N]`, `/search`, статус доставки. ✓
5. **AppCore API:** `ChatService` — `send_message`, `history`, `search`, callbacks. ✓

### GUI (основной чат)

- [x] **Список чатов** (левая колонка): аватар, название, превью, время, badge непрочитанных (`ChatListModel`)
- [x] **Окно переписки:** пузыри входящие/исходящие, поле ввода, отправка.
- [x] **Статус:** «в сети» при активной сессии; last seen из контактов при просмотре истории
- [ ] **Состояния:** «доставлено», «ошибка отправки» с retry.
- [x] **Иконки:** send (SVG), attach (кнопка → drawer файлов), search (фильтр в чате)
- [x] Поиск по истории в текущем чате (локальный фильтр).

### Рефакторинг

- `messaging/` отдельно от `connection`.
- Не более 300 строк на файл; при превышении — разбиение.

### Тесты

- Encode/decode сообщения (MsgV2 + chat_id). ✓
- Два in-memory узла: send → recv → equals. ✓
- 10 сообщений подряд без потерь. ✓

### Критерий приёмки

10 сообщений туда-обратно без потерь на localhost (автотест `ten messages roundtrip ok`). GUI: переписка в `nyx-app` ✓.

---

## Фаза 4 — Индексация и передача файлов

**Статус:** backend ✓; share policy ✓; progress bar в чате ✓.

**Цель:** пользователь указывает папки; другой узел запрашивает файл по hash/пути; передача идёт chunked по Bulk stream.

### Задачи агента

1. **FileIndex:** сканирование, hash (SHA-256), метаданные (размер, mtime, mime). ✓
2. **Политики:** корни с `GroupId` — личка (zero) vs поле. ✓ (`entries_for_session`, `find_for_session`)
3. Протокол: `FileOffer`, `FileRequest`, `FileChunk`, `FileComplete`, `ListReq/Resp` на `kBulkStream`. ✓
4. Прогресс и возобновление (chunk offset). ✓ (offset в чанках; resume после обрыва — позже)
5. **CLI:** `/index`, `/files`, `/remote`, `/get <hash>`, `/sendfile <путь|hash>`. ✓
6. **Transport:** очередь фрагментов ARQ, упорядочивание msg_id для Noise, drain UDP. ✓

### GUI

- [x] **Настройки «Мои папки»:** вкладка «Файлы» — index, remote, get
- [x] **В чате:** progress bar при передаче (фаза 8, частично)
- [x] **В чате:** кнопка скрепки → панель «Файлы»

### Рефакторинг

- `storage/index/` и `storage/transfer/` — разные модули.
- Абстракция `BlobStore` для чтения с диска.

### Тесты

- Индекс папки с 3 файлами. ✓ (`file index three ok`)
- Передача файла 1 МБ между loopback Connection. ✓ (`file transfer 1mb ok`, чанки 8 KiB)
- Share policy: личка vs поле, deny + успешная загрузка. ✓ (`share policy ok`)

### Критерий приёмки

На машине B скачан файл, совпадает hash с оригиналом на A. ✓ (автотест loopback)

---

## Фаза 5 — Группы («поля»)

**Статус:** backend + CLI ✓; share policy ✓ (hub + member + NodeService).

**Цель:** пользователь создаёт **поле** (группу), приглашает участников по invite, ведёт **общий чат** и задаёт **политики файлов только для этого поля**.

### Задачи агента

1. `GroupId`, `ChatId` (личка = f(userA,userB), поле = group_id), список участников, роли (owner, member). ✓
2. **Group invite:** token на rendezvous (тот же Register/Lookup). ✓
3. **Добавление участников:** GroupJoin / JoinAck / MemberJoined на kChatStream. ✓
4. Fan-out: **star через hub создателя** (`GroupHub`). ✓
5. CLI: `group create`, `group hub`, `group join`, `/members`. ✓
6. `MessageStore`: `%APPDATA%/Nyx/groups/<group_id>.jsonl`. ✓
7. **Share policy** (файл только для поля). ✓ (`FileIndex::share_group`, `GroupHub::attach_files`)

### GUI

- [x] **Создание группы / hub / join:** вкладка «Поля»
- [x] **Приглашение:** copy invite (`copyLastGroupInvite`)
- [ ] **Приглашение:** QR (post-polish)
- [x] **Групповой чат:** тот же layout; автор под пузырём
- [x] **Права на файлы:** scope в статусе при индексации

### Рефакторинг

- Вынести group state machine в `groups/state_machine.hpp`.
- Диаграмма в ARCHITECTURE.md обновить.

### Критерий приёмки

**Три участника** в одном поле: создатель + два приглашённых; сообщение от любого видят все. ✓ (`group three members ok`)

Файл с политикой «только поле X» — ✓ (`share policy ok`; hub обрабатывает kBulkStream на соединениях участников).

---

## Фаза 6 — Discovery и NAT hardened

**Статус:** backend ✓, GUI LAN ✓ (авто-скан, кнопка «Подключить»).

### Задачи агента

1. Улучшенный hole punch, параллельные попытки, STUN-like определение внешнего IP. ✓
2. Опционально: несколько rendezvous, DHT заготовка (можно отложить post-MVP).
3. LAN discovery без token: multicast `239.255.77.77:34779`, bind+join fix. ✓

### Критерий приёмки

- [x] LAN: `listen` (mDNS по умолчанию), `browse`, `connect --peer`
- [x] GUI: авто-скан peers, connect из списка
- [x] NAT: hole punch, STUN hint при register
- [x] Индикатор типа соединения в шапке чата (LAN / Интернет / Поле)
- [ ] DHT / несколько rendezvous (post-MVP)

---

## Фаза 7 — Backend polish и релиз ядра

**Статус:** rekey ✓, reconnect test ✓, soak test 24h ☐ (ручной).

### Задачи

1. Логирование в файл, уровни, ротация. ✓ (`Nyx/log.hpp`, `%APPDATA%/Nyx/logs/`)
2. Rekey по protocol.md (1 GB / 24h). ✓ (`ControlKind::Rekey`, SHA-256 KDF)
3. CI: build + nyx-tests + nyx-appcore-tests. ✓
4. Установщик или zip: `scripts/package-win.ps1`. ✓ (zip)
5. Rendezvous URL в настройках GUI. ✓ (поле в сайдбаре)
6. GUI: вкладки **Поля**, **Файлы** (паритет с CLI). ✓
7. **Рефакторинг (DRY/KISS):** `libnyx` helpers (`exchange_hello`, `is_handshake_datagram`, `parse_host_port`, `is_lan_ipv4`), `apps/common` (`nyx-app-common`), split `NodeService`. ✓

### Общие модули libnyx (рефакторинг)

| Модуль | Что вынесено |
|--------|----------------|
| `Nyx/app` | `exchange_hello`, `remember_contact` |
| `Nyx/proto` | `is_handshake_datagram`, `EndpointHint::host_string()` |
| `Nyx/util` | `parse_host_port` |
| `Nyx/nat` | `is_lan_ipv4` |
| `Nyx/paths` | `default_logs_dir`, `default_log_file_path` |

### Критерий приёмки

Сутки работы двух узлов с переподключением; логи без спама; тесты зелёные. Автотест: `reconnect flow ok`.

---

## Фаза 8 — GUI: Telegram-like polish

**Цель:** интерфейс выглядит как современный мессенджер, а не технический прототип. Функции уже есть; фокус на дизайне, иконках, анимациях.

### Визуальный язык

| Элемент | Ориентир |
|---------|----------|
| Layout | Две колонки: чаты ~320px, переписка flex; на узком окне — стек экранов |
| Пузыри | Входящие нейтральные; исходящие accent; скругление 12px |
| Типографика | 14px текст, 12px meta; заголовок чата 16px semibold |
| Темы | Dark (`#17212b`, пузырь `#2b5278`) и Light; переключатель в настройках |
| Анимации | Появление сообщений 150ms, smooth scroll, skeleton при загрузке |
| Иконки | SVG 24px, единый stroke; см. `docs/GUI.md` |

### Задачи агента

1. Design system в QML: `Theme.qml` ✓ (dark/light), `ChatBubble` ✓, `AvatarBadge` ✓, `IconButton` ✓, `EmptyState` ✓.
2. Полный набор иконок: send ✓, attach ✓, search ✓, settings ✓; group, folder, copy, qr, wifi, moon/sun — частично MDL2 + SVG
3. Пустые состояния: «Нет чатов», «Выберите чат», «Нет файлов». ✓
4. Настройки: Профиль ✓, Сеть ✓, Оформление ✓; Конфиденциальность, Папки — заготовки в UI
5. Системные уведомления при новом сообщении (окно свёрнуто). ✓ (tray + flash)
6. Горячие клавиши: Ctrl+Enter ✓, Ctrl+K ✓, Esc ✓.
7. Локализация: `i18n/nyx_ru.ts` ✓ (задел); полный qsTr — в процессе.
8. Доступность: focus ring ✓, контраст Theme ✓; масштаб 125% — `fontScale` в Theme (задел).

### Рефакторинг

- QML до 200 строк на файл; компоненты в `ui/components/`, `ui/panels/`, `ui/dialogs/`. ✓
- Иконки в `resources/icons/`. ✓ (частично)
- Скриншоты: `docs/images/ui/`.

### Критерий приёмки (человек)

1. Тёмная тема, список чатов и переписка выглядят цельно. ✓
2. Сообщение с анимацией; файл с progress bar. ✓ (progress bar в input bar)
3. Light/dark без перезапуска.
4. Скриншоты в docs напоминают Telegram Desktop.

### После MVP (не блокирует)

- Стикеры, реакции, голосовые, обои чата, мобильный клиент.

Подробнее: [GUI.md](GUI.md).

---

## План работ для агента (ближайший спринт)

Приоритет на ближайшие 1–2 сессии:

| # | Задача | Файлы | Done when |
|---|--------|-------|-----------|
| 1 | Документировать публичный API в headers | `include/nyx/*.hpp` | У каждой public функции /** */ на русском |
| 2 | `.gitignore` для build артеfactов и temp | `.gitignore` | Чистый git status |
| 3 | Фаза 1: интерактивный echo chat | `apps/nyx-node/`, `tests/` | ✓ критерий фазы 1 (CLI) |
| 4 | Каркас nyx-app (Qt) + экран подключения | `apps/nyx-app/` | Окно запускается, ввод token |
| 5 | Обновить APPLICATION + ARCHITECTURE | `docs/` | ✓ echo описан; GUI каркас — после п.4 |

После каждого пункта: `cmake --build build`, `nyx-tests`, ручная проверка listen/connect.

---

## Фаза 9 — Rendezvous production & интернет-связь

**Статус:** в работе (multi-RV ✓, deploy docs ✓, GUI сеть ✓).

**Цель:** пользователь поднимает rendezvous на VDS; клиенты стабильно находят друг друга через NAT.

### Задачи

1. `network.json`: список rendezvous, режим Auto/LAN/Internet. ✓
2. `RendezvousPool`: register на все, lookup failover. ✓
3. Register refresh 120 s при listen. ✓
4. Сервер: rate limit, logging, graceful shutdown. ✓
5. Deploy: systemd, Docker, `DEPLOY_RENDEZVOUS.md`. ✓
6. GUI: NetworkSettingsSection, probe server. ✓
7. Register ACK + HMAC. ☐
8. STUN mapped port fix. ☐
9. TURN relay fallback. ☐

### Критерий приёмки

Два клиента за разными NAT подключаются через VDS rendezvous; token живёт >30 мин.

---

## Фаза 10 — Безопасность & pen-test

**Статус:** аудит v1 ✓ (`SECURITY_AUDIT.md`); hardening в процессе.

1. Rate limit rendezvous ✓
2. Pen-test script ☐
3. Signed lookup response ☐
4. Profile encryption / OS keychain ☐
5. External crypto audit ☐

---

## Фаза 11 — Протокол carrier-grade (SLO 10⁻⁶)

**Цель:** ≤0.000001% разрывов/потерь на слабом канале; устойчивость к DPI.

См. [PROTOCOL_EVOLUTION.md](PROTOCOL_EVOLUTION.md):

1. FEC + multipath UDP
2. 0-RTT session resume
3. Obfuscated framing v2
4. TURN + WebSocket fallback
5. Static identity in Noise XX
6. Formal verification & fuzz

**Не блокирует MVP**, но задаёт инженерный потолок проекта.

---

## Правила для всех фаз

1. **Документация обязательна.** Любой PR/коммит меняет поведение → правка `docs/` в том же changeset.
2. **Комментарий к функции** — кратко: что делает, параметры, ошибки, связь с протоколом.
3. **Тесты** — только критический путь; не гнаться за 100% coverage.
4. **Рефакторинг** — не откладывать «на потом»; каждая фаза включает день на читаемость.
5. **Язык docs** — русский, термины из GLOSSARY.md.
6. **Человек проверяет** сценарии из таблиц «Критерий приёмки»; агент не закрывает фазу без галочки в ROADMAP (можно чекбоксы в git).
7. **GUI:** экраны добавляются по фазам 1–6; визуальный polish — фаза 8. QML только через AppCore.

---

## Матрица: фаза → модули

| Фаза | proto | crypto | transport | mux | connection | identity | messaging | files | groups | GUI (nyx-app) |
|------|-------|--------|-----------|-----|------------|----------|-----------|-------|--------|----------------|
| 0 | ✓ | ✓ | ✓ | ✓ | ✓ | | | | | |
| 1 | | | | ✓ | ✓ | | | | | каркас, connect |
| 2 | ✓ | | | | ✓ | ✓ | | | | профиль, контакты |
| 3 | ✓ | | | ✓ | ✓ | ✓ | ✓ | | | чаты, пузыри |
| 4 | ✓ | | ✓ | ✓ | ✓ | | | ✓ | | файлы, progress |
| 5 | ✓ | | | | ✓ | ✓ | ✓ | ✓ | ✓ | группы |
| 6 | | | | | ✓ | | | | | discovery UI |
| 7 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | log, zip, Поля/Файлы UI |
| 8 | | | | | | | | | | design polish |
| 9 | ✓ | | | | ✓ | | | | | network.json, deploy RV |
| 10 | ✓ | ✓ | | | ✓ | ✓ | | | | security audit |
| 11 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | FEC, DPI, SLO 10⁻⁶ |

---

## История изменений roadmap

| Дата | Изменение |
|------|-----------|
| 2026-06-20 | Первая версия после успешного listen/connect/ping на localhost |
| 2026-06-20 | Фаза 6: LAN discovery fix (239.255.77.77:34779), GUI авто-скан |
| 2026-06-21 | Фаза 7: file log, CI appcore-tests, GUI Поля/Файлы, package-win |
| 2026-06-19 | Фазы 9–11: multi-rendezvous, VDS deploy, SECURITY_AUDIT, PROTOCOL_EVOLUTION |
