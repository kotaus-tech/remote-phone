# Передача проекта следующему агенту

**Срез состояния: 2026-10-08.** Последний коммит с кодом перед этой запиской — `eeca2042dc5a35bf144735d1afa94ccf4b73283c` (`Fix WebRTC NV12 video frame conversion`, 2026-10-08 15:28 UTC). Эта записка добавляется отдельным docs-only коммитом; она не включает изменения кода или правки других документов. Базовая ветка репозитория — `main` (`1ca44561876780c2dfc638593468a7f1d601308e`).

## 1. Конечная цель

Довести локальное приложение Android + Windows до надёжного сценария: пользователь явно запускает временный PIN-сеанс на телефоне, сопрягает его с Windows-ПК в одной локальной сети, а живое изображение Camera2 с телефона появляется как Windows Media Foundation virtual camera и выбирается в webcamtests.com, OBS, Discord и других приложениях. Сеанс должен безопасно останавливаться и запускаться повторно. Для этого проекта CI и unit-тесты необходимы, но конечный результат подтверждается только на целевом оборудовании.

В текущем коде реализован путь живой **камеры**, но его сквозная работа на OPPO → Windows ещё не подтверждена. Пользовательский synthetic-camera PoC проверил только установку и выдачу тестовых движущихся полос/счётчика, не изображение телефона.

Уточнение области: README в первой строке также упоминает трансляцию экрана, а в дизайн-документе она обсуждалась как возможный режим. Однако текущий Android UI явно показывает режим «Экран» как отключённый («приложение не считывает содержимое экрана»), а README запрещает чтение экрана. Не трактовать этот пункт интерфейса или старый макет как разрешение включить MediaProjection. До отдельного решения пользователя фактический объём handoff — только камера; захват экрана, удалённое управление и обход защищённых окон не реализовывать.

## 2. Жёсткие продуктовые и архитектурные требования

- **Только локальная сеть:** discovery через DNS-SD/mDNS, резерв — введённый вручную локальный IP. Нет облака, учётных записей, relay-сервера, STUN/TURN, сохранённых устройств или постоянной записи пары.
- **Одноразовое сопряжение:** временный 8-значный PIN, срок 5 минут, максимум 5 корректно сформированных попыток за сеанс; PIN не сериализуется в сетевые сообщения, не попадает в discovery, журналы и диагностику. Новый PIN/ключи создаются только при явном новом сеансе. Изменение IP/сокета не должно сбрасывать лимит.
- **Криптография:** только общий Rust OPAQUE-core по RFC 9807 (`opaque-ke = 4.0.1`, Ristretto255/SHA-512, профиль Argon2id, описанный в `docs/PAIRING_PROTOCOL.md`). Нельзя заменять его самодельным PAKE или автоматически откатываться на слабый протокол. SDP/ICE-сигнализация после аутентификации защищается SIGNAL AEAD; WebRTC-медиа использует DTLS-SRTP.
- **Медиа и приватность:** только видеотрек Camera2; нет микрофона/аудиотрека, записи на диск, чтения экрана, удалённого ввода или текстовых команд. `RTCDataChannel` сейчас зарезервирован/закрывает входящие сообщения; не считать его реализованным каналом управления.
- **CPU-first:** поддерживать основной путь CPU `WebRTC VideoFrame → RGBA copy → NV12 conversion → frame pipe/shared memory → Media Foundation`. Ограничивать очередь свежим кадром и пропускать устаревшее вместо наращивания задержки. GPU shared-texture — отдельная необязательная диагностика/возможная оптимизация; переключать основной тракт можно только после сравнимых измерений полного CPU- и GPU-пути на одном оборудовании.
- **Windows Setup:** COM media source регистрируется per-machine в 64-разрядном HKLM; DLL ставится в Program Files. Elevation нужна только для Setup/удаления; само приложение не запускается от администратора. Не использовать ручную COM-регистрацию, отдельные native-архивы или PowerShell-сценарии как пользовательский способ установки.
- **UX:** русский интерфейс, тёмная тема, явные разрешения/состояния и понятная ошибка с одним следующим действием. Не показывать счётчик записанных в pipe кадров как доказательство, что Media Foundation или внешнее приложение реально показали кадр.
- **Работа с пользователем:** у пользователя нет ADB и он не может прислать Android-журнал. Не просить `adb`, `logcat` или сырые логи с телефона. Для аппаратной проверки просить только выполнимые шаги и наблюдаемые результаты на экране; ошибки Android диагностировать по встроенному экрану «Диагностика» и автоматическим тестам.

## 3. Фактическая архитектура на коммите `eeca2042`

### Android

- Kotlin, Jetpack Compose, Material 3; главный экран и режимы находятся в `apps/android/app/src/main/java/tech/kotaus/remotephone/MainActivity.kt`. Камера запрашивает runtime permission; режим «Экран» отключён.
- `PhonePairingHost.kt` поднимает временный WebSocket endpoint, создаёт PIN-сеанс, публикует его через Android `NsdManager` (`_remotephone._tcp.`) и делегирует криптографию JNI-мосту. PIN/ключевой материал принадлежат временной сессии.
- `PhoneCameraWebRtc.kt` создаёт Camera2 capturer и только один WebRTC video track; использует пустой список ICE-серверов, ограничивает захват максимумом 30 fps и разрешением не выше 3840×2160. Аудио и screen capturer в этом классе отсутствуют.
- `NativePairing.kt` вызывает `remote_phone_jni.cpp`. Общий Rust C-bridge (`native/pairing-bridge`) линкуется статически в `libremote_phone_jni.so` для `arm64-v8a` и `x86_64`; отдельный Android `libremote_phone_pairing_bridge.so` не ожидается.
- Важный фактический разрыв с `DESIGN_SYSTEM.md`: документ описывает foreground service для долгой сессии, но в текущем `AndroidManifest.xml` нет Service-компонента/foreground-service permission. `MainActivity` владеет `PhonePairingHost` через Compose `DisposableEffect` и закрывает его при dispose. Фоновая работа, блокировка экрана, lifecycle ColorOS и поворот Activity не доказаны; не называть их реализованными.

### Windows / Electron

- Electron 44 + React/TypeScript. Renderer/UI — `apps/desktop/src`; main/preload/IPC — `apps/desktop/electron/main.cjs` и `preload.cjs`. У окна включены `contextIsolation`, `sandbox`, отключён `nodeIntegration`.
- `pairing.cjs` использует `bonjour-service` для DNS-SD/mDNS, проверяет локальный адрес, соединяется с телефоном по WebSocket и вызывает Rust C ABI через `koffi`. UI также предусматривает ручной ввод локального IP.
- `rtcCameraSession.ts` создаёт `RTCPeerConnection({ iceServers: [] })`, предлагает только приём видео и доставляет offer/answer/ICE через зашифрованный pairing-сигнал. Полученный трек показывается в скрытом `<video>` для извлечения кадров.
- На коммите `eeca2042` Chromium `VideoFrame.copyTo` вызывается с поддерживаемым `format: 'RGBA'`; JavaScript затем вручную конвертирует RGBA в плотно упакованный limited-range BT.709 NV12. Не запрашивать `copyTo({format: 'NV12'})`.
- Main process запускает `RemotePhone.VirtualCameraHost.exe --application-session`, передаёт кадры через stdin и на закрытии приложения закрывает pipe, чтобы host удалил сеансовую камеру. Очередь ограничена; при незавершённой записи новый кадр пропускается.

### Native / Rust / Windows camera

- `native/pairing-core` — общий Rust OPAQUE, проверка протокольных кадров/transcript, лимиты попыток и шифрование SIGNAL. `native/pairing-bridge` предоставляет C ABI: Windows загружает DLL, Android статически линкует архив в JNI.
- Windows C++/MSVC Media Foundation source основан на зафиксированном MIT-примере Microsoft Windows-Camera (`626f8b19c5f367602f2e89c6b314573d3776c9df`), его лицензия сохранена в `native/vendor/Windows-Camera/LICENSE`. Setup регистрирует CLSID в HKLM64, ставит native-файлы в Program Files, создаёт ACL для ProgramData-журнала; приложение запускается обычно, не с повышением.
- `native/virtual-camera-host/main.cpp` принимает бинарный NV12-пакет через stdin. `native/frame-transport/SharedNv12FrameBuffer.*` содержит трёхслотовое shared-memory кольцо до 3840×2160, метки времени/sequence, очистку опубликованных кадров и масштабирование до режима virtual camera. Кадр считается устаревшим после 1500 мс. Пока живого кадра нет, source может выдавать синтетический fallback.
- Media Foundation объявляет NV12 720p/1080p/4K при 30/60 fps и RGB32 640×480@30. Это список заявленных режимов source, а не обещание, что Camera2 телефона поддерживает каждый режим или что он уже проверен сквозным путём.
- GPU addon (`apps/desktop/native/gpu-texture-probe`) — диагностическая проба D3D11 shared texture/readback, не production-медиатракт.

## 4. Реализовано и доказано

### Код и автоматические доказательства

- Общий OPAQUE/Rust-core, wire format, PIN/лимиты, аутентификация сторон и защищённый SIGNAL; исходники и оговорки — `docs/PAIRING_PROTOCOL.md`.
- Android JNI статически содержит Rust bridge. GitHub Actions проверяет состав APK и ELF `DT_NEEDED`/`SONAME`; Android API 35 x86_64 emulator test проходит через production Kotlin → JNI → Rust, завершает handshake и проверяет round-trip encrypted SIGNAL.
- Desktop-код offer/answer/ICE и приём WebRTC-видео, RGBA→NV12 преобразование, Windows native host/shared memory и упаковка Setup присутствуют в коммите.
- Удалён ручной `delete this` у N-API `AsyncWorker`; Windows CI выполняет lifecycle stress test success/error и проверяет исходники на повторное ручное освобождение.
- Последний green CI для кодового HEAD `eeca2042` — [run 37801041033](https://github.com/kotaus-tech/remote-phone/actions/runs/37801041033), 2026-10-08. Успешны три job: Rust pairing-core, Android APK (включая emulator test), Windows Setup/native (включая desktop tests и проверку установки/удаления). Этот run подтверждает только перечисленные автоматические проверки/сборки.

### Подтверждённое пользователем на оборудовании

- Synthetic-camera PoC пройден 2026-10-07 на Windows 11 25H2 build 26200.9457, Setup из CI run `37547871094`: пользователь подтвердил движущиеся полосы и растущий счётчик в webcamtests.com, OBS и Discord; в Discord работали предпросмотр и звонок; после закрытия «Видоискателя» камера остановилась/исчезла. Это доказательство установленной виртуальной камеры и lifecycle synthetic PoC, не камеры телефона.
- В актуальной **незакоммиченной** правке `docs/HARDWARE_TEST.md` зафиксирован пользовательский smoke report, что сопряжение теперь проходит. Это только один успешный сценарий: лимит/ошибочный PIN/истечение/повторное подключение и полный набор физических сетевых проверок не закрыты. В коммите `eeca2042` эта более поздняя запись отсутствует; здесь она сохранена именно как отчёт пользователя, а не как полный тестовый результат.
- Та же локальная правка `docs/HARDWARE_TEST.md` фиксирует пользовательский результат отдельной GPU-пробы: 1280×720, 151 кадр, 127 уникальных, 30,1 fps, D3D11 readback в среднем 0,48 мс / максимум 0,8 мс, 0 пропусков. Это диагностическое чтение одной 720p shared texture, не 4K, не CPU-vs-GPU сравнение и не camera E2E. Эта правка документа также не входит в исходный кодовый HEAD/hand-off commit.

## 5. Реализовано, но не проверено физически

- Сквозной сценарий **живое изображение Camera2 → WebRTC → desktop NV12 → host/stdin → shared memory → Media Foundation → внешнее приложение**. CI собирает компоненты и проверяет части протокола/кольца, но пользователь ещё не подтвердил реальное изображение OPPO во внешнем приложении.
- Физическая совместимость пары OPPO Reno 15 / Android 16 / ColorOS 16 ↔ Windows 11 x64: Camera2-разрешения и частота, WebRTC ICE/DTLS-SRTP, маршрут без интернета, mDNS и запасной ввод IP. Одно сообщение об успешном сопряжении не закрывает эту матрицу.
- Негативные сценарии PIN, ровно пять попыток, пятиминутное истечение, очистка ключей/адресов/кадров и новый сеанс; подмена/повтор SDP/ICE на устройстве; отсутствие PIN в наблюдаемом сетевом обмене/логах.
- Camera lifecycle: разрешение/отказ, камера занята, фоновый режим/блокировка экрана, остановка, повторный запуск, закрытие обеих программ и корректное исчезновение live-камеры. Особенно проверить Activity-scoped host и отсутствие Android foreground service.
- Реальные cadence, пропуски, цвет/ориентация, качество и end-to-end задержка. Не заявлять достижение 100 мс: в репозитории это измерительный ориентир, а не доказанный результат. Argon2id время/пиковая память также измерялись только на hosted Linux CI, не на OPPO/Windows.
- OPAQUE crate 4.0.1 не следует называть независимо аудированным: известное в документации crate упоминание аудита относится к старой версии 0.5.0, не к используемой 4.0.1.
- Физический synthetic PoC не доказывает, что live frame проходит через Global mapping в контексте Windows Frame Server: hosted Windows runner может запретить `IMFVirtualCamera::Start` (`E_ACCESSDENIED`), а часть self-test использует Local namespace.

## 6. Крупные приоритетные milestones

1. **Блокер — живой видеокадр на целевых устройствах.** Свежие APK и Setup из последнего зелёного CI; OPPO Reno 15 и Windows 11 в одной Wi‑Fi сети; после PIN-сопряжения выбрать установленную камеру во внешнем Windows-приложении. Завершение: внешнее приложение показывает именно live-картинку телефона (изменение кадра при движении/закрытии объектива), а не синтетические полосы/fallback; соединение и камера останавливаются и повторно запускаются.
2. **Закрыть физическую матрицу pairing/privacy.** Проверить автообнаружение и ручной локальный IP без интернета; корректный/ошибочный PIN; пятиминутное истечение; 5 попыток с блокировкой; явную очистку/новую сессию. Завершение: сценарии дают ожидаемые UI-состояния и лимиты не сбрасываются новым сокетом/IP; PIN не показывается в диагностике/передаваемых данных. Не просить у пользователя ADB/Logcat.
3. **Camera lifecycle/ColorOS и реальное завершение Windows-сессии.** Проверить выдачу разрешения и отказ, камеру, занятую другим приложением, выключение/повторный запуск; проверить поведение при уходе приложения в фон и блокировке телефона. Завершение: зафиксировано, поддерживается ли только активный экран; если продукт обязан работать в фоне — отдельно согласовать и реализовать Service, поскольку его сейчас нет.
4. **Измерить CPU baseline и режимы.** Сначала 720p30, затем 1080p30 и только потом режимы выше; фиксировать фактически выбранные Camera2/MF режим, cadence источника, пропуски, CPU/GPU, время получения первого кадра, задержку и восстановление после обрыва. Завершение: результаты на одном и том же целевом ПК записаны, UI не выдаёт пропущенные/записанные кадры за выведенные. Не обещать 4K60 с телефона: Android sender ограничен 30 fps.
5. **Только после CPU baseline решить вопрос GPU.** Сравнить CPU и GPU end-to-end на одинаковом разрешении, fps, наборе приложений и сценариях восстановления. Без измеренного выигрыша оставить GPU-пробу необязательной, CPU-first путь — основным.
6. **Security/release review.** Сверить актуальный код с RFC 9807 и `PAIRING_PROTOCOL.md`, проверить wipe/лимиты/ошибки, зависимость `opaque-ke 4.0.1` и пакеты; до публичного security claim определить, достаточен ли существующий уровень независимой проверки. RFC-векторы сами по себе не являются аудитом production-сборки.

## 7. Точное место остановки и история Git

- Рабочая ветка: `arena/71439185-remote-phone`; remote: `origin https://github.com/kotaus-tech/remote-phone.git`.
- GitHub default/base branch: `main`, на момент handoff `1ca44561876780c2dfc638593468a7f1d601308e` (`Initial commit`, 2026-10-05). Кодовая вершина feature-ветки: `eeca2042dc5a35bf144735d1afa94ccf4b73283c`; ветка содержит 63 коммита и 114 изменённых файлов относительно `main` до добавления этой записки.
- Опорные коммиты истории: `e0ba9067` — AsyncWorker lifetime / CPU-first 4K; `dcb212d6` — CPU WebRTC camera transport; `2788a963` — успешная Android emulator validation; `e5eb6cf8` — desktop WebRTC NV12 camera-session tests; `eeca2042` — исправление WebRTC NV12 frame conversion. Последний исходный коммит — `eeca2042`; не приписывать этой вершине локальные изменения ниже.
- В первоначальном sandbox checkout локальная ветка ошибочно находилась на grafted/shallow base `1ca4456`. До handoff получена актуальная ветка с `origin`, локальную ветку fast-forward-нули до `eeca2042` без замены файлов; история и текущие remote refs проверены. Базовая `main` не менялась.
- **Важное разделение PR:** сам новый commit этой задачи содержит только `docs/HANDOFF.md`. Но PR из feature-ветки в `main` включает также накопленные 63 проектных коммита/114 файлов; PR не является doc-only diff.
- В рабочей копии уже были незастейдженные изменения относительно `eeca2042`: они не были частью последнего green CI и намеренно не добавляются в handoff commit/PR. После Merge PR их не будет в `main`; новый агент не должен предполагать, что они доступны в чистом checkout. В частности, локальный эксперимент касается host `WAITING/READY/CONSUMED` статусов/ack, RGB32 output, frame writer/backpressure и метрик, а также переносимых C++ frame-processing tests. Незакоммиченная запись `docs/HARDWARE_TEST.md` включает пользовательские smoke reports из разделов 4–5, но сама не войдёт в PR.

  Изменённые относительно `eeca2042` файлы:

  ```text
  .github/workflows/build.yml
  apps/desktop/electron/main.cjs
  apps/desktop/package.json
  apps/desktop/src/App.tsx
  apps/desktop/src/remotePhone.d.ts
  apps/desktop/src/rtcCameraSession.test.mjs
  apps/desktop/src/rtcCameraSession.ts
  docs/HARDWARE_TEST.md
  native/frame-transport/SharedNv12FrameBuffer.cpp
  native/frame-transport/SharedNv12FrameBuffer.h
  native/vendor/Windows-Camera/Samples/VirtualCamera/VirtualCameraMediaSource/SimpleMediaStream.cpp
  native/vendor/Windows-Camera/Samples/VirtualCamera/VirtualCameraMediaSource/SimpleMediaStream.h
  native/vendor/Windows-Camera/Samples/VirtualCamera/VirtualCameraMediaSource/VirtualCameraMediaSource.vcxproj
  native/vendor/Windows-Camera/Samples/VirtualCamera/VirtualCameraMediaSource/VirtualCameraMediaSource.vcxproj.filters
  native/virtual-camera-host/main.cpp
  scripts/build-native.ps1
  ```

  Незатреканные файлы рабочей копии:

  ```text
  apps/desktop/electron/camera-frame-writer.cjs
  apps/desktop/electron/camera-frame-writer.test.cjs
  apps/desktop/electron/camera-status.cjs
  apps/desktop/electron/camera-status.test.cjs
  native/frame-transport/Nv12FramePipeProtocol.h
  native/frame-transport/Nv12FramePipeReader.h
  native/frame-transport/Nv12FrameScaler.h
  native/frame-transport/Nv12Rgb32Converter.h
  native/frame-transport/tests/Nv12FrameProcessingTest.cpp
  scripts/test-frame-processing.sh
  ```

  Рабочая копия `docs/HARDWARE_TEST.md` утверждает, что некоторые переносимые Linux-проверки этой незакоммиченной версии уже выполнялись локально, но свежие Windows build/self-test ещё не запускались. Ветка/CI на `eeca2042` эти новые тесты не содержат: не считать их результат частью merge или автоматически доказанной проверкой. Перед любым будущим переносом сначала посмотреть полный diff и проверить изменения на чистой базе; не делать `git add -A`.

## 8. Расследованные проблемы и ошибки, которые нельзя повторять

- **WebCodecs и NV12:** прямой `VideoFrame.copyTo(..., format: 'NV12')` ненадёжен/не поддерживается выбранным Chromium API. Коммит `eeca2042` использует `RGBA` copy с явной layout/stride-проверкой, затем ручное CPU преобразование в NV12. Не возвращать NV12 как формат `copyTo`.
- **Windows COM camera registration:** старые эксперименты с HKCU не видны службе Frame Server; сообщения об activation могли относиться к другому процессу/старой DLL. Попытки завершались `IMFVirtualCamera::Start` с `0x80070003`. Не повторять HKCU/manual registration, не делать вывод по одному `media_source_activate`; использовать свежий Setup с HKLM64 и проверять путь/hash фактически загруженной DLL.
- **Hosted runner ≠ реальное устройство:** `E_ACCESSDENIED` при `IMFVirtualCamera::Start` означает, что capture smoke нужно пометить skipped, а не green/pass. Green build, ring self-test в Local namespace и успешный Setup не доказывают Global mapping в Frame Server или вывод в OBS/Discord.
- **N-API lifecycle:** пользователь сообщил Event Viewer Application Error 1000, `ntdll.dll`, `0xc0000374` (`STATUS_HEAP_CORRUPTION`) на RTX 5070. В `Napi::AsyncWorker` callbacks нашли ручной `delete this`, удалили его; повторный lifecycle stress прошёл в [run 37688517771](https://github.com/kotaus-tech/remote-phone/actions/runs/37688517771). Это вероятная причина, а не доказанный root cause без стека/дампа. Не возвращать ручное освобождение; при повторном Windows crash сначала определить faulting module/exception/offset в Event Viewer, затем сопоставить приложение/журнал, и только после — журнал Media Foundation. `MF_E_SAMPLEALLOCATOR_EMPTY` сам по себе не объясняет heap corruption.
- **JNI:** прежняя проблема Android native loading исправлена статической линковкой Rust bridge в `libremote_phone_jni.so`. Не возвращать схему с отдельным ожидаемым `libremote_phone_pairing_bridge.so`; проверять APK через `check-android-apk-native.sh` и API 35 x86_64 emulator test.
- **Frame counters:** на исходной вершине `eeca2042` успешная запись stdin/счётчик renderer доказывает доставку в pipe, не получение кадра Media Foundation и тем более не отображение внешним приложением. Текущая незакоммиченная копия добавляет acknowledgement/status, но не попадает в PR. Не писать в статусе «камера показывает live» без проверки внешним приложением.
- **GPU:** пользовательский 720p readback — отдельная техническая проба без CPU-baseline; она не оправдывает переключение на GPU и не доказывает полную задержку. Не блокировать CPU путь из-за отсутствия GPU.
- **Диагностика Android:** пользователь не может предоставить ADB/Logcat; не повторять запрос таких данных. Просить наблюдаемый результат/текст встроенного экрана, не сырые журналы.

## 9. Тестовая матрица

| Область | Автоматическое доказательство на кодовой вершине `eeca2042` | Физический статус / что остаётся |
|---|---|---|
| Rust OPAQUE-core | `cargo test --all-targets --locked`: RFC 9807 vectors, форматы/порядок/повторы/ошибки/лимиты; CI run [37801041033](https://github.com/kotaus-tech/remote-phone/actions/runs/37801041033) зелёный. Argon2id timings — только hosted Linux. | Измерить latency/пиковую память на OPPO и Windows; проверить устройство-в-устройство негативные сценарии и сетевую границу. |
| Android JNI/APK | arm64 и x86_64 Rust targets, static bridge, APK ELF/package checks и API 35 x86_64 emulator handshake/encrypted SIGNAL; latest run `37801041033` зелёный. | Полная установленная проверка на OPPO Reno 15 / Android 16 / ColorOS 16; разрешения, фон/lifecycle и Camera2. Без ADB/Logcat. |
| Desktop pairing/signaling | Node tests для pairing, signal validation, framing и WebRTC session; Windows `npm test` в latest run успешен. | Физическая mDNS/manual-IP работа на одной сети без интернета и вся PIN-матрица. |
| CPU frame transport | Windows native build и `--cpu-frame-ring-self-test`: shared-memory ring/масштабирование/метаданные/очистка; latest Windows job успешен. | Global mapping в настоящем Frame Server, реальные Camera2 кадры и подтверждение внешнего output не доказаны этим self-test. |
| Setup/virtual camera | CI собирает x64 Setup/native и проверяет установку/удаление, HKLM64/Program Files/ProgramData ACL. | Synthetic PoC подтверждён в трёх приложениях; это не live-camera E2E. Проверять только свежим Setup, без ручной регистрации. |
| Live Camera2 → Windows video | Android/Windows части собираются; тесты desktop session проверяют кодовую логику и conversion. | **Не пройдено физически. Главный release blocker:** подтвердить живое изображение телефона в webcamtests/OBS/Discord, повторы и cleanup. |
| GPU probe | Windows CI lifecycle stress на AsyncWorker зелёный; user report 720p readback приведён в разделе 4. | Нет полного GPU camera path и CPU-vs-GPU benchmark; GPU не production выбор. |
| WIP frame-processing job | На `eeca2042` этой отдельной job нет. Рабочая копия содержит незатреканные portable C++ tests и изменение workflow. | Не входит в текущий PR и не подтверждена его CI; при отдельной будущей задаче переносить/проверять независимо. |

Синтетический hardware PoC: [run 37547871094](https://github.com/kotaus-tech/remote-phone/actions/runs/37547871094) + подтверждение пользователя. JNI emulator validation: [run 37717302209](https://github.com/kotaus-tech/remote-phone/actions/runs/37717302209). Последняя общая автоматическая проверка source HEAD: [run 37801041033](https://github.com/kotaus-tech/remote-phone/actions/runs/37801041033). Ни один из этих CI runs не заменяет физическую проверку живой камеры.

## 10. Definition of Done проекта

- Свежий зелёный CI собирает Android APK, Windows x64 Setup/native и Rust core; проходит весь committed unit/instrumentation/self-test suite и install/uninstall. Ошибки в сборке не скрыты как skip; условно пропущенный capture smoke явно отмечен.
- На целевых OPPO Reno 15 и Windows 11 пользователь проходит pairing по mDNS и резервному локальному IP в одной сети без интернета; positive, negative PIN, пять попыток, истечение и повторный сеанс имеют ожидаемое поведение. PIN не появляется в передаваемых/показываемых диагностических данных.
- Пользователь видит именно живой кадр телефона (не синтетический fallback) в выбранной Windows virtual camera минимум в webcamtests.com, OBS и Discord. Проверены фактически выбранные режимы, цвет/ориентация, cadence/drop и восстановление после остановки; режимы, которые физически не прошли тест, не рекламируются как подтверждённые.
- Остановка приложения/сеанса закрывает Camera2/WebRTC, очищает временное состояние и live buffers, удаляет сеансовую виртуальную камеру; повторный запуск создаёт новую сессию и снова работает. Приложение не требует admin, только per-machine installer.
- Нет аудио, записи, экранного захвата, удалённого ввода, аккаунтов/cloud/STUN/TURN или сохранённой пары. Любое расширение этой политики требует отдельного явного согласования.
- CPU/GPU, cadence и end-to-end задержка измерены на целевых системах; нельзя заявлять ≤100 мс без воспроизводимого end-to-end измерения. Все пользовательские аппаратные результаты в будущем вносятся в `docs/HARDWARE_TEST.md` только после фактического подтверждения; CI не отмечается как hardware pass.
- Предупреждение по OPAQUE crate 4.0.1 и независимому аудиту разрешено до публичного security claim/release.

Этот Definition of Done не утверждает, что проект уже готов: live camera E2E и несколько физических пунктов остаются открытыми. Реализация трансляции экрана также не включена в текущую разрешённую область.

## 11. Инструкции следующему агенту

1. После Merge начни с `docs/HANDOFF.md`, затем прочитай `README.md`, `docs/HARDWARE_TEST.md`, `docs/PAIRING_PROTOCOL.md`, `docs/DESIGN_SYSTEM.md` и `docs/VIRTUAL_CAMERA_POC.md`; сверяй тексты с исходниками, а не копируй старые статусы.
2. В чистом checkout проверь `git status --short`, base/HEAD и последний GitHub Actions результат. Ожидаемая committed точка исходного кода — `eeca2042` плюс docs-only HANDOFF commit; локальные WIP-файлы из раздела 7 **не попадут** в main через этот PR.
3. Не делай `git add -A` и не переносить автоматически всю текущую рабочую копию. Если когда-нибудь понадобится продолжить эксперимент из списка WIP, сначала получить/сопоставить его diff с актуальной main и отдельной задачей решить, какие части безопасно переносить.
4. Не перепроектируй pairing/transport, не начинай GPU production path и не включай экран/управление до объективного результата и явного продуктового решения. Сначала закрой один сквозной hardware blocker из раздела 12.
5. Для физических проверок указывай свежие CI artifacts; Setup устанавливать с ожидаемым elevation, приложение запускать обычным пользователем. Проси пользователя присылать только ответы на конкретные шаги/состояния UI — никаких ADB, Logcat или Android-журналов.
6. Сохраняй русскую документацию и предыдущие результаты; новые измерения добавляй в `docs/HARDWARE_TEST.md`, не затирая старую историю. Отделяй сообщения пользователя, automated/unit/emulator результаты и physical E2E.

## 12. Следующее действие для агента

**Провести один контролируемый сквозной тест live-камеры на целевых устройствах, прежде чем менять код.** Взять APK и Windows Setup из самого свежего успешного GitHub Actions run для merged `main` (на момент handoff последняя зелёная проверка исходного кода — `37801041033` на `eeca2042`; artifact retention ограничен, поэтому при истечении срока дождаться свежего зелёного run). Установить Setup обычным продуктовым способом с elevation только у установщика, установить APK на OPPO Reno 15, запустить оба приложения обычным пользователем. Подключить оба устройства к одной Wi‑Fi сети без интернета, на телефоне явно выбрать «Веб-камера»/разрешить Camera permission, затем выполнить pairing по найденному устройству (ручной локальный IP — только если mDNS не сработал). В OBS выбрать виртуальную камеру и проверить, что движение телефона/закрытие объектива меняет именно живое изображение; убедиться, что это не движущиеся полосы и не synthetic fallback. Затем повторить в webcamtests.com и Discord. Закрыть сеанс и приложения, убедиться, что камера остановилась, и повторно запустить pairing/live-сеанс.

**Критерий:** реальное изображение OPPO подтверждено в трёх приложениях, локальное подключение и отсутствие synthetic fallback подтверждены, после остановки камера очищается, повторный сеанс работает. Результат — краткие pass/fail ответы пользователя по каждому шагу и видимый текст UI; ADB/Logcat и сырые Android-логи не запрашивать. Только после этого выбирать следующий engineering task по конкретному провалу.
