$ErrorActionPreference = 'Stop'

$cameraClsid = '{7B89B92E-FE71-42D0-8A41-E137D06EA184}'
$registrationPath = "Software\Classes\CLSID\$cameraClsid"

if (-not [Environment]::Is64BitProcess) {
    throw 'Запустите 64-разрядный PowerShell: тестовый источник собран для x64.'
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Не запускайте PowerShell от имени администратора. Для проверки HKCU нужен обычный, неповышенный сеанс пользователя.'
}

# В установленном приложении host и DLL лежат рядом со скриптом. При запуске из репозитория
# используем созданную scripts/build-native.ps1 папку apps/desktop/native-runtime.
$hostPath = Join-Path $PSScriptRoot 'RemotePhone.VirtualCameraHost.exe'
$dllPath = Join-Path $PSScriptRoot 'VirtualCameraMediaSource.dll'
if (-not (Test-Path -LiteralPath $hostPath) -or -not (Test-Path -LiteralPath $dllPath)) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $runtimeDirectory = Join-Path $repoRoot 'apps\desktop\native-runtime'
    $hostPath = Join-Path $runtimeDirectory 'RemotePhone.VirtualCameraHost.exe'
    $dllPath = Join-Path $runtimeDirectory 'VirtualCameraMediaSource.dll'
}
if (-not (Test-Path -LiteralPath $hostPath)) {
    throw 'Тестовая программа не найдена. Установите Setup.exe с PoC-файлами или сначала соберите нативные компоненты.'
}
if (-not (Test-Path -LiteralPath $dllPath)) {
    throw 'DLL источника видеокадров не найдена рядом с тестовой программой.'
}

$diagnosticEnvironmentVariable = 'REMOTE_PHONE_VCAM_DIAGNOSTIC_LOG_PATH'
$previousDiagnosticLogPath = [Environment]::GetEnvironmentVariable($diagnosticEnvironmentVariable, 'Process')
$diagnosticLogPath = Join-Path ([IO.Path]::GetTempPath()) ("RemotePhoneVirtualCamera-{0}.log" -f [guid]::NewGuid().ToString('N'))
try {
    $diagnosticHeader = "poc_started={0:O}`r`n" -f [DateTime]::Now
    [IO.File]::WriteAllText($diagnosticLogPath, $diagnosticHeader, [Text.UTF8Encoding]::new($false))
    $diagnosticAcl = Get-Acl -LiteralPath $diagnosticLogPath
    foreach ($serviceSid in @('S-1-5-11', 'S-1-5-19', 'S-1-5-20')) {
        $sid = [Security.Principal.SecurityIdentifier]::new($serviceSid)
        $rule = [Security.AccessControl.FileSystemAccessRule]::new(
            $sid,
            [Security.AccessControl.FileSystemRights]::AppendData,
            [Security.AccessControl.AccessControlType]::Allow)
        [void]$diagnosticAcl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $diagnosticLogPath -AclObject $diagnosticAcl
    [Environment]::SetEnvironmentVariable($diagnosticEnvironmentVariable, $diagnosticLogPath, 'Process')
}
catch {
    Remove-Item -LiteralPath $diagnosticLogPath -Force -ErrorAction SilentlyContinue
    throw
}

$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::CurrentUser,
    [Microsoft.Win32.RegistryView]::Registry64)
$classKey = $null
$serverKey = $null
$createdRegistration = $false

try {
    $existingKey = $registry.OpenSubKey($registrationPath)
    if ($null -ne $existingKey) {
        $existingKey.Dispose()
        throw 'HKCU уже содержит регистрацию этого CLSID. Скрипт не изменит существующие пользовательские данные.'
    }

    $classKey = $registry.CreateSubKey($registrationPath)
    if ($null -eq $classKey) { throw 'Не удалось создать временную COM-запись в HKCU.' }
    $createdRegistration = $true
    $classKey.SetValue('', 'Видоискатель — источник камеры', [Microsoft.Win32.RegistryValueKind]::String)
    $classKey.Dispose()
    $classKey = $null

    $serverKey = $registry.CreateSubKey("$registrationPath\InprocServer32")
    if ($null -eq $serverKey) { throw 'Не удалось создать временную InprocServer32-запись в HKCU.' }
    $serverKey.SetValue('', [System.IO.Path]::GetFullPath($dllPath), [Microsoft.Win32.RegistryValueKind]::String)
    $serverKey.SetValue('ThreadingModel', 'Both', [Microsoft.Win32.RegistryValueKind]::String)
    $serverKey.Flush()
    $serverKey.Dispose()
    $serverKey = $null

    Write-Host 'Временная COM-запись создана только для текущего пользователя (HKCU); системный реестр не изменяется.'
    Write-Host 'В тестовом кадре шестизначный счётчик увеличивается на каждый сформированный кадр.'
    Write-Host 'Проверьте браузер и Discord; после теста скрипт покажет выбранный формат и фактическую частоту RequestSample.'
    Write-Host 'Нажмите Enter здесь, чтобы остановить и удалить тестовую камеру.'
    & $hostPath
    if ($LASTEXITCODE -ne 0) {
        throw "Тестовая программа завершилась с кодом $LASTEXITCODE. Проверьте код ошибки выше."
    }
}
finally {
    if ($null -ne $serverKey) { $serverKey.Dispose() }
    if ($null -ne $classKey) { $classKey.Dispose() }
    if ($createdRegistration) {
        $registry.DeleteSubKeyTree($registrationPath, $false)
        Write-Host 'Временная COM-запись текущего пользователя удалена из HKCU.'
    }
    $registry.Dispose()

    [Environment]::SetEnvironmentVariable($diagnosticEnvironmentVariable, $previousDiagnosticLogPath, 'Process')
    if (Test-Path -LiteralPath $diagnosticLogPath) {
        try {
            Write-Host 'Журнал выбранного media type и RequestSample (после вывода временный файл будет удалён):'
            $diagnosticText = Get-Content -LiteralPath $diagnosticLogPath -Raw -Encoding UTF8
            if ([string]::IsNullOrWhiteSpace($diagnosticText)) {
                Write-Host 'Записей нет: источник мог не запуститься или процесс Frame Server не смог записать журнал.'
            }
            else {
                Write-Host $diagnosticText
            }
        }
        catch {
            Write-Warning "Не удалось прочитать журнал диагностики: $($_.Exception.Message)"
        }
        finally {
            Remove-Item -LiteralPath $diagnosticLogPath -Force -ErrorAction SilentlyContinue
        }
    }
}
