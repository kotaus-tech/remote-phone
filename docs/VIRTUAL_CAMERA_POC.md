# Ранний PoC виртуальной камеры — этап 2

Этап 2 включает только синтетический тестовый источник из официального примера Microsoft. Он нужен для ранней проверки нативной DLL, COM-регистрации в `Setup.exe`, установки/удаления и видимости камеры в приложениях Windows. Он не принимает видео с телефона, не проверяет задержку и не является реализацией режима «Веб-камера».

- Upstream: [`Windows-Camera/Samples/VirtualCamera`](https://github.com/microsoft/Windows-Camera/tree/master/Samples/VirtualCamera), исходники зафиксированы в `native/vendor/Windows-Camera` на commit `626f8b19c5f367602f2e89c6b314573d3776c9df`.
- Копия лицензии Microsoft MIT находится в `native/vendor/Windows-Camera/LICENSE`.
- Тестовый запускающий код использует `MFCreateVirtualCamera` из Windows SDK, `MFVirtualCameraLifetime_Session` и `MFVirtualCameraAccess_CurrentUser`; CLSID media source и атрибут `VCAM_KIND` взяты из зафиксированного официального примера.
- `Setup.exe` регистрирует COM media source для Windows; удаление останавливает тестовый процесс и удаляет COM-регистрацию. CI run [#37384956873](https://github.com/kotaus-tech/remote-phone/actions/runs/37384956873) (commit `8cc4494`) успешно собрал x64 DLL/host и `Setup.exe`, а также проверил на Windows Server 2025 runner установку файлов, создание COM-ключа, удаление и очистку регистрации. Реальное перечисление камеры и выдачу тестовых кадров на Windows 11 подтверждает только ручной чек-лист в `docs/HARDWARE_TEST.md`; этап 2 пока не принят пользователем.

## Ручной запуск

После сборки Windows workflow и установки `Setup.exe`, из корня репозитория запустить PowerShell-скрипт:

```powershell
.\scripts\run-virtual-camera-poc.ps1
```

Оставить окно PowerShell открытым, выбрать «Видоискатель — тестовый источник» в приложении «Камера» Windows или OBS. После проверки нажать Enter в PowerShell: источник остановится и будет удалён из списка устройств.

**Ожидаемый результат этапа 2:** движущийся тестовый кадр виден в Windows Camera и хотя бы одном из Chrome/Edge/OBS; CI и `Setup.exe` собирают/регистрируют/удаляют нативную DLL. Живой поток OPPO и критерии задержки из `docs/HARDWARE_TEST.md` — отдельный обязательный PoC сразу после этапа 3 и до основной разработки режима «Веб-камера».
