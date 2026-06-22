# Графический интерфейс (nyx-app)

Краткая выжимка для разработчиков. Полный план: [ROADMAP.md](ROADMAP.md).

## Стек

Qt 6.5+ · Qt Quick (QML) · `NodeController` + `NodeService` (AppCore) · libnyx · SVG-иконки (`resources/icons/`).

## Структура

```
apps/nyx-app/
├── main.cpp
├── appcore/node_service*.cpp
├── qt/node_controller.*, message_model.*, chat_list_model.*, lan_peer_model.*, file_list_model.*
├── i18n/nyx_ru.ts
├── resources/icons/*.svg          # nyx-logo, nyx-mark, UI icons
└── ui/
    ├── main.qml                 # layout: список чатов + переписка
    ├── Theme.qml
    ├── controls/                # NyxButton, NyxTextField, NyxComboBox, NyxSegmentTabButton
    ├── panels/                  # ChatListPanel, MainContentPanel, ChatView, FilesPanel, ConnectionDrawer
    ├── dialogs/                 # SettingsDialog, OnboardingDialog, GroupsDialog
    └── components/              # NyxLogo, ChatBubble, ChatListItem, NyxIcon, …
```

## Layout (фаза 8)

| Колонка | Ширина | Содержимое |
|---------|--------|------------|
| Слева | ~320px | Список чатов, поиск, профиль, кнопка «Подключение» |
| Справа | flex | Шапка чата, сообщения, ввод, progress bar |
| Drawer | справа | Listen / Connect / Поле / LAN |
| Панель файлов | вместо чата | `FilesPanel` при `mainViewMode = 1` |

## QML API (`app` — контекстное свойство NodeController)

| Свойство / метод | Назначение |
|------------------|------------|
| `mainViewMode`, `openFilesView()`, `showChatView()` | Переключение чат ↔ файлы |
| `fileScopeGroupId`, `fileScopeLabel` | Область для новых папок и вкладки «Доступ» |
| `fileShareRoots` | Все проиндексированные корни (личные и поля), с меткой области |
| `localFiles`, `remoteFiles` | `FileListModel` (уровень дерева) |
| `fileSelectedShareRoot`, `fileBrowsePath`, `fileBrowseCrumbs` | Навигация по иерархии |
| `canRemoveShareFolder` | Можно убрать share-папку (владелец поля или ManageShares) |
| `browseIntoFolder`, `browseUp`, `addDroppedUrls` | Браузер и DnD |
| `canFileUpload/Download/OpenRemote`, `canManageFileShares/Roles` | Права текущего пользователя |
| `fileRoleList`, `fileMemberAccess`, `setMemberFileRole`, `createFileRole` | Роли поля |
| `addIndexedFolder(url)` | Индекс + drag-and-drop |
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
| Esc | Файлы → чат; закрыть drawer; отключиться |

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
4. Единый стиль — см. `.cursor/rules/nyx-ui.mdc` и `Theme.qml`.
