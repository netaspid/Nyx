# Графический интерфейс (nyx-app)

Краткая выжимка для разработчиков. Полный план: [ROADMAP.md](ROADMAP.md).

## Стек

Qt 6.5+ · Qt Quick (QML) · `NodeController` + `NodeService` (AppCore) · libnyx · SVG-иконки (`resources/icons/`).

## Структура

```
apps/nyx-app/
├── main.cpp
├── appcore/node_service*.cpp
├── qt/node_controller.*, message_model.*, chat_list_model.*, lan_peer_model.*
├── i18n/nyx_ru.ts
├── resources/icons/*.svg          # nyx-logo, nyx-mark, UI icons
└── ui/
    ├── main.qml                 # layout: список чатов + переписка
    ├── Theme.qml
    ├── controls/                # NyxButton, NyxTextField
    ├── panels/                  # ChatListPanel, ChatView, ConnectionDrawer
    ├── dialogs/                 # SettingsDialog, OnboardingDialog
    └── components/              # NyxLogo, ChatBubble, ChatListItem, NyxIcon, …
```

## Layout (фаза 8)

| Колонка | Ширина | Содержимое |
|---------|--------|------------|
| Слева | ~320px | Список чатов, поиск, профиль, кнопка «Подключение» |
| Справа | flex | Шапка чата, сообщения, ввод, progress bar |
| Drawer | справа | Listen / Connect / Поля / Файлы / LAN |

## QML API (`app` — контекстное свойство NodeController)

| Свойство / метод | Назначение |
|------------------|------------|
| `profileNickname`, `profileIdShort` | Профиль (R/W nickname в настройках) |
| `needsOnboarding`, `completeOnboarding(nick)` | Первый запуск |
| `chatList` | `ChatListModel` — контакты + поля + превью |
| `refreshChatList()`, `openConversation(...)` | Список чатов |
| `searchMessages(query)` | Локальный фильтр в текущем чате |
| `inChat`, `canSendMessage`, `peerStatusText` | Сессия / offline история |
| `connectionPanelOpen` | Drawer подключения |
| `lastGroupInvite`, `copyLastGroupInvite()` | Invite поля |
| `trayAvailable`, `hideToTray()`, `showMainWindow` | Системный tray |
| `fileProgress*` | Progress bar передачи файлов |
| `rendezvousList`, `discoveryMode`, `networkStatus` | Интернет / rendezvous |
| `saveNetworkSettings()`, `testRendezvousServer()` | Сохранить и проверить bootstrap |

## Горячие клавиши

| Комбинация | Действие |
|------------|----------|
| Ctrl+Enter | Отправить сообщение |
| Ctrl+K | Панель подключения |
| Esc | Закрыть drawer / отключиться |

## Сборка

```powershell
cmake --build build
.\build\nyx-tests.exe
.\build\nyx-appcore-tests.exe
.\build\nyx-app.exe
```

Конфиг сети: `%APPDATA%/Nyx/network.json`. Инструкция VDS: [DEPLOY_RENDEZVOUS.md](DEPLOY_RENDEZVOUS.md).

## Правила

1. Backend → `NodeService` → `NodeController` → QML.
2. Не дублировать протокол в QML.
3. QML ≤200 строк на файл; новые экраны — в `panels/` / `dialogs/`.
