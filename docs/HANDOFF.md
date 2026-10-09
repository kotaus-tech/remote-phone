# Передача проекта следующему агенту

**Срез состояния: 2026-10-08 (обновление после смены агента).** Кодовая вершина — merge `a79a12b` (PR #1, ветка `arena/71439185-remote-phone` влита в `main`); локальная ветка сессии `arena/f4239082-remote-phone` стоит на той же вершине, рабочая копия чистая. Последний зелёный CI на `main` — [run 37832938506](https://github.com/kotaus-tech/remote-phone/actions/runs/37832938506) (2026-10-08). Последний green CI исходного кода до merge — [run 37801041033](https://github.com/kotaus-tech/remote-phone/actions/runs/37801041033) на `eeca2042` (`Fix WebRTC NV12 video frame conversion`).

> **Исправление scope (важно):** предыдущая версия этого handoff ошибочно сводила проект только к режиму «Веб-камера» и запрещала реализовывать захват экрана. Это была ошибка временного сужения предыдущего handoff, **а не изменение требований пользователя**. Исходное ТЗ проекта включает **два обязательных полноценных режима**: «Экран» (трансляция экрана телефона на Windows + управление телефоном с ПК жестами) и «Веб-камера» (камера телефона как настоящая Windows virtual camera), плюс удалённое управление через Accessibility Service. Этот handoff исправлен под полный scope. Удалённое управление и оба режима — не опция и не «потом».

## 1. Конечная цель (полный scope исходного ТЗ)

Довести локальное приложение Android + Windows 11 x64 до надёжного продукта «Видоискатель» с **двумя равноправными режимами**, работающими по защищённому локальному соединению (временный PIN-сеанс, OPAQUE, LAN-only, без облака/аккаунтов/STUN/TURN):

1. **«Экран»** — живая трансляция всего экрана телефона (MediaProjection, foreground service, переживает сворачивание/блокировку/ротацию/открытие камеры) на Windows с ориентиром 30–60 FPS и ~100–150 мс в стабильной 5 GHz LAN, **плюс полноценное управление телефоном с ПК жестами** через Accessibility Service (tap, long press, live swipe, scroll, pinch/zoom, Back/Home/Recents). Accessibility Service выполняет жесты и **не читает содержимое экрана** — никакого OCR/анализа UI. Ввод текста с клавиатуры запрещён.
2. **«Веб-камера»** — живое изображение Camera2 с телефона появляется как настоящая Windows Media Foundation virtual camera (`MFCreateVirtualCamera`, COM media source, C++/MSVC, per-machine Setup) и работает в Camera/Chrome/Edge/Firefox/Discord/Telegram/Zoom/Teams/OBS. Live Camera2 → WebRTC → Windows virtual camera; видео only, без аудио. Полный набор camera controls (модули, zoom, focus, exposure, ISO, shutter, WB, stabilization, HDR — по реальным возможностям Camera2), пресеты Stream/Call/Night, mirror toggle, телеметрия телефона.

Оба режима обязательны. USB — только как будущий transport для Webcam **после** полного закрытия Wi-Fi Webcam, обоих телефонов и virtual camera E2E и явного подтверждения пользователя; сейчас только transport abstraction. Не реализовывать USB в основном цикле.

## 2. Жёсткие продуктовые и архитектурные требования

- **Платформы:** Windows 11 x64 (Electron + React + TypeScript, Fluent-like UI, тёмная тема); Android **минимум API 30 / Android 11** (сейчас `minSdk = 26` — **поднять до 30**, это известный долг), Kotlin + Jetpack Compose + Material 3. Целевые устройства: OPPO Reno 15 / Android 16 / ColorOS 16 и POCO X3 NFC / Android 11 / MIUI 12.5.7. Возможности определяются runtime capability detection, без жёстких предположений о камерах/FPS конкретных устройств.
- **Транспорт:** WebRTC по локальной Wi-Fi (DTLS-SRTP для media, без STUN/TURN), mDNS/DNS-SD discovery + manual local IP fallback, отдельный защищённый data channel для команд/телеметрии. Транспорт отделить от session/camera/UI так, чтобы потом можно было добавить USB transport без переписывания этих слоёв.
- **Одноразовое сопряжение:** временный 8-значный PIN на каждый Start-сеанс, срок 5 минут, максимум 5 попыток, PIN не передаётся по сети/mDNS и не попадает в logs/diagnostics; после authentication — ключи временной сессии и защищённый канал. Reconnect после краткого Wi-Fi interruption без повторного PIN, пока телефон продолжает сессию. Один телефон — один PC одновременно. Нет cloud/аккаунтов/saved devices/persistent pairing/relay/STUN/TURN.
- **Криптография:** общий Rust OPAQUE-core по RFC 9807 (`opaque-ke = 4.0.1`, Ristretto255/SHA-512, Argon2id профиль из `docs/PAIRING_PROTOCOL.md`). Не переписывать без объективной причины; исправлять только реальные проблемы.
- **Android lifecycle (ключевое архитектурное требование):** долгоживущий session живёт в **foreground service**, а не в Activity/Compose `DisposableEffect`. Session должен переживать minimize, переход в другое приложение, открытие штатной/сторонней камеры, блокировку/гашение экрана (насколько допускает Android), поворот, возврат в приложение — без уничтожения capture session и без повторного запроса MediaProjection. Работать на Android 11/MIUI 12.5.7 и Android 16/ColorOS 16 с учётом battery optimization/autostart/background restrictions (onboarding — вести пользователя в нужные system settings, честно помечать «не проверено», если состояние нельзя надёжно узнать).
- **Режим «Экран»:** MediaProjection; камера/микрофон/аудио НЕ используются; 30–60 FPS, low latency, Quality Auto/Max/Economy, без искусственной буферизации; DRM/`FLAG_SECURE` не обходить (защищённые окна остаются чёрными в рамках Android); остановка из Android UI, из persistent notification и с Windows.
- **Управление с ПК:** только жесты (coandidаты только внутри видимого frame), жест следует за мышью максимально непосредственно, без искусственной queue, при disconnect палец всегда released, keyboard text input запрещён.
- **Веб-камера:** видео only, аудио запрещено; горизонтальный 16:9; mirror toggle; все реальные Camera2 resolutions/fps (включая 4K60 если доступно); приоритет latency → стабильность → качество; при потере live stream — No signal + controlled recovery без перезапуска внешних приложений.
- **Camera capability detection:** модули main/ultrawide/tele/macro/front/logical; контроли zoom/torch/AF/tap focus/focus lock/AE/EV/tap metering/manual ISO/shutter/exposure lock/WB/WB presets/manual WB Kelvin/tint/WB lock/stabilization/OIS/EIS/noise/sharpness/flicker/HDR — каждый Auto/Manual и Lock где API допускает; неподдерживаемое скрыто или disabled с честной подсказкой; изменения live. Пресеты Stream/Call/Night + restore last settings.
- **Телеметрия на Windows:** battery, charging, temperature, thermal status, Wi-Fi signal, network speed, actual FPS, bitrate, latency, dropped frames; warnings (перегрев, низкий заряд, слабый Wi-Fi, занятая камера, missing permission, ColorOS/MIUI restrictions). При перегреве — graceful degradation FPS/resolution + понятное предупреждение, без падения.
- **Windows virtual camera:** Media Foundation (`MFCreateVirtualCamera`, COM media source, C++/MSVC, no DirectShow, no third-party driver); per-machine Setup, x64 HKLM registration, Program Files, корректный uninstall; приложение не требует admin; никакой ручной COM-регистрации. CPU-first путь `WebRTC VideoFrame → RGBA → NV12 → shared memory → Media Foundation` — базовый production path (не возвращать `VideoFrame.copyTo(... NV12)`). GPU optimisation не должна блокировать CPU path. Доказательство — внешнее приложение показывает именно live phone image, а не счётчик/stdin/shared-memory записи.
- **UX:** страницы Windows: Devices/Connection, Screen, Webcam, Settings, Diagnostics (ни Screen, ни Webcam не должны остаться placeholder/тестовой панелью). Android main: две крупные карточки «Экран»/«Веб-камера», понятные Start/Stop, current status, pairing PIN, control toggle где применимо. Progressive disclosure (простой режим по умолчанию, Pro отдельно). Dark only, Russian only, единая design system на обеих платформах.
- **Do NOT:** recording, audio, keyboard text injection, cloud, accounts, remote server, saved devices, STUN/TURN, bypass FLAG_SECURE, screen reading, OCR, другие ОС, light theme, USB (пока).
- **Работа с пользователем:** у пользователя нет ADB и он не может прислать Android-журнал. Не просить `adb`/`logcat`/сырые логи. Для аппаратной проверки — только выполнимые шаги и наблюдаемые результаты на экране; диагностика через встроенный экран «Диагностика».

## 3. Фактическая архитектура на вершине `a79a12b`

### Android (`apps/android`)

- Kotlin, Jetpack Compose, Material 3; `minSdk = 26` (**долг: поднять до 30**), `targetSdk = 35`, `compileSdk = 35`; WebRTC AAR `io.github.webrtc-sdk:android:150.7871.01`; Java-WebSocket для pairing endpoint.
- `MainActivity.kt` — две карточки режимов; карточка «Экран» **отключена** в UI («приложение не считывает содержимое экрана»), переключатель управления disabled. `PhonePairingHost` создаётся в Compose и владеется `DisposableEffect` — **session привязан к Activity lifecycle**, это главный архитектурный недочёт (см. §5).
- `PhonePairingHost.kt` — временный WebSocket endpoint, PIN-сеанс, mDNS (`_remotephone._tcp.`) через `NsdManager`, делегирует криптографию JNI/Rust. PIN/ключи — во временной сессии. Один активный connection, лимит 5 попыток, срок 5 минут.
- `PhoneCameraWebRtc.kt` — Camera2 capturer → один WebRTC video track; ICE-серверов нет; захват ограничен 30 fps и 3840×2160; аудио и screen capturer отсутствуют; `RTCDataChannel` принимается, но сообщения **не обрабатываются** (канал фактически зарезервирован). Первый back-facing camera — без capability detection и без camera controls.
- `NativePairing.kt` + `remote_phone_jni.cpp` — JNI-мост; общий Rust C-bridge (`native/pairing-bridge`) статически линкуется в `libremote_phone_jni.so` для `arm64-v8a` и `x86_64`.
- **Отсутствует:** foreground service (нет Service-компонента и `FOREGROUND_SERVICE*` permissions в манифесте), MediaProjection, Accessibility Service, camera controls, telemetry, reconnect/ICE-restart.

### Windows / Electron (`apps/desktop`)

- Electron 44 + React/TypeScript; main/preload/IPC — `electron/main.cjs`, `preload.cjs`; `contextIsolation`, `sandbox`, без `nodeIntegration`. Renderer — `src/App.tsx`.
- `pairing.cjs` — `bonjour-service` DNS-SD/mDNS, проверка локального адреса, WebSocket к телефону, Rust C ABI через `koffi`; ручной ввод локального IP.
- `rtcCameraSession.ts` — `RTCPeerConnection({iceServers: []})`, `recvonly` video + data channel `remote-phone-control` (закрывает входящие сообщения), offer/answer/ICE через зашифрованный pairing-сигнал; трек → скрытый `<video>` → `VideoFrame.copyTo(RGBA)` → ручная конвертация в NV12 → stdin host-процесса.
- `main.cjs` — запускает `RemotePhone.VirtualCameraHost.exe --application-session`, пишет NV12-кадры в stdin (очередь ограничена, незавершённая запись → кадр пропускается), закрытие stdin удаляет сеансовую камеру.
- Страница **«Экран» — placeholder** (нет видео, нет управления); «Настройки» — placeholder; «Веб-камера» — ранняя тестовая панель + GPU-probe эксперимент.

### Native / Rust / Windows camera (`native/`)

- `native/pairing-core` — общий Rust OPAQUE, проверка кадров/transcript, лимиты попыток, шифрование SIGNAL. `native/pairing-bridge` — C ABI: Windows грузит DLL, Android линкует статически в JNI.
- Windows C++/MSVC Media Foundation source на базе зафиксированного MIT-примера Microsoft Windows-Camera (`626f8b19c5f367602f2e89c6b314573d3776c9df`, лицензия в `native/vendor/Windows-Camera/LICENSE`). Setup регистрирует CLSID в HKLM64, DLL в Program Files, ACL ProgramData-журнала; приложение без elevation.
- `native/virtual-camera-host/main.cpp` — NV12-пакеты через stdin; `native/frame-transport/SharedNv12FrameBuffer.*` — трёхслотовое shared-memory кольцо до 3840×2160, метки времени/sequence, очистка, масштабирование; кадр устаревает через 1500 мс; без живого кадра — синтетический fallback.
- Media Foundation объявляет NV12 720p/1080p/4K @30/60 и RGB32 640×480@30 — это заявленные режимы source, не обещание возможностей телефона.
- GPU addon (`apps/desktop/native/gpu-texture-probe`) — диагностическая проба D3D11 shared texture/readback, не production-тракт.

## 4. Реализовано и доказано

### Код и автоматические доказательства

- Общий OPAQUE/Rust-core, wire format, PIN/лимиты, аутентификация сторон, защищённый SIGNAL; детали и оговорки — `docs/PAIRING_PROTOCOL.md`.
- Android JNI статически содержит Rust bridge; CI проверяет состав APK и ELF `DT_NEEDED`/`SONAME`; API 35 x86_64 emulator test проходит production Kotlin → JNI → Rust handshake + round-trip encrypted SIGNAL.
- Desktop offer/answer/ICE, приём WebRTC-видео, RGBA→NV12 конвертация, Windows native host/shared memory и упаковка Setup присутствуют.
- Удалён ручной `delete this` у N-API `AsyncWorker`; Windows CI выполняет lifecycle stress test.
- Зелёный CI: последний на `main` — [run 37832938506](https://github.com/kotaus-tech/remote-phone/actions/runs/37832938506); кодовой HEAD до merge — [run 37801041033](https://github.com/kotaus-tech/remote-phone/actions/runs/37801041033) на `eeca2042`. Три job: Rust pairing-core, Android APK (+emulator test), Windows Setup/native (+desktop tests, install/uninstall).

### Подтверждённое пользователем на оборудовании

- **Synthetic-camera PoC пройден 2026-10-07** на Windows 11 25H2 build 26200.9457, Setup из CI run `37547871094`: движущиеся полосы + растущий счётчик в webcamtests.com, OBS, Discord (в Discord — предпросмотр и звонок); после закрытия «Видоискателя» камера остановилась/искоренилась. Это доказывает установку виртуальной камеры и lifecycle synthetic PoC, **не камеры телефона**.
- Пользовательский smoke report: сопряжение проходит (один успешный сценарий; полная PIN-матрица и физические сетевые проверки не закрыты).
- GPU-проба (пользователь, 2026-10-08): 1280×720, 151 кадр, 127 уникальных, 30,1 fps, D3D11 readback avg 0,48 мс / max 0,8 мс, 0 пропусков — диагностика одной 720p shared texture, не 4K, не CPU-vs-GPU сравнение, не camera E2E.

## 5. Реализовано, но не проверено физически (и чего нет в коде)

**Не проверено физически:**

- Сквозной сценарий **живое Camera2 → WebRTC → desktop NV12 → host/stdin → shared memory → Media Foundation → внешнее приложение** (главный blocker для режима «Веб-камера»).
- Физическая матрица pairing: автообнаружение + manual IP без интернета; корректный/ошибочный PIN; 5 попыток; истечение 5 минут; очистка ключей; новый сеанс; PIN отсутствует в сетевом обмене/логах.
- Camera lifecycle: разрешение/отказ, занятая камера, фоновый режим, блокировка экрана, остановка, повторный запуск, исчезновение live-камеры.
- Реальные cadence, пропуски, цвет/ориентация, качество, end-to-end задержка. **Не заявлять ≤100 мс** — это ориентир, а не измеренный результат. Argon2id время/память измерены только на hosted Linux CI.
- OPAQUE crate 4.0.1 не называть независимо аудированной (упоминание аудита относится к 0.5.0).
- Hosted Windows runner ≠ физический Frame Server: `E_ACCESSDENIED` на `IMFVirtualCamera::Start` в CI — capture smoke помечается skipped, не pass.

**Отсутствует в коде (нужно реализовать):**

- Foreground service / long-lived session layer (session сейчас в Activity `DisposableEffect`).
- Режим «Экран»: MediaProjection, screen capturer, Screen WebRTC, lifecycle после minimize/rotation/screen-off; страница Screen на Windows (сейчас placeholder).
- Управление с ПК: Accessibility Service (жесты, без чтения экрана), data channel protocol команд, Windows viewer жесты; keyboard input запрещён.
- Reconnect без повторного PIN (ICE restart / переподключение в рамках сессии).
- Camera capability detection (модули, контроли), camera presets, restore last settings, mirror toggle, 30 fps cap снять до реальных возможностей.
- Телеметрия телефона (battery/temp/thermal/Wi-Fi/FPS/bitrate/latency/drops) и warnings; thermal degradation.
- MIUI/ColorOS onboarding (battery optimization, autostart, background activity, pinning) с переходами в system settings.
- minSdk 26 → 30.
- Production UX: Settings page, tray, Diagnostics расширение; Android/Windows как «одно приложение».

## 6. Roadmap (крупные milestone, порядок из исходного ТЗ)

0. **(сделано)** stack/design/mockup; CI/build skeletons; discovery + pairing + encrypted connection (код есть, физически не закрыто).
1. **Текущий milestone — long-lived Android session/service foundation + настоящий Screen streaming (MediaProjection) + lifecycle.** Foreground service владеет pairing host + media session; screen capture живёт вне Activity; работает при minimize/app-switch/rotation/screen-off; reconnect без повторного PIN; общий WebRTC слой для обоих режимов (camera + screen), data channel для команд/телеметрии; Windows получает реальный Screen viewer (aspect ratio, fullscreen, AOT, индикаторы). После — физический Screen test на OPPO (30+ мин, app switching, внешняя камера, reconnect).
2. **Screen control:** Accessibility Service (только жесты, без чтения экрана), протокол команд по data channel (tap/long-press/swipe/scroll/precise/pinch/back/home/recents), Windows viewer жесты мышью, «палец отпущен при disconnect».
3. **Wi-Fi Webcam доведение:** долгая работа через сервис (screen-off), live Camera2 E2E в виртуальную камеру, reconnect, No signal/recovery.
4. **Full camera control + telemetry + presets:** capability detection, модули, все контроли, пресеты Stream/Call/Night, restore last, mirror, телеметрия + thermal degradation.
5. **Оба телефона:** POCO X3 NFC (Android 11/MIUI 12.5.7) — установка, pairing, Webcam, модули, screen-off, charging, 1-часовой screen-off charging Webcam тест; OPPO Reno 15 — оба режима, 30+ мин Screen, external camera, gestures, reconnect.
6. **Production virtual camera matrix:** Camera/Chrome/Edge/Firefox/Discord/Telegram/Zoom/Teams/OBS с live phone image; Setup/uninstall; CPU baseline измерения; GPU — только после сравнительных замеров.
7. **Final polish:** onboarding (MIUI/ColorOS), tray, diagnostics, measured latency, production UX обеих платформ.
8. **Security/release review** перед публичным security claim.
9. **USB (только после явного подтверждения пользователя и закрытия Wi-Fi Webcam):** bundled ADB, port forwarding, transport abstraction уже заложена; Screen mode остаётся Wi-Fi only; измерения latency в `docs/HARDWARE_TEST.md`.

Не пропускать Screen «потому что код лучше подготовлен под Webcam» — оба направления обязательны.

## 7. История Git и CI

- Базовая ветка: `main` = `a79a12b` (merge PR #1). Ветка сессии: `arena/f4239082-remote-phone` на той же вершине. До merge: 63 коммита / 114 файлов относительно `1ca4456` (`Initial commit`), кодовая вершина `eeca2042`.
- Опорные коммиты: `e0ba9067` — AsyncWorker lifetime / CPU-first 4K; `dcb212d6` — CPU WebRTC camera transport; `2788a963` — Android emulator validation; `e5eb6cf8` — desktop WebRTC NV12 camera-session tests; `eeca2042` — исправление WebRTC NV12 frame conversion.
- Последний green CI на `main`: [run 37832938506](https://github.com/kotaus-tech/remote-phone/actions/runs/37832938506). Кодовой green: [run 37801041033](https://github.com/kotaus-tech/remote-phone/actions/runs/37801041033).
- Старый handoff упоминал незакоммиченные WIP-файлы (frame-processing эксперименты, `docs/HARDWARE_TEST.md` локальные правки). **Эти изменения не попали в `main`** — после merge чистый checkout их не содержит. Не предполагать их наличие; при необходимости переносить по diff отдельной задачей, не `git add -A`.

## 8. Расследованные проблемы и ошибки, которые нельзя повторять

- **Micro-commit workflow** — работать крупными связными milestone, а не цепочкой коммитов «одна строка → commit».
- **Camera-only scope** — ошибка предыдущего handoff, исправлена; оба режима обязательны.
- **Synthetic camera ≠ live phone camera**; **frame counter ≠ внешнее приложение показывает кадр**.
- **Hosted Windows runner ≠ физический Frame Server** (`E_ACCESSDENIED` → skipped, не pass).
- **`VideoFrame.copyTo(..., format: 'NV12')`** — ненадёжно; использовать RGBA copy + ручная конвертация (зафиксировано в `eeca2042`).
- **Ручной `delete this` в `Napi::AsyncWorker` callbacks** — убран (подозрение в `STATUS_HEAP_CORRUPTION` 0xc0000374 на RTX 5070); не возвращать.
- **HKCU/manual COM registration** — Frame Server не видит; `IMFVirtualCamera::Start → 0x80070003`. Только per-machine Setup, HKLM64.
- **GPU probe как production pipeline** — нет; CPU-first основной, GPU после сравнительных замеров.
- **Требовать ADB/logcat у пользователя** — запрещено.
- **JNI:** не возвращать схему с отдельным `libremote_phone_pairing_bridge.so`; статическая линковка в `libremote_phone_jni.so`.

## 9. Тестовая матрица

| Область | Автоматическое доказательство (green CI) | Физический статус |
|---|---|---|
| Rust OPAQUE-core | `cargo test --all-targets --locked`: RFC 9807 vectors, форматы/порядок/повторы/ошибки/лимиты; run 37801041033 | Argon2id на OPPO/Windows не измерен; негативные сценарии на устройствах не закрыты |
| Android JNI/APK | arm64 + x86_64 Rust targets, static bridge, APK ELF checks, API 35 x86_64 emulator handshake + encrypted SIGNAL; run 37801041033 | OPPO/POCO физически не закрыты; foreground service/lifecycle не проверены |
| Desktop pairing/signaling | Node tests (pairing, signal validation, framing, WebRTC session); Windows `npm test` в run 37801041033 | mDNS/manual IP, PIN-матрица физически не закрыты |
| CPU frame transport | Windows native build + `--cpu-frame-ring-self-test` (Local namespace); run 37801041033 | Global mapping в реальном Frame Server и внешний output не доказаны self-test |
| Setup/virtual camera | CI: x64 Setup/native, install/uninstall, HKLM64/Program Files/ProgramData ACL | Synthetic PoC подтверждён в 3 приложениях; **live camera E2E не подтверждена** |
| Live Camera2 → Windows video | компоненты собраны; desktop tests проверяют логику и conversion | **Не пройдено физически — главный blocker «Веб-камеры»** |
| Screen streaming | — (не реализовано) | Не реализовано; после milestone 1 — физический тест OPPO 30+ мин |
| Screen control | — (не реализовано) | Не реализовано |
| GPU probe | lifecycle stress green; пользовательская 720p проба записана | Нет CPU-vs-GPU benchmark; GPU не production выбор |

Синтетический hardware PoC: [run 37547871094](https://github.com/kotaus-tech/remote-phone/actions/runs/37547871094) + подтверждение пользователя. JNI emulator validation: [run 37717302209](https://github.com/kotaus-tech/remote-phone/actions/runs/37717302209). Последняя автоматическая проверка кода: [run 37801041033](https://github.com/kotaus-tech/remote-phone/actions/runs/37801041033); на `main`: [run 37832938506](https://github.com/kotaus-tech/remote-phone/actions/runs/37832938506). Ни один CI run не заменяет физическую проверку.

## 10. Definition of Done проекта (полный, из исходного ТЗ)

Проект не считается готовым, пока не выполнено: green CI; APK + Windows Setup; Android 11+; OPPO Reno 15 и POCO X3 NFC проверены; **Screen streaming** (30+ мин стабильно, app switching, foreground service, MediaProjection); **Accessibility control** (mouse gestures, no keyboard, no screen reading, no protected-window bypass); **Webcam** (live Camera2, virtual camera, Camera/Chrome/Edge/Firefox/Discord/Telegram/Zoom/Teams/OBS, screen-off Webcam); модули/zoom/focus/exposure/ISO/shutter/WB/stabilization/HDR where supported; пресеты; mirror; телеметрия (battery/temperature/Wi-Fi/FPS/bitrate/latency/drops) + thermal degradation; reconnect; POCO 1-часовой screen-off charging тест; OPPO Screen тест; корректный onboarding; diagnostics; setup/uninstall; без audio/recording/keyboard text input/cloud/accounts/STUN/TURN. USB — только после отдельного явно начатого и завершённого post-Wi-Fi milestone.

**Главный критерий:** не говорить «готово», потому что код компилируется. «Готово» = функция есть в коде + проходит автоматические проверки + там, где это hardware-dependent, **реально подтверждена на целевом устройстве**. Если физическая проверка не выполнена — писать именно «не проверено физически».

## 11. Инструкции следующему агенту

1. Начинай с `docs/HANDOFF.md`, затем `README.md`, `docs/DESIGN_SYSTEM.md`, `docs/HARDWARE_TEST.md`, `docs/PAIRING_PROTOCOL.md`, `docs/VIRTUAL_CAMERA_POC.md`; сверяй тексты с исходниками.
2. Работай **крупными связными milestone** (реализация → тесты → CI → документация → hardware checklist), без micro-commit цепочек и искусственных checkpoint-коммитов.
3. Вся коммуникация с пользователем — **только на русском** (английский — только для имён API/классов/библиотек/SHA).
4. Не начинай с USB и с косметических UI-правок; не зацикливайся на одном маленьком баге.
5. Не делай `git add -A` без разбора; не переноси чужие WIP-изменения без сопоставления diff.
6. Не сужай scope: оба режима + управление обязательны. Если документ противоречит исходному ТЗ — исправляй документ. Если код реализует меньше ТЗ — реализуй функцию.
7. Не переписывай Rust pairing-core/OPAQUE/SIGNAL без объективной причины.
8. Физические проверки: только свежие CI artifacts; Setup — с elevation только у установщика; просить у пользователя только выполнимые шаги и наблюдаемые результаты UI, **никаких ADB/logcat**.
9. Новые измерения добавляй в `docs/HARDWARE_TEST.md`, не затирая историю; отделяй automated/emulator/physical результаты; CI не отмечай как hardware pass.

## 12. Следующее действие для агента

**Milestone 1 реализован в коде (см. «Состояние после Milestone 1» ниже) и отправлен на CI; физически НЕ проверен.** 

Состав реализованного: (a) `minSdk` 26 → 30; (b) `PhoneSessionService` — foreground service (types `camera|mediaProjection`, persistent notification + Stop, rotation через `DisplayManager.DisplayListener`, thermal degradation MODERATE/SEVERE → ECONOMY, EMERGENCY → стоп трансляции), владеющий pairing host и media session вне Activity; (c) `PhoneMediaSession` — общий WebRTC слой для Camera2 и `ScreenCapturerAndroid` (MediaProjection), data channel `remote-phone-control` со строгим протоколом (`session-info`, `telemetry` телефон→ПК; `stop-stream`, `quality` ПК→телефон; без текста), профили качества Auto/Max/Economy (битрейт/формат), track переживает ренеготиацию без рестарта MediaProjection; (d) reconnect без повторного PIN: `PhonePairingSession.resume_connection` в Rust-core (+FFI `rp_phone_resume_connection` + JNI), телефон держит WebSocket-эндпоинт и mDNS, десктоп (`pairing.cjs`) хранит native handle и session id, переподключается до ~2 минут, повторный offer прикрепляется к тому же видеотреку; (e) Android UI: карточка «Экран» включена, запрос MediaProjection-consent, выбор качества, телеметрия-карточка; (f) Windows: настоящий Screen viewer (видео с aspect ratio, fullscreen F/F11/Esc/double-click, always-on-top, качество, остановка, телеметрия), страница Screen больше не placeholder; (g) тесты: Rust resume-тесты, emulator-тест resume через JNI, `sessionControl.test.mjs` (строгая валидация, отказ текстового ввода), desktop-тесты зелёные локально.

**Следующие шаги по порядку:** 1) убедиться в зелёном CI этой ветки и обновить этот раздел run-ссылкой; 2) физический Screen test на OPPO (30+ мин, app switching, внешняя камера, rotation, screen off, reconnect) по чек-листу в `docs/HARDWARE_TEST.md`; 3) Milestone 2 — Screen control (Accessibility Service, только жесты), протокол команд по тому же data channel; 4) Wi-Fi Webcam E2E в виртуальную камеру (live Camera2 → OBS/Discord с живой картинкой телефона); 5) full camera controls + телеметрия на Windows + presets; 6) оба телефона; 7) virtual camera matrix; 8) polish; 9) USB — только после явного подтверждения пользователя.
