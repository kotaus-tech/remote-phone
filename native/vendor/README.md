# Внешний исходный код

## Virtual Camera media source

Каталог `Windows-Camera/Samples/VirtualCamera/VirtualCameraMediaSource` содержит исходники Microsoft для синтетического тестового источника и COM media source виртуальной камеры. Они скопированы без изменений из `microsoft/Windows-Camera`, commit `626f8b19c5f367602f2e89c6b314573d3776c9df` (ветка `master`, 2026-09-21). Копия лицензии MIT находится рядом в `Windows-Camera/LICENSE`; README официального примера — в `Windows-Camera/Samples/VirtualCamera/README.md`.

На этапе 2 используется только синтетический режим официального примера для ранней проверки Windows-устройства, установщика и CI. Источник не содержит потоков телефона и не является готовой функцией продукта.

Перед изменением кода Microsoft сверять изменения с upstream и сохранять лицензионные уведомления. API Windows и декларации SDK сверять по Windows SDK, который зафиксирован сборкой.
