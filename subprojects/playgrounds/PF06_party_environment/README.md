# PF06 — party environment

Визуальная лаборатория динамического окружения для `party_adventure`. Она отделена от будущего
character-controller/animation/navigation playground, чтобы rendering не зависел сразу от всего 3D
gameplay stack.

## Первый наблюдаемый результат

- небольшой garden/valley из authored proxy geometry;
- движущееся солнце и стабильные directional shadows;
- управляемая смена времени суток;
- fog/weather parameters;
- environment color grading;
- несколько динамических элементов окружения;
- фиксированные camera points для сравнения состояний.

Generated terrain, character controller, skeletal animation, physics и navigation сюда не входят.

## Куда смотреть

Пока executable отсутствует. Локальные `resources/` будут владеть authored proxy scene, lighting,
weather и grading presets; `src/` — timeline/parameter/debug glue. Character movement должен получить
отдельную будущую laboratory directory, а не расти внутри этой сцены.

## Definition of Done

Одна сцена убедительно проходит несколько зафиксированных lighting/weather states без ручной
пересборки graph, а camera presets и debug targets позволяют сравнить тени, fog и grading.
