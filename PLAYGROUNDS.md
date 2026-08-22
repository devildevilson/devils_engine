# devils_engine — каталог маленьких playground-проектов

Этот документ описывает ограниченные проверочные проекты. Playground — основная единица текущего
планирования: законченный срез, который можно запустить и оценить глазами/на слух либо который доказывает
крупную симуляционную систему. Нумерованные задачи движка остаются каталогом зависимостей и не задают
порядок работы сами по себе.

Playground отвечает на конкретный технический вопрос и заканчивается наблюдаемым результатом. Если он
оказался полезен, его можно расширять следующими именованными slices; некоторые стенды естественно
превратятся в ранние срезы игр. До этого момента они не обязаны иметь сюжет, баланс, production UI или
законченный art style.

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
- Имеет свой `README.md`: назначение, запуск, точки входа, debug views, граница и Definition of Done.
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
- небольшой Visage overlay с названием/описанием сцены, controls и FPS/frame time;
- render-target/debug overlay;
- CPU/GPU timings и resource residency;
- capture screenshot + config/resource/build fingerprints;
- reset scenario в исходное состояние;
- deterministic seed;
- опциональный headless запуск с фиксированным числом ticks/frames.

Frame pacing этой оболочки не выводится из present mode: MAILBOX/FIFO/IMMEDIATE выбирают tearing и
поведение presentation queue, а отдельный deadline + sleep limiter задаёт частоту producer loop. На обычном
desktop предпочтителен MAILBOX-first; fallback между FIFO и IMMEDIATE выбирается явно по политике платформы.

Первый рабочий common slice уже используется PF01: `frame_pacer` держит абсолютный deadline и отбрасывает
пропущенное расписание без catch-up burst, а non-interactive Visage-overlay загружает общий Lua entry/MSDF
font и показывает описание сцены, controls, сглаженные FPS/frame time. GPU upload/font descriptor и
`draw_ui` остаются обычными ресурсами render graph конкретной лаборатории.

Это не полноценный editor. Оболочка только делает маленькие experiments наблюдаемыми и повторяемыми.

### Режим работы

- В каждый момент активна одна campaign и один её лабораторный срез.
- Мелкая задача из `ROADMAP.md` берётся только как обнаруженный blocker текущего результата.
- Числовой/focused test закрепляет контракт, но не выбирает направление работы.
- Независимая идея, не мешающая текущему срезу, остаётся в backlog/parking lot.
- Лаборатория закрывается запускаемым сценарием, debug-представлением, минимальным regression contract
  и обновлённой документацией.
- Все площадки физически живут под `subprojects/playgrounds/<CODE>_<human_name>/`; уже доказанная общая
  оболочка постепенно собирается в `subprojects/playgrounds/common/`.

## Текущий фокус — Painter visual stack

Активная campaign: семь независимых painter-лабораторий `PF01`–`PF07`. `PF01_forward_plus`, `PF02_shadows`,
[`PF03_post_processing`](subprojects/playgrounds/PF03_post_processing/README.md) и
[`PF04_stencil_effects`](subprojects/playgrounds/PF04_stencil_effects/README.md) закрыты; следующий bounded result —
[`PF05_scene_effects`](subprojects/playgrounds/PF05_scene_effects/README.md). PF04 закрыт основными срезами: `D24S8`
selection/outline/debug; независимые bits `0x02/0x04` для local tint и spatial window; runtime
`StencilReference/CompareMask/WriteMask`; asymmetric front/back fixture; а после закрытия — отдельная coverage+depth selection mask,
трёхпиксельный depth-aware screen-space outline, optional through-wall selector и `depth_fail_op`-силуэт скрытой цели на bit `0x40` с явным порядком относительно portal. Внутреннее
представление dynamic states свёрнуто из растущего набора bool-полей в один массив Vulkan values;
их человеческие имена и mapping задаёт один X-macro list в `painter/common.h`.

`PF03` закрыт 2026-08-21 полной запускаемой post-цепочкой, numeric/debug контрактами и shader-аудитом.
Финальная незакрытая техника, TAAU, теперь действительно реконструирует: при масштабе 0.5 ошибка против
native TAA составляет `2.98/255`, у простого upscale — `7.40/255`; непрерывный coverage дополнительно убрал
заметное переключение кромок при движении камеры. Аудит, исправленные контракты и
сознательно оставленные production-границы записаны в
[`SHADER_AUDIT.md`](subprojects/playgrounds/PF03_post_processing/SHADER_AUDIT.md). Проект build-вариантов уже
выбранного render graph, declared-value references и частичных material/step patches вынесен в
[`RENDER_PROFILES.md`](subprojects/playgrounds/PF03_post_processing/RENDER_PROFILES.md): основной state —
независимые graph-specific settings, общий preset лишь массово их записывает, а concrete variant выводится
resolver'ом. Реализация остаётся отдельным backlog, а не блокером следующей площадки.

Лаборатории не образуют CMake/source dependency chain. Более поздняя площадка может выборочно взять
зафиксированный baseline ранней либо общий код из `common`/`libs/painter`, после чего развивается
независимо. Поэтому расширение ранней gallery не меняет автоматически `PF06`.

## Painter campaign: семь лабораторий

| Порядок | Директория | Наблюдаемый результат | Первый consumer |
| --- | --- | --- | --- |
| 1 | `PF01_forward_plus` | много движущихся lights, cluster heatmap и Forward+/simple comparison | общий renderer |
| 2 | `PF02_shadows` | directional/spot shadow maps, atlas и bias diagnostics | общий renderer |
| 3 | `PF03_post_processing` | независимая расширяемая post-effect gallery | общий renderer |
| 4 | `PF04_stencil_effects` | outline, local mask и portal/mirror/window proof | общий renderer |
| 5 | `PF05_scene_effects` | 3D SDF, decals, particles/weather, cel shading, billboards и world-space UI | общий renderer |
| 6 | `PF06_submarine_light_room` | густая тёмная сцена со светом как препятствием | `submarine_coop` |
| 7 | `PF07_party_environment` | динамический свет, погода и окружение | `party_adventure` |

`PF01`–`PF05` доказывают отдельные painter capabilities. `PF06` и `PF07` фиксируют только нужное им
подмножество этих возможностей в собственных resources/presets. Исходники и CMake targets лабораторий
не зависят друг от друга.

Текущие ограниченные результаты PF05 — запускаемые Crimson MSDF и screen-space decal slices: один atlas обслуживает UI,
индивидуальные world glyph matrices на прямой/quadratic Bézier и три billboard space — spherical world-size,
cylindrical/Y-locked и constant-pixel world-anchored screen-size. Fixed font height обрезает хвост по длине,
при одной длине размер выводится из метрик, а optional detail texture стилизует fill без изменения MSDF coverage.
World/billboard glyph coverage теперь пишет depth. Настоящий decal pass растеризует ориентированные box-volume,
восстанавливает world position из opaque depth, ограничивает её через `world_to_decal`, фильтрует receiver по
scene normal и проецирует MSDF на дальнюю и боковую стены. `F`/`--no-decals` дают наблюдаемый A/B.
Лаборатории по-прежнему не зависят друг от друга; общие production fixes принадлежат `libs/painter`.

## 1. Painter visual stack

Главный вопрос campaign: можно ли последовательно доказать lighting, shadows, post-processing и stencil
на маленьких наблюдаемых сценах, а затем собрать из выбранного подмножества два разных project looks?

Общая dataflow-граница:

```text
scene instances + lights
  -> depth prepass
  -> Forward+ cluster/light assignment
  -> forward HDR rendering <- shadow maps
  -> selected post chain
  -> swapchain/debug target viewer
```

Подробная граница, точки входа и Definition of Done принадлежат README каждой директории:

- [`PF01_forward_plus`](subprojects/playgrounds/PF01_forward_plus/README.md);
- [`PF02_shadows`](subprojects/playgrounds/PF02_shadows/README.md);
- [`PF03_post_processing`](subprojects/playgrounds/PF03_post_processing/README.md);
- [`PF04_stencil_effects`](subprojects/playgrounds/PF04_stencil_effects/README.md);
- [`PF05_scene_effects`](subprojects/playgrounds/PF05_scene_effects/README.md);
- [`PF06_submarine_light_room`](subprojects/playgrounds/PF06_submarine_light_room/README.md);
- [`PF07_party_environment`](subprojects/playgrounds/PF07_party_environment/README.md).

Минимальная общая shell должна появляться из потребностей `PF01`, а не проектироваться целиком заранее.
Повторённый стабильный код camera/debug/capture переезжает в
[`common`](subprojects/playgrounds/common/README.md); renderer contracts — в `libs/painter` после
доказательства, а feature resources остаются у лабораторий.

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
priorities и virtual voices. Может использовать ту же proxy geometry, что `PF06_submarine_light_room`, но
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

## Физическая организация

Площадки каталогизируются по коду и человеческому имени. У каждой есть собственная директория и README;
наличие отдельного executable определяется её topology и независимостью эксперимента:

```text
subprojects/
  playgrounds/
    common/
    AU01_spatial_audio/
    AU02_directional_coloration/
    PF01_forward_plus/
    PF02_shadows/
    PF03_post_processing/
    PF04_stencil_effects/
    PF05_scene_effects/
    PF06_submarine_light_room/
    PF07_party_environment/
    ...
```

`common` — не mega-demo и не источник feature inheritance. Он содержит только маленькую shared shell.
Отдельный executable оправдан независимым экспериментом, отличающейся thread/process topology,
renderer presence, dedicated-server режимом или набором platform dependencies.
