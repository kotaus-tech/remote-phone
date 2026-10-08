#!/usr/bin/env bash
set -euo pipefail

if [[ ! -x ./gradlew ]]; then
  echo "Не найден apps/android/gradlew; проверьте working-directory шага эмулятора." >&2
  exit 2
fi

log="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/android-emulator-test.log"
if ./gradlew --no-daemon --stacktrace connectedDebugAndroidTest > "$log" 2>&1; then
  cat "$log"
else
  status=$?
  cat "$log"
  diagnostics=$(
    { grep -Ein -C 1 'FAILURE:|What went wrong:|Execution failed for task|Caused by:|UnsatisfiedLinkError|AssertionError|Tests? run:|FAILED|No tests found|error:' "$log" || true; } \
      | sed -n '1,80p'
  )
  if [[ -z "$diagnostics" ]]; then
    diagnostics=$(tail -n 120 "$log")
  fi
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    {
      echo '### Ошибка Android instrumented test: существенные строки Gradle/JUnit'
      printf '%s\n' "$diagnostics"
      echo '### Хвост журнала'
      tail -n 80 "$log"
    } >> "$GITHUB_STEP_SUMMARY"
  fi
  details=$(printf '%s\n' "$diagnostics" | tr -d '\r' | tr '\n' ' ' | cut -c 1-3500 | sed 's/%/%25/g')
  echo "::error title=Android emulator test failure::$details"
  exit "$status"
fi
