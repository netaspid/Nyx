# Nyx

P2P-мессенджер на C++: личные и групповые чаты, файлы, поиск в LAN, связь через интернет с rendezvous-сервером. Трафик шифруется Noise (XX, ChaChaPoly).

Основной клиент — `nyx-app` (Qt 6, QML). `nyx-node` — CLI для отладки. `nyx-rendezvous` — bootstrap для связи через NAT.

Стек: C++17, CMake, [noise-c](https://github.com/rweather/noise-c).

## Сборка (Windows, всё сразу)

Нужны CMake 3.16+ и Qt 6.5+ (Quick, Qml) в `C:/Qt/6.x/mingw_64`. CMake сам найдёт Qt и MinGW из Qt Tools.

```powershell
cmake -B build -G "MinGW Makefiles"
cmake --build build -j
```

Результат в `build/`:

| Файл | Назначение |
|------|------------|
| `nyx-app.exe` | GUI-клиент |
| `nyx-node.exe` | CLI |
| `nyx-rendezvous.exe` | Bootstrap-сервер |
| **`NyxSetup.exe`** | **Установщик для пользователей (~130 MB)** |

Раздавайте пользователям **`build\NyxSetup.exe`**. Qt и все DLL уже внутри.

Промежуточные файлы установщика лежат в `build\_installer\` — это служебная папка, её не трогают.

Явные пути (если несколько версий Qt):

```powershell
cmake -B build -G "MinGW Makefiles" `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/mingw_64" `
  -DCMAKE_C_COMPILER="C:/Qt/Tools/mingw1310_64/bin/gcc.exe" `
  -DCMAKE_CXX_COMPILER="C:/Qt/Tools/mingw1310_64/bin/g++.exe"
cmake --build build -j
```

## Linux (без GUI)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNYX_BUILD_GUI=OFF
cmake --build build -j
```

## Тесты

```powershell
.\build\nyx-tests.exe
.\build\nyx-appcore-tests.exe
```

## Запуск

Два инстанса в одной LAN — rendezvous не нужен. Через интернет — поднять `nyx-rendezvous`, token на слушающей стороне, Connect на второй.

Профиль и ключи: `%APPDATA%\nyx\` (Windows) или `~/.config/nyx/` (Linux).

## Документация

- [docs/APPLICATION.md](docs/APPLICATION.md) — CLI, GUI, сценарии
- [docs/BUILD_GUI_WINDOWS.md](docs/BUILD_GUI_WINDOWS.md) — Qt/MinGW, установщик
- [docs/GUI.md](docs/GUI.md) — QML и NodeController
- [docs/DEPLOY_RENDEZVOUS.md](docs/DEPLOY_RENDEZVOUS.md) — rendezvous на VDS
- [docs/ROADMAP.md](docs/ROADMAP.md) — план разработки
