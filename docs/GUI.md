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
    └── components/              # NyxLogo, ChatBubble, ChatListItem, FieldInfoPopup, …
```

## Layout (фаза 8)

| Колонка | Ширина | Содержимое |
|---------|--------|------------|
| Слева | ~320px | Список чатов, поиск, профиль, вкладки Поля / Файлы |
| Справа | flex | Шапка чата (имя поля — клик → `FieldInfoPopup`), сообщения, ввод, progress bar |
| StatusBar | внизу | «N активных сессий» + короткий статус; кнопки «Сеть» / «Откл.» |
| Drawer | справа (Ctrl+K / «Сеть») | Пригласить / Личный чат / Поле + LAN + блок подсказок |
| Модалки | Settings, Поля, … | Закрытие через X в шапке (`DialogChrome`), без кнопки «Закрыть» |
| Панель файлов | вместо чата | `FilesPanel` при `mainViewMode = 1` |

## Экран входа (`AccountGate.qml`)

| Свойство / метод | Назначение |
|------------------|------------|
| `sessionUnlocked`, `accountList`, `accountGateError` | Состояние входа |
| `pendingRecoveryPhrase`, `needsRecoveryConfirm` | Показ 12 слов после создания |
| `lastAccountId` | Пресэлектро выбора аккаунта |
| `createAccount(nick, pass, confirm, rememberMe)` | Создать и показать recovery |
| `confirmRecoveryPhraseSaved()`, `copyRecoveryPhrase()` | Подтверждение сохранения фразы |
| `unlockAccount(id, pass, rememberMe)`, `tryUnlockRemembered(id)` | Вход по паролю или token |
| `resetPasswordWithRecovery(id, phrase, newPass, confirm)` | Сброс пароля |
| `signOut()` | Выход (чистит remember) |

## QML API (`app` — контекстное свойство NodeController)

| Свойство / метод | Назначение |
|------------------|------------|
| `mainViewMode`, `openFilesView()`, `showChatView()` | Переключение чат ↔ файлы |
| `fileScopeGroupId`, `fileScopeLabel` | Область для новых папок и вкладки «Доступ» |
| `fileShareRoots` | Все проиндексированные корни (личные и поля), с меткой области |
| `localFileList`, `remoteFileList` | Файлы текущего уровня (QVariantList) |
| `fileResourcesRoot`, `fileRemoteBrowseCrumbs` | Навигация на вкладке «Ресурсы» |
| `setFilesSection(0/1/2)` | Обзор / Ресурсы / Доступ |
| `fileSelectedShareRoot`, `fileBrowsePath`, `fileBrowseCrumbs` | Навигация по иерархии |
| `setFileAccessTarget`, `filePathMemberAccess`, `setPathMemberFileRole`, `clearPathMemberGrant` | Назначение прав на папку/файл (из «Обзор», не из «Доступ») |
| `canEditFileRolePermissions(roleId)` | Можно ли менять набор прав роли (кроме owner) |
| `statusText` | Строка статуса внизу окна |
| `toast`, `toastIsError`, `clearToast()` | Тосты справа внизу |
| `browseIntoFolder`, `browseUp`, `addDroppedUrls` | Браузер и DnD |
| `canFileUpload/Download/OpenRemote`, `canManageFileShares/Roles` | Права на текущий уровень (Обзор или Ресурсы) |
| `canDownloadFileAt(root, rel)`, `canOpenRemoteFileAt(root, rel)` | Права на конкретный объект (кнопки в списке) |
| `downloadFile(hash, name, root, rel)`, `downloadRemoteFolder(root, rel)` | Скачать файл (диалог «Сохранить как») или папку (выбор каталога) |
| `fileRoleList`, `fileMemberAccess`, `setMemberFileRole`, `createFileRole` | Роли поля |
| `refreshFieldRoster()` | Обновить roster поля из hub/хранилища (участники, «Доступ») |
| `addIndexedFolder(url)` | Индекс + drag-and-drop |
| `lastGroupInvite`, `copyLastGroupInvite()` | Invite поля |
| `trayAvailable`, `hideToTray()`, `showMainWindow` | Системный tray |
| `fileProgress*` | Progress bar передачи файлов |
| `rendezvousList`, `discoveryMode`, `networkStatus` | Интернет / rendezvous |
| `saveNetworkSettings()`, `testRendezvousServer()` | Сохранить и проверить bootstrap |
| `openConversation` | Выбор чата + auto `ensureSession` (hub/join/DM) |
| `disconnectChat(key)`, `sessionStateForKey`, `sessionSummary` | Multi-session UX |
| `dmInboxToken`, `copyDmInboxToken()` | Стабильный inbox token для лички |
| `autoStartOwnedHub` | Master-switch автоподключения сессий после старта и периодический reconnect полей/DM |

## Горячие клавиши

| Комбинация | Действие |
|------------|----------|
| Ctrl+Enter | Отправить сообщение |
| Ctrl+K | Advanced-панель сети |
| Esc | Файлы → чат; закрыть drawer |
| ПКМ по чату | Отключиться / копировать invite |

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
