#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 <apk> [llvm-readelf]" >&2
  exit 2
fi
APK="$1"
READELF="${2:-llvm-readelf}"

if [[ ! -f "$APK" ]]; then
  echo "APK не найден: $APK" >&2
  exit 1
fi
if ! command -v "$READELF" >/dev/null 2>&1; then
  echo "Не найден llvm-readelf: $READELF" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
mapfile -t LIBRARIES < <(unzip -Z1 "$APK" | awk '/^lib\/[^/]+\/[^/]+\.so$/')
if [[ ${#LIBRARIES[@]} -eq 0 ]]; then
  echo "В APK не найдены нативные библиотеки." >&2
  exit 1
fi

for library in "${LIBRARIES[@]}"; do
  extracted="$TMP_DIR/$library"
  mkdir -p "$(dirname "$extracted")"
  unzip -p "$APK" "$library" > "$extracted"
  echo "===== llvm-readelf -d $library ====="
  dynamic="$($READELF -d "$extracted")"
  printf '%s\n' "$dynamic"
  if ! printf '%s\n' "$dynamic" | awk '
      /\(NEEDED\)|\(SONAME\)/ {
          value = $0
          sub(/^.*\[/, "", value)
          sub(/\].*$/, "", value)
          if (index(value, "/") != 0) {
              printf "Ошибка: DT_NEEDED/DT_SONAME содержит путь: %s\n", value > "/dev/stderr"
              invalid = 1
          }
      }
      END { exit invalid }
    '; then
    echo "Нативная библиотека содержит путь в DT_NEEDED/DT_SONAME: $library" >&2
    exit 1
  fi
done
