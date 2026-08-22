# PF04 — stencil effects

Небольшая effect gallery для полного stencil path: depth/stencil attachment, material state, write/read
masks, reference values и использование маски последующими graphics passes.

## Запуск

```bash
cmake --build build-debug --target PF04_stencil_effects -j2
./build-debug/subprojects/playgrounds/PF04_stencil_effects/bin/PF04_stencil_effects
```

Опции: `--validation`, `--uncapped`, `--fixed-camera`, `--stencil-debug`, `--no-local-effect`. Управление:
WASD/QE, мышь, Shift; `L` включает локальный cyan tint, `V` — magenta-визуализацию selection bit,
Escape закрывает окно.

## Первый работающий срез

Первый executable использует обычный Painter graph и `D24S8` attachment:

```text
scene geometry
  → selected cube: depth pass + stencil reference 1 / replace
  → expanded back-face shell: stencil != 1 + depth test
  → fullscreen debug tint: stencil == 1
  → Visage overlay
  → present
```

Выбранный синий куб записывает `1` только там, где его fragment действительно прошёл reversed-Z depth
test. Увеличенная вдоль normal оболочка рисует только back faces и проверяет `stencil != 1`, поэтому от неё
остаётся оранжевое кольцо вокруг видимого силуэта; стоящий спереди объект продолжает скрывать outline через
depth test. Debug consumer не читает stencil как texture: fullscreen triangle проходит fixed-function
`stencil == 1` и накладывает magenta tint ровно на записанные пиксели.

Проверено 2026-08-22 на Iris Xe: debug-view визуально совпадает с выбранной геометрией, outline остаётся
снаружи маски, восьмисекундный запуск с Vulkan validation не выдаёт ошибок.

## Независимый локальный эффект

Второй срез делит stencil на независимые области ответственности:

```text
selected cube → write mask 0x01, reference 0x01
world-space rectangle → depth test → write mask 0x02, reference 0x02, color mask none
outline/debug → compare mask 0x01
fullscreen local tint → compare mask 0x02 → alpha blend cyan
```

В пересечении прямоугольника и выбранного объекта stencil содержит `0x03`. Это намеренная проверка:
outline продолжает видеть selection bit, а local tint — свой bit, потому что оба consumer'а маскируют
не относящиеся к ним разряды. Proxy rectangle не виден сам по себе: step задаёт `mask = none`, depth write
выключен, а прошедшие depth test fragments меняют только stencil. Кнопка `L` отключает consumer через UBO,
не меняя static pipeline state и не перестраивая граф.

Tint здесь намеренно blend-only. Настоящая десатурация должна прочитать исходный цвет, но читать и писать
один и тот же color attachment в обычном pass нельзя из-за feedback loop; для неё понадобится input
attachment/subpass либо отдельная ping-pong цель. Этот срез доказывает stencil routing, а не скрывает
неверный ресурсный контракт за шейдером.

## Что уже доказано в Painter

- combined depth/stencil resource создаётся, очищается и сохраняет оба aspect внутри pass;
- config уже проводит static front/back `fail/pass/depth_fail`, compare, read/write masks и reference в
  `VkStencilOpState` pipeline;
- несколько обычных graph steps последовательно используют одну stencil mask;
- fullscreen graphics consumer может использовать stencil без sampled descriptor;
- stencil writer совмещается с reversed-Z depth test, а consumer не обходит occlusion.
- разные эффекты могут безопасно делить байт через read/write masks и сохранять чужие bits;
- step-level `mask = none` действительно создаёт depth/stencil-only draw внутри color render pass.

В Painter по пути исправлен разбор color write masks: непустая маска теперь заменяет RGBA, `none` даёт
ноль, а отсутствующие blend expressions сохраняют валидные Vulkan defaults вместо sentinel `UINT32_MAX`.
Регресс покрыт `painter_shader_prepare_test` (`13` cases, `147` assertions).

Reference/read/write masks сейчас **static pipeline state**. В material `dynamic` поддержан только
`depth_bias`; runtime controls потребуют добавить `StencilReference`, `StencilCompareMask` и
`StencilWriteMask`, а затем команды их установки. Первый срез намеренно не изображает runtime изменением
UBO: fixed-function stencil state от UBO не меняется.

## Следующие срезы

1. Пространственный window proof: geometry aperture пишет mask, второй view рисуется только внутри неё;
   полноценные recursive portal и mirror clipping в первый proof не входят.
2. Dynamic reference/read/write masks и UI controls.
3. Намеренно разные front/back operations с debug fixture, где результат обеих сторон виден отдельно.

## Definition of Done

Outline, локальный post-effect и window mask используют обычные Painter materials/render graph; static и
dynamic front/back stencil state проверены явно, а debug view позволяет объяснить, почему конкретный pixel
прошёл или не прошёл test. Production parsing/execution fixes принадлежат `libs/painter`; демонстрационные
shaders и visualization остаются в лаборатории.
