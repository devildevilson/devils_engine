# PF05 — scene effects

Независимая effect gallery между базовыми renderer capabilities и художественными project-сценами.
Она проверяет небольшие пространственные эффекты, которым тесно внутри post-processing или stencil lab,
но которые ещё не должны проектироваться сразу под конкретную игру.

## Планируемые срезы

- аналитический 3D SDF вдоль отрезка и кривой Безье; существующий MSDF path служит источником уже
  проверенных distance-field primitives, но не подменяет пространственный proof;
- screen-space decals с reconstruction из depth/normal и ограниченным decal volume;
- particles, emitter lifecycle и простая particle physics;
- rain и snow как два наблюдаемо разных consumer particle-системы;
- cel shading с управляемыми lighting bands и outline policy;
- spherical, cylindrical и screen-aligned billboards;
- маленькое world-space UI окно над объектом: имя, health bar и несколько полей состояния.

Дополнительные эффекты добавляются только отдельными закрываемыми срезами. Площадка не является одной
обязательной mega-scene и не должна связывать все техники в один pipeline.

## Граница

Каждый срез использует обычные painter resources/materials/render graph и владеет локальными shaders и
fixtures. Production parsing/execution fixes принадлежат `libs/painter`; effect semantics и debug views
остаются в лаборатории до второго реального consumer.

## Definition of Done

У каждого принятого эффекта есть запускаемый наблюдаемый сценарий, фиксированная camera/debug view,
объяснённая граница алгоритма и минимальная проверка, отличающая работающую технику от passthrough.
