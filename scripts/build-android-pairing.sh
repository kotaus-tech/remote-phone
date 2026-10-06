#!/usr/bin/env bash
set -euo pipefail

REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
NDK_ROOT="${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:-}}"

if [[ -z "$NDK_ROOT" && -n "$ANDROID_SDK_ROOT" && -d "$ANDROID_SDK_ROOT/ndk" ]]; then
  NDK_ROOT="$(find "$ANDROID_SDK_ROOT/ndk" -mindepth 1 -maxdepth 1 -type d -print | sort -V | tail -n 1)"
fi
if [[ -z "$NDK_ROOT" || ! -d "$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin" ]]; then
  echo "Android NDK для Linux x64 не найден. Установите NDK 27.2.12479018." >&2
  exit 1
fi

LINKER="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang"
if [[ ! -x "$LINKER" ]]; then
  echo "Не найден Android C-linker: $LINKER" >&2
  exit 1
fi

export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$LINKER"
cargo build \
  --manifest-path "$REPOSITORY_ROOT/native/Cargo.toml" \
  --package remote-phone-pairing-bridge \
  --target aarch64-linux-android \
  --release \
  --locked

JNI_LIBS="$REPOSITORY_ROOT/apps/android/app/build/generated/jniLibs/arm64-v8a"
mkdir -p "$JNI_LIBS"
cp "$REPOSITORY_ROOT/native/target/aarch64-linux-android/release/libremote_phone_pairing_bridge.so" \
  "$JNI_LIBS/libremote_phone_pairing_bridge.so"
echo "Собран Rust pairing-core для Android arm64: $JNI_LIBS/libremote_phone_pairing_bridge.so"
