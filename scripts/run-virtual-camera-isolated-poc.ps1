$ErrorActionPreference = 'Stop'

$cameraClsid = '{5F94713D-D05B-41E3-AED6-378B3BDE847A}'
$registrationPath = "Software\Classes\CLSID\$cameraClsid"
$serverPath = "$registrationPath\InprocServer32"
$classesRootPath = "CLSID\$cameraClsid"

if (-not [Environment]::Is64BitProcess) {
    throw 'Запустите 64-разрядный PowerShell: изолированный тестовый источник собран для x64.'
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Не запускайте PowerShell от имени администратора. Изолированный тест использует только регистрацию HKCU.'
}

# В установленном приложении host и DLL лежат рядом со скриптом. При запуске из репозитория
# используем папку apps/desktop/native-runtime, созданную scripts/build-native.ps1.
$hostPath = Join-Path $PSScriptRoot 'RemotePhone.VirtualCameraHost.exe'
$dllPath = Join-Path $PSScriptRoot 'VirtualCameraMediaSource.dll'
if (-not (Test-Path -LiteralPath $hostPath) -or -not (Test-Path -LiteralPath $dllPath)) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $runtimeDirectory = Join-Path $repoRoot 'apps\desktop\native-runtime'
    $hostPath = Join-Path $runtimeDirectory 'RemotePhone.VirtualCameraHost.exe'
    $dllPath = Join-Path $runtimeDirectory 'VirtualCameraMediaSource.dll'
}
if (-not (Test-Path -LiteralPath $hostPath)) {
    throw 'Тестовая программа не найдена. Скачайте и распакуйте свежий архив нативных компонентов.'
}
if (-not (Test-Path -LiteralPath $dllPath)) {
    throw 'DLL источника видеокадров не найдена рядом с тестовой программой.'
}

# Не использовать этот тестовый CLSID, если он уже зарегистрирован где-либо.
$registrationViews = @(
    [pscustomobject]@{ Name = '64-bit'; Value = [Microsoft.Win32.RegistryView]::Registry64 },
    [pscustomobject]@{ Name = '32-bit'; Value = [Microsoft.Win32.RegistryView]::Registry32 }
)
$registrationHives = @(
    [pscustomobject]@{ Name = 'HKCU'; Value = [Microsoft.Win32.RegistryHive]::CurrentUser; Path = $registrationPath },
    [pscustomobject]@{ Name = 'HKLM'; Value = [Microsoft.Win32.RegistryHive]::LocalMachine; Path = $registrationPath },
    [pscustomobject]@{ Name = 'HKCR'; Value = [Microsoft.Win32.RegistryHive]::ClassesRoot; Path = $classesRootPath }
)
foreach ($view in $registrationViews) {
    foreach ($hive in $registrationHives) {
        $baseKey = $null
        $existingKey = $null
        try {
            $baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive.Value, $view.Value)
            $existingKey = $baseKey.OpenSubKey($hive.Path, $false)
            if ($null -ne $existingKey) {
                throw "Изолированный CLSID уже зарегистрирован: $($hive.Name) ($($view.Name)). Ничего не изменено."
            }
        }
        finally {
            if ($null -ne $existingKey) { $existingKey.Dispose() }
            if ($null -ne $baseKey) { $baseKey.Dispose() }
        }
    }
}

$diagnosticEnvironmentVariable = 'REMOTE_PHONE_VCAM_DIAGNOSTIC_LOG_PATH'
$previousDiagnosticLogPath = [Environment]::GetEnvironmentVariable($diagnosticEnvironmentVariable, 'Process')
$diagnosticLogPath = Join-Path ([IO.Path]::GetTempPath()) ("RemotePhoneIsolatedVirtualCamera-{0}.log" -f [guid]::NewGuid().ToString('N'))
try {
    $diagnosticHeader = "poc_mode=isolated_hkcu_clsid`r`npoc_source_clsid=$cameraClsid`r`npoc_started={0:O}`r`n" -f [DateTime]::Now
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

$registry = $null
$classKey = $null
$serverKey = $null
$createdRegistration = $false

try {
    $registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::CurrentUser,
        [Microsoft.Win32.RegistryView]::Registry64)
    $existingKey = $registry.OpenSubKey($registrationPath)
    if ($null -ne $existingKey) {
        $existingKey.Dispose()
        throw 'HKCU уже содержит изолированный тестовый CLSID. Скрипт не изменит существующие данные.'
    }

    $classKey = $registry.CreateSubKey($registrationPath)
    if ($null -eq $classKey) { throw 'Не удалось создать временную регистрацию в HKCU.' }
    $createdRegistration = $true
    $classKey.SetValue('', 'Видоискатель — COM-изоляционный тест', [Microsoft.Win32.RegistryValueKind]::String)
    $classKey.Dispose()
    $classKey = $null

    $serverKey = $registry.CreateSubKey($serverPath)
    if ($null -eq $serverKey) { throw 'Не удалось создать временную InprocServer32-запись в HKCU.' }
    $serverKey.SetValue('', [System.IO.Path]::GetFullPath($dllPath), [Microsoft.Win32.RegistryValueKind]::String)
    $serverKey.SetValue('ThreadingModel', 'Both', [Microsoft.Win32.RegistryValueKind]::String)
    $serverKey.Flush()
    $serverKey.Dispose()
    $serverKey = $null

    Write-Host "Создана только временная HKCU-регистрация с уникальным CLSID $cameraClsid; HKLM не изменяется."
    Write-Host 'Ищите устройство «Видоискатель — COM-изоляционный тест»; Windows может дополнить отображаемое имя типом устройства.'
    Write-Host 'Проверьте браузер, OBS 32.2.2 и Discord по одному; отметьте счётчик и журнал RequestSample.'
    Write-Host 'Нажмите Enter здесь, чтобы остановить и удалить временную камеру.'
    & $hostPath '--isolated-clsid'
    if ($LASTEXITCODE -ne 0) {
        throw "Тестовая программа завершилась с кодом $LASTEXITCODE. Проверьте код ошибки выше."
    }
}
finally {
    if ($null -ne $serverKey) { $serverKey.Dispose() }
    if ($null -ne $classKey) { $classKey.Dispose() }
    if ($createdRegistration) {
        $registry.DeleteSubKeyTree($registrationPath, $false)
        Write-Host 'Временная COM-регистрация уникального CLSID удалена из HKCU.'
    }
    if ($null -ne $registry) { $registry.Dispose() }

    [Environment]::SetEnvironmentVariable($diagnosticEnvironmentVariable, $previousDiagnosticLogPath, 'Process')
    if (Test-Path -LiteralPath $diagnosticLogPath) {
        try {
            Write-Host 'Журнал изолированного COM-теста (после вывода временный файл будет удалён):'
            $diagnosticText = Get-Content -LiteralPath $diagnosticLogPath -Raw -Encoding UTF8
            if ([string]::IsNullOrWhiteSpace($diagnosticText)) {
                Write-Host 'Записей нет: Frame Server не активировал диагностический источник или не смог записать журнал.'
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
