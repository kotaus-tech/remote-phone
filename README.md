# Видоискатель

Личный проект для трансляции экрана и использования камеры телефона как веб-камеры Windows.

## Статус

- **Этап 1 согласован:** Electron + React/TypeScript для Windows; Kotlin + Jetpack Compose для Android; утверждены тёмная дизайн-система и список экранов.
- **Сопряжение:** реализованы локальный OPAQUE/Rust-core, Android host/JNI, desktop-клиент, mDNS/DNS-SD и резервный ввод локального IP. Протокол и ограничения перебора описаны в [PAIRING_PROTOCOL.md](docs/PAIRING_PROTOCOL.md); аппаратный ретест Android/Windows-багфиксов пользователь попросил совместить со следующим крупным этапом.
- **Исправление ошибки загрузки JNI проверено CI:** Android статически включает Rust pairing-core в `libremote_phone_jni.so` для `arm64-v8a` и `x86_64`; отдельный Rust `.so` не нужен. Зелёный [CI run 37716664513](https://github.com/kotaus-tech/remote-phone/actions/runs/37716664513) собрал APK, проверил `DT_NEEDED`/`SONAME` нативных библиотек через `llvm-readelf` и успешно запустил на x86_64-эмуляторе JNI-загрузку, полный OPAQUE-handshake и round-trip зашифрованного SIGNAL. Экран «Диагностика» показывает текст исключения и кнопку «Скопировать». Аппаратная проверка OPPO Reno 15 ↔ Windows 11 остаётся отдельным следующим этапом.
- **Ранний PoC виртуальной камеры пройден 2026-10-07:** пользователь подтвердил движущиеся полосы и растущий счётчик в webcamtests.com, OBS и Discord; в Discord работает предпросмотр и звонок. Камера остановилась/исчезла после закрытия «Видоискателя». Проверка проведена на Windows 11 25H2 build 26200.9457 с Setup из [зелёного CI run 37547871094](https://github.com/kotaus-tech/remote-phone/actions/runs/37547871094).
- **Текущий этап — живой видеотракт Camera2/WebRTC:** код Android Camera2 sender, OPAQUE-защищённой v1-сигнализации offer/answer/ICE и desktop receiver собран зелёным CI run [37703702143](https://github.com/kotaus-tech/remote-phone/actions/runs/37703702143). Полевая проверка сопряжения и фактической выдачи телефонного видео ещё не выполнена. Видео планируется только по локальному WebRTC DTLS-SRTP без STUN/TURN, записи и аудио; отдельный `RTCDataChannel` не принимает ввод или текстовые команды.
- **Основной видеотракт:** выбран CPU-first путь `VideoFrame.copyTo → NV12 → shared memory → Media Foundation`; desktop-код уже передаёт ограниченные кадры в Windows host, а native ring поддерживает масштабирование и резервный синтетический кадр. Run [37703702143](https://github.com/kotaus-tech/remote-phone/actions/runs/37703702143) зелёный для APK, Windows Setup, native-компонентов и desktop-тестов, но не подтверждает сквозную работу на устройствах. Поддерживается цель до 4K 3840×2160 с меньшими режимами; GPU shared-texture → D3D11 остаётся необязательным и может быть выбран только после сравнительных измерений.
- **Текущая граница реализации и crash triage:** синтетический PoC принят; код живого Camera2/WebRTC CPU-тракта проходит CI, но его работа на целевых устройствах ещё не подтверждена. При вылете на RTX 5070 Event Viewer показал Application Error 1000, `ntdll.dll`, `0xc0000374` (`STATUS_HEAP_CORRUPTION`); подозрение — ручное `delete this` у `Napi::AsyncWorker` в callbacks. Исправление удаляет ручное освобождение и добавляет повторный CI stress-тест success/error. Следующие сбои сначала проверяются по Event Viewer/минидампу (модуль, exception code), затем по журналам приложения и только после — виртуальной камеры. Результаты и аппаратные замеры ведутся в [HARDWARE_TEST.md](docs/HARDWARE_TEST.md). Политика продукта остаётся без записи, аудио, ввода текста, USB/ADB, облака/аккаунтов, сохранённых устройств, чтения экрана и обхода защищённых окон.

Документы:

- [Решение по стеку, дизайн-система и этапы PoC](docs/DESIGN_SYSTEM.md)
- [Протокол локального сопряжения и защищённая сигнализация](docs/PAIRING_PROTOCOL.md)
- [Интерактивный макет Windows и Android](docs/prototype/index.html)
- [Чек-лист целевого оборудования и критерии PoC](docs/HARDWARE_TEST.md)
- [Ранний PoC виртуальной камеры](docs/VIRTUAL_CAMERA_POC.md)

HTML-макет — демонстрация интерфейса; он не подключается к камере, сети, разрешениям или управлению телефоном.
