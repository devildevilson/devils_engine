# PF02 — shadows

Независимая лаборатория shadow rendering поверх минимального painter baseline, доказанного `PF01`.
Она не зависит от target или исходников `PF01`: нужный baseline копируется выборочно либо берётся из
уже общего API `libs/painter`/`../common`.

## Что уже можно потрогать

- самостоятельный `PF02_shadows` executable и TAVL graph;
- `2048 × 2048` reverse-Z directional atlas с четырьмя `1024 × 1024` camera-frustum cascades;
- фиксированный `2048 × 2048` spot atlas: четыре `1024 × 1024` региона в сетке `2 × 2`, записанные
  одним config-defined `draw_regions` step;
- четыре цветных spot lights; один light движется, каждый имеет собственную view-projection matrix;
- открытая сцена с floor/walls, пятью cube casters, наклонным receiver и тонким contact caster;
- practical split (`lambda = 0.68`), rotation-independent cascade sphere, light-space texel snapping и
  12% blend band; `9` включает окраску выбранных каскадов;
- отдельные directional/spot-atlas shadow passes и обычный forward pass с runtime hard, 3×3 PCF и rotated
  Poisson фильтрами; spot lights дополнительно имеют экспериментальный PCSS;
- main-поток консервативно пересекает caster bounds с каждым spot cone, пакует подходящие instance lanes и
  формирует два generic region spans (stage/casters) на источник;
- оба atlas pass используют `draw_regions`: региональная команда отдельно несёт viewport/scissor,
  dynamic depth bias, индекс GPU-записи и диапазон draw-group spans; cascade/spot matrices и light colors
  остаются в обычных storage buffers;
- raster constant/slope и receiver constant/slope bias независимо меняются во время работы; presets
  мгновенно обнуляют либо восстанавливают все четыре значения;
- прямые depth debug-views всего directional CSM atlas и spot atlas в правом верхнем углу;
- `dynamic = [ depth_bias ]` принадлежит material; статический raster bias остаётся альтернативой для
  материалов без dynamic state;
- overlay показывает caster occupancy каждого atlas region, packed count, caster/receiver bias и
  сглаженные GPU timestamps для directional/spot/forward/blit passes и полного render graph;
- общие free camera, Visage overlay и независимый frame pacer из `../common`.

Сборка и запуск из корня:

```sh
cmake --build build-debug --target PF02_shadows -j 2
./build-debug/subprojects/playgrounds/PF02_shadows/bin/PF02_shadows --validation
```

Управление: `WASD`, `Q/E`, mouse look, `Shift`; `Z/X` — receiver constant, `C/V` — receiver slope,
`B/N` — raster constant, `M/,` — raster slope, `0/1` — zero/default bias. `2/3/4` изолируют все,
directional или spot lights; `5/6/7/8` выбирают hard/PCF/Poisson/PCSS; `[` и `]` меняют softness;
`9` включает cascade tint; `Esc` завершает работу.

CLI-presets `--zero-bias`, `--all-lights`, `--directional-only`, `--spot-only`, `--hard`, `--pcf`,
`--poisson`, `--pcss`, `--cascade-debug` позволяют воспроизвести сравнение сразу после запуска.
`--uncapped` отключает только 60 FPS limiter.

## Полный наблюдаемый результат

- четыре stabilized directional cascades в одном atlas с blend bands;
- shadow atlas для spot lights;
- статические и движущиеся casters;
- runtime depth/slope bias controls;
- просмотр directional shadow map и atlas regions;
- GPU timings и явная atlas occupancy/culling diagnostics.

Point-light cubemap shadows — отдельное расширение после стабильного первого среза.

## Куда смотреть

Точка входа — `src/main.cpp`; порядок проходов —
`resources/render_config/render_graphs/shadows.tavl`; shadow sampling —
`resources/shaders/shadowed.frag.glsl`, а прямой просмотр depth — `shadow_debug.frag.glsl`.

## Следующий срез

1. Проверить CSM texel snapping на repeatable camera rail и сделать cascade-aware bias policy: одинаковый
   normalized bias сейчас означает разный world-space offset в каскадах разной глубины.
2. Добавить conservative directional caster culling и вывести blend-band/stability diagnostics, если
   визуального tint и полного depth atlas окажется недостаточно.
3. На каскадах уточнить directional softness; текущий режим `PCSS` применяет PCSS
   только к spot lights, а для directional map намеренно использует расширенный rotated Poisson.
4. После измерений заменить фиксированную раскладку минимальным atlas allocation/lifetime contract.

Bias fixtures, четыре независимых bias controls, hard/PCF/Poisson/spot-PCSS A/B и первый CSM baseline уже
работают. Текущий PCSS — исследовательская blocker-search эвристика, а не зафиксированный production
preset; runtime softness нужен в том числе для поиска light leaking и потери contact shadow.

GPU `complete graph` измеряет интервал от начала первого command buffer до конца present-blit pass на
graphics queue; он намеренно не является временем `vkQueuePresentKHR`, scanout или CPU frame.

## Definition of Done

Движущиеся light и caster дают стабильную объяснимую тень без грубого acne/peter-panning на тестовых
поверхностях; atlas, bias и стоимость доступны в debug UI.
