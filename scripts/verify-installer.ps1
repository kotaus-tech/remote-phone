param(
    [Parameter(Mandatory = $true)]
    [string] $InstallerPath
)

$ErrorActionPreference = 'Stop'
$InstallerPath = [System.IO.Path]::GetFullPath($InstallerPath)
$cameraClsid = '{7B89B92E-FE71-42D0-8A41-E137D06EA184}'
$comSubKey = "Software\Classes\CLSID\$cameraClsid\InprocServer32"
$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine,
    [Microsoft.Win32.RegistryView]::Registry64)
$installDirectory = Join-Path $env:ProgramFiles 'Kotaus\RemotePhone-Installer-CI'
$programFilesPrefix = [System.IO.Path]::GetFullPath($env:ProgramFiles).TrimEnd('\') + '\'
$logDirectory = Join-Path $env:ProgramData 'Kotaus\RemotePhone\logs'
$logPath = Join-Path $logDirectory 'VirtualCameraMediaSource.log'
$installed = $false
$captureSmokeResult = 'не запускался'

function Assert-LogAccessRule {
    param(
        [System.Security.AccessControl.FileSystemAccessRule[]] $Rules,
        [string] $Sid,
        [System.Security.AccessControl.FileSystemRights] $RequiredRights,
        [string] $FriendlyName,
        [string] $ObjectName
    )

    $matchingRule = $Rules | Where-Object {
        $_.IdentityReference.Value -eq $Sid -and
        $_.AccessControlType -eq [System.Security.AccessControl.AccessControlType]::Allow -and
        (($_.FileSystemRights -band $RequiredRights) -eq $RequiredRights)
    } | Select-Object -First 1
    if ($null -eq $matchingRule) {
        throw "В ProgramData отсутствуют нужные права $FriendlyName ($Sid) на объект журнала $ObjectName."
    }
}

try {
    if (-not (Test-Path -LiteralPath $InstallerPath)) {
        throw "Setup.exe не найден: $InstallerPath"
    }

    $normalizedInstallDirectory = [System.IO.Path]::GetFullPath($installDirectory)
    if (-not $normalizedInstallDirectory.StartsWith($programFilesPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Проверочный каталог установки должен быть внутри Program Files: $normalizedInstallDirectory"
    }
    if (Test-Path -LiteralPath $installDirectory) {
        Remove-Item -LiteralPath $installDirectory -Recurse -Force
    }

    Write-Host 'Установка свежего Setup.exe в Program Files…'
    $install = Start-Process -FilePath $InstallerPath -ArgumentList "/S /D=$installDirectory" -Wait -PassThru
    if ($install.ExitCode -ne 0) { throw "Setup.exe завершился с кодом $($install.ExitCode)." }
    $installed = $true

    $installedDll = Join-Path $installDirectory 'resources\native\VirtualCameraMediaSource.dll'
    $installedHost = Join-Path $installDirectory 'resources\native\RemotePhone.VirtualCameraHost.exe'
    $installedPairingBridge = Join-Path $installDirectory 'resources\native\remote_phone_pairing_bridge.dll'
    foreach ($nativeFile in @($installedDll, $installedHost, $installedPairingBridge)) {
        if (-not (Test-Path -LiteralPath $nativeFile)) {
            throw "Нативный компонент не попал в Setup.exe: $nativeFile"
        }
        if (-not ([System.IO.Path]::GetFullPath($nativeFile).StartsWith(
                $programFilesPrefix,
                [System.StringComparison]::OrdinalIgnoreCase))) {
            throw "Нативная DLL/программа установлена вне Program Files: $nativeFile"
        }
    }

    $shippedScripts = @(Get-ChildItem -LiteralPath (Join-Path $installDirectory 'resources\native') -Filter '*.ps1' -Recurse -File)
    if ($shippedScripts.Count -ne 0) {
        throw "Setup.exe не должен содержать пользовательские PowerShell-сценарии: $($shippedScripts[0].FullName)"
    }

    $registrationKey = $registry.OpenSubKey($comSubKey, $false)
    if ($null -eq $registrationKey) {
        throw "Установщик не создал 64-разрядную HKLM COM-регистрацию: $comSubKey"
    }
    try {
        $registeredDll = [string] $registrationKey.GetValue('')
        $threadingModel = [string] $registrationKey.GetValue('ThreadingModel')
    }
    finally {
        $registrationKey.Dispose()
    }
    if ([System.IO.Path]::GetFullPath($registeredDll) -ne [System.IO.Path]::GetFullPath($installedDll)) {
        throw "HKLM64 указывает не на DLL из Program Files: $registeredDll"
    }
    if ($threadingModel -ne 'Both') { throw 'HKLM64 COM-регистрация не содержит ThreadingModel=Both.' }

    if (-not (Test-Path -LiteralPath $logDirectory -PathType Container)) {
        throw "Setup.exe не создал каталог журнала ProgramData: $logDirectory"
    }
    if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) {
        throw "Setup.exe не создал файл журнала ProgramData: $logPath"
    }
    $logDirectoryAcl = Get-Acl -LiteralPath $logDirectory
    $logDirectoryRules = $logDirectoryAcl.GetAccessRules(
        $true,
        $true,
        [System.Security.Principal.SecurityIdentifier])
    Assert-LogAccessRule -Rules $logDirectoryRules -Sid 'S-1-5-18' -RequiredRights ([System.Security.AccessControl.FileSystemRights]::FullControl) -FriendlyName 'SYSTEM' -ObjectName $logDirectory
    Assert-LogAccessRule -Rules $logDirectoryRules -Sid 'S-1-5-19' -RequiredRights ([System.Security.AccessControl.FileSystemRights]::Modify) -FriendlyName 'LocalService' -ObjectName $logDirectory
    Assert-LogAccessRule -Rules $logDirectoryRules -Sid 'S-1-5-20' -RequiredRights ([System.Security.AccessControl.FileSystemRights]::Modify) -FriendlyName 'NetworkService' -ObjectName $logDirectory
    Assert-LogAccessRule -Rules $logDirectoryRules -Sid 'S-1-5-32-545' -RequiredRights ([System.Security.AccessControl.FileSystemRights]::ReadAndExecute) -FriendlyName 'Users' -ObjectName $logDirectory

    $logFileAcl = Get-Acl -LiteralPath $logPath
    $logFileRules = $logFileAcl.GetAccessRules(
        $true,
        $true,
        [System.Security.Principal.SecurityIdentifier])
    Assert-LogAccessRule -Rules $logFileRules -Sid 'S-1-5-18' -RequiredRights ([System.Security.AccessControl.FileSystemRights]::FullControl) -FriendlyName 'SYSTEM' -ObjectName $logPath
    Assert-LogAccessRule -Rules $logFileRules -Sid 'S-1-5-19' -RequiredRights ([System.Security.AccessControl.FileSystemRights]::Modify) -FriendlyName 'LocalService' -ObjectName $logPath
    Assert-LogAccessRule -Rules $logFileRules -Sid 'S-1-5-20' -RequiredRights ([System.Security.AccessControl.FileSystemRights]::Modify) -FriendlyName 'NetworkService' -ObjectName $logPath
    $userLogRights = [System.Security.AccessControl.FileSystemRights]::ReadData -bor [System.Security.AccessControl.FileSystemRights]::AppendData
    Assert-LogAccessRule -Rules $logFileRules -Sid 'S-1-5-32-545' -RequiredRights $userLogRights -FriendlyName 'Users' -ObjectName $logPath

    $installedHash = (Get-FileHash -LiteralPath $installedDll -Algorithm SHA256).Hash
    Write-Host "Установленная DLL: $installedDll"
    Write-Host "SHA-256 DLL: $installedHash"
    Write-Host 'Проверены Program Files, HKLM64, отсутствие пользовательских сценариев и ACL журнала ProgramData.'

    $frameServer = Get-Service -Name 'FrameServer' -ErrorAction SilentlyContinue
    $windowsBuild = [System.Environment]::OSVersion.Version.Build
    if ($null -eq $frameServer -or $windowsBuild -lt 22000) {
        $captureSmokeResult = "пропущен: API/FrameServer недоступен (build=$windowsBuild; FrameServer=$($null -ne $frameServer))"
        Write-Host "Media Foundation camera smoke test $captureSmokeResult."
    }
    else {
        $logLengthBeforeSmoke = (Get-Item -LiteralPath $logPath).Length
        Write-Host 'Запуск Media Foundation smoke test: проверка типов NV12/RGB32 и захват кадров 1080p60…'
        $smokeOutput = & $installedHost '--ci-smoke' 2>&1
        $smokeExitCode = $LASTEXITCODE
        $smokeOutput | ForEach-Object { Write-Host $_ }
        $smokeCaptureAvailable = $true
        if ($smokeExitCode -ne 0) {
            $failureLines = @($smokeOutput | ForEach-Object { [string] $_ })
            if (Test-Path -LiteralPath $logPath -PathType Leaf) {
                $failureLines += @(Get-Content -LiteralPath $logPath -Tail 20 -Encoding UTF8 -ErrorAction SilentlyContinue)
            }
            $failureDetails = @($failureLines | Where-Object { $_ -match '(?i)CI_CAMERA|error|failed|HRESULT|код 0x' } | Select-Object -Last 20)
            if ($failureDetails.Count -eq 0) { $failureDetails = @($failureLines | Select-Object -Last 20) }
            $failureMessage = ($failureDetails -join ' | ')
            if ($failureMessage.Length -gt 3500) { $failureMessage = $failureMessage.Substring($failureMessage.Length - 3500) }
            $escapedFailureMessage = $failureMessage.Replace('%', '%25').Replace("`r", '%0D').Replace("`n", '%0A')
            if (($failureLines -join "`n") -match 'CI_CAMERA_START_FAILED hresult=0x80070005') {
                $smokeCaptureAvailable = $false
                $captureSmokeResult = 'пропущен: hosted runner запретил IMFVirtualCamera::Start (E_ACCESSDENIED)'
                Write-Host "::warning title=Media Foundation camera capture::$escapedFailureMessage"
                if ($env:GITHUB_STEP_SUMMARY) {
                    Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value "`n- Media Foundation capture smoke: $captureSmokeResult. Установка и удаление Setup проверяются отдельно."
                }
            }
            else {
                Write-Host "::error title=Media Foundation camera smoke::$escapedFailureMessage"
                throw "Media Foundation smoke test завершился с кодом $smokeExitCode."
            }
        }

        if ($smokeCaptureAvailable) {
            if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) {
                throw "Источник не создал журнал в ProgramData: $logPath"
            }
            $logBytes = [System.IO.File]::ReadAllBytes($logPath)
            $logOffset = [int] $logLengthBeforeSmoke
            if ($logBytes.Length -le $logOffset) {
                throw 'Media Foundation smoke test не добавил записи в журнал ProgramData.'
            }
            $newLogContent = [System.Text.Encoding]::UTF8.GetString(
                $logBytes,
                $logOffset,
                $logBytes.Length - $logOffset)
            $identityLine = $newLogContent -split '\r?\n' |
                Where-Object { $_.Contains('media_source_identity') } |
                Select-Object -First 1
            if ($null -eq $identityLine) {
                throw 'Журнал этого smoke test не содержит новую запись идентификации media source.'
            }
            $hashMatch = [regex]::Match($identityLine, 'sha256=(?<hash>[A-Fa-f0-9]{64})')
            $processMatch = [regex]::Match($identityLine, 'pid=(?<pid>[0-9]+) process="(?<name>[^"]+)" process_path="(?<path>[^"]+)"')
            $dllMatch = [regex]::Match($identityLine, 'dll_path="(?<path>[^"]+)"')
            if (-not $hashMatch.Success -or -not $processMatch.Success -or -not $dllMatch.Success) {
                throw 'Новая запись журнала не содержит полный SHA-256, PID и пути процесса/DLL.'
            }
            if (-not [string]::Equals($hashMatch.Groups['hash'].Value, $installedHash, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Источник загрузил DLL с SHA-256 $($hashMatch.Groups['hash'].Value), ожидалась установленная DLL $installedHash."
            }
            $loggedDllPath = [System.IO.Path]::GetFullPath($dllMatch.Groups['path'].Value)
            if (-not [string]::Equals($loggedDllPath, [System.IO.Path]::GetFullPath($installedDll), [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Источник загрузил DLL не из установленного Program Files: $loggedDllPath"
            }
            $captureSmokeResult = 'пройден: режимы перечислены, захвачены меняющиеся кадры 1080p60'
            Write-Host 'Media Foundation перечислил режимы, захватил меняющиеся кадры 1080p60 и записал идентификатор DLL/процесса.'
        }
    }

    $uninstaller = Get-ChildItem -LiteralPath $installDirectory -Filter 'Uninstall*.exe' -File | Select-Object -First 1
    if ($null -eq $uninstaller) { throw 'В установленном каталоге не найден uninstaller.' }
    Write-Host 'Удаление Setup.exe и проверка удаления HKLM64 COM-регистрации…'
    $uninstall = Start-Process -FilePath $uninstaller.FullName -ArgumentList '/S' -Wait -PassThru
    if ($uninstall.ExitCode -ne 0) { throw "Удаление завершилось с кодом $($uninstall.ExitCode)." }
    $installed = $false

    $remainingRegistration = $registry.OpenSubKey($comSubKey, $false)
    if ($null -ne $remainingRegistration) {
        $remainingRegistration.Dispose()
        throw 'После удаления осталась 64-разрядная HKLM COM-регистрация виртуальной камеры.'
    }
    if (Test-Path -LiteralPath $installDirectory) {
        throw "Деинсталлятор оставил каталог приложения: $installDirectory"
    }

    Write-Host "Setup.exe: установка, удаление, HKLM64, Program Files и ACL проверены; Media Foundation capture smoke: $captureSmokeResult."
}
catch {
    $message = $_.Exception.Message
    if ($message.Length -gt 3500) { $message = $message.Substring($message.Length - 3500) }
    $escapedMessage = $message.Replace('%', '%25').Replace("`r", '%0D').Replace("`n", '%0A')
    Write-Host "::error title=Setup install verification::$escapedMessage"
    throw
}
finally {
    if ($installed -and (Test-Path -LiteralPath $installDirectory)) {
        $uninstaller = Get-ChildItem -LiteralPath $installDirectory -Filter 'Uninstall*.exe' -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $uninstaller) {
            try {
                Start-Process -FilePath $uninstaller.FullName -ArgumentList '/S' -Wait -PassThru | Out-Null
            }
            catch {
                Write-Warning "Автоочистка проверочной установки завершилась ошибкой: $($_.Exception.Message)"
            }
        }
    }
    $registry.Dispose()
}
