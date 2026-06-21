# Сборка nyx-app на Windows (Qt + MinGW)

## Одна команда

```powershell
cmake -B build -G "MinGW Makefiles"
cmake --build build -j
```

CMake сам найдёт Qt в `C:/Qt/6.x/mingw_64` и MinGW из Qt Tools.

После сборки:

- `build\nyx-app.exe` — клиент
- **`build\NyxSetup.exe`** — установщик для раздачи пользователям

`build\_installer\` — служебные файлы сборки установщика (stub, pack, staging). Не запускать.

## Явные пути Qt/MinGW

Если установлено несколько версий Qt:

```powershell
cmake -B build -G "MinGW Makefiles" `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/mingw_64" `
  -DCMAKE_C_COMPILER="C:/Qt/Tools/mingw1310_64/bin/gcc.exe" `
  -DCMAKE_CXX_COMPILER="C:/Qt/Tools/mingw1310_64/bin/g++.exe"
cmake --build build -j
```

При смене компилятора удалите `build/` и сконфигурируйте заново.

## Проблема «молча не запускается»

1. **QML не загрузился** — процесс выходит с кодом 1. Запустите из консоли, будут сообщения Qt.
2. **Конфликт MinGW** — exe собран одним g++, а `windeployqt` положил DLL от другого. Используйте MinGW из Qt Tools (см. выше).

## Запуск из build

```powershell
.\build\nyx-app.exe
```

## Установщик

`NyxSetup.exe` содержит приложение, Qt, MinGW runtime и деинсталлятор. На ПК пользователя ничего ставить вручную не нужно.

Если окно не появляется:

```powershell
.\build\nyx-app.exe 2>&1
```

Пришлите текст из консоли.
