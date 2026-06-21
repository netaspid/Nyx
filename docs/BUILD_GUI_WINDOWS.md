# Сборка nyx-app на Windows (Qt + MinGW)

## Проблема «молча не запускается»

Чаще всего одно из двух:

1. **QML не загрузился** — процесс сразу выходит с кодом 1 (нет окна). После пересборки в консоли будут сообщения Qt.
2. **Конфликт MinGW** — exe собран одним g++ (например 15.2), а `windeployqt` положил `libstdc++-6.dll` от Qt (13.1). Тогда Windows показывает ошибку DLL или процесс падает без окна.

## Рекомендуемая сборка

Используйте **MinGW из Qt**, тот же что и для Qt DLL:

```powershell
Remove-Item -Recurse -Force build   # если меняете компилятор
cmake -B build -G "MinGW Makefiles" `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/mingw_64" `
  -DCMAKE_C_COMPILER="C:/Qt/Tools/mingw1310_64/bin/gcc.exe" `
  -DCMAKE_CXX_COMPILER="C:/Qt/Tools/mingw1310_64/bin/g++.exe"
cmake --build build
```

`windeployqt` и копирование MinGW runtime выполняются автоматически после сборки `nyx-app`.

## Запуск

```powershell
.\build\nyx-app.exe --profile aspid.json
```

Запускайте из любой папки — DLL лежат в `build\` рядом с exe.

## Если окно всё ещё не появляется

```powershell
.\build\nyx-app.exe --profile aspid.json 2>&1
```

Пришлите текст из консоли — там будут ошибки QML или Qt platform plugin.
