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
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    {
      echo '### Ошибка Android instrumented test'
      tail -n 120 "$log"
    } >> "$GITHUB_STEP_SUMMARY"
  fi
  details=$(tail -n 80 "$log" | tr -d '\r' | tr '\n' ' ' | cut -c 1-3500 | sed 's/%/%25/g')
  echo "::error title=Android emulator test failure::$details"
  exit "$status"
fi
