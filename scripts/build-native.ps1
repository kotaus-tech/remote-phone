$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vendorRoot = Join-Path $repoRoot 'native\vendor\Windows-Camera\Samples\VirtualCamera'
$sourceProject = Join-Path $vendorRoot 'VirtualCameraMediaSource\VirtualCameraMediaSource.vcxproj'
$sourcePackages = Join-Path $vendorRoot 'VirtualCameraMediaSource\packages.config'
$packageDirectory = Join-Path $vendorRoot 'packages'
$buildRoot = Join-Path $repoRoot 'native\build'
$sourceOut = Join-Path $buildRoot 'sample-out'
$sourceObj = Join-Path $buildRoot 'sample-obj'
$hostBuild = Join-Path $buildRoot 'host'
$runtimeDirectory = Join-Path $repoRoot 'apps\desktop\native-runtime'
$mediaSourceHeader = Join-Path $vendorRoot 'VirtualCameraMediaSource\VirtualCameraMediaSource.h'
$installerInclude = Join-Path $repoRoot 'apps\desktop\installer.nsh'
$desktopPackage = Join-Path $repoRoot 'apps\desktop\package.json'

foreach ($commandName in @('msbuild', 'nuget', 'cmake', 'cargo')) {
    if (-not (Get-Command $commandName -ErrorAction SilentlyContinue)) {
        throw "Не найдена команда $commandName. Установите инструменты сборки Windows C++ и повторите сборку."
    }
}

$headerText = Get-Content -LiteralPath $mediaSourceHeader -Raw
$clsidMatch = [regex]::Match($headerText, 'VIRTUALCAMERAMEDIASOURCE_CLSID\s*=\s*L"(?<clsid>\{[0-9A-Fa-f-]+\})"')
$versionMatch = [regex]::Match($headerText, 'VIRTUALCAMERAMEDIASOURCE_BUILD_VERSION\s*=\s*L"(?<version>[0-9]+\.[0-9]+\.[0-9]+)"')
if (-not $clsidMatch.Success) { throw 'В VirtualCameraMediaSource.h не найден CLSID установленного источника.' }
if (-not $versionMatch.Success) { throw 'В VirtualCameraMediaSource.h не найдена версия источника.' }
$desktopVersion = (Get-Content -LiteralPath $desktopPackage -Raw | ConvertFrom-Json).version
if ($versionMatch.Groups['version'].Value -ne $desktopVersion) {
    throw "Версия COM-источника ($($versionMatch.Groups['version'].Value)) не совпадает с версией приложения ($desktopVersion)."
}
if (-not (Get-Content -LiteralPath $installerInclude -Raw).Contains($clsidMatch.Groups['clsid'].Value)) {
    throw "Setup.exe регистрирует не тот CLSID, что native media source: $($clsidMatch.Groups['clsid'].Value)"
}

New-Item -ItemType Directory -Force -Path $packageDirectory, $buildRoot, $sourceOut, $sourceObj | Out-Null
Write-Host 'Восстанавливаются зафиксированные пакеты официального Microsoft-примера…'
& nuget restore $sourcePackages -PackagesDirectory $packageDirectory -NonInteractive -Verbosity quiet
if ($LASTEXITCODE -ne 0) { throw "NuGet restore завершился с кодом $LASTEXITCODE." }

$solutionDirectory = $vendorRoot.TrimEnd('\') + '\'
Write-Host 'Собирается COM media source Microsoft VirtualCameraMediaSource (x64)…'
& msbuild $sourceProject /m /nologo /verbosity:minimal `
    /p:Configuration=Release `
    /p:Platform=x64 `
    "/p:SolutionDir=$solutionDirectory" `
    "/p:OutDir=$($sourceOut.TrimEnd('\'))\" `
    "/p:IntDir=$($sourceObj.TrimEnd('\'))\"
if ($LASTEXITCODE -ne 0) { throw "Сборка media source завершилась с кодом $LASTEXITCODE." }

Write-Host 'Собирается управляющий host и Media Foundation smoke test (x64)…'
& cmake -S (Join-Path $repoRoot 'native\virtual-camera-host') -B $hostBuild -A x64
if ($LASTEXITCODE -ne 0) { throw "Конфигурация host завершилась с кодом $LASTEXITCODE." }
& cmake --build $hostBuild --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Сборка host завершилась с кодом $LASTEXITCODE." }

$mediaSourceDll = Join-Path $sourceOut 'VirtualCameraMediaSource.dll'
$cameraHost = Join-Path $hostBuild 'Release\RemotePhone.VirtualCameraHost.exe'
Write-Host 'Собирается C ABI мост pairing-core для Windows x64…'
& cargo build --manifest-path (Join-Path $repoRoot 'native\Cargo.toml') --package remote-phone-pairing-bridge --release --locked
if ($LASTEXITCODE -ne 0) { throw "Сборка pairing-core завершилась с кодом $LASTEXITCODE." }
$pairingBridgeDll = Join-Path $repoRoot 'native\target\release\remote_phone_pairing_bridge.dll'
foreach ($artifact in @($mediaSourceDll, $cameraHost, $pairingBridgeDll)) {
    if (-not (Test-Path -LiteralPath $artifact)) {
        throw "Не найден ожидаемый native-артефакт: $artifact"
    }
}

if (Test-Path -LiteralPath $runtimeDirectory) {
    Remove-Item -LiteralPath $runtimeDirectory -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $runtimeDirectory | Out-Null
Copy-Item -LiteralPath $mediaSourceDll -Destination (Join-Path $runtimeDirectory 'VirtualCameraMediaSource.dll') -Force
Copy-Item -LiteralPath $cameraHost -Destination (Join-Path $runtimeDirectory 'RemotePhone.VirtualCameraHost.exe') -Force
Copy-Item -LiteralPath $pairingBridgeDll -Destination (Join-Path $runtimeDirectory 'remote_phone_pairing_bridge.dll') -Force

$unexpectedScripts = Get-ChildItem -LiteralPath $runtimeDirectory -Filter '*.ps1' -Recurse
if ($unexpectedScripts.Count -ne 0) {
    throw 'В native-runtime не должны попадать PowerShell-сценарии.'
}
Write-Host "Нативные x64-компоненты версии $desktopVersion готовы для включения только в Setup.exe."
