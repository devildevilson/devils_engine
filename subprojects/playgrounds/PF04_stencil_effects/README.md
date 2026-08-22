# PF04 — stencil effects

Небольшая effect gallery для полного stencil path: depth/stencil attachment, material state, write/read
masks, reference values и использование маски последующими graphics passes.

## Запуск

```bash
cmake --build build-debug --target PF04_stencil_effects -j2
./build-debug/subprojects/playgrounds/PF04_stencil_effects/bin/PF04_stencil_effects
```

Опции: `--validation`, `--uncapped`, `--fixed-camera`, `--stencil-debug`, `--no-local-effect`,
`--no-window`, `--no-face-fixture`, `--frames=N`, `--dump=file.ppm`. Управление: WASD/QE, мышь, Shift; `P` включает spatial
window, `L` — локальный cyan tint, `V` — magenta-визуализацию selection bit. `R` переносит selection
между bits `0x01/0x08/0x80`, `C` включает read/compare mask, `X` — write mask, `F` — front/back fixture.
Escape закрывает окно.
Для детерминированных прогонов есть `--selection-channel=0..2`, `--selection-compare=0|1` и
`--selection-write=0|1`.

При `--fixed-camera` GLFW явно переключается в `CURSOR_NORMAL`: мышь освобождена, потому что camera look
не используется. `--dump` переносит прямо из `scene_color` в PPM после точного кадра `--frames`; если номер
не указан, пишется первый кадр. Это тот же frame-exact подход, что в PF03, без случайного тайминга desktop
screenshot.

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

## Spatial window

Третий срез использует ещё один независимый bit и повторно рисует сцену с другой камерой:

```text
world-space aperture → depth test main view → write stencil bit 0x04, no color/depth write
fullscreen clear → stencil == 0x04 → dark-blue background + reverse-Z depth = 0
scene geometry + alternate camera → stencil == 0x04 → ordinary reverse-Z depth/write
```

Локальный depth clear принципиален. Глубина основной и alternate camera лежит в разных пространствах, и
сравнивать вторую сцену с первой нельзя. Fullscreen fragment очищает общий depth attachment только внутри
aperture, после чего окно получает собственный корректный depth ordering, не требуя второго depth image.
Пиксели с selection/local bits сохраняют их: aperture меняет только `0x04`.

Это spatial window, а не полноценный recursive portal: alternate camera задана явно, oblique near-plane
clipping, преобразование камеры через пару порталов и рекурсия пока не входят в proof. Проверено A/B через
`--dump`: с window виден холодный второй ракурс и его checker floor, с `--no-window` aperture не оставляет
следов. Четырёхкадровый dump с Vulkan validation проходит чисто.

## Что уже доказано в Painter

- combined depth/stencil resource создаётся, очищается и сохраняет оба aspect внутри pass;
- config уже проводит static front/back `fail/pass/depth_fail`, compare, read/write masks и reference в
  `VkStencilOpState` pipeline;
- несколько обычных graph steps последовательно используют одну stencil mask;
- fullscreen graphics consumer может использовать stencil без sampled descriptor;
- stencil writer совмещается с reversed-Z depth test, а consumer не обходит occlusion.
- разные эффекты могут безопасно делить байт через read/write masks и сохранять чужие bits;
- step-level `mask = none` действительно создаёт depth/stencil-only draw внутри color render pass.
- часть общего depth attachment можно очистить stencil-ограниченным draw и использовать для второго view.
- front и back faces одного draw могут применять разные `VkStencilOpState` и давать независимо наблюдаемый результат.

В Painter по пути исправлен разбор color write masks: непустая маска теперь заменяет RGBA, `none` даёт
ноль, а отсутствующие blend expressions сохраняют валидные Vulkan defaults вместо sentinel `UINT32_MAX`.
Регресс вместе с dynamic stencil parsing покрыт `painter_shader_prepare_test` (`14` cases,
`157` assertions).

## Dynamic reference и masks

Material теперь может объявить:

```text
dynamic = [ stencil_reference, stencil_compare_mask, stencil_write_mask ]
```

А graphics step ссылается на runtime-константу `stencil_state = selection_stencil_state` формата
`{uint reference, uint compare_mask, uint write_mask}`. Pipeline включает стандартные Vulkan dynamic states,
а при записи command buffer Painter вызывает `vkCmdSetStencilReference`, `vkCmdSetStencilCompareMask` и
`vkCmdSetStencilWriteMask` для обеих граней. Writer, outline и debug используют одну константу, поэтому
смена selection channel согласованно обновляет всех потребителей на следующем update event без пересборки
pipeline или render graph.

Внутри `material` больше нет отдельных `dynamic_depth_bias`,
`dynamic_stencil_reference` и других bool-полей. Все states хранятся в
`std::array<uint32_t, 16> dynamic`, свободные слоты равны `UINT32_MAX`. Parser по-прежнему
принимает человеческие имена и кладёт в массив numeric values соответствующих Vulkan enums.
Единое соответствие имён и Vulkan values задаёт
`DEVILS_ENGINE_PAINTER_DYNAMIC_STATE_LIST` в `common.h`. Из него X-macro строит плотный
`dynamic_state::values`, string conversion и `to_vulkan()`; generated `static_assert` остаётся в
`common.cpp`. Новый core state теперь добавляется одной строкой в этот список и не требует
ещё одного поля в публичной структуре; лимит 16 и duplicate states проверяются громко.

Проверено frame-exact dumps: перенос `0x01 → 0x08` даёт побитово тот же участок сцены (`AE = 0` после
исключения меняющегося текста overlay); write mask `0` убирает selection debug; compare mask `0` делает
masked operands равными нулю, поэтому `equal` tint проходит везде, а `not_equal` outline — нигде. Это
наблюдаемые последствия fixed-function формул, а не UBO-имитация в shader.

## Asymmetric front/back fixture

Один draw подаёт два screen-space quad с противоположным winding и `cull = none`:

```text
left front faces → replace, write/read mask 0x30, reference 0x10 → green consumer
right back faces → invert,  write/read mask 0x30                 → yellow consumer sees 0x30
```

Оба consumer'а — fullscreen stencil tests, поэтому цвета показывают именно fixed-function
результ, а не ветку в shader. Mask `0x30` не затрагивает bits `0x01/0x02/0x04`, а selection может
отдельно переехать на `0x80`. `F` и `--no-face-fixture` отключают writer: A/B dump убирает оба
прямоугольника без следа в остальной сцене. Финальный четырёхкадровый dump с Vulkan validation
проходит чисто.

## Definition of Done

Definition of Done достигнут: outline, локальный post-effect и window mask используют обычные Painter
materials/render graph; static, dynamic и асимметричный front/back stencil state проверены явно, а debug
views позволяют объяснить, почему конкретный pixel прошёл или не прошёл test. Production
parsing/execution fixes принадлежат `libs/painter`; демонстрационные shaders и visualization остаются в лаборатории.
