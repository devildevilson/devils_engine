# PF04 — stencil effects

Небольшая effect gallery для полного stencil path: attachment, material state, write/read masks,
reference values и использование маски последующими passes.

## Первый наблюдаемый результат

- outline выбранного объекта;
- локальный post-effect через stencil mask;
- один пространственно понятный пример: portal, mirror или window mask;
- visualization stencil buffer;
- runtime read/write mask и reference controls.

## Куда смотреть

Пока executable отсутствует. Локальные materials/render resources будут показывать stencil state и
consumer passes; код debug visualization останется в lab, пока не появится второй обычный consumer.
Production parsing/execution fixes принадлежат `libs/painter`.

## Definition of Done

Все три сценария используют обычные painter materials/render graph, front/back stencil operations
проверяются явно, а debug view позволяет объяснить, почему конкретный pixel прошёл или не прошёл тест.
