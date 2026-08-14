# PF02 — shadows

Независимая лаборатория shadow rendering поверх минимального painter baseline, доказанного `PF01`.
Она не зависит от target или исходников `PF01`: нужный baseline копируется выборочно либо берётся из
уже общего API `libs/painter`/`../common`.

## Первый наблюдаемый результат

- одна directional shadow map;
- shadow atlas для spot lights;
- статические и движущиеся casters;
- runtime depth/slope bias controls;
- просмотр shadow atlas и cascade/atlas regions;
- caster/light culling и GPU timings.

Point-light cubemap shadows — отдельное расширение после стабильного первого среза.

## Куда смотреть

Пока executable отсутствует. При старте лаборатория получит локальные `CMakeLists.txt`, `src/`,
`resources/` и shaders; из `PF01` переносится только минимальный зафиксированный scene/lighting
baseline. Общая camera/debug shell берётся из `../common`, если к тому моменту она уже доказана.

## Definition of Done

Движущиеся light и caster дают стабильную объяснимую тень без грубого acne/peter-panning на тестовых
поверхностях; atlas, bias и стоимость доступны в debug UI.
