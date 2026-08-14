# PF01 — Forward+

Текущий активный painter playground. Первый запускаемый срез уже проводит комнату через полностью
конфиговый `depth prepass -> compute light assignment -> Forward+ -> present` граф.

## Что уже можно потрогать

- закрытая комната из внутренних сторон куба: белые/слегка тонированные стены и checker wall;
- 96 цветных анимированных point lights;
- Blinn–Phong diffuse/specular и простая material окраска;
- свободная камера: `WASD`, `Q/E`, mouse look, `Shift`, выход по `Esc`;
- HDR color (`R16G16B16A16`) и reversed-Z `D32` depth;
- 16×16 screen tiles, bounded список до 64 lights на tile и depth-aware compute culling;
- material-level shader `definitions` из TAVL (tile constants, checker variant, specular power).

Сборка и запуск из корня:

```sh
cmake --build build-debug --target PF01_forward_plus -j 2
./build-debug/subprojects/playgrounds/PF01_forward_plus/bin/PF01_forward_plus
```

`--validation` включает Vulkan validation layers. Конфиги читаются из `resources/render_config/`,
шейдеры — из `resources/shaders/`; порядок проходов не прошит в `main.cpp`.

## Первый наблюдаемый результат

- небольшая комната и открытая площадка из простых meshes; комната готова;
- свободная camera и повторяемая camera rail; свободная camera готова;
- HDR color и depth/stencil targets;
- instance, material и light buffers;
- depth prepass, tile light assignment и forward opaque pass; первый вариант готов;
- десятки или сотни движущихся lights; сейчас 96 point lights;
- переключение simple forward / Forward+;
- cluster heatmap, light-list overflow counters и CPU/GPU timings;
- просмотр промежуточных targets.

## Куда смотреть

Точка входа — `src/main.cpp`; конфигурация pipeline — `resources/render_config/render_graphs/forward_plus.tavl`,
материалы и defines — `resources/render_config/materials/materials.tavl`, вычисление списков —
`resources/shaders/light_cull.comp.glsl`. В `../common` уже лежит input-neutral `free_camera` и
reversed-Z projection; Vulkan host и feature resources пока остаются локальными.

## Следующий срез

1. Добавить переключаемый naive/simple-forward graph с тем же светом и камерой.
2. Показать tile heatmap и переполнение списка, не только bounded storage.
3. Добавить repeatable camera rail и CPU/GPU timings для честного A/B.
4. После этого — target viewer и небольшой regression/headless contract.

## Граница

Внутрь первого среза не входят production PBR library, shadows, полноценный post stack, skeletal
animation и большой outdoor world. Задачи `RND-*` подтягиваются только как блокеры этого результата.

## Definition of Done

Сцену можно запустить и сравнить в двух lighting modes. Heatmap объясняет распределение lights,
overflow ограничен и виден, render targets доступны для просмотра, а стоимость основных этапов
видна в timings.
