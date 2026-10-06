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

foreach ($commandName in @('msbuild', 'nuget', 'cmake', 'cargo')) {
    if (-not (Get-Command $commandName -ErrorAction SilentlyContinue)) {
        throw "Не найдена команда $commandName. Установите инструменты сборки Windows C++ и повторите сборку."
    }
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

Write-Host 'Собирается тестовый host для MFCreateVirtualCamera (x64)…'
& cmake -S (Join-Path $repoRoot 'native\virtual-camera-host') -B $hostBuild -A x64
if ($LASTEXITCODE -ne 0) { throw "Конфигурация тестового host завершилась с кодом $LASTEXITCODE." }
& cmake --build $hostBuild --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Сборка тестового host завершилась с кодом $LASTEXITCODE." }

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

New-Item -ItemType Directory -Force -Path $runtimeDirectory | Out-Null
Copy-Item -LiteralPath $mediaSourceDll -Destination (Join-Path $runtimeDirectory 'VirtualCameraMediaSource.dll') -Force
Copy-Item -LiteralPath $cameraHost -Destination (Join-Path $runtimeDirectory 'RemotePhone.VirtualCameraHost.exe') -Force
Copy-Item -LiteralPath $pairingBridgeDll -Destination (Join-Path $runtimeDirectory 'remote_phone_pairing_bridge.dll') -Force
$pocScript = Join-Path $repoRoot 'scripts\run-virtual-camera-poc.ps1'
$comAuditScript = Join-Path $repoRoot 'scripts\inspect-virtual-camera-com.ps1'
foreach ($scriptPath in @($pocScript, $comAuditScript)) {
    try {
        $null = [scriptblock]::Create((Get-Content -LiteralPath $scriptPath -Raw))
    } catch {
        throw "Сценарий PowerShell '$scriptPath' содержит синтаксическую ошибку: $($_.Exception.Message)"
    }
}
Copy-Item -LiteralPath $pocScript `
    -Destination (Join-Path $runtimeDirectory 'Run-VirtualCameraPoC.ps1') -Force
Copy-Item -LiteralPath $comAuditScript `
    -Destination (Join-Path $runtimeDirectory 'Inspect-VirtualCameraCom.ps1') -Force
Write-Host 'Нативные x64-компоненты, сценарий PoC и read-only аудит COM-регистрации готовы для включения в Setup.exe.'
