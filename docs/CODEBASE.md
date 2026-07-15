# Обзор кода: с чего начать

Практическая карта репозитория для самостоятельного чтения. Архитектура слоёв — в [ARCHITECTURE.md](ARCHITECTURE.md), сценарии запуска — в [APPLICATION.md](APPLICATION.md), UI и QML API — в [GUI.md](GUI.md).

## Исполняемые программы

| Бинарник | Точка входа | Назначение |
|----------|-------------|------------|
| **nyx-app** | `apps/nyx-app/main.cpp` | Графическое приложение (Qt 6 + QML) |
| **nyx-node** | `apps/nyx-node/main.cpp` | CLI: listen, connect, group — отладка без GUI |
| **nyx-rendezvous** | `apps/nyx-rendezvous/main.cpp` | Bootstrap-сервер для обмена токенами |
| **nyx-tests** | `tests/test_main.cpp` | Unit-тесты libnyx |
| **nyx-appcore-tests** | `tests/test_node_service.cpp` | Тесты AppCore без Qt |

Сборка: `cmake --build build`. GUI включается опцией `NYX_BUILD_GUI` (по умолчанию ON), нужен Qt 6.5+.

## Где main GUI

Цепочка от запуска до экрана:

```
apps/nyx-app/main.cpp
    → QQmlApplicationEngine загружает qrc:/ui/main.qml
    → в QML доступен объект app (NodeController)
apps/nyx-app/ui/main.qml          — корневое окно, layout, горячие клавиши
apps/nyx-app/ui/panels/           — крупные зоны экрана
apps/nyx-app/ui/dialogs/          — модальные окна
apps/nyx-app/ui/components/       — мелкие блоки (пузырь, строка списка, тост)
apps/nyx-app/ui/controls/         — кнопки, поля ввода (дизайн-система)
apps/nyx-app/ui/Theme.qml           — цвета и отступы
```

**C++ между QML и сетью:**

```
apps/nyx-app/qt/node_controller.*   — QObject для QML (свойства, сигналы)
apps/nyx-app/qt/*_model.*           — модели списков (чаты, сообщения, файлы, LAN)
apps/nyx-app/appcore/node_service*  — логика без Qt (поток worker, сценарии)
```

Правило: QML не вызывает libnyx напрямую — только через `NodeController` → `NodeService`. Подробный список свойств `app` — в [GUI.md](GUI.md).

### Layout главного окна (`main.qml`)

| Зона | Файл | Что внутри |
|------|------|------------|
| Слева | `panels/ChatListPanel.qml` | Список чатов, поиск, профиль |
| Справа | `panels/MainContentPanel.qml` | Переключатель чат / файлы |
| Переписка | `panels/ChatView.qml` | Сообщения, поле ввода |
| Файлы | `panels/FilesPanel.qml` | Индекс, обмен, права |
| Сеть (advanced) | `panels/ConnectionDrawer.qml` | Listen, connect, поле, LAN (не основной UX) |
| Multi-session | `appcore/node_service*.cpp`, `session_types.hpp` | Параллельные hub/DM/join |
| Intents | `include/nyx/session_intent.hpp` | Auto-reconnect persist |
| Вход | `AccountGate.qml` | Аккаунты, recovery-фраза, remember-me 30 дней, сброс пароля |
| Настройки | `dialogs/SettingsDialog.qml` | Профиль, сеть |
| Онбординг | `dialogs/OnboardingDialog.qml` | Первый запуск |

Ресурсы QML и иконки перечислены в `apps/nyx-app/resources.qrc`.

## Дерево репозитория

```
game.ru/
├── apps/
│   ├── nyx-app/              GUI + AppCore + Qt-обёртки
│   ├── nyx-node/             CLI-сессии (chat, group, node)
│   ├── nyx-rendezvous/       сервер rendezvous
│   ├── nyx-setup/            установщик Windows (NyxSetup.exe)
│   ├── nyx-uninstall/        деинсталлятор
│   └── common/               общий код CLI и AppCore (connect, label)
├── include/nyx/              публичные заголовки библиотеки libnyx
├── src/                      реализации libnyx (1 файл ≈ 1 модуль)
├── tests/                    тесты
├── cmake/                    noise-c, MinGW, installer
├── docs/                     документация
├── scripts/                  деплой rendezvous, pen-test
└── CMakeLists.txt            цели сборки
```

## libnyx: слои снизу вверх

Читать снизу вверх — так проще понять зависимости.

### Транспорт и wire format

| Модуль | Заголовок | Реализация | О чём |
|--------|-----------|------------|-------|
| Типы, константы | `types.hpp` | — | PacketType, StreamType, размеры |
| Байтовый протокол | `proto.hpp` | `proto.cpp` | Frame, rendezvous, control |
| Утилиты | `util.hpp` | `util.cpp` | CRC, LE, hex |
| UDP | `udp.hpp` | `udp.cpp` | Сокет |
| NAT / hole punch | `nat.hpp` | `nat.cpp` | Пробитие через rendezvous |
| Крипто | `crypto.hpp` | `crypto.cpp` | Noise XX handshake, Session |
| Надёжность | `transport.hpp` | `transport.cpp` | ARQ, фрагментация |
| Мультиплексор | `mux.hpp` | `mux.cpp` | Потоки control / data / bulk |
| Соединение | `connection.hpp` | `connection.cpp` | Высокоуровневый connect |

Спецификация пакетов: [protocol.md](protocol.md).

### Идентичность и хранение

| Модуль | Файлы | О чём |
|--------|-------|-------|
| Ключи, профиль | `identity.hpp`, `profile_crypto.hpp` | Ed25519, nickname |
| Аккаунты | `account_store.hpp` | Несколько профилей на диске |
| Пути данных | `paths.hpp` | `%APPDATA%/Nyx/...` |
| Сеть | `network_config.hpp`, `rendezvous_pool.hpp` | bootstrap, список серверов |

### Мессенджер

| Модуль | Файлы | О чём |
|--------|-------|-------|
| Формат сообщений | `messaging.hpp` | ChatMessage, Bye, Ack (Msg / MsgV2) |
| Id чата | `chat_id.hpp` | DM и group chat id |
| Сервис чата | `chat_service.hpp` | Отправка, приём, legacy Text |
| История | `message_store.hpp`, `conversation.hpp` | JSONL на диске |
| Outbox | `outbox.hpp` | Очередь при обрыве |

Точка входа для разбора формата кадра: `src/messaging.cpp` (`ChatMessage::encode` / `decode`).

### Файлы

| Модуль | Файлы | О чём |
|--------|-------|-------|
| Индекс | `file_index.hpp` | Сканирование папок |
| Протокол файлов | `file_proto.hpp` | Запросы по сети |
| Передача | `file_transfer.hpp`, `blob_store.hpp` | Chunk upload/download |
| Права | `file_access.hpp` | Роли в поле |
| Хеш | `file_hash.hpp` | Идентификация blob |

### Поля (группы)

| Модуль | Файлы | О чём |
|--------|-------|-------|
| Модель поля | `group.hpp` | GroupId, roster |
| Протокол | `group_proto.hpp` | Group-кадры |
| Создатель (hub) | `group_hub.hpp` | Fan-out сообщений |
| Участник | `group_member.hpp` | Join по invite |

### Прочее

| Модуль | Файлы | О чём |
|--------|-------|-------|
| mDNS LAN | `mdns.hpp` | Обнаружение в локалке |
| Rendezvous server | `rendezvous_server.hpp` | Логика сервера |
| Hello (legacy app) | `app.hpp` | Ранний кадр приветствия |
| CLI console | `console.hpp` | UTF-8 в терминале |

## AppCore: как GUI дотягивается до сети

`NodeService` разбит по зонам ответственности (удобно открывать по задаче):

| Файл | Зона |
|------|------|
| `node_service.cpp` | Конструктор, worker, профиль, общее |
| `node_service_connect.cpp` | Listen, connect, LAN, rendezvous |
| `node_service_chat.cpp` | Личка, история, отправка |
| `node_service_group.cpp` | Поля, hub, invite |
| `node_service_files.cpp` | Индекс, browse, upload/download, ACL |

Фоновый поток `worker_` крутит сетевые сессии; в UI данные приходят через callbacks и mutex-защищённые снимки (см. ARCHITECTURE.md, раздел NodeService).

## CLI nyx-node (без GUI)

Параллельные «сессии» для тех же сценариев:

| Файл | Сценарий |
|------|----------|
| `node_session.cpp` | listen / connect 1:1 |
| `chat_session.cpp` | Интерактивный чат в терминале |
| `group_session.cpp` | create / join поля |
| `cli.cpp` | Разбор аргументов, `print_usage` |

Общая логика подключения: `apps/common/connect_via_hint.cpp`.

## Рекомендуемые маршруты чтения

### «Хочу понять окно приложения»

1. `apps/nyx-app/main.cpp`
2. `apps/nyx-app/ui/main.qml`
3. `apps/nyx-app/ui/panels/ChatListPanel.qml` + `ChatView.qml`
4. `apps/nyx-app/qt/node_controller.hpp` (какие свойства есть у `app`)
5. `apps/nyx-app/appcore/node_service_chat.cpp`

### «Хочу понять, как уходит сообщение»

1. QML: кнопка отправки в `ChatView.qml` → метод `app`
2. `node_controller.cpp` → `NodeService::send_message`
3. `src/chat_service.cpp`
4. `src/messaging.cpp` (сериализация)
5. `src/mux.cpp` → `src/connection.cpp` (если нужен путь до UDP)

### «Хочу понять подключение двух узлов»

1. `apps/nyx-app/appcore/node_service_connect.cpp` или `apps/nyx-node/node_session.cpp`
2. `src/nat.cpp`, `src/rendezvous.cpp`
3. `src/crypto.cpp` (handshake)
4. `docs/protocol.md` — кадры rendezvous

### «Хочу понять файлы и права»

1. `apps/nyx-app/ui/panels/FilesPanel.qml`
2. `apps/nyx-app/appcore/node_service_files.cpp`
3. `src/file_index.cpp`, `src/file_transfer.cpp`, `src/file_access.cpp`

### «Хочу понять поля (группы)»

1. `apps/nyx-app/appcore/node_service_group.cpp`
2. `src/group_hub.cpp` / `src/group_member.cpp`
3. `include/nyx/group_proto.hpp`

## Где что на диске у пользователя

Логика путей: `src/paths.cpp`, заголовок `include/nyx/paths.hpp`.

Типично на Windows: `%APPDATA%/Nyx/` — профили, `chats/`, `groups/`, `network.json`. Подробнее в APPLICATION.md.

## Тесты как подсказка

| Тест | Что проверяет |
|------|----------------|
| `tests/test_main.cpp` | Кодек, handshake, proto, messaging |
| `tests/test_node_service.cpp` | NodeService без Qt |

Запуск: `.\build\nyx-tests.exe`, `.\build\nyx-appcore-tests.exe`.

## Связанные документы

| Документ | Когда открывать |
|----------|-----------------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Слои, принципы, потоки данных |
| [GUI.md](GUI.md) | QML API, layout, горячие клавиши |
| [APPLICATION.md](APPLICATION.md) | Как запустить и что уже работает |
| [protocol.md](protocol.md) | Байтовый формат |
| [GLOSSARY.md](GLOSSARY.md) | Термины: поле, rendezvous, ChatId |
| [ROADMAP.md](ROADMAP.md) | Что ещё не сделано |

## Быстрые ответы

**Где main GUI?** — `apps/nyx-app/main.cpp` → `ui/main.qml`.

**Где бизнес-логика приложения?** — `apps/nyx-app/appcore/node_service*.cpp`.

**Где сетевой стек?** — `include/nyx/` + `src/` (libnyx).

**Где не лезть в первый день?** — `cmake/noise_*`, FetchContent noise-c, скрипты installer (`nyx-setup/`) — это инфраструктура сборки, не продуктовая логика.
