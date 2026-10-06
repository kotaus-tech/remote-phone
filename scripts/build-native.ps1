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

function Stop-NativeBuild {
    param(
        [string] $Title,
        [int] $ExitCode,
        [object[]] $Output
    )

    $lines = @($Output | ForEach-Object { [string] $_ })
    $details = @($lines | Where-Object { $_ -match '(?i)error|failed|exception|fatal|not found|cannot' } | Select-Object -Last 12)
    if ($details.Count -eq 0) { $details = @($lines | Select-Object -Last 12) }
    $message = ($details -join ' | ')
    if ([string]::IsNullOrWhiteSpace($message)) { $message = "Код завершения: $ExitCode" }
    if ($message.Length -gt 3500) { $message = $message.Substring($message.Length - 3500) }
    $escapedMessage = $message.Replace('%', '%25').Replace("`r", '%0D').Replace("`n", '%0A')
    Write-Host "::error title=$Title::$escapedMessage"
    throw "$Title завершилась с кодом $ExitCode."
}

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
$restoreOutput = & nuget restore $sourcePackages -PackagesDirectory $packageDirectory -NonInteractive -Verbosity quiet 2>&1
$restoreExitCode = $LASTEXITCODE
$restoreOutput | ForEach-Object { Write-Host $_ }
if ($restoreExitCode -ne 0) {
    Stop-NativeBuild -Title 'NuGet restore' -ExitCode $restoreExitCode -Output $restoreOutput
}

$solutionDirectory = $vendorRoot.TrimEnd('\') + '\'
Write-Host 'Собирается COM media source Microsoft VirtualCameraMediaSource (x64)…'
$mediaSourceBuildOutput = & msbuild $sourceProject /m /nologo /verbosity:minimal `
    /p:Configuration=Release `
    /p:Platform=x64 `
    "/p:SolutionDir=$solutionDirectory" `
    "/p:OutDir=$($sourceOut.TrimEnd('\'))\" `
    "/p:IntDir=$($sourceObj.TrimEnd('\'))\" 2>&1
$mediaSourceBuildExitCode = $LASTEXITCODE
$mediaSourceBuildOutput | ForEach-Object { Write-Host $_ }
if ($mediaSourceBuildExitCode -ne 0) {
    Stop-NativeBuild -Title 'MSBuild media source' -ExitCode $mediaSourceBuildExitCode -Output $mediaSourceBuildOutput
}

Write-Host 'Собирается управляющий host и Media Foundation smoke test (x64)…'
$hostConfigureOutput = & cmake -S (Join-Path $repoRoot 'native\virtual-camera-host') -B $hostBuild -A x64 2>&1
$hostConfigureExitCode = $LASTEXITCODE
$hostConfigureOutput | ForEach-Object { Write-Host $_ }
if ($hostConfigureExitCode -ne 0) {
    Stop-NativeBuild -Title 'CMake configure host' -ExitCode $hostConfigureExitCode -Output $hostConfigureOutput
}
$hostBuildOutput = & cmake --build $hostBuild --config Release --parallel 2>&1
$hostBuildExitCode = $LASTEXITCODE
$hostBuildOutput | ForEach-Object { Write-Host $_ }
if ($hostBuildExitCode -ne 0) {
    Stop-NativeBuild -Title 'CMake build host' -ExitCode $hostBuildExitCode -Output $hostBuildOutput
}

$mediaSourceDll = Join-Path $sourceOut 'VirtualCameraMediaSource.dll'
$cameraHost = Join-Path $hostBuild 'Release\RemotePhone.VirtualCameraHost.exe'
Write-Host 'Собирается C ABI мост pairing-core для Windows x64…'
$pairingBuildOutput = & cargo build --manifest-path (Join-Path $repoRoot 'native\Cargo.toml') --package remote-phone-pairing-bridge --release --locked 2>&1
$pairingBuildExitCode = $LASTEXITCODE
$pairingBuildOutput | ForEach-Object { Write-Host $_ }
if ($pairingBuildExitCode -ne 0) {
    Stop-NativeBuild -Title 'Cargo pairing bridge' -ExitCode $pairingBuildExitCode -Output $pairingBuildOutput
}
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
