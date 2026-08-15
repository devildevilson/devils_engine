# PF02 — shadows

Независимая лаборатория shadow rendering поверх минимального painter baseline, доказанного `PF01`.
Она не зависит от target или исходников `PF01`: нужный baseline копируется выборочно либо берётся из
уже общего API `libs/painter`/`../common`.

## Что уже можно потрогать

- самостоятельный `PF02_shadows` executable и TAVL graph;
- отдельная `2048 × 2048` reverse-Z directional shadow map;
- фиксированный `2048 × 2048` spot atlas: четыре `1024 × 1024` региона в сетке `2 × 2`, записанные
  одним config-defined `draw_regions` step;
- четыре цветных spot lights; один light движется, каждый имеет собственную view-projection matrix;
- открытая сцена с floor/walls и пятью cube casters, два из которых движутся;
- отдельные directional/atlas shadow passes и обычный forward pass с 3×3 PCF для обоих типов;
- main-поток консервативно пересекает caster bounds с каждым spot cone, пакует подходящие instance lanes и
  формирует два generic region spans (stage/casters) на источник;
- региональная команда отдельно несёт viewport/scissor, dynamic depth bias, индекс GPU-записи и диапазон
  draw-group spans; light matrices/colors остаются в обычном storage buffer;
- receiver constant/slope bias; базовый bias меняется во время работы клавишами `Z/X`;
- прямые depth debug-views directional map и всего spot atlas в правом верхнем углу;
- `dynamic = [ depth_bias ]` принадлежит material; статический raster bias остаётся альтернативой для
  материалов без dynamic state;
- общие free camera, Visage overlay и независимый frame pacer из `../common`.

Сборка и запуск из корня:

```sh
cmake --build build-debug --target PF02_shadows -j 2
./build-debug/subprojects/playgrounds/PF02_shadows/bin/PF02_shadows --validation
```

Управление: `WASD`, `Q/E`, mouse look, `Shift`, `Z/X` для receiver bias, `Esc`.
`--uncapped` отключает только 60 FPS limiter.

## Полный наблюдаемый результат

- одна directional shadow map;
- shadow atlas для spot lights;
- статические и движущиеся casters;
- runtime depth/slope bias controls;
- просмотр shadow atlas и cascade/atlas regions;
- GPU timings и явная atlas occupancy/culling diagnostics.

Point-light cubemap shadows — отдельное расширение после стабильного первого среза.

## Куда смотреть

Точка входа — `src/main.cpp`; порядок проходов —
`resources/render_config/render_graphs/shadows.tavl`; shadow sampling —
`resources/shaders/shadowed.frag.glsl`, а прямой просмотр depth — `shadow_debug.frag.glsl`.

## Следующий срез

1. Показать selected region, dynamic caster bias и atlas occupancy/culling counts в overlay.
2. Добавить GPU timings.
3. Сравнить receiver bias с raster depth bias и зафиксировать policy против acne/peter-panning.
4. Заменить фиксированную раскладку минимальным atlas allocation/lifetime contract.

## Definition of Done

Движущиеся light и caster дают стабильную объяснимую тень без грубого acne/peter-panning на тестовых
поверхностях; atlas, bias и стоимость доступны в debug UI.
