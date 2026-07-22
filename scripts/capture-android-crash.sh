#!/usr/bin/env bash
# Capture Nyx crash log from a USB-connected Android phone.
set -euo pipefail

ADB="${ADB:-$HOME/Android/Sdk/platform-tools/adb}"
OUT="${1:-/tmp/nyx-crash.log}"

if [[ ! -x "$ADB" ]]; then
  echo "adb not found at $ADB" >&2
  exit 1
fi

# Fix common Ubuntu permission issue (needs sudo once).
if "$ADB" devices 2>&1 | grep -q 'no permissions'; then
  echo "[nyx] Fixing USB permissions (sudo)…"
  mapfile -t nodes < <(lsusb | awk '/Xiaomi|2a45|2717|18d1|Google Inc|ID 0e8d/{printf "/dev/bus/usb/%03d/%03d\n",$2,$4}' | tr -d ':')
  for n in "${nodes[@]}"; do
    [[ -e "$n" ]] && sudo chmod a+rw "$n" || true
  done
  # Persist udev rule for Xiaomi ADB
  if [[ ! -f /etc/udev/rules.d/51-android.rules ]]; then
    echo '[nyx] Installing udev rule for Android USB…'
    echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="2717", MODE="0666", GROUP="plugdev"
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", MODE="0666", GROUP="plugdev"
SUBSYSTEM=="usb", ATTR{idVendor}=="0e8d", MODE="0666", GROUP="plugdev"' \
      | sudo tee /etc/udev/rules.d/51-android.rules >/dev/null
    sudo udevadm control --reload-rules
    sudo udevadm trigger
  fi
  "$ADB" kill-server || true
  "$ADB" start-server
fi

echo "[nyx] Devices:"
"$ADB" devices -l

if ! "$ADB" devices | grep -qE '[[:space:]]device$'; then
  echo ""
  echo "Телефон не готов. Если статус 'unauthorized':"
  echo "  1) Разблокируй экран телефона"
  echo "  2) Прими диалог «Разрешить отладку по USB» (галочка «Всегда»)"
  echo "  3) Если диалога нет: Для разработчиков → Отозвать разрешения отладки USB → переткни кабель"
  exit 1
fi

echo "[nyx] Model: $("$ADB" shell getprop ro.product.model | tr -d '\r')"
echo "[nyx] Android: $("$ADB" shell getprop ro.build.version.release | tr -d '\r')"
echo "[nyx] Clearing logcat, launch Nyx on the phone NOW…"
"$ADB" logcat -c
sleep 6
"$ADB" logcat -d -v threadtime > "$OUT"
echo "[nyx] Saved: $OUT ($(wc -l < "$OUT") lines)"
echo "[nyx] Crash snippets:"
rg -n -i 'FATAL|AndroidRuntime|Fatal signal|SIGSEGV|SIGABRT|org\.nyx|libnyx|Qt |DEBUG.*nyx' "$OUT" | head -80 || true
