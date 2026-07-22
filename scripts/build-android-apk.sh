#!/usr/bin/env bash
# One-shot: install deps (Ubuntu), configure Qt/Android, build and sign a sideloadable APK.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${NYX_ANDROID_BUILD_DIR:-$ROOT/build-android}"
QT_VER="${NYX_QT_VERSION:-6.5.3}"
QT_ROOT="${NYX_QT_ROOT:-$HOME/.local/Qt}"
SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
NDK_VERSION="${NYX_ANDROID_NDK_VERSION:-25.2.9519653}"
API_LEVEL="${NYX_ANDROID_API:-34}"
BUILD_TOOLS="${NYX_ANDROID_BUILD_TOOLS:-34.0.0}"
OUT_APK="$BUILD_DIR/Nyx.apk"
KEYSTORE="${NYX_ANDROID_KEYSTORE:-$HOME/.android/debug.keystore}"
KEY_ALIAS="${NYX_ANDROID_KEY_ALIAS:-androiddebugkey}"
STOREPASS="${NYX_ANDROID_STOREPASS:-android}"
KEYPASS="${NYX_ANDROID_KEYPASS:-android}"

log() { printf '[nyx-android] %s\n' "$*"; }
die() { printf '[nyx-android] ERROR: %s\n' "$*" >&2; exit 1; }

need_cmd() { command -v "$1" >/dev/null 2>&1 || die "missing command: $1"; }

pkg_installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'install ok installed'; }

# Prefer a real JDK 17 (javac present). OpenJDK JRE-only packages are not enough for Gradle.
find_jdk17_home() {
  local cand
  for cand in \
    "${JAVA_HOME:-}" \
    /usr/lib/jvm/jdk-17*-oracle* \
    /usr/lib/jvm/java-17-openjdk-amd64 \
    /usr/lib/jvm/java-1.17.0-openjdk-amd64 \
    /usr/lib/jvm/zulu-17* \
    /usr/lib/jvm/temurin-17*; do
    [[ -n "$cand" && -x "$cand/bin/javac" && -x "$cand/bin/java" ]] || continue
    local major
    major="$("$cand/bin/java" -version 2>&1 | sed -n 's/.*version "\([0-9]*\).*/\1/p' | head -1)"
    [[ "$major" == "17" ]] && { printf '%s' "$cand"; return 0; }
  done
  return 1
}

run_apt() {
  if command -v sudo >/dev/null 2>&1; then
    sudo "$@"
  else
    "$@"
  fi
}

install_apt() {
  local pkgs=(
    curl unzip wget ca-certificates
    build-essential cmake ninja-build perl
    python3 python3-venv python3-pip
    libxkbcommon-dev
  )
  local missing=()
  local p
  for p in "${pkgs[@]}"; do
    pkg_installed "$p" || missing+=("$p")
  done
  if ! find_jdk17_home >/dev/null; then
    missing+=(openjdk-17-jdk)
  fi

  apt_try_install() {
    local -a want=("$@")
    ((${#want[@]})) || return 0
    log "Installing apt packages: ${want[*]} (may need sudo)…"
    # Broken third-party PPAs (webupd8team/java, trueconf, …) make `apt-get update`
    # exit non-zero even when Ubuntu main indexes are usable — do not abort.
    if ! run_apt apt-get update -qq; then
      log "WARN: apt-get update failed (broken third-party repos?). Continuing with cached indexes…"
    fi
    run_apt env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${want[@]}"
  }

  if ((${#missing[@]})); then
    apt_try_install "${missing[@]}" || log "WARN: apt-get install had errors; checking tools…"
  else
    log "Required apt packages already present — skipping apt."
  fi

  for p in curl unzip wget ca-certificates build-essential cmake ninja-build perl \
           python3 python3-venv python3-pip; do
    pkg_installed "$p" || die "required package missing after apt: $p"
  done
  if ! find_jdk17_home >/dev/null; then
    die "No JDK 17 with javac found. Fix apt repos or install openjdk-17-jdk / Oracle JDK 17, then re-run."
  fi
}

pick_java17() {
  local home
  home="$(find_jdk17_home)" || die "Need JDK 17 with javac for Gradle. Set JAVA_HOME to a JDK 17 install."
  export JAVA_HOME="$home"
  export PATH="$JAVA_HOME/bin:$PATH"
  log "JAVA_HOME=$JAVA_HOME ($("$JAVA_HOME/bin/java" -version 2>&1 | head -1))"
}

ensure_sdk() {
  export ANDROID_SDK_ROOT="$SDK_ROOT"
  export ANDROID_HOME="$SDK_ROOT"
  mkdir -p "$SDK_ROOT/cmdline-tools"
  if [[ ! -x "$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager" ]]; then
    log "Downloading Android cmdline-tools…"
    local zip=/tmp/nyx-android-cmdline-tools.zip
    curl -fsSL -o "$zip" \
      https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
    rm -rf /tmp/nyx-cmdline-tools
    mkdir -p /tmp/nyx-cmdline-tools
    unzip -q "$zip" -d /tmp/nyx-cmdline-tools
    rm -rf "$SDK_ROOT/cmdline-tools/latest"
    mv /tmp/nyx-cmdline-tools/cmdline-tools "$SDK_ROOT/cmdline-tools/latest"
  fi
  export ANDROID_NDK_ROOT="$SDK_ROOT/ndk/${NDK_VERSION}"
  if [[ -d "$ANDROID_NDK_ROOT" \
        && -d "$SDK_ROOT/platforms/android-${API_LEVEL}" \
        && -d "$SDK_ROOT/build-tools/${BUILD_TOOLS}" ]]; then
    log "ANDROID_SDK_ROOT=$ANDROID_SDK_ROOT (cached)"
    log "ANDROID_NDK_ROOT=$ANDROID_NDK_ROOT"
    return 0
  fi
  local sdkm="$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager"
  # Accept licenses once; avoid `yes |` hang on already-accepted licenses.
  yes | "$sdkm" --sdk_root="$SDK_ROOT" --licenses >/dev/null 2>&1 || true
  "$sdkm" --sdk_root="$SDK_ROOT" \
    "platform-tools" \
    "platforms;android-${API_LEVEL}" \
    "build-tools;${BUILD_TOOLS}" \
    "ndk;${NDK_VERSION}" || true
  [[ -d "$ANDROID_NDK_ROOT" ]] || die "NDK not found at $ANDROID_NDK_ROOT"
  log "ANDROID_SDK_ROOT=$ANDROID_SDK_ROOT"
  log "ANDROID_NDK_ROOT=$ANDROID_NDK_ROOT"
}

ensure_aqt() {
  if command -v aqt >/dev/null 2>&1; then
    return
  fi
  if command -v pipx >/dev/null 2>&1; then
    pipx install aqtinstall >/dev/null 2>&1 || pipx upgrade aqtinstall >/dev/null 2>&1 || true
  fi
  if ! command -v aqt >/dev/null 2>&1; then
    local venv="$ROOT/.venv-aqt"
    python3 -m venv "$venv"
    "$venv/bin/pip" install -q aqtinstall
    export PATH="$venv/bin:$PATH"
  fi
  need_cmd aqt
}

ensure_qt() {
  local host="$QT_ROOT/$QT_VER/gcc_64"
  local andr="$QT_ROOT/$QT_VER/android_arm64_v8a"
  mkdir -p "$QT_ROOT"
  if [[ ! -x "$host/bin/qmake" ]]; then
    log "Fetching Qt $QT_VER host (gcc_64)…"
    aqt install-qt linux desktop "$QT_VER" gcc_64 -O "$QT_ROOT" -m qtmultimedia
  fi
  if [[ ! -x "$andr/bin/qt-cmake" ]]; then
    log "Fetching Qt $QT_VER Android (android_arm64_v8a)…"
    aqt install-qt linux android "$QT_VER" android_arm64_v8a -O "$QT_ROOT" -m qtmultimedia --autodesktop
  fi
  export QT_HOST_PATH="$host"
  QT_ANDROID="$andr"
  [[ -x "$QT_ANDROID/bin/qt-cmake" ]] || die "qt-cmake missing under $QT_ANDROID"
  log "Qt Android: $QT_ANDROID"
  log "Qt host:    $QT_HOST_PATH"
}

ensure_debug_keystore() {
  mkdir -p "$(dirname "$KEYSTORE")"
  if [[ ! -f "$KEYSTORE" ]]; then
    log "Creating Android debug keystore at $KEYSTORE…"
    keytool -genkeypair -v \
      -keystore "$KEYSTORE" \
      -storepass "$STOREPASS" \
      -keypass "$KEYPASS" \
      -alias "$KEY_ALIAS" \
      -keyalg RSA -keysize 2048 -validity 10000 \
      -dname "CN=Android Debug,O=Android,C=US"
  fi
}

configure_and_build() {
  export ANDROID_SDK_ROOT ANDROID_HOME ANDROID_NDK_ROOT JAVA_HOME QT_HOST_PATH
  export PATH="$JAVA_HOME/bin:$PATH"
  log "Configuring $BUILD_DIR…"
  # If _deps were already fetched, skip git update (avoids failing on flaky GitHub).
  "$QT_ANDROID/bin/qt-cmake" -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DANDROID_SDK_ROOT="$ANDROID_SDK_ROOT" \
    -DANDROID_NDK_ROOT="$ANDROID_NDK_ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DQT_HOST_PATH="$QT_HOST_PATH" \
    -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
  log "Building…"
  cmake --build "$BUILD_DIR" -j"$(nproc)"
}

sign_apk() {
  ensure_debug_keystore
  local deploy_settings="$BUILD_DIR/android-nyx-app-deployment-settings.json"
  [[ -f "$deploy_settings" ]] || die "missing $deploy_settings (build nyx-app first)"
  local androiddeployqt="$QT_HOST_PATH/bin/androiddeployqt"
  [[ -x "$androiddeployqt" ]] || die "androiddeployqt not found"
  local android_build="$BUILD_DIR/android-build"
  log "Packaging and signing APK (sideload-ready)…"
  "$androiddeployqt" \
    --input "$deploy_settings" \
    --output "$android_build" \
    --apk "$OUT_APK" \
    --release \
    --sign "$KEYSTORE" "$KEY_ALIAS" \
    --storepass "$STOREPASS" \
    --keypass "$KEYPASS"
  [[ -f "$OUT_APK" ]] || die "APK was not produced at $OUT_APK"
  log "Done: $OUT_APK"
  log "Copy to the phone, allow install from this source, and open the APK."
}

main() {
  [[ "$(uname -s)" == "Linux" ]] || die "This script is for Ubuntu/Linux. On Windows use scripts/build-android-apk.ps1"
  if [[ "${1:-}" == "--deps-only" ]]; then
    install_apt
    pick_java17
    ensure_sdk
    ensure_aqt
    ensure_qt
    ensure_debug_keystore
    log "Dependencies ready."
    exit 0
  fi
  install_apt
  pick_java17
  ensure_sdk
  ensure_aqt
  ensure_qt
  configure_and_build
  sign_apk
}

main "$@"
