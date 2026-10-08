#!/usr/bin/env bash
set -euo pipefail

REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
NDK_ROOT="${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:-}}"
if [[ -z "$NDK_ROOT" && -n "$ANDROID_SDK_ROOT" && -d "$ANDROID_SDK_ROOT/ndk" ]]; then
  NDK_ROOT="$(find "$ANDROID_SDK_ROOT/ndk" -mindepth 1 -maxdepth 1 -type d -print | sort -V | tail -n 1)"
fi
NDK_BIN="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin"
BRIDGE="$REPOSITORY_ROOT/native/target/aarch64-linux-android/release/libremote_phone_pairing_bridge.so"
CLANGXX="$NDK_BIN/aarch64-linux-android35-clang++"
READELF="$NDK_BIN/llvm-readelf"

for required in "$BRIDGE" "$CLANGXX" "$READELF"; do
  if [[ ! -e "$required" ]]; then
    echo "Не найден файл для диагностики динамической линковки: $required" >&2
    exit 1
  fi
done

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
PROBE="$TMP_DIR/libremote_phone_jni_dynamic_probe.so"
"$CLANGXX" \
  -shared \
  -fPIC \
  -std=c++17 \
  -DNDEBUG \
  -Wl,--no-undefined \
  -I"$REPOSITORY_ROOT/native/pairing-bridge/include" \
  "$REPOSITORY_ROOT/apps/android/app/src/main/cpp/remote_phone_jni.cpp" \
  "$BRIDGE" \
  -llog \
  -o "$PROBE"

echo "===== Cargo Rust cdylib (legacy runtime dependency candidate; not packaged) ====="
"$READELF" -d "$BRIDGE"
echo "===== JNI shared object linked against the full-path cdylib (legacy probe; not packaged) ====="
"$READELF" -d "$PROBE"
