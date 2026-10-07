# Видоискатель

Личный проект для трансляции экрана и использования камеры телефона как веб-камеры Windows.

## Статус

- **Этап 1 согласован:** Electron + React/TypeScript для Windows; Kotlin + Jetpack Compose для Android; утверждены тёмная дизайн-система и список экранов.
- **Сопряжение:** реализованы локальный OPAQUE/Rust-core, Android host/JNI, desktop-клиент, mDNS/DNS-SD и резервный ввод локального IP. Протокол и ограничения перебора описаны в [PAIRING_PROTOCOL.md](docs/PAIRING_PROTOCOL.md); аппаратный ретест Android/Windows-багфиксов пользователь попросил совместить со следующим крупным этапом.
- **Ранний PoC виртуальной камеры пройден 2026-10-07:** пользователь подтвердил движущиеся полосы и растущий счётчик в webcamtests.com, OBS и Discord; в Discord работает предпросмотр и звонок. Камера остановилась/исчезла после закрытия «Видоискателя». Проверка проведена на Windows 11 25H2 build 26200.9457 с Setup из [зелёного CI run 37547871094](https://github.com/kotaus-tech/remote-phone/actions/runs/37547871094).
- **Следующий этап — интеграция живой камеры телефона:** цель — подать поток Camera2 с Android через OPAQUE-защищённую локальную WebRTC-сигнализацию и DTLS-SRTP в Windows Media Foundation virtual camera. Отдельный `RTCDataChannel` остаётся для будущих команд; видео работает без STUN/TURN, записи и аудио.
- **Обязательный технический стоп-гейт:** Electron GPU shared-texture → D3D11 мост должен быть проверен на целевом ПК; CPU fallback нельзя выдавать за прохождение GPU-гейта. Результаты и измерения ведутся в [HARDWARE_TEST.md](docs/HARDWARE_TEST.md).
- **Текущая граница реализации:** синтетический PoC принят; новый обязательный GPU-gate добавлен в приложение как диагностический тест Electron shared texture → hardware D3D11, со staging-readback метриками, логом ProgramData и без CPU-bitmap fallback. Зелёный [CI run 37552336303](https://github.com/kotaus-tech/remote-phone/actions/runs/37552336303) собрал APK, Windows Setup и native addon, проверил его упаковку и установку/удаление Setup. GPU-gate ещё не пройден на целевом ПК, а передача живого изображения телефона и WebRTC всё ещё не реализованы. Политика продукта остаётся без записи, аудио, ввода текста, USB/ADB, облака/аккаунтов, сохранённых устройств, чтения экрана и обхода защищённых окон.

Документы:

- [Решение по стеку, дизайн-система и этапы PoC](docs/DESIGN_SYSTEM.md)
- [Протокол локального сопряжения и защищённая сигнализация](docs/PAIRING_PROTOCOL.md)
- [Интерактивный макет Windows и Android](docs/prototype/index.html)
- [Чек-лист целевого оборудования и критерии PoC](docs/HARDWARE_TEST.md)
- [Ранний PoC виртуальной камеры](docs/VIRTUAL_CAMERA_POC.md)

HTML-макет — демонстрация интерфейса; он не подключается к камере, сети, разрешениям или управлению телефоном.
