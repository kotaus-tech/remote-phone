$ErrorActionPreference = 'Stop'

$hostPath = Join-Path $PSScriptRoot '..\apps\desktop\native-runtime\RemotePhone.VirtualCameraHost.exe'
$hostPath = [System.IO.Path]::GetFullPath($hostPath)

if (-not (Test-Path -LiteralPath $hostPath)) {
    throw 'Тестовый источник не найден. Сначала установите приложение из Setup.exe.'
}

Write-Host 'Откройте «Камера» Windows, Chrome, Edge или OBS, чтобы проверить изображение.'
Write-Host 'Нажмите Enter здесь, чтобы корректно остановить и удалить тестовую камеру.'
& $hostPath
if ($LASTEXITCODE -ne 0) {
    throw "Тестовый источник завершился с кодом $LASTEXITCODE."
}
