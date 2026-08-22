# PF04 — stencil effects

Небольшая effect gallery для полного stencil path: depth/stencil attachment, material state, write/read
masks, reference values и использование маски последующими graphics passes.

## Запуск

```bash
cmake --build build-debug --target PF04_stencil_effects -j2
./build-debug/subprojects/playgrounds/PF04_stencil_effects/bin/PF04_stencil_effects
```

Опции: `--validation`, `--uncapped`, `--fixed-camera`, `--stencil-debug`. Управление: WASD/QE, мышь,
Shift; `V` включает magenta-визуализацию stencil reference 1, Escape закрывает окно.

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

## Что уже доказано в Painter

- combined depth/stencil resource создаётся, очищается и сохраняет оба aspect внутри pass;
- config уже проводит static front/back `fail/pass/depth_fail`, compare, read/write masks и reference в
  `VkStencilOpState` pipeline;
- несколько обычных graph steps последовательно используют одну stencil mask;
- fullscreen graphics consumer может использовать stencil без sampled descriptor;
- stencil writer совмещается с reversed-Z depth test, а consumer не обходит occlusion.

Reference/read/write masks сейчас **static pipeline state**. В material `dynamic` поддержан только
`depth_bias`; runtime controls потребуют добавить `StencilReference`, `StencilCompareMask` и
`StencilWriteMask`, а затем команды их установки. Первый срез намеренно не изображает runtime изменением
UBO: fixed-function stencil state от UBO не меняется.

## Следующие срезы

1. Локальный post-effect: отдельный stencil bit ограничивает fullscreen tint/desaturation.
2. Пространственный window proof: geometry aperture пишет mask, второй view рисуется только внутри неё;
   полноценные recursive portal и mirror clipping в первый proof не входят.
3. Dynamic reference/read/write masks и UI controls.
4. Намеренно разные front/back operations с debug fixture, где результат обеих сторон виден отдельно.

## Definition of Done

Outline, локальный post-effect и window mask используют обычные Painter materials/render graph; static и
dynamic front/back stencil state проверены явно, а debug view позволяет объяснить, почему конкретный pixel
прошёл или не прошёл test. Production parsing/execution fixes принадлежат `libs/painter`; демонстрационные
shaders и visualization остаются в лаборатории.
