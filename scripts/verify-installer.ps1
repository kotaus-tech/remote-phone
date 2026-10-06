param(
    [Parameter(Mandatory = $true)]
    [string] $InstallerPath
)

$ErrorActionPreference = 'Stop'
$InstallerPath = [System.IO.Path]::GetFullPath($InstallerPath)
$cameraClsid = '{7B89B92E-FE71-42D0-8A41-E137D06EA184}'
$comKey = "Registry::HKEY_LOCAL_MACHINE\Software\Classes\CLSID\$cameraClsid\InprocServer32"
$installDirectory = Join-Path $env:TEMP 'RemotePhone-Installer-CI'

if (-not (Test-Path -LiteralPath $InstallerPath)) {
    throw "Setup.exe не найден: $InstallerPath"
}
if (Test-Path -LiteralPath $installDirectory) {
    Remove-Item -LiteralPath $installDirectory -Recurse -Force
}

Write-Host 'Установка Setup.exe в отдельный временный каталог…'
$install = Start-Process -FilePath $InstallerPath -ArgumentList "/S /D=$installDirectory" -Wait -PassThru
if ($install.ExitCode -ne 0) { throw "Setup.exe завершился с кодом $($install.ExitCode)." }

$installedDll = Join-Path $installDirectory 'resources\native\VirtualCameraMediaSource.dll'
$installedHost = Join-Path $installDirectory 'resources\native\RemotePhone.VirtualCameraHost.exe'
$installedPairingBridge = Join-Path $installDirectory 'resources\native\remote_phone_pairing_bridge.dll'
$installedPocScript = Join-Path $installDirectory 'resources\native\Run-VirtualCameraPoC.ps1'
if (-not (Test-Path -LiteralPath $installedDll)) { throw "DLL не попала в Setup.exe: $installedDll" }
if (-not (Test-Path -LiteralPath $installedHost)) { throw "Тестовая программа не попала в Setup.exe: $installedHost" }
if (-not (Test-Path -LiteralPath $installedPairingBridge)) { throw "Rust-мост сопряжения не попал в Setup.exe: $installedPairingBridge" }
if (-not (Test-Path -LiteralPath $installedPocScript)) { throw "Сценарий проверки COM-регистрации текущего пользователя не попал в Setup.exe: $installedPocScript" }
if (-not (Test-Path -LiteralPath $comKey)) { throw "Установщик не создал COM-регистрацию: $comKey" }

$registration = Get-ItemProperty -LiteralPath $comKey
if ([System.IO.Path]::GetFullPath($registration.'(default)') -ne [System.IO.Path]::GetFullPath($installedDll)) {
    throw "COM-регистрация указывает не на установленную DLL: $($registration.'(default)')"
}
if ($registration.ThreadingModel -ne 'Both') { throw 'COM-регистрация не содержит ThreadingModel=Both.' }

$uninstaller = Get-ChildItem -LiteralPath $installDirectory -Filter 'Uninstall*.exe' | Select-Object -First 1
if ($null -eq $uninstaller) { throw 'В установленном каталоге не найден uninstaller.' }
Write-Host 'Удаление приложения и проверка очистки регистрации…'
$uninstall = Start-Process -FilePath $uninstaller.FullName -ArgumentList '/S' -Wait -PassThru
if ($uninstall.ExitCode -ne 0) { throw "Удаление завершилось с кодом $($uninstall.ExitCode)." }
if (Test-Path -LiteralPath $comKey) { throw 'После удаления осталась COM-регистрация виртуальной камеры.' }

Write-Host 'Проверены установка, вложение нативных библиотек и тестовой программы, сценария PowerShell, COM-регистрация и очистка Setup.exe.'
