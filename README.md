# Nyx

P2P-мессенджер на C++: личные и групповые чаты, файлы, поиск в LAN, связь через интернет с rendezvous-сервером. Трафик шифруется Noise (XX, ChaChaPoly).

Основной клиент - `nyx-app` (Qt 6, QML). `nyx-node` - CLI для отладки и скриптов. `nyx-rendezvous` - bootstrap для связи через NAT; в одной сети можно обойтись без него.

Стек: C++17, CMake, [noise-c](https://github.com/rweather/noise-c).

## Сборка

Нужны CMake 3.16+, компилятор с C++17, MinGW или MSVC на Windows, GCC/Clang на Linux.

```powershell
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

GUI дополнительно требует Qt 6.5+ (Quick, Qml):

```powershell
cmake -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH="C:/Qt/6.x/mingw_64"
cmake --build build --target nyx-app
```

На Linux без Qt:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNYX_BUILD_GUI=OFF
cmake --build build -j
```

Тесты:

```powershell
.\build\nyx-tests.exe
.\build\nyx-appcore-tests.exe
```


## Запуск

Два инстанса в одной LAN - rendezvous не нужен. В GUI: "Слушать" на одном, "В локальной сети" на другом.

Через интернет: поднять `nyx-rendezvous`, на слушающей стороне скопировать token, на второй - Connect.

Профиль и ключи лежат в `%APPDATA%\nyx\` (Windows) или `~/.config/nyx/` (Linux).


## Документация

- [docs/APPLICATION.md](docs/APPLICATION.md) - сборка, CLI, GUI, два клиента
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - слои и модули
- [docs/GUI.md](docs/GUI.md) - QML и NodeController
- [docs/ROADMAP.md](docs/ROADMAP.md) - план разработки
- [docs/DEPLOY_RENDEZVOUS.md](docs/DEPLOY_RENDEZVOUS.md) - rendezvous на VDS

## Структура

```
include/nyx/     протокол и сервисы
src/             реализация libnyx
apps/nyx-app/    GUI + appcore
apps/nyx-node/   CLI
apps/common/     общий код CLI и GUI
tests/           интеграционные тесты
docs/            документация
```
