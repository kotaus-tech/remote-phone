$ErrorActionPreference = 'Stop'

$cameraClsid = '{7B89B92E-FE71-42D0-8A41-E137D06EA184}'
$registrationPath = "Software\Classes\CLSID\$cameraClsid\InprocServer32"

if (-not [Environment]::Is64BitProcess) {
    throw 'Запустите 64-разрядный PowerShell: аудит сравнивает 32- и 64-разрядные представления реестра для x64-камеры.'
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Запустите аудит в обычном, неповышенном PowerShell. Запрашивать или использовать права администратора не нужно.'
}

$classesRootRegistrationPath = "CLSID\$cameraClsid\InprocServer32"
$hives = @(
    [pscustomobject]@{ Name = 'HKCU'; Value = [Microsoft.Win32.RegistryHive]::CurrentUser; Path = $registrationPath },
    [pscustomobject]@{ Name = 'HKLM'; Value = [Microsoft.Win32.RegistryHive]::LocalMachine; Path = $registrationPath },
    # HKCR is already the merged Classes root; it must not include the Software\Classes prefix.
    [pscustomobject]@{ Name = 'HKCR (объединённый вид)'; Value = [Microsoft.Win32.RegistryHive]::ClassesRoot; Path = $classesRootRegistrationPath }
)
$views = @(
    [pscustomobject]@{ Name = '64-bit'; Value = [Microsoft.Win32.RegistryView]::Registry64 },
    [pscustomobject]@{ Name = '32-bit'; Value = [Microsoft.Win32.RegistryView]::Registry32 }
)

Write-Output 'Аудит COM-регистрации VirtualCameraMediaSource — только чтение; реестр не изменяется.'
Write-Output "CLSID: $cameraClsid"
Write-Output "Компьютер: $env:COMPUTERNAME; процесс PowerShell: 64-bit; повышенные права: нет"
Write-Output 'Для каждого найденного InprocServer32 выводятся путь, версия файла и SHA-256.'

foreach ($view in $views) {
    foreach ($hive in $hives) {
        $location = "$($hive.Name)\$($view.Name)\$($hive.Path)"
        $baseKey = $null
        $serverKey = $null

        Write-Output ""
        Write-Output "[$location]"
        try {
            $baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive.Value, $view.Value)
            $serverKey = $baseKey.OpenSubKey($hive.Path, $false)
            if ($null -eq $serverKey) {
                Write-Output 'Запись отсутствует.'
                continue
            }

            $valueNames = $serverKey.GetValueNames()
            if ($valueNames -notcontains '') {
                Write-Output 'Значение пути (по умолчанию) отсутствует.'
                continue
            }

            $rawPath = [string]$serverKey.GetValue(
                '',
                $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            $valueKind = $serverKey.GetValueKind('').ToString()
            $threadingModel = [string]$serverKey.GetValue(
                'ThreadingModel',
                '(не задан)',
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)

            Write-Output "Тип значения пути: $valueKind"
            Write-Output "ThreadingModel: $threadingModel"
            Write-Output "Путь в реестре: $rawPath"

            if ([string]::IsNullOrWhiteSpace($rawPath)) {
                Write-Output 'Путь к DLL пуст.'
                continue
            }

            $pathInRegistry = $rawPath.Trim()
            if ($pathInRegistry.Length -ge 2 -and
                $pathInRegistry[0] -eq '"' -and
                $pathInRegistry[$pathInRegistry.Length - 1] -eq '"') {
                $pathInRegistry = $pathInRegistry.Substring(1, $pathInRegistry.Length - 2)
            }
            $resolvedPath = [Environment]::ExpandEnvironmentVariables($pathInRegistry)
            Write-Output "Развёрнутый путь: $resolvedPath"
            if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
                Write-Output 'Файл по этому пути не найден.'
                continue
            }

            $file = Get-Item -LiteralPath $resolvedPath -ErrorAction Stop
            $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($file.FullName)
            Write-Output "FileVersion: $($version.FileVersion)"
            Write-Output "ProductVersion: $($version.ProductVersion)"
            Write-Output "Размер файла: $($file.Length) байт"
            Write-Output "Изменён: $($file.LastWriteTime.ToString('o'))"
            try {
                $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256 -ErrorAction Stop).Hash
                Write-Output "SHA-256: $hash"
            }
            catch {
                Write-Output "Не удалось прочитать SHA-256: $($_.Exception.Message)"
            }
        }
        catch {
            Write-Output "Ошибка чтения этой записи: $($_.Exception.Message)"
        }
        finally {
            if ($null -ne $serverKey) { $serverKey.Dispose() }
            if ($null -ne $baseKey) { $baseKey.Dispose() }
        }
    }
}

Write-Output ''
Write-Output 'Скрипт ничего не создавал, не удалял и не менял в HKCU/HKLM. HKCR показан только как объединённое представление.'
Write-Output 'Вывод помогает сравнить регистрации, но сам по себе не доказывает, какую DLL уже загрузил Frame Server.'
