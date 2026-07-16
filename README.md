# Nyx

P2P-мессенджер: личные чаты, поля (группы), обмен файлами. Связь напрямую между узлами, шифрование Noise. Через интернет нужен небольшой UDP-сервер rendezvous для нахождения адресов.

## Стек

C++17, CMake, Qt 6 (QML) для GUI, [noise-c](https://github.com/rweather/noise-c).

| Бинарник | Назначение |
|----------|------------|
| `nyx-app` | Клиент |
| `nyx-rendezvous` | Bootstrap (UDP) |
| `nyx-node` | CLI для отладки |

## Установка на Windows

Нужны CMake 3.16+ и Qt 6.5+ (MinGW) в `C:\Qt\...`.

```powershell
cmake -B build -G "MinGW Makefiles"
cmake --build build -j
```

Для пользователей раздавайте `build\NyxSetup.exe` — внутри клиент и зависимости Qt.

Данные приложения: `%APPDATA%\Nyx\`.

Администрирование (rendezvous, сеть, типовые сбои): [docs/ADMIN.md](docs/ADMIN.md).

## Лицензия

Copyright © владельцы проекта. Все права защищены, если иное не оговорено отдельным файлом LICENSE.
