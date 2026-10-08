#!/usr/bin/env bash
set -euo pipefail

REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
NDK_ROOT="${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:-}}"

if [[ -z "$NDK_ROOT" && -n "$ANDROID_SDK_ROOT" && -d "$ANDROID_SDK_ROOT/ndk" ]]; then
  NDK_ROOT="$(find "$ANDROID_SDK_ROOT/ndk" -mindepth 1 -maxdepth 1 -type d -print | sort -V | tail -n 1)"
fi
NDK_BIN="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin"
if [[ -z "$NDK_ROOT" || ! -d "$NDK_BIN" ]]; then
  echo "Android NDK для Linux x64 не найден. Установите NDK 27.2.12479018." >&2
  exit 1
fi

export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$NDK_BIN/aarch64-linux-android35-clang"
export CARGO_TARGET_X86_64_LINUX_ANDROID_LINKER="$NDK_BIN/x86_64-linux-android35-clang"
for linker in "$CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER" "$CARGO_TARGET_X86_64_LINUX_ANDROID_LINKER"; do
  if [[ ! -x "$linker" ]]; then
    echo "Не найден Android C-linker: $linker" >&2
    exit 1
  fi
done

TARGETS=(aarch64-linux-android x86_64-linux-android)
ABIS=(arm64-v8a x86_64)
for index in "${!TARGETS[@]}"; do
  target="${TARGETS[$index]}"
  abi="${ABIS[$index]}"
  cargo build \
    --manifest-path "$REPOSITORY_ROOT/native/Cargo.toml" \
    --package remote-phone-pairing-bridge \
    --target "$target" \
    --release \
    --locked

  archive="$REPOSITORY_ROOT/native/target/$target/release/libremote_phone_pairing_bridge.a"
  static_libs="$REPOSITORY_ROOT/apps/android/app/build/generated/pairingStatic/$abi"
  if [[ ! -f "$archive" ]]; then
    echo "Не собрана статическая библиотека Rust: $archive" >&2
    exit 1
  fi
  mkdir -p "$static_libs"
  cp "$archive" "$static_libs/libremote_phone_pairing_bridge.a"
  echo "Собран Rust pairing-core для Android $abi: $static_libs/libremote_phone_pairing_bridge.a"
done
