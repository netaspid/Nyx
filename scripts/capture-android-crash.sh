#!/usr/bin/env bash
# Capture Nyx crash log from a USB-connected Android phone.
set -euo pipefail

ADB="${ADB:-$HOME/Android/Sdk/platform-tools/adb}"
OUT="${1:-/tmp/nyx-crash.log}"

if [[ ! -x "$ADB" ]]; then
  echo "adb not found at $ADB" >&2
  exit 1
fi

usb_android_nodes() {
  lsusb | awk '/Xiaomi|2a45|2717|18d1|Google Inc|ID 0e8d|MediaTek/{printf "/dev/bus/usb/%03d/%03d\n",$2,$4}' | tr -d ':'
}

fix_usb_perms() {
  echo "[nyx] Fixing USB permissions (sudo)…"
  mapfile -t nodes < <(usb_android_nodes)
  for n in "${nodes[@]}"; do
    [[ -e "$n" ]] && sudo chmod a+rw "$n" || true
  done
  if [[ ! -f /etc/udev/rules.d/51-android.rules ]]; then
    echo '[nyx] Installing udev rule for Android USB…'
    echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="2717", MODE="0666", GROUP="plugdev"
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", MODE="0666", GROUP="plugdev"
SUBSYSTEM=="usb", ATTR{idVendor}=="0e8d", MODE="0666", GROUP="plugdev"' \
      | sudo tee /etc/udev/rules.d/51-android.rules >/dev/null
    sudo udevadm control --reload-rules
    sudo udevadm trigger
  fi
}

reset_usb_android() {
  mapfile -t nodes < <(usb_android_nodes)
  if [[ ${#nodes[@]} -eq 0 ]]; then
    echo "[nyx] USB: телефон не виден в lsusb (режим зарядки? MTP? другой кабель/порт?)"
    return 1
  fi
  echo "[nyx] USB reset: ${nodes[*]}"
  for n in "${nodes[@]}"; do
    [[ -e "$n" ]] || continue
    sudo chmod a+rw "$n" 2>/dev/null || true
    # USBDEVFS_RESET — оживляет «залипший» ADB без перетыкания кабеля
    python3 - "$n" <<'PY' 2>/dev/null || true
import fcntl, os, sys
USBDEVFS_RESET = 21780
path = sys.argv[1]
fd = os.open(path, os.O_RDWR)
try:
    fcntl.ioctl(fd, USBDEVFS_RESET, 0)
    print(f"[nyx] reset ok: {path}")
finally:
    os.close(fd)
PY
  done
}

restart_adb() {
  echo "[nyx] Restarting adb…"
  "$ADB" kill-server >/dev/null 2>&1 || true
  killall -q adb 2>/dev/null || true
  sleep 1
  "$ADB" start-server >/dev/null
  sleep 1
}

device_ready() {
  "$ADB" devices 2>/dev/null | grep -qE '[[:space:]]device$'
}

# --- recover if needed ---
if ! device_ready; then
  state="$("$ADB" devices 2>&1 || true)"
  echo "[nyx] Initial devices:"
  echo "$state"

  if echo "$state" | grep -q 'no permissions'; then
    fix_usb_perms
    restart_adb
  fi

  if ! device_ready; then
    # Phone often present in lsusb but missing from adb (stale server / hung USB).
    if usb_android_nodes | grep -q .; then
      restart_adb
      sleep 1
      if ! device_ready; then
        reset_usb_android || true
        sleep 2
        restart_adb
      fi
    fi
  fi
fi

echo "[nyx] Devices:"
"$ADB" devices -l

if ! device_ready; then
  state="$("$ADB" devices 2>&1 || true)"
  echo ""
  if echo "$state" | grep -qE '[[:space:]]unauthorized'; then
    echo "Телефон: unauthorized."
    echo "  1) Разблокируй экран"
    echo "  2) Прими «Разрешить отладку по USB» (галочка «Всегда»)"
    echo "  3) Нет диалога: Для разработчиков → Отозвать разрешения отладки USB → переткни кабель"
  elif echo "$state" | grep -qE '[[:space:]](offline|no permissions)'; then
    echo "Телефон в плохом USB-состоянии ($state)."
    echo "  Попробуй другой порт/кабель, режим «Передача файлов» / PTP, не только зарядка."
  elif ! usb_android_nodes | grep -q .; then
    echo "Телефон не виден ни в adb, ни в lsusb."
    echo "  Проверь кабель/порт. На Xiaomi: уведомление USB → «Передача файлов» или «PTP»."
    echo "  Отладка по USB должна быть включена."
  else
    echo "Телефон есть в USB, но adb его не видит (часто залипший стек)."
    echo "  Уже пробовали kill adb + USB reset. Если снова пусто:"
    echo "  • на телефоне: USB → Передача файлов, затем выкл/вкл «Отладка по USB»"
    echo "  • другой USB-порт (лучше напрямую, не через хаб)"
    echo "  • sudo $(basename "$0") …  если chmod на /dev/bus/usb не сработал"
  fi
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
