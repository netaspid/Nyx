# Приложение Nyx: как работает сейчас

Документ описывает поведение кода на текущей стадии (**v0.9 — фазы 1–6 завершены, фаза 7 в работе, рефакторинг слоя приложений**). Обновлять при каждом изменении пользовательских сценариев, CLI или GUI.

## Назначение

Nyx — P2P-мессенджер: rendezvous, шифрование Noise, личный и групповой чат, файлы, LAN discovery. **Основной клиент:** `nyx-app` (Qt). CLI `nyx-node` — для отладки и автоматизации.

## Состав программ

| Программа | Роль |
|-----------|------|
| **`nyx-app`** | GUI (Qt 6 QML): listen/connect/LAN, чат, файлы, поля |
| `nyx-node` | CLI: те же сценарии + команды `/index`, `/get`, `group` |
| `nyx-rendezvous` | Реестр invite-токенов (TTL 5 мин), **не нужен для LAN** |
| `nyx-tests` | Интеграционные тесты libnyx |
| `nyx-appcore-tests` | Тесты `NodeService` без Qt |

Библиотека `Nyx` — протокол. **`nyx-app-common`** — общий код GUI и CLI (метки соединения, connect по hint, цикл чат+файлы). **`nyx-appcore`** — оркестратор для GUI и тестов (`NodeService` разбит на `node_service_{connect,chat,group}.cpp`).

### Слой apps/common

| Модуль | Назначение |
|--------|------------|
| `connection_label` | Метка типа связи: LAN / Интернет / Поле |
| `connect_via_hint` | Lookup → hole-punch → Noise handshake (DRY для CLI и GUI) |
| `direct_chat_loop` | Общий pump ChatService + FileTransferService |

## Аккаунты и авторизация

Локальные аккаунты (без сервера). Identity — пара Ed25519, файл `profile.nyx` шифруется паролем (PBKDF2 + HMAC).

| Сценарий | Поведение |
|----------|-----------|
| Создать аккаунт | Ник + пароль; показывается **recovery-фраза из 12 слов** (BIP39 English). Без неё сброс пароля невозможен |
| Войти | Пароль; опция **«Запомнить меня на 30 дней»** |
| Запомнить меня | На Windows ключ сессии оборачивается DPAPI (`unlock.token`); при следующем запуске — авто-вход и преселект последнего аккаунта |
| Выйти | Сбрасывает сессию и remember-токен |
| Сброс пароля | Recovery-фраза → новый пароль; remember-токен сбрасывается |

Файлы: `%APPDATA%/nyx/accounts/<id>/profile.nyx`, `recovery.nyx`, `unlock.token`; реестр `%APPDATA%/nyx/registry.json`; prefs `%APPDATA%/nyx/auth_prefs.json`.

## Логирование (фаза 7)

События AppCore и CLI пишутся в файл:

```
%APPDATA%/Nyx/logs/Nyx.log
```

Ротация при ~5 МБ → `Nyx.log.1`. Уровень по умолчанию: Info.

## Сборка

### Backend + CLI

```powershell
cmake -B build -G "MinGW Makefiles"
cmake --build build
.\build\nyx-tests.exe
.\build\nyx-appcore-tests.exe
```

### GUI

```powershell
cmake -B build -G "MinGW Makefiles" `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/mingw_64" `
  -DCMAKE_CXX_COMPILER="C:/Qt/Tools/mingw1310_64/bin/g++.exe"
cmake --build build
```

См. [BUILD_GUI_WINDOWS.md](BUILD_GUI_WINDOWS.md). Установщик `build\NyxSetup.exe` собирается той же командой `cmake --build build -j`.

## GUI: два инстанса

### Через LAN (без rendezvous)

**Окно 1 — слушатель**

```powershell
.\build\nyx-app.exe --profile aspid.json
```

→ **Слушать**. Узел автоматически публикуется в LAN (multicast `239.255.77.77:34779`).

**Окно 2 — подключение**

```powershell
.\build\nyx-app.exe --profile test.json --nickname Test
```

Через ~5 с в блоке **«В локальной сети»** появится peer → **Подключить**. В шапке чата: тип связи (`LAN`, `LAN (token)` и т.д.).

### Через invite token (нужен rendezvous)

```powershell
.\build\nyx-rendezvous.exe
```

Alice: **Слушать** → скопировать token. Bob: вкладка **Connect** → token → **Подключиться**.

### Параметры `nyx-app`

| Параметр | Описание |
|----------|----------|
| `--profile PATH` | Отдельный профиль на инстанс |
| `--nickname NAME` | Переопределить nickname (сохраняется в профиль) |
| `--rendezvous HOST:PORT` | Rendezvous (также поле внизу сайдбара) |

### Сессии (multi-session)

После входа в аккаунт NodeService поднимает параллельно:
- стабильный **DM inbox** (`dm_inbox.token`);
- hub всех полей, где пользователь владелец (если включён автозапуск);
- join к известным полям и DM по `session_intents.json` / inbox token peer из контактов.

Выбор чата в списке вызывает auto-connect. Hub поля и личный P2P больше не взаимоисключающие. «Отключиться» (ПКМ по чату, шапка, status bar) останавливает только эту сессию и отключает auto-reconnect для неё. Offline-чаты (host недоступен) серые: историю открыть можно, писать нельзя.

**Поля:** писать можно только пока hub владельца в сети (сессия Live). При остановке hub участникам уходит Bye; при обрыве — keep-alive сбрасывает join. Сообщения в офлайн-поле не принимаются. После возврата владельца клиенты с включённым intent сами переподключаются (таймер ~8 с и быстрый retry после Offline). История на hub и у клиентов сходится через JoinAck (последние 100) с дедупом по id.

Advanced-панель сети (Ctrl+K / «Сеть» в status bar): разовый listen, connect по token, join поля, LAN.

### Файлы (панель вместо чата)

Вкладка **Файлы** в сайдбаре или кнопка 📎 переключает правую область на `FilesPanel` (`mainViewMode = 1`), не модальное окно. **Esc** — назад к чату.

| Вкладка | Содержимое |
|---------|------------|
| **Обзор** | Share-папки, breadcrumb, вход в папки; ПКМ или щит — назначение ролей на файл/папку; drag-and-drop |
| **Ресурсы** | То же дерево для файлов поля; скачать / открыть по сети |
| **Доступ** | Пресеты прав (конструктор), роли из прав, участники поля |

Права хранятся в `file_access.json` в каталоге аккаунта (`%APPDATA%/Nyx/accounts/<id>/`). Роли, пресеты и назначения сохраняются между запусками. Встроенные роли: Владелец (не редактируется), Участник и Только чтение (можно менять набор прав). Свои роли создаются на вкладке «Доступ». На share-корень (ПКМ по папке в «Обзоре») и на подпапки — роль из списка или прямые права. Hub рассылает политику ACL участникам при входе и после каждого изменения (PolicyPush); клиент участника обновляет кнопки «Скачать»/«Открыть» по этой политике. Hub проверяет те же права при выдаче файла. Запросы на скачивание из GUI ставятся в очередь и выполняются в worker-потоке сессии (по одному файлу); при отказе или ошибке открытия на стороне отправителя приходит FileDeny. Перед скачиванием в GUI — диалог «Сохранить как» с исходным именем; для папки — выбор каталога (имена и подпапки сохраняются). CLI кладёт файлы в `%APPDATA%/Nyx/downloads/`.

**Строка статуса** внизу окна — число live-сессий, диагностика, кнопки «Сеть» и «Откл.». Ошибки и подтверждения — тосты справа внизу.

| Право | Операция |
|-------|----------|
| List | Видеть списки |
| Download | Скачивать |
| Upload | Отправлять |
| Delete | Удалять (протокол — позже) |
| OpenRemote | Открытие через скачивание / stream (в разработке) |
| ManageShares | Индекс папок |
| ManageRoles | Роли и назначения |

**Область в шапке:** «Личные файлы» — только для P2P-чата (1:1). «Поле: …» — общий индекс группы. Личные папки участники поля **не увидят**. Пути и имена файлов с кириллицей поддерживаются (UTF-8).

**Доступ** — пресеты прав (именованные маски), роли (набор прав, встроенные member/viewer редактируются владельцем), список участников поля с ролями.

**Обзор** — ПКМ или замок: **роль на объект** (без участников в списке), ниже — переопределения для участников. Участники показаны чипами под breadcrumb.

При первом входе участник добавляется в `groups.json` и **остаётся** в roster после выхода из hub. Создатель поля всегда в списке участников.

**Чат поля** — клик по имени поля в шапке открывает окно с участниками, invite и статусом hub.

**Ресурсы** — общий каталог поля с hub; обновление кнопкой в шапке.

## CLI

### Listen / connect

```powershell
.\build\nyx-rendezvous.exe

.\build\nyx-node.exe listen --profile alice.json
.\build\nyx-node.exe connect --token <hex> --profile bob.json
```

**LAN:** mDNS включён по умолчанию при `listen`. Отключить: `--no-lan`.

```powershell
.\build\nyx-node.exe browse
.\build\nyx-node.exe connect --peer 192.168.1.10:49737 --profile bob.json
```

### Файлы

| Команда | Действие |
|---------|----------|
| `/index C:\share` | Индекс папки |
| `/files` | Локальный список |
| `/remote` | Список у peer |
| `/get <hash>` | Скачать в `%APPDATA%/Nyx/downloads/` |
| `/sendfile <путь>` | Отправить файл |

### Поля

```powershell
nyx-node group create "Моё поле" --profile alice.json
nyx-node group hub --group <group_id_hex> --profile alice.json
nyx-node group join --token <invite_hex> --profile bob.json
```

### Параметры CLI

| Параметр | По умолчанию |
|----------|--------------|
| `--rendezvous HOST:PORT` | `127.0.0.1:3478` |
| `--nickname NAME` | из профиля / OS username |
| `--profile PATH` | `%APPDATA%/Nyx/profile.json` |
| `--no-lan` | mDNS **включён**; флаг отключает публикацию |
| `--peer HOST:PORT` | прямой connect (LAN) |
| `--timeout MS` | для `browse` (3000) |

## LAN discovery (фаза 6)

- Канал: multicast **`239.255.77.77:34779`** (собственный протокол Nyx, не системный mDNS :5353).
- **Listen** публикует beacon каждую ~1 с.
- **GUI** сканирует LAN каждые 5 с в фоне (`scan_lan_peers`, не прерывает listen).
- **Rendezvous не требуется** для LAN connect.

## Данные на диске

| Путь | Содержимое |
|------|------------|
| `%APPDATA%/Nyx/profile.json` | Ключи, nickname |
| `%APPDATA%/Nyx/contacts.json` | Контакты |
| `%APPDATA%/Nyx/chats/*.jsonl` | История лички |
| `%APPDATA%/Nyx/groups/*.jsonl` | История полей |
| `%APPDATA%/nyx/accounts/<id>/file_index.json` | Индекс share-папок аккаунта |
| `%APPDATA%/nyx/accounts/<id>/files_ui.json` | Последняя область и выбранная папка в «Файлы» |
| `%APPDATA%/Nyx/downloads/` | Скачанные файлы |
| `%APPDATA%/Nyx/logs/Nyx.log` | Лог (фаза 7) |

## Что работает

- Транспорт Noise, rendezvous, hole punch, STUN hint
- Личный чат 1:1 (CLI + GUI), история, Outbox/Ack
- Файлы: индекс, передача chunked (CLI + GUI вкладка)
- Поля: star hub, group join (CLI + GUI вкладка)
- LAN discovery (CLI + GUI авто-скан)
- GUI: тёмная тема, пузыри, token copy, индикатор типа соединения

## В очереди (фаза 7+)

- Rekey сессии (1 GB / 24 h) — автоматически в `Connection::drive()` / `send_stream`
- Share policy «файл только для поля X»
- Список чатов, tray, light theme (фаза 8)
- DHT / несколько rendezvous (post-MVP)

## Тесты

```powershell
.\build\nyx-tests.exe          # libnyx + mdns browse + log
.\build\nyx-appcore-tests.exe  # NodeService
```

CI (GitHub Actions): Linux build, `nyx-tests`, `nyx-appcore-tests` (GUI off).
