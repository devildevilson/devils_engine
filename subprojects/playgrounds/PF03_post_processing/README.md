# PF03 — post processing

Расширяемая независимая галерея post-effects. Она специально не является обязательной зависимостью
остальных лабораторий: `PF05` может зафиксировать небольшой нужный ему набор, пока `PF03` продолжает
расти и экспериментировать.

## Первый наблюдаемый результат

- HDR input и корректный passthrough;
- exposure и tone mapping;
- bloom;
- color grading;
- один spatial effect: SSAO (первый non-temporal contact-shadow proof уже принадлежит `PF02`);
- resource-driven chain с typed inputs/outputs;
- включение эффектов по одному и просмотр каждого промежуточного target;
- GPU time и memory cost каждого pass.

Motion vectors/history/TAA, depth pyramid, denoise и volumetrics добавляются последующими именованными
slices, а не входят скрыто в первый DoD. Stochastic soft-shadow visibility + temporal accumulation также
начинается только здесь. PF02 оставляет contact masks как opt-in spatial proof: single-depth silhouettes
не позволяют надёжно восстановить скрытую сторону blocker, поэтому HZB/history reconstruction исследуется здесь.

## Куда смотреть

Пока executable отсутствует. Будущие `resources/` и shaders лаборатории являются главным местом
расширения effect chain; специализированный host-код остаётся в её `src/`. Только повторённая shell и
доказанные generic graph contracts могут переехать в `../common` и `libs/painter`.

## Definition of Done

Цепочка валидирует missing/incompatible inputs, каждый эффект имеет passthrough, итог и промежуточные
targets видны в runtime, а добавление одного простого эффекта не требует специализированного C++ pass.
