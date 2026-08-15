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
- edge anti-aliasing и физическая мягкость разделены: runtime hard/3×3 PCF/rotated-Poisson выбирают
  фиксированный AA kernel, а независимый spot-PCSS использует emitter radius в мировых единицах;
- main-поток консервативно пересекает caster bounds с каждым spot cone, пакует подходящие instance lanes и
  формирует два generic region spans (stage/casters) на источник;
- оба atlas pass используют `draw_regions`: региональная команда отдельно несёт viewport/scissor,
  dynamic depth bias, индекс GPU-записи и диапазон draw-group spans; cascade/spot matrices и light colors
  остаются в обычных storage buffers;
- raster constant/slope остаются raw Vulkan diagnostics; receiver normal offset вычисляется из world-size
  shadow texel конкретного cascade/spot depth, а каждый tap получает receiver-plane depth correction через
  экранные производные; zero/default presets управляют всеми bias-компонентами;
- camera depth prepass питает отдельный half-resolution compute pass: восемь коротких screen-space шагов
  с occupied-ray refinement формируют один directional и четыре spot contact-shadow канала; восстановленная
  receiver plane отсекает grazing-angle self-intersection и blockers с обратной стороны поверхности,
  а `N·L`, cone и range rejection пропускают заведомо лишние rays; короткий `0.24 m` preset закрывает
  bias-gap, но single-depth silhouette остаётся фундаментальным ограничением screen-space метода, поэтому
  этот исследовательский режим выключен по умолчанию и включается через `F`/`--contact`;
- прямые debug-views показывают оба полных atlas и обе contact masks в правом верхнем углу;
- `dynamic = [ depth_bias ]` принадлежит material; статический raster bias остаётся альтернативой для
  материалов без dynamic state;
- overlay показывает caster occupancy каждого atlas region, world-texel/receiver-plane bias, независимые
  AA/PCSS/contact режимы и сглаженные GPU timestamps для directional/spot/depth/contact/forward/blit passes;
- общие free camera, Visage overlay и независимый frame pacer из `../common`.

Сборка и запуск из корня:

```sh
cmake --build build-debug --target PF02_shadows -j 2
./build-debug/subprojects/playgrounds/PF02_shadows/bin/PF02_shadows --validation
```

Управление: `WASD`, `Q/E`, mouse look, `Shift`; `Z/X` — base normal bias, `C/V` — его slope,
`G/H` — receiver-plane scale, `B/N` — raster constant, `M/,` — raster slope, `0/1` — zero/default bias.
`2/3/4` изолируют все/directional/spot lights; `5/6/7` выбирают hard/PCF/Poisson edge AA, `[`/`]`
меняют AA radius. `8` независимо включает spot PCSS, `;`/`'` меняют emitter radius в метрах, `F`
включает contact shadows, `R` целиком отключает sampling shadow maps для A/B, `9` — cascade tint;
`Esc` завершает работу.

CLI-presets `--zero-bias`, `--all-lights`, `--directional-only`, `--spot-only`, `--hard`, `--pcf`,
`--poisson`, `--pcss`, `--contact`, `--no-contact`, `--map-shadows`, `--no-map-shadows`,
`--receiver-plane`, `--no-receiver-plane`, `--cascade-debug` позволяют воспроизвести сравнение.
`--uncapped` отключает только 60 FPS limiter.

## Полный наблюдаемый результат

- четыре stabilized directional cascades в одном atlas с blend bands;
- shadow atlas для spot lights;
- статические и движущиеся casters;
- runtime raw-raster, world-texel normal и receiver-plane bias controls;
- независимые edge AA, spot PCSS и screen-space contact-shadow A/B;
- просмотр обоих atlas и contact masks;
- GPU timings и явная atlas occupancy/culling diagnostics.

Point-light cubemap shadows — отдельное расширение после стабильного первого среза.

## Куда смотреть

Точка входа — `src/main.cpp`; порядок проходов —
`resources/render_config/render_graphs/shadows.tavl`; shadow sampling —
`resources/shaders/shadowed.frag.glsl`, screen-space rays — `contact_shadows.comp.glsl`, а просмотр
atlas/masks — `shadow_debug.frag.glsl`.

## Следующий срез

1. Собрать `guarded contact` preset и сравнить его с `contact off` на фиксированных camera bookmarks:
   противоположный угол blocker, grazing receiver, край экрана и вытянутая стенка. Последовательно проверить:
   - thickness threshold, масштабируемый локальной производной linear depth/экранным footprint;
   - fade по длине ray и camera depth;
   - viewport-edge fade, backface/`N·L` mask и conservative depth-discontinuity rejection;
   - depth-derivative confidence: умеренный slope расширяет допустимую thickness, silhouette jump отклоняет ray.
2. Проверить `dual-depth contact`: front depth + back depth того же instance, normal и instance ID образуют
   camera-ray thickness interval. Сравнить качество, GPU time и память с guarded single-depth preset;
   альбедо в этот минимальный occlusion G-buffer не включать. Режимы лаборатории: `contact off` →
   `contact guarded` → `contact dual-depth`; HZB/history/temporal stabilization остаются PF03.
3. Проверить CSM texel snapping и новый world-texel bias на repeatable camera rail.
4. Добавить conservative directional caster culling и вывести blend-band/stability diagnostics, если
   визуального tint и полного depth atlas окажется недостаточно.
5. Отдельным spatial-срезом исследовать физическую directional area-light softness; текущий PCSS
   намеренно применяется только к spot lights, а directional shadows получают лишь выбранный edge AA.
6. После измерений заменить фиксированную раскладку минимальным atlas allocation/lifetime contract.

Bias fixtures, world-texel/receiver-plane correction, independent hard/PCF/Poisson AA, spot-PCSS,
half-resolution contact masks и CSM baseline уже работают. Текущий PCSS и восьмишаговый screen-space ray —
исследовательские presets. Stochastic sampling, history и temporal accumulation намеренно оставлены PF03.

GPU `complete graph` измеряет интервал от начала первого command buffer до конца present-blit pass на
graphics queue; он намеренно не является временем `vkQueuePresentKHR`, scanout или CPU frame.

## Definition of Done

Движущиеся light и caster дают стабильную объяснимую тень без грубого acne/peter-panning на тестовых
поверхностях; atlas, bias и стоимость доступны в debug UI.
