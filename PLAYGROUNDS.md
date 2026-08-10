# devils_engine — каталог маленьких playground-проектов

Этот документ описывает намеренно маленькие проверочные проекты. Playground — не вертикальный срез
игры и не обещание контента: это живой consumer одного-двух движковых контрактов и одной характерной
проектной особенности.

Playground должен быстро отвечать на конкретный технический вопрос. Если он оказался полезен, его можно
расширять следующими независимыми slices; со временем некоторые стенды естественно превратятся в ранние
срезы игр. До этого момента они не обязаны иметь сюжет, баланс, production UI или законченный art style.

## Общие правила

### Что считается хорошим playground

- Запускается отдельным executable или отдельным явно выбранным scenario.
- Имеет один главный технический риск и не более двух вторичных.
- Использует обычные engine resources, manifests и lifecycle, а не специальный тестовый путь.
- Имеет маленький authored либо generated fixture, который легко понять глазами.
- Позволяет менять ключевые параметры в runtime и видеть промежуточные состояния.
- Поддерживает headless validation там, где результат не обязан оцениваться глазами.
- Имеет измеримые критерии завершения: frame time, determinism, memory, latency, artifact hash или
  корректный state transition.
- Может быть удалён без потери проектного контента: ценность остаётся в закреплённом engine contract,
  tests и reusable resources/tools.

### Чего в playground быть не должно

- попытки сразу собрать полный gameplay loop;
- production-контента и большого числа уникальных assets;
- временной архитектуры, обходящей обычный `simul`/`demiurg`/`painter` path;
- project semantics внутри engine только ради удобства одного стенда;
- нескольких крупных неизвестных одновременно;
- требования «выглядеть как готовая игра».

### Общая оболочка

Полезно дать всем graphical playgrounds одну небольшую оболочку:

- свободная и фиксированная camera;
- pause, single-step и time scale;
- выбор scenario без перекомпиляции;
- runtime parameter inspector;
- render-target/debug overlay;
- CPU/GPU timings и resource residency;
- capture screenshot + config/resource/build fingerprints;
- reset scenario в исходное состояние;
- deterministic seed;
- опциональный headless запуск с фиксированным числом ticks/frames.

Это не полноценный editor. Оболочка только делает маленькие experiments наблюдаемыми и повторяемыми.

## Рекомендуемый первый набор

| Приоритет | Playground | Главный вопрос | Первый project consumer |
| --- | --- | --- | --- |
| 1 | `painter_feature_lab` | можно ли подключать visual feature почти без C++ | общий renderer |
| 2 | `submarine_light_room` | работает ли свет как пространственное и игровое препятствие | `submarine_coop` |
| 3 | `party_movement_garden` | пригодна ли 3D action foundation для исследования generated world | `party_adventure` |
| 4 | `hierarchy_sim_lab` | объясним и ограничен ли многоуровневый AI | `medieval_hero_manager` |
| 5 | `tower_floor_lab` | достаточно ли grid/resolve/generator primitives для маленького этажа | `tower_crawler` |
| 6 | `generator_contract_lab` | можно ли собирать typed deterministic passes через Lua glue | несколько проектов |
| 7 | `commander_mission_lab` | способен ли planner решать миссию только по доступным observations и честно объяснять replan | `commander_simulator` |
| 8 | `swarm_field_lab` | масштабируются ли layered fields, assignment и flow без per-unit микроконтроля | `zerg_brain` |
| 9 | `apates_campaign_bridge_lab` | сохраняется ли один canonical character между campaign и encounter | `apates_quest` |

Эти стенды не следует начинать одновременно. `painter_feature_lab` создаёт общую visual infrastructure;
остальные становятся её потребителями по мере готовности нужных базовых систем.

## 1. `painter_feature_lab` — лаборатория визуальных features

### Вопрос

Можно ли добавить новую visual feature как render-graph fragment + materials + shaders + parameter
schema, не добавляя специализированный C++ pass?

### Минимальная сцена

- закрытая комната, наружная площадка и проход между ними;
- несколько простых meshes: плоскость, куб, сфера, лестница, тонкая геометрия;
- матовые, металлические, emissive, прозрачные и alpha-tested материалы;
- один статический и один движущийся объект;
- directional, point и spot light;
- свободная camera и повторяемая camera rail.

### Последовательные slices

1. HDR offscreen scene, depth и normal targets.
2. Generic fullscreen-triangle и compute-dispatch feature passes.
3. Parameter blocks и live inspector.
4. Просмотр любого промежуточного render target.
5. Post-processing compositor и typed feature inputs/outputs.
6. SSAO как первый spatial effect.
7. Motion vectors, jitter и temporal history lifecycle.
8. TAA как первый temporal effect.
9. Bloom, exposure, tone mapping и color grading.
10. Depth pyramid и общие reconstruction/denoise primitives.
11. Простой screen-space contact shadow или outline.
12. Минимальный volumetric fog consumer.

### Инструменты, которые должен доказать стенд

- shader/config hot reload;
- shader-interface reflection и descriptor validation;
- transient/history resource lifecycle;
- reset history при camera cut, resize и teleport;
- enable/disable с корректным passthrough;
- quality presets;
- GPU timing и memory cost каждого pass;
- capture промежуточных targets;
- loud errors для cycles, missing inputs, incompatible formats и двойного producer.

### Не входит

- production PBR material library;
- большая outdoor scene;
- полный editor;
- окончательный набор эффектов конкретной игры.

### Definition of Done первого среза

Новый grayscale/outline/blur effect добавляется только ресурсным feature manifest, material и shader.
C++ не меняется; граф валидируется, эффект можно выключить, его output можно просмотреть, а GPU time
виден в inspector.

## 2. `submarine_light_room` — тесная FPS-комната и давящее освещение

### Вопрос

Можно ли сделать освещение одновременно выразительным визуальным слоем и понятным препятствием, не
делая gameplay зависимым от чтения framebuffer?

### Минимальная сцена

- две тесные комнаты подлодки, коридор, дверь и маленький технический отсек;
- FPS controller;
- ручной фонарь;
- обычная, аварийная и повреждённая лампы;
- один подбираемый предмет;
- один интерактивный выключатель или breaker;
- пар/дым в одной зоне;
- движущийся либо качающийся локальный объект для проверки теней и physics interaction.

### Первый gameplay slice

- подобрать и бросить предмет;
- включить/выключить фонарь;
- отключить питание одной комнаты;
- открыть дверь и изменить распространение света/тумана;
- показать отдельный authoritative показатель освещённости комнаты или объекта.

### Visual slices

1. Per-pixel local lighting в HDR.
2. Spot/point shadows и contact shadows.
3. Ограниченная exposure policy: темнота не должна автоматически превращаться в серый день.
4. Emissive fixtures, flicker и аварийный красный свет.
5. SSAO и подчёркнутые тесные контакты поверхностей.
6. Локальный volumetric smoke/steam.
7. Wet material highlights и простые water/caustic accents.
8. Project color grading.

### Две модели освещения

Renderer строит богатую картинку. Gameplay отдельно хранит дешёвую приближённую модель:

- питание и состояние источников;
- light/visibility value комнаты или spatial cell;
- наличие грубой прямой видимости;
- дверь/стена/дым как modifiers;
- пороги обнаружения для actors.

Эти модели должны коррелировать, но gameplay никогда не определяет видимость чтением pixels.

### Не входит

- полноценная подлодка;
- flooding/pressure simulation;
- co-op networking;
- сложный enemy AI;
- production ragdoll.

### Definition of Done первого среза

Игрок может обесточить комнату, осветить её фонарём и подобрать предмет. Визуальное и gameplay-состояние
освещённости изменяются согласованно; debug view показывает оба, а renderer остаётся presentation-only.

## 3. `party_movement_garden` — третье лицо, terrain и объёмный туман

### Вопрос

Можно ли комфортно управлять персонажем на небольшой сложной 3D-локации и собирать атмосферу из
generated terrain, authored modules, animation, physics, navigation и volumetric rendering?

### Минимальная сцена

- небольшой valley/garden размером в несколько минут ходьбы не требуется: достаточно 30–60 секунд;
- склон, уступ, узкий проход, мост, небольшая пещера и одна открытая площадка;
- third-person controller и orbit camera;
- один skinned character с idle/walk/run/jump;
- один подбираемый объект и один physics object;
- простой companion/navigation agent;
- две fog volumes: низина и пещера.

### Последовательные slices

1. Character controller, slopes/steps и camera collision.
2. Animation blending, root-motion policy и foot placement probe.
3. Interaction ray/query и pickup.
4. Navmesh/path query через мост и пещеру.
5. Height fog.
6. Froxel volume injection и light scattering.
7. Temporal reprojection, depth-aware upsample и history diagnostics.
8. Один generated terrain patch из graph/height/volume artifact.
9. Вставка одного authored module в generated patch.
10. Запекание либо построение collision/nav artifacts из того же package.

### Не входит

- party combat;
- quest system;
- полноценная генерация мира;
- несколько классов персонажей;
- co-op.

### Definition of Done первого среза

Персонаж проходит склон, мост и пещеру без camera/physics discontinuities. Fog volumes корректно
пересекают геометрию и освещение, temporal history переживает обычное движение и сбрасывается при
teleport. Companion строит маршрут по той же локации.

## 4. `hierarchy_sim_lab` — симуляция иерархического AI

### Вопрос

Можно ли ограниченно, детерминированно и объяснимо выполнять решения на уровнях персонажа, группы,
организации и региона, не превращая `acumen` в политическую симуляцию?

### Представление

Стенд преимущественно headless. Его UI — не игровая карта, а несколько простых views:

- граф регионов и дорог;
- список actors и groups;
- дерево goals/operations/tasks;
- timeline обязательств и interruptions;
- ownership ресурсов;
- explanation выбранного решения;
- budgets, expanded nodes, cache hits и rejected candidates.

### Минимальная модель

- 4 региона и 5–7 дорог;
- 2 фракции;
- 3 группы по несколько logical actors;
- ограниченные ресурсы: еда, деньги и influence;
- несколько операций: travel, recruit, guard, trade, investigate;
- локальные события: нехватка еды, опасная дорога, выгодная возможность;
- разные cadences для actor/group/faction decisions.

### Последовательные slices

1. Goal candidate selection с объяснимым utility score.
2. Persistent operation instance с duration/commitment/interruption.
3. Разбиение group goal на actor tasks.
4. Reservation ограниченного ресурса или дороги.
5. Multi-cadence scheduler.
6. Aggregate actors, из которых несколько materialize в detailed state.
7. Save/resume посередине операции.
8. Serial-vs-MT и 1/2/4-worker identity.
9. Batch прогон сотен seeds со статистическим отчётом.

### Ownership

- engine: workflow cursor, scheduling, budgets, reservations primitives, traces and inspectors;
- project: politics, utility factors, operation schemas, faction relations and consequences.

### Не входит

- полноценная экономика;
- дипломатический UI;
- detailed combat;
- 3D presentation;
- тысячи типов событий.

### Definition of Done первого среза

Одинаковый seed даёт одинаковый outcome после save/resume и при разном числе workers. Для каждой
операции можно ответить: кто её начал, почему, какие ресурсы заняты, какой шаг выполняется и почему она
была продолжена, отменена или прервана.

## 5. `tower_floor_lab` — квадратный этаж и небольшой tactical loop

### Вопрос

Достаточно ли project-owned grid semantics поверх общих query/resolve/workflow primitives для
детерминированного маленького этажа?

### Минимальная карта

- квадратная grid-карта 12×12 либо 16×16;
- стены, двери, опасные клетки и один exit;
- player party из двух units;
- два типа противников;
- один интерактивный объект;
- простая отдельная map presentation.

### Игровые features

- grid movement и occupancy;
- path preview;
- line of sight и простое укрытие;
- initiative/reservation;
- melee attack и один area shape;
- один status либо environmental hazard;
- enemy scorer из двух-трёх решений;
- group exit condition.

### Generator slice

Генератор создаёт не игру, а один проверяемый floor artifact:

1. room/connection graph;
2. rasterization в grid;
3. placement start/exit/objective;
4. reachability validation;
5. простой semantic quest graph из 3–5 nodes;
6. bounded repair при недостижимом exit;
7. provenance view: какой pass создал клетку/дверь/objective.

### Не входит

- campaign;
- большой набор abilities;
- inventory/equipment;
- полноценный quest narrative;
- production procedural variety.

### Definition of Done первого среза

Один seed создаёт canonical floor bytes/hash. Этаж гарантированно проходим, actor movement и combat
возобновляются после save, а visual map и tactical state читают одну grid model без дублирования координат.

## 6. `generator_contract_lab` — typed passes и Lua glue

### Вопрос

Можно ли собирать разные генераторы из одного host/toolkit, сохраняя typed artifacts, determinism,
provenance, validation и bounded repair?

### Первый pipeline

Намеренно абстрактная 2D region map:

1. coarse continent mask;
2. height/noise field;
3. water and moisture propagation;
4. biome classification;
5. Voronoi-like regions;
6. adjacency graph;
7. validation и один repair pass.

### Второй pipeline

Небольшой adventure graph без production terrain:

1. hierarchy задач;
2. decomposition до конечных objectives;
3. region graph;
4. content-node graph;
5. примитивные volumes/boxes вместо финальных meshes;
6. connectivity validation.

### Что проверяется

- C++ tools регистрируются как reusable building blocks;
- отдельный headless Lua environment связывает passes;
- pass видит только объявленные typed inputs;
- artifacts получают seed/version/hash;
- provenance связывает output с pass/tool/input fingerprints;
- serial и MT execution дают одинаковый canonical result;
- cache hit не меняет результат;
- invalid artifact не публикуется;
- viewer показывает fields, graphs, filters, groups и validation issues.

### Definition of Done первого среза

Lua переставляет и параметризует проходы без изменения C++. Неверная связь типов отклоняется до
execution, одинаковый seed даёт одинаковый artifact hash, а выбранная клетка/область объясняет своё
происхождение через provenance chain.

## Дополнительные playgrounds

### 7. `network_session_lab`

Маленькая статическая комната с двумя peers и dedicated server. Проверяет ENet transport, HTTPS-like
rendezvous mock, key bootstrap, encrypted UDP envelope, NAT/relay adapter seam, intents, snapshots,
reconnect и diagnostics. Не содержит большой физики или MMO replication. Первый рост — co-op pickup;
следующий — один authoritative moving object.

### 8. `planet_streaming_lab`

Небольшая условная планета или кольцо cells без финальной графики. Camera/observer движется между cells;
система строит macro regions, загружает соседние artifacts, отменяет устаревшие jobs и проверяет epochs,
cache, residency и stable package hashes. Это ранний consumer `mmo_planet_shooter`, но не MMO server.

### 9. `city_actor_lab`

Один квартал-прокси из простых blocks, несколько actors, vertical links и одна group operation. Проверяет
сложную navigation, hierarchical actor/group intent, короткий replay и scene streaming для
`bandit_in_the_shell`. Позже можно добавить skinned character, ragdoll handoff и crowd pressure, но не
в одном первом slice.

### 10. `resource_churn_lab`

Автоматически загружает, публикует, заменяет и выгружает textures, meshes, sounds, shaders и generated
artifacts. Проверяет fence/task lifetime, stable handles, artifact epochs, bindless slot reuse,
dependency invalidation и memory budgets. Визуальная часть показывает текущие slots и intentionally
missing/corrupt resources.

### 11. `localized_text_lab`

Небольшая UI-галерея с несколькими scripts/locales. Проверяет fallback, plural/select/case forms, typed
parameters, safe tags, inline icons, pseudo-localization, RTL/bidi, shaping, font fallback, wrapping,
hit spans и locale-switch invalidation. Gameplay поставляет только loc-key и typed values.

### 12. `headless_run_lab`

Универсальная оболочка автономного scenario: seed/config, fixed tick budget, statistics, failure
artifacts и comparison между builds. Первым consumer может стать простой `cardgame` player policy без
новых карт. Позже сюда подключаются generation batch runs и dedicated-server soak tests.

### 13. `audio_environment_lab`

Три соединённые комнаты с разными материалами, дверями и acoustic zones. Проверяет
`(material, action/impact, context) -> sound event`, шаги, удары, reverb/filters, obstruction, portals,
priorities и virtual voices. Может использовать ту же proxy geometry, что `submarine_light_room`, но
остаётся отдельным аудио-сценарием.

### 14. `swarm_field_lab`

Плоская карта 128×128 с 1 000 proxy creatures, двумя food areas, одной составной operation
`approach corridor → terminal area` и одной pressure front. Проверяет versioned layered fields, brush
journal, dirty recompute, group demand/allocation, flow field, congestion, hysteresis и explanation
выбранного actor. Headless и visual runs обязаны давать один canonical hash; затем количество actors
увеличивается ступенями 1k → 10k → целевой budget. В первый slice не входят production animation,
физиология улья и полноценный бой.

### 15. `commander_mission_lab`

Граф 8–12 tactical areas, четыре абстрактных бойца, укреплённый прямой путь, обход, подготовленная
оборонительная позиция, одна неизвестная угроза, дополнительная цель и deadline эвакуации. Planner
получает только immutable squad-knowledge snapshot. Scenario проверяет mission/route plan records,
evidence-backed explanation, один interrupt, partial replan, reservation cleanup, недостижимый intent и
batch no-progress/cycle detector. Сначала это headless simulation + 2D graph/timeline inspector; 3D не нужен.

### 16. `apates_campaign_bridge_lab`

Фиксированный замкнутый граф 12–20 провинций, один persistent ruler/hero, несколько knowledge holders,
одна институциональная rule chain и одно encounter. Проверяет calendar workflow, rumor/contact/mapped-route
views, truthful rule preview и lifecycle `reserve → materialize → outcome journal → atomic reconcile`.
Первая encounter execution может быть headless/discrete: важно вернуть смерть/рану/время/предмет/
witness ровно один раз. Save/resume и different-worker runs не должны менять политический outcome.

### 17. `globe_topology_lab`

Отдельный от campaign bridge стенд замкнутой поверхности. Создаёт маленький immutable world package с
surface cells/provinces, adjacency и несколькими routes. Проверяет seam/pole-safe distance, полный обход,
projection switching, ray→surface→province picking, map-mode buffers, label anchors и terra-incognita,
которая не раскрывает hidden boundaries через hover/picking. После topology proof сюда добавляются
generator provenance и culture/history layers; distributed MMO coordinates не входят в первый scope.

## Как playground растёт в срез игры

Расширение должно идти не «добавим ещё контента», а последовательными доказанными рисками:

```text
isolated engine contract
        ↓
project-specific consumer
        ↓
second consumer / generalization review
        ↓
persistence + diagnostics + failure paths
        ↓
small repeatable gameplay loop
        ↓
early game slice
```

Перед каждым расширением стоит ответить:

1. Какой новый технический вопрос появляется?
2. Нельзя ли проверить его меньшим fixture?
3. Какая часть остаётся project-owned?
4. Что после proof действительно переносится в engine?
5. Как воспроизвести failure и сравнить результат между запусками?

Если следующий шаг отвечает только «нужно больше контента», playground уже выполнил движковую задачу.
Дальнейшее развитие должно происходить в проекте либо ждать возвращения интереса к его gameplay.

## Предлагаемая физическая организация

На ранней стадии лучше не создавать семнадцать полностью независимых приложений. Достаточно одного
общего laboratory host и нескольких project-owned executables там, где topology действительно различна:

```text
subprojects/
  playgrounds/
    visual_lab/
      scenarios/
        painter_features/
        submarine_light_room/
        party_movement_garden/
        audio_environment/
    simulation_lab/
      scenarios/
        hierarchy_sim/
        tower_floor/
        generator_contract/
        headless_run/
        commander_mission/
        swarm_field/
        apates_campaign_bridge/
    infrastructure_lab/
      scenarios/
        resource_churn/
        planet_streaming/
        globe_topology/
        network_session/
        localized_text/
```

Это ориентир, не обязательная архитектура. Отдельный executable оправдан, если различаются thread/process
topology, renderer presence, dedicated-server режим или набор platform dependencies. Общий mega-demo,
который обязан загрузить все системы сразу, также не нужен.
