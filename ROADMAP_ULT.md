# devils_engine — ультимативный project-driven roadmap

Срез требований десяти проектов, обновлённый 2026-08-14:

- `apates_quest`;
- `bandit_in_the_shell`;
- `cardgame`;
- `commander_simulator`;
- `medieval_hero_manager`;
- `mmo_planet_shooter`;
- `party_adventure`;
- `submarine_coop`;
- `tower_crawler`;
- `zerg_brain`.

Документ сводит движковые требования проектов, уточняет границы уже существующих библиотек и предлагает порядок развития. Это не перечень контента и не обещание превратить движок в набор готовых жанровых решений.

## Главный вывод

У движка уже сильная основа для data-driven и headless simulation:

- ECS, versioned entity ids, запросы и snapshot;
- ресурсы, модули, staged loading и stable resource handles;
- prefab;
- gameplay-функции, devils_script, FSM и локальный GOAP;
- deterministic deferred record/seal/commit;
- resumable turn cursor и presentation barriers;
- pointer-free interaction provenance и bounded resolution;
- app runtime, broker, render/assets/sound workers;
- Vulkan render graph, 2D/UV animation, Lua/Nuklear UI;
- headless/animated и worker-count identity tests у живых consumers.

Повторяющиеся пробелы концентрируются в двенадцати больших направлениях:

1. долговечное состояние: save envelope, migrations, transactions, replay diagnostics;
2. дискретная симуляция: calendar queue, persistent workflows, multi-cadence host;
3. мир: logical identity, simulation LOD, materialization, residency и procedural artifacts;
4. полноценная 3D action-база: scene transforms, physics, character, navigation, skeletal animation и high-level rendering;
5. инструменты: authoring, debug overlays, inspectors, batch/fuzz/soak harness;
6. сеть: transport, replication, prediction, authority handoff и distributed persistence.
7. локализация и текст: locale tables, language forms, tags, shaping/layout and compiled UI text;
8. стандартные asset formats: KTX/KTX2, 3D meshes/scenes/animations and canonical import/runtime interfaces.
9. генераторы: typed pass contracts, reusable C++ primitives, deterministic Lua orchestration, provenance and validation/repair;
10. знания и причинность: observations, immutable filtered views, evidence-backed explanations and retention;
11. пространственные поля: versioned layered fields, brushes, dirty updates, mass flow and heatmap tooling;
12. планетарная поверхность: projection-independent topology, coordinates, picking and globe/map presentation.

Первые три направления в значительной степени собираются из уже существующих библиотек. 3D требует нескольких новых крупных kernels. Сетевой стек нельзя честно собрать из broker и ECS snapshot: это отдельная программа развития.

## Текущий режим развития

Большой список ниже задаёт horizon и зависимости, но не является очередью снизу вверх. Работа движется
по одной playground/project campaign с конечным наблюдаемым результатом либо по одной крупной
симуляционной системе. Focused tests закрепляют найденные контракты, но не выбирают следующую тему.

Текущая campaign — Painter visual stack под `subprojects/playgrounds/`:

1. `PF01_forward_plus` — активная laboratory shell и Forward+ proof;
2. `PF02_shadows` — directional/spot shadow maps;
3. `PF03_post_processing` — независимая расширяемая post gallery;
4. `PF04_stencil_effects` — stencil/masked effect gallery;
5. `PF05_submarine_light_room` — тёмный project look для `SC`;
6. `PF06_party_environment` — динамическое окружение для `PA`.

Каждая лаборатория независима на уровне executable/resources/source. Поздняя lab выборочно фиксирует
нужные результаты ранней, а общий стабильный код переезжает в `playgrounds/common` или owner-library
только после реального повторения. Точные границы находятся в `PLAYGROUNDS.md` и локальных README.

## Принципы

### Механизм в engine, семантика в project

В engine принадлежат:

- ownership/lifetime contracts;
- stable ids и version checks;
- scheduling/ordering/budgets;
- serialization envelopes и migration registry;
- resource/artifact loading;
- standard resource-format adapters and canonical runtime intermediates;
- generator pass host, reusable native primitives, deterministic Lua glue and provenance/validation shell;
- neutral knowledge/provenance records, filtered-view boundaries and inspection shell;
- versioned layered-field storage/common algorithms and closed-surface topology primitives;
- generic queries;
- localization tables/forms/tag compiler and compiled-text UI path;
- headless execution;
- diagnostics and tools;
- transport/replication primitives, если сеть действительно начата.

В project принадлежат:

- конкретные компоненты и authoritative records;
- damage/death/status rules;
- карты, фракции, экономика, логистика и события;
- utility factors и AI scoring;
- generation grammar;
- generator pass composition, semantic schemas, constraints and repair policy;
- translated string content, project terminology and semantic tag handlers;
- import profiles and semantic material conventions;
- UI read models конкретной игры;
- materialization codecs и conservation invariants конкретного типа.

### Project-first перед преждевременной генерализацией

Если механизм пока нужен одному проекту, он сначала живёт рядом с ним. В engine переносится:

- после второго consumer;
- либо когда граница уже очевидна и не содержит жанровой семантики;
- либо когда без общего owner невозможно обеспечить lifetime, thread safety или persistence.

### Не расширять готовую библиотеку до соседней ответственности

- `catalogue` не становится replay/netcode/serializer;
- `resolve` не становится card/tactics/combat framework;
- `mood` не становится persistent workflow owner;
- `acumen` не выбирает жизненные цели и не становится strategic planner;
- `demiurg` не становится world database;
- `aesthetics::sink` не становится replication protocol;
- `simul::broker` не становится network transport.

## Легенда

### Статус

| Метка | Значение |
| --- | --- |
| `READY` | контракт существует и проверен живым consumer/test |
| `PARTIAL` | пригодны primitives, но целевой owner/lifecycle отсутствует |
| `MISSING` | отдельной переиспользуемой реализации нет |
| `PROJECT-FIRST` | первый implementation должен жить в проекте |
| `PROJECT` | семантика должна остаться проектной |

### Сложность

Оценка относится к одному опытному разработчику, включает design, implementation, focused tests и минимальную диагностику, но не production-content.

| Класс | Ориентир | Смысл |
| --- | --- | --- |
| `S` | 2–5 рабочих дней | локальный контракт или небольшой adapter |
| `M` | 1–3 недели | одна ограниченная подсистема с тестами |
| `L` | 1–2 месяца | новый owner/kernel и первый consumer |
| `XL` | 3–6 месяцев | несколько связанных runtime/tooling частей |
| `XXL` | 6–18+ месяцев | самостоятельная программа, эксплуатация или research |

Оценки не складываются буквально: инфраструктурные зависимости и интеграция способны увеличить срок.

### Сокращения проектов

| Код | Проект |
| --- | --- |
| `APQ` | `apates_quest` |
| `BITS` | `bandit_in_the_shell` |
| `CG` | `cardgame` |
| `CMD` | `commander_simulator` |
| `MHM` | `medieval_hero_manager` |
| `MMO` | `mmo_planet_shooter` |
| `PA` | `party_adventure` |
| `SC` | `submarine_coop` |
| `TC` | `tower_crawler` |
| `ZB` | `zerg_brain` |

## Профили глобальных фич проектов

Это не оценка объёма контента. Профиль показывает максимальный технический уровень, который должен поддержать runtime, и помогает не навязывать всем проектам самый дорогой вариант подсистемы.

| Проект | Графика/сцены | Skeletal animation | Физика | Навигация | Gameplay networking | Generator | AI/simulation |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `APQ` | strategic globe + отдельная короткая small-party TBT scene | средняя/сложная для героев и чудовищ | не нужна глобальному ядру; local format определяется TBT proof | сложный province/route graph + будущая compact tactical navigation | поздний authoritative-host co-op/PvP proof; не первый prototype | очень сложный spherical planet/culture/history/special-place generator | суточный auto-WEGO, multi-cadence politics/knowledge, persistent operations and materialization |
| `BITS` | полная сложная 3D city scene | сложная, включая ragdoll integration | сложная | сложная actor navigation | отсутствует | отсутствует либо минимален | сложный иерархический AI, factions/politics |
| `CG` | простая 2D с глубиной, несколько сцен | почти отсутствует | отсутствует | отсутствует | отсутствует | отсутствует | runtime AI отсутствует; GOAP только для headless run statistics |
| `CMD` | semantic tactical map, позднее optional 3D squad view | небольшой набор при 3D presentation | упрощённая либо отсутствует в первом proof | сложная semantic route/cover/threat navigation | не требуется | отсутствует | сложный inspectable hierarchical squad planner и knowledge boundaries |
| `MHM` | 2.5D либо очень простое 3D, плоский стилизованный landscape | небольшой стандартный набор | отсутствует | сложная, прежде всего graph/strategic | отсутствует | отсутствует | наиболее сложный hierarchical AI, factions/politics |
| `MMO` | возможно 2.5D, простой landscape | небольшой набор | простая | средняя | очень сложный MMO stack | сложный planet generator | strategic/local AI остаётся project system |
| `PA` | полная 3D, сложный landscape/scenes | сложная | сложная | сложная | простой по продуктовой модели co-op | сложный generator небольших миров | простой event-driven AI |
| `SC` | простая 3D, средний cave/vessel landscape | сложный skeletal runtime, но небольшой и простой набор clips | сложная | достаточно простая | co-op | средний, возможно streaming generator | средний |
| `TC` | простая 2.5D + отдельная map presentation | небольшой набор | отсутствует | простая grid navigation | отсутствует | небольшие floors, возможен относительно крупный quest graph | простой tactical AI |
| `ZB` | простая 2.5D/3D mass-RTS scene | animation LOD важнее сложности отдельных clips | минимальная | сложные flow fields/crowd movement | не требуется | authored maps либо лёгкий scenario generation | массовые layered fields, group allocation, economy and pressure fronts |

### `bandit_in_the_shell`

- Renderer является максимальным 3D consumer: плотные сцены, сложные материалы/освещение, skinned actors, crowds, LOD/HLOD и многоуровневые interiors.
- Animation stack должен поддержать сложные state graphs, blending, IK/root motion and physics handoff to ragdoll.
- Physics требуется не только для queries/controller, но и для динамических тел, constraints, vehicles/props and ragdoll.
- Navigation сложная: interiors, streets, vertical links, dynamic blockers, groups and crowds.
- Полноценного gameplay networking нет. Допустим небольшой сетевой platform layer для telemetry/statistics and explicit screenshot sharing.
- Screenshot sharing — provider adapter к внешнему HTTPS API либо platform share flow. Он требует user consent, OAuth/token lifecycle, privacy/rate-limit/error policy и не должен тянуть replication/session code.
- Короткие replays — input/intents + exact game-time deltas + checkpoint/fingerprints; presentation может быть восстановлена или записана отдельным коротким track.
- World generator не является ожидаемой опорой проекта: город преимущественно authored. Streaming/editor/validation важнее generator framework.
- Hierarchical AI объединяет local actors, groups, factions and simplified politics, но остаётся меньше глобальной модели `MHM`.

### `cardgame`

- Нужны несколько простых 2D scenes с depth ordering, camera/layout transitions and lightweight particles/UI presentation.
- Skeletal animation, physics, navigation and networking отсутствуют.
- Runtime enemy behavior задаётся rules/intent sequences, не general AI.
- GOAP используется только в headless autonomous run simulation для статистики и проверки build/run policies; он не является обязательной частью player-facing combat runtime.
- Главные общие зависимости: localization/UI, run/profile persistence, headless statistics and resource schemas.

### `medieval_hero_manager`

- Presentation допускает 2.5D либо очень простое 3D; terrain может быть плоским, а подъёмы/горы — стилизованными слоями и условными переходами.
- Нужен небольшой reusable набор стандартных clips, без физики и сложного action controller.
- Сложность navigation находится в иерархии маршрутов, областей, дорог, доступности и travel planning, а не обязательно в детальном 3D navmesh.
- Главный технический consumer calendar/multi-cadence/materialization and hierarchical AI.
- Factions/politics — наиболее глубокая project simulation среди перечисленных проектов.
- Gameplay networking и world generator не требуются.

### `mmo_planet_shooter`

- Presentation может ограничиться 2.5D, простым landscape и небольшим набором clips.
- Physics простая, navigation средняя; это уменьшает local action stack, но не сложность distributed state.
- Networking — максимальный consumer: dedicated servers, discovery/connectivity, encrypted UDP, replication, prediction, interest, reconnect, authority handoff and operations.
- Planet generator сложный на macro/topology/logistics/streaming уровне, хотя финальный landscape может оставаться визуально простым.
- Generator correctness, stable package and server/client artifact hashes важнее сложной локальной геометрии.

### `party_adventure`

- Полная 3D presentation, сложные animation and physics interactions.
- Navigation and terrain generation сложные: world contract/tasks/graphs должны воплощаться в проходимую geometry with collision/nav and authored modules.
- Co-op прост по продуктовой цели, но implementation всё равно требует authoritative interactions, physics reconciliation, join/reconnect and shared world persistence.
- World generator — главный 3D reference consumer общего generator contract.
- AI простой и в основном запускается игровыми событиями/локальными состояниями, без тяжёлого hierarchical planner.

### `submarine_coop`

- Graphics простая 3D; визуальная сложность ниже физической и сетевой.
- Skeletal infrastructure остаётся сложной из-за body/equipment/interactions/network pose, хотя число и художественная сложность clips невелики.
- Physics максимальной сложности: moving vessel frame, characters/items, impulses, breaches, compartments and environment solvers.
- Co-op networking должен синхронизировать этот state, поэтому его integration risk выше, чем предполагает небольшое число игроков.
- Generator средней сложности: caves/route segments могут строиться streaming-подобно `MMO`, но macro package меньше.
- Navigation сравнительно простая; AI средний.

### `tower_crawler`

- Простая 2.5D scene and separate floor/map presentation.
- Небольшой animation set; physics and networking отсутствуют.
- Navigation — deterministic integer grid/path/LOS, не 3D navmesh.
- AI простой tactical command scorer.
- Generator создаёт небольшие floors, но semantic quest/event graph одного этажа может быть относительно крупным; geometry generation проще semantic validation.

### `zerg_brain`

- Верхняя планка проекта находится не в сложности одного actor, а в количестве actors и пространственных слоёв: priority, food, threat, pressure, visibility, territory and flow.
- Массовое movement требует flow fields, congestion/local avoidance, movement classes и точных fallback-paths для специальных существ.
- Strategic operation задаёт terminal area + optional approach corridor; project самостоятельно переводит её в demand и group assignments.
- `acumen` не должен запускаться на каждом существе: нужен project-owned hierarchical allocator с hysteresis, reservations and explanations.
- Rendering требует instancing, culling и animation/effect/shadow LOD; simulation, visible и fully animated counts измеряются отдельно.
- Physics, networking и сложный world generator не являются prerequisites первого proof.
- Headless economy/field/batch tools важнее production presentation.

### `commander_simulator`

- Первый полноценный consumer knowledge-bounded planning: planner физически не должен видеть canonical truth, только versioned observations.
- Mission, route, tactical and local plans имеют разные budgets/interrupts; локальная смена укрытия не перестраивает миссию целиком.
- Semantic tactical map содержит authored affordances и dynamic knowledge overlays, а не сводится к navmesh.
- Explanation обязано хранить реальные candidates, evidence и rejection reasons во время решения; отдельный текст после факта недостаточен.
- Главный ранний runtime — headless graph mission + 2D tactical inspector. 3D squad scene является поздним presentation consumer.
- Networking и world generator не требуются.

### `apates_quest`

- Первый proof — `apates_campaign_bridge_lab` на фиксированном замкнутом графе 12–20 провинций;
  global sphere и 3D tactical scene намеренно идут отдельными следующими scenarios.
- Главный long-lived consumer — automatically advancing daily campaign: на границе суток фиксируются
  persistent commands, затем стабильно идут movement/routes, contacts, mandatory actions, due events и
  publication/stop conditions; хозяйство, политика, AI и демография имеют отдельные cadences.
- Journey хранит route queue/current transition/progress/ETA/interruption; до завершения перехода actor
  остаётся логически в origin со статусом marching, не действует там как свободный defender, а встречное
  движение создаёт boundary encounter вместо постоянной сущности «на ребре».
- Политическая модель строится как character/house/title → governing institution → seats → policies,
  claims, de-jure traditions, de-facto relation graph and bounded casus belli. Universal binary culture-
  question schema отменена; culture/religion/title/house имеют разные sources of authority.
- Canonical truth отделена от holder-specific rumor/contact/mapped-route/surveyed/current-intelligence,
  evidence, deed and myth versions; transfer/report/map sale являются явными transactions.
- Army battle — persistent multi-day strategic encounter (contact/preparation → manoeuvre → decisive
  clash → pursuit/withdrawal), не вторая обязательная TBT. Hero duel имеет заранее typed stake и может
  быть принят, отклонён или нарушен без автоматического завершения всей войны.
- Generated immutable world package включает closed spherical topology, 3–4k uneven land provinces
  plus larger sea zones, routes/climate, cultures/history, wonders/great works and Apate-role bindings;
  сохранять только seed нельзя, а first generator proof использует маленькую surface.
- Globe требует seam-free adjacency, projection-independent picking, province/map-mode buffers and terra-incognita filtering without geometry leaks.
- Tactical encounter временно materialize-ит persistent characters/items и возвращает один atomic typed
  outcome journal; autoresolve использует ту же schema. Первый reconcile proof headless, затем отдельная
  короткая TBT (герой + 3–5 спутников, ориентир 6–10 rounds / 10–15 minutes).
- На время manual TBT campaign calendar остановлен. Multiplayer только учитывается command/pulse/save
  boundary; отдельный authoritative-host lab идёт после single-player vertical slice.
- Localization особенно чувствительна к procedural names, titles, gender/case forms and structured rule explanations.
- Полный 3D tactical stack не должен блокировать первый headless political/knowledge proof.

### Архитектурные следствия

- Нельзя делать полную 3D action-базу prerequisite для `CG`, `MHM`, `TC`, `CMD`, `ZB` или глобального ядра `APQ`.
- `BITS` и `PA` определяют верхнюю планку общего renderer/animation/navigation; `SC` — physics/skeletal integration; `MMO` — networking; `MHM/APQ` — long-lived hierarchical simulation; `CMD` — inspectable knowledge-bounded planning; `ZB` — mass fields/flow/LOD.
- 2D/2.5D path должен оставаться first-class, а не режимом полного 3D renderer с обязательной physics scene.
- Общая skeletal integration должна масштабироваться от нескольких простых clips до full animation graph/ragdoll, не заставляя простые проекты платить полный runtime cost.
- Generator contract нужен `PA/MMO/SC/TC/APQ` и как tooling может помочь лёгким `ZB` scenarios, но не должен становиться зависимостью authored `BITS`, `CG`, `MHM` или `CMD`.
- «Простой co-op» описывает продуктовый scope, а не автоматически низкую инженерную сложность networked physics.
- External telemetry/screenshot sharing — отдельный web/platform concern и не аргумент добавлять gameplay networking в offline projects.
- Knowledge/provenance shell не определяет, чему верит сторона: engine даёт ids, filtered views, causal links and inspection; semantic truth/rumor/evidence policy принадлежит проекту.
- Layered-field toolkit не является swarm AI: engine владеет storage/versioning/algorithms/debugging, project — значением каждого слоя и решениями на его основе.

## Сводная capability matrix

| Capability | Проекты | Сейчас | Основной недостающий контракт | Ownership | Сложность |
| --- | --- | --- | --- | --- | --- |
| Stable runtime ids | все | `READY` для ECS/resource ids | logical persistent id registry и mapping к resident ids | engine mechanism + project types | `M` |
| Exact snapshot/resume | все | `READY/PARTIAL` | owner sections, save boundaries и compact policies | engine + project adapters | `M–L` |
| Save envelope/slots | все | `MISSING` | atomic writes, metadata, limits, fingerprints | engine | `M` |
| Schema migrations | все | `MISSING` | versioned sections и migration registry | engine + project migrations | `L` |
| Replay/first divergence | BITS, CMD, ZB, APQ; также deterministic tools | `PARTIAL` | input/observation/time log, checkpoints, presentation policy, hash bisect | engine mechanism | `L` |
| Typed commands/intents | все | `PARTIAL` | versioned command/result/rejection envelope | engine core + project payloads | `M` |
| Preview/read model | CG, TC, BITS, PA, CMD, APQ, ZB | `PARTIAL` | shared validation query, state-version token and filtered semantic snapshot | project-first, generic shell later | `M` |
| Persistent workflow | CG, MHM, BITS, PA, TC, SC, CMD, APQ, ZB | `PARTIAL` | serializable stage/cursor owner outside combat; APQ journeys/orders/quests/army encounters are first-class workflows | engine primitive + project state | `M–L` |
| Calendar/due queue | MHM, BITS, PA, MMO, APQ, CMD | `MISSING` | canonical ordering, cancellation, catch-up, budgets; APQ adds project-owned daily pulse/stop semantics | engine queue + project phase policy | `L` |
| Multi-cadence simulation | MHM, BITS, PA, MMO, APQ, CMD, ZB | `PARTIAL` | explicit phase graph/cadence/dirty scheduler | project-first over engine executors | `L` |
| Simulation LOD | MHM, BITS, PA, MMO, APQ, ZB | `MISSING` | logical↔aggregate↔resident lifecycle | engine mechanism + project codecs | `XL` |
| World residency/streaming | BITS, PA, MMO, SC, APQ | `PARTIAL` | hierarchical cells, epochs, prefetch, reconcile | engine | `XL` |
| Procedural artifact pipeline | PA, MMO, SC, TC, APQ | `MISSING` | seed/version/hash, CPU/external stages, cache | engine + project generators | `L–XL` |
| Generator contract | PA, MMO, SC, TC, APQ; optional ZB scenarios | `MISSING/PARTIAL` | typed passes, C++ tool registry, deterministic Lua glue, provenance, validation/repair | engine host/toolkit + project pipelines | `XL` |
| Standard resource formats | все 3D-проекты + UI | `PARTIAL` | importer/runtime interfaces, KTX/KTX2, 3D meshes, validation and fallback | engine adapters over third-party codecs | `L–XL` |
| Spatial query toolkit | BITS, MHM, PA, MMO, SC, APQ, CMD, ZB | `READY` as utilities | owner services, versions, multiple graph/field meanings | engine utilities + project graphs | `M` |
| Layered spatial fields and mass flow | ZB прежде всего; также CMD/MMO/strategy tools | `MISSING/PARTIAL` | versioned layers, brush/dirty updates, flow solver, congestion and heatmap inspection | engine toolkit + project semantics | `L–XL` |
| Knowledge/observation/provenance views | CMD, APQ, MHM, BITS; частично ZB | `MISSING/PARTIAL` | stable fact ids, immutable filtered views, transfer, causal trace and retention | engine shell + project fact semantics | `L–XL` |
| Planetary surface topology/globe | APQ, MMO | `MISSING` | projection-independent cells/adjacency/coordinates, picking, LOD and map-mode buffers | engine toolkit + project planet grammar | `XL` |
| Scene transform hierarchy | BITS, PA, MMO, SC, APQ, CMD, ZB | `MISSING` | world/local transforms, instances, publication | engine | `L` |
| Physics/query backend | BITS/PA/SC complex, MMO simple; APQ/CMD/ZB only when detailed local scene requires it | `MISSING` | tiered engine adapter/lifetime over likely Jolt backend | engine adapter + third-party backend | `L–XL` |
| Character controller/camera | BITS, PA, MMO, SC; optional APQ/CMD | `MISSING` | kinematic controller, first/third-person rigs | engine + project policies | `L–XL` |
| Navigation | BITS/PA complex, MHM/APQ/CMD graph-heavy, ZB mass-flow, MMO medium, SC/TC simple | `MISSING/PARTIAL` | tiered graph/grid/flow/navmesh adapters, streaming and debug | engine adapters + project policies | `L–XL` |
| Skeletal animation | BITS/PA complex, SC complex runtime/simple clips, APQ medium, MHM/MMO/TC/CMD/ZB small or LOD-heavy | `MISSING` | scalable adapter over third-party runtime, resources, notifies, ragdoll handoff and skinning | engine adapter + third-party runtime | `L–XL` |
| High-level 3D rendering | BITS/PA full, SC simple 3D, APQ globe+tactical, CMD optional 3D, ZB mass scene, MHM/MMO/TC 2.5D, CG 2D | `PARTIAL` | tiered scene batches, lights, shadows, culling and LOD without mandatory full-3D cost | engine renderer layer | `XL` |
| Grid tactics | TC | `PROJECT-FIRST` | cells, path/LOS/shapes/reservations | project-first | `L` |
| Turn/resolution kernel | CG, TC; later APQ small-party TBT | `READY/PARTIAL` | TC consumer, generic command shell; APQ encounter/outcome schema stays project-owned | engine primitives + project rules | `M–L` |
| Gamepad/contexts | CG, TC, BITS, PA, MMO, SC, APQ; optional CMD | `MISSING/PARTIAL` | joystick backend, focus, schemes/rebinding | engine | `M–L` |
| Localization pipeline | все | `MISSING/PARTIAL` | locale tables, language forms, safe text tags, compiled UI representation | engine + project string tables | `L–XL` |
| Compiled localized text in UI | все | `MISSING` | shaping/layout/cache/render API over compiled localized strings | engine/visage + text libraries | `L–XL` |
| World-space/debug UI | BITS, PA, MMO, SC, TC, APQ, CMD, ZB | `MISSING` | overlays, graph/field/globe picking, stable markers | engine tools | `L` |
| Data/schema inspection | все | `PARTIAL` | cross-resource validators and browsers | engine framework + project validators | `M–L` |
| Generated-content inspection tools | PA, MMO, SC, TC, APQ; ZB scenario fields | `PARTIAL` | simplified maps/graphs/heatmaps and batch reports | engine tool shell + project views | `M–L` |
| Full level authoring tools | BITS, PA, MMO, SC | `MISSING` | scene editing, transactions, undo/redo, nav/physics metadata | engine editor framework | `XL` |
| Hierarchical goal/operation AI | MHM, BITS, MMO, CMD, APQ; group allocation in ZB | `PARTIAL` | goal selection, persistent operations, knowledge boundaries, multi-level plans and explanation | project-first over engine scheduler/GOAP/tooling | `XL` |
| Headless GOAP/content simulation | CG, CMD, APQ, ZB, затем остальные data-driven проекты | `PARTIAL` | autonomous policy runner, scenario inputs and statistical export | engine harness + project policies | `M–L` |
| Headless host/scenario harness | networking, затем все проекты | `PARTIAL` | dedicated-server executable shell, scenario orchestration/export/bisect | engine runtime/test tool | `M–L` |
| Advanced spatial audio | BITS, MMO, PA, SC | `PARTIAL` | EFX-like environment DSP, occlusion/portals, semantic material/action sound maps | engine | `L–XL` |
| VoIP/capture | SC, MMO | `MISSING` | capture, codec, jitter, network routing | engine/network | `XL` |
| Networking | PA/SC co-op, APQ late authoritative WEGO, MMO full | `MISSING` | ENet UDP data plane + HTTPS master/rendezvous + key bootstrap + NAT/relay provider + tiered replication/prediction; APQ first needs only commands/pulse/hash/reconnect lab | engine program + project session semantics + third-party backends | `XXL` |
| Telemetry/social screenshot sharing | BITS, возможно другие offline projects | `MISSING` | HTTPS client, consent/auth/token, screenshot staging/upload and provider adapters | engine platform/web shell + provider-specific adapter | `M–L` |
| Distributed authority/persistence | MMO | `MISSING` | cell leases/handoff/journal/operations | unique platform | `XXL` |

## Что уже существует и чего конкретно не хватает

### `libs/aesthetics`

Сильная сторона:

- versioned entity ids;
- sparse/dense component storage;
- views/queries;
- query rebuild after snapshot load;
- canonical binary serialization;
- schema fingerprint/checksum/compression;
- save/load into a clean world.

Не хватает:

- migration старых component schemas;
- sectioned owner-level save format;
- logical persistent ids отдельно от resident ECS ids;
- compact delta/change tracking;
- transaction journal;
- replication codecs/baselines;
- materialization registry;
- более явной classification `authoritative / derived / ephemeral`.

Вывод: сохранять ECS можно уже сейчас. Campaign/run/profile/network formats должны оборачивать его, а не подменяться `dump_world`.

### `libs/simul`

Сильная сторона:

- app runtime и worker ownership;
- standard broker/messages;
- lifecycle/loading/window/settings;
- headless/optional subsystems;
- turn pipeline and transient presentation barriers;
- watchdog and resume boundary pattern.

Не хватает:

- persistent scene/run stack;
- calendar scheduler;
- generic persistent workflow records;
- delivery/backpressure diagnostics всех каналов;
- reusable checkpoint/save coordinator;
- server process/session topology;
- cross-process transport.

Вывод: `turn_pipeline` следует сохранить маленьким. Calendar и workflow должны быть соседними owners, а не новыми режимами того же класса.

### `libs/demiurg`

Сильная сторона:

- discovery/module override;
- stable logical resource handles;
- dependency gating;
- CPU/external staged transitions;
- list subresources;
- Lua resource access;
- hot GPU asset lifetime for texture/mesh.

Не хватает:

- resource/build fingerprint slices поверх готового canonical ordered module-set fingerprint;
- discovery catalog установленных folder/`.zip`/`.mod` модулей с metadata/version/dependencies;
- несколько ordered TAVL module profiles, active-profile selection и boot-time применение до resource registry;
- save manifest фактически загруженных module ids/versions/fingerprints и structured compatibility report;
- явная degraded-load policy: missing/changed modules могут быть warning вместо fatal только по решению проекта/игрока, с opaque preservation либо destructive-save warning;
- per-section resource schema migration metadata;
- priority/cancellation/budgets для procedural CPU artifact jobs;
- content-addressed artifact cache;
- explicit artifact epochs;
- общий importer/runtime-resource contract для стандартных внешних форматов;
- KTX/KTX2 texture resources с mip/layer/cubemap metadata, compressed BC7 и fallback/transcode policy;
- форматы 3D mesh/scene/animation с единым canonical CPU intermediate;
- dependency/resource graph inspection UI;
- safe unload contract для active sound data.

Вывод: procedural world artifacts можно вести через похожий staged lifecycle, но не следует маскировать generated mutable world state под обычный immutable resource.

### `libs/catalogue`

Сильная сторона:

- domain logging/introspection/statistics;
- bounded inline deferred calls;
- collect/elect/structural strategies;
- semantic seal;
- deterministic MT tests.
- passive constexpr phase metadata: owner, reads/writes, write policy, derived strategy and budgets;
  explicit caller-owned inspection registry без executor instrumentation.

Не хватает:

- bounded codecs только для новых доказанных signatures;
- возможно aggregation shell для diagnostic sources, но только после двух consumers с одинаковым
  snapshot/reset lifecycle; timings, semantic rejection, overflow и first divergence пока не
  сводятся в один универсальный event variant.

Не добавлять:

- replay format;
- RPC/network transport;
- world persistence;
- gameplay event ontology.

### `libs/act`

Сильная сторона:

- typed gameplay categories;
- immutable execution context;
- native/devils_script backend;
- deterministic RNG inputs;
- call arguments/lists;
- intent seam;
- building-block registration.

Не хватает:

- завершённого string/object/vector marshalling там, где появится consumer;
- versioned generic command envelope around project intents;
- structured rejection/preview result;
- capability metadata для authoring/security scopes;
- stable disk/network codecs, но только в отдельном owning layer.

### `libs/acumen`

Сильная сторона:

- bounded symbolic state;
- A*;
- cache and caller-owned scratch;
- config resources;
- proven parallel decision path.

Не хватает:

- goal candidate selection and utility explanation;
- duration/commitment/interruption owner;
- spatial action generation;
- multi-agent reservations;
- hierarchical/strategic planning;
- budget inspector.

Вывод: эти пункты в основном project-first. Engine может позже принять bounded scorer/trace, но не политическую или тактическую семантику.

### `libs/mood`

Основная задача закрыта:

- stateless transition store;
- config resource;
- guards/actions;
- runtime stepping/settle;
- conventions separated from storage.
- opt-in owned graph/step/settle diagnostic views с guard results и точной stop reason без
  instrumentation обычного runtime path.

Не хватает:

- capability/tooling around state graphs.

Persistent multi-day event или repair action не следует хранить внутри `mood`; они нуждаются в отдельном serializable instance/cursor.

### `libs/resolve`

Сильная сторона:

- pointer-free work provenance;
- bounded journals;
- semantic ids/order;
- target grouping;
- host-paced frontier;
- damage route primitives;
- hard non-recursive retaliation contract.

Не хватает только доказанных общих extensions:

- generic diagnostic export;
- additional neutral route/outcome helpers после второго consumer;
- common fault/report adapters.

Не добавлять:

- cards, initiative, grid movement;
- elemental/status/death rules;
- realtime physics hits;
- project report grouping.

Следующий полезный consumer — `tower_crawler`, но его grid and tactical outcomes должны оставаться project-owned.

### `libs/prefab`

Сильная сторона:

- component rows;
- inheritance;
- references and callbacks;
- config-defined spawn.

Не хватает:

- logical persistent identity policy;
- materialization/dematerialization codecs;
- validation tooling and dependency browser;
- staged construction rollback при частичном failure.

### `libs/utils`

Сильная сторона:

- deterministic hashing/PRNG;
- pools/channels/thread pool;
- grid/kd-tree/dynamic AABB tree;
- geometry queries;
- timelines;
- compression and file utilities.

Не хватает:

- service-level ownership/versioning вокруг spatial containers;
- atomic file transaction helpers;
- clearer error/expected APIs;
- reusable canonical priority queue serialization helpers;
- artifact cache/index primitives;
- reusable generation primitives: noise fields, Voronoi/Delaunay, flood/distance transforms, deterministic graph transforms;
- canonical parallel group/filter/reduce helpers for generation units;
- test utilities for canonical byte/hash comparison.

### `libs/painter`

Сильная сторона:

- Vulkan device/queues;
- render graph;
- buffers/descriptors;
- mesh/texture resources;
- bindless slots;
- indirect batches;
- headless/no-present mode;
- fence-safe texture/mesh unload.

Не хватает:

- high-level 3D scene instance layer;
- transform/skinning buffers;
- skeletal rendering;
- lighting/shadows;
- decals/transparency;
- visibility/occlusion;
- material/mesh LOD and HLOD;
- runtime-generated mesh lifecycle;
- KTX/KTX2 upload path without forced decompression of supported GPU block formats;
- canonical 3D mesh/material/skeleton upload interfaces независимо от конкретного importer;
- GPU timings/residency/debug capture suitable for project tools.

### `libs/flow`

Сильная сторона:

- 2D/2.5D/UV animation resource/playback;
- actions/notifies as ids;
- stable resource handles.

Не хватает:

- adapter к выбранной сторонней skeletal-animation runtime/library;
- skeleton/clip resources;
- pose sampling;
- blending/crossfade;
- root motion contract;
- IK;
- animation graph;
- GPU bone matrices/skinning;
- animation LOD.

### `libs/input`

Сильная сторона:

- keyboard/mouse;
- canonical names;
- config bindings and live save/reload;
- action events.

Не хватает:

- gamepad/joystick;
- control contexts/schemes;
- device hotplug;
- axis/deadzone/curve model;
- UI focus navigation;
- conflict-aware live rebinding.

### `libs/sound`

Сильная сторона:

- miniaudio playback;
- PCM/compressed resources;
- pooled spatial/non-spatial voices;
- listener and distance attenuation;
- task progress/state;
- volume groups.

Не хватает:

- resource/task lifetime pinning;
- parameterized semantic sound events;
- data-driven map `(material, action/impact, context) -> semantic sound event` для шагов, ударов и трения;
- priorities/virtual voices;
- EFX-like environment processing: effect zones, sends, reverb, low/high-pass and obstruction parameters;
- occlusion/portals/reverb;
- per-listener mixes;
- capture device;
- Opus/VoIP/jitter;
- network voice routing.

### `libs/visage` и `libs/bindings`

Сильная сторона:

- sandboxed Lua;
- Nuklear;
- UI budgets/error isolation;
- fonts/images/effects;
- host facade;
- read-only resource/gameplay access.

Не хватает:

- gamepad focus/navigation;
- world-space overlays/markers;
- picking/debug draw;
- large-list virtualization;
- DPI/layout units;
- editor host/panels;
- generic data/schema/tree/table inspectors;
- API layout/render compiled localized text: styled runs, inline objects, wrapping, clipping and hit spans;
- shaping/bidi/fallback-font cache keyed by locale/font/width/parameters;
- отдельный headless Lua environment для generator scripts; текущий visage environment остаётся UI-owned;
- multi-context policy.

### Локализация — новой библиотеки пока нет

Уже есть полезные seams:

- `act::string` возвращает `utils::id`, то есть gameplay может передавать loc-key без готовой строки;
- `demiurg` умеет модули, overrides, list resources and stable handles;
- `visage` умеет MSDF fonts и UI text;
- Lua host может получать read-only gameplay values.

Не хватает:

- locale manifest and fallback chain;
- таблиц `loc_key -> source template` с module override;
- compile-time validation всех locales/keys/placeholders;
- plural/cardinal/ordinal/select/gender/case forms;
- безопасного bounded tag grammar;
- compiled pointer-free text program для UI;
- Unicode normalization, shaping, bidi, line breaking and font fallback policy;
- parameter binding without building ad-hoc strings in gameplay;
- locale switch/cache invalidation;
- missing-key/tag/form diagnostics and pseudo-localization.

Локализация должна быть отдельным engine concern. Project поставляет строки, допустимые project tags и значения параметров, но не собственный parser/layout pipeline.

## Новые общие системы: можно ли собрать их из существующего

### 1. Durable persistence layer

Назначение: единый долговечный контейнер для run/campaign/profile/checkpoint state, который безопасно переживает перезапуск процесса, изменение схем и повреждение отдельного slot, но не знает семантику payload конкретной игры.

Статус: `PARTIAL`, составим из существующего примерно на 60%.

Уже есть:

- `aesthetics` serializer/sink;
- `utils` compression/hash/file IO;
- project cursors;
- `demiurg::resource_handle`;
- canonical state hashes в consumers.

Не хватает:

- envelope;
- section registry;
- version/migration graph;
- atomic slot transaction;
- metadata/index;
- fingerprints;
- load limits;
- journal/checkpoint abstraction;
- first-divergence helpers.

Рекомендуемая граница:

```text
persistence
  envelope
  section registry
  migrations
  atomic slots
  checkpoint/journal primitives
  diagnostics

project
  section payloads
  compatibility policy
  migration functions
  checkpoint boundaries
```

Сложность: `L`, journal/crash recovery повышает до `XL`.

### 2. Calendar scheduler

Назначение: детерминированно исполнять редкие и длительные события по игровому времени без покадрового обновления каждого объекта, поддерживая ускоренную промотку, бюджеты и остановку на значимом условии.

Статус: `MISSING`, составим из существующего примерно на 40%.

Уже есть:

- integer/rational game time;
- turn cursor pattern;
- deterministic ordering culture;
- pool and deferred commit;
- snapshot serializer.

Не хватает:

- canonical serializable priority queue;
- due-event record;
- cancellation generation;
- recurring/catch-up policy;
- budgeted advance;
- stop reasons/breakpoints;
- diagnostics.

Engine владеет механизмом очереди. Project определяет event kinds/payloads and phase semantics.

Сложность: `L`.

### 3. Persistent workflow host

Назначение: хранить и возобновлять многостадийную операцию, сцену или длительное действие на явных cursor boundaries, включая deadlines, presentation waits и exactly-once commit.

Статус: `PARTIAL`, составим из существующего примерно на 70%.

Уже есть:

- `turn_pipeline`;
- presentation barriers;
- project serializable cursors;
- `mood` tables;
- `resolve` work/frontier.

Не хватает:

- generic instance id/stage/cursor conventions;
- deadline integration;
- idempotent stage token;
- suspend/resume outside combat;
- result/provenance envelope;
- editor/trace.

Рекомендуется маленький header-only/core primitive рядом с `simul`, но только после `cardgame::run_session` или MHM operation consumer.

Сложность: `M–L`.

### 4. Logical identity and materialization

Назначение: связать долговечный логический объект с временным ECS/physics/render представлением, гарантируя одного owner, корректный round-trip и отсутствие дублирования при загрузке и выгрузке.

Статус: `MISSING`, составим из существующего примерно на 45%.

Уже есть:

- ECS versioned ids;
- prefab;
- snapshots;
- resource handles;
- streaming/chunk example;
- GPU unload;
- relation contract.

Не хватает:

- persistent logical id registry;
- logical↔resident mapping with epoch;
- materialization state machine;
- reserve/commit/rollback;
- project codec interface;
- reconciliation;
- double-owner detection;
- conservation audit;
- batch operations.

Engine владеет mappings/lifecycle. Project владеет codecs/invariants.

Сложность: `XL`.

### 5. World residency and artifact pipeline

Назначение: решать, какие части мира и какие производные CPU/GPU/physics/nav artifacts должны быть resident, асинхронно готовить их и безопасно retire/reconcile при уходе из области.

Статус: `PARTIAL`, составим из существующего примерно на 50%.

Уже есть:

- demiurg staged resource loading;
- broker/workers;
- tile chunk flow;
- spatial structures;
- painter external upload/unload;
- resource handles.

Не хватает:

- hierarchical cell graph;
- residency epochs/state;
- prefetch and multiple budgets;
- CPU artifact jobs/cancellation;
- content-addressed cache;
- collision/nav artifacts;
- canonical override layer;
- reconcile before unload;
- inspector.

Generated immutable artifacts можно вести рядом с demiurg lifecycle. Mutable world state остаётся world/session owner.

Сложность: `XL`.

### 6. Generator contract

Назначение: стандартизировать устройство процедурных генераторов без стандартизации конкретного мира — дать typed stages, reusable C++ инструменты, deterministic Lua orchestration, шаблоны массовой обработки, provenance, validation and bounded repair.

Статус: `MISSING/PARTIAL`, составим из существующего примерно на 35%.

Уже есть:

- deterministic hashes/PRNG inputs;
- spatial containers and geometry queries;
- thread pool and bounded task payloads;
- `demiurg` resources/modules/staged jobs;
- devils_script/Lua integration experience;
- `catalogue` timings/logging;
- snapshot/hash/artifact direction;
- project descriptions of top-down graph and terrain pipelines.

Текущего общего generator owner, pass registry и generation-specific Lua environment нет.

#### Базовая модель

```text
generator definition/resources
  -> resolve native tools and scripts
    -> compile pass graph
      -> execute typed passes
        -> validate
          -> bounded local repair or explicit rewind
            -> seal package/artifacts + provenance
```

Generator — не одна функция `generate(seed)`. Это versioned pipeline с явными intermediate representations и границами, на которых результат можно:

- проверить;
- закэшировать;
- визуализировать;
- сравнить;
- повторить;
- частично перестроить;
- передать следующей стадии без project-specific pointer.

#### Контракт прохода

Каждый `generation_pass` объявляет:

- stable pass id and implementation version;
- input artifact kinds/schemas;
- output artifact kinds/schemas;
- dependency passes;
- deterministic RNG domain;
- unit kind: pixel/cell/region/node/edge/volume/module/object;
- read/write mode and whether in-place mutation is allowed;
- execution mode: serial, parallel independent units, grouped, reduce or external job;
- hard limits: units, iterations, attempts, output bytes and wall-time diagnostic budget;
- cacheability and input hash policy;
- validation rules;
- allowed repair/rewind target;
- provenance detail level.

Pass получает immutable inputs and caller-owned scratch. Output публикуется только после успешного завершения и validation. Частично заполненный artifact не становится входом следующей стадии.

#### Общие промежуточные представления

Engine не навязывает один world model, но предоставляет typed containers:

- scalar/vector raster fields;
- masks, labels and distance fields;
- point sets and weighted samples;
- planar/abstract graphs with typed nodes/edges;
- hierarchy/DAG of semantic tasks;
- constraints, reservations and protected corridors/volumes;
- regions/polygons/cells;
- spline/path networks;
- height/density/SDF/voxel fields;
- module placement and named ports/slots;
- mesh/collision/nav artifact references;
- validation issues and provenance records.

Project может регистрировать собственный artifact type, если он serializable, versioned, bounded and inspectable.

#### Переиспользуемый C++ toolkit

Минимальный набор native primitives:

- deterministic Perlin/Simplex/Worley/value noise, fBm, ridged noise and domain warp;
- seeded field sampling independent of traversal order;
- Voronoi/Delaunay and nearest-site partition;
- flood fill, connected components, distance transform and watershed-like segmentation;
- morphology/smoothing/erosion primitives;
- weighted sampling, Poisson-like spacing and blue-noise candidates;
- graph connectivity, paths, cuts, cycles, spanning structures and reachability;
- constraint filtering/scoring/assignment;
- stable group/sort/filter/map/reduce;
- region adjacency and boundary extraction;
- spline corridors and width/clearance envelopes;
- SDF/CSG field operations, volume carve/union/subtract;
- module ports, fit tests and spatial packing;
- mesh extraction/baking adapters;
- validation traversal and structured issue emission.

Не все primitives нужно реализовать заранее. Registry расширяется под реальные passes, а generic contract остаётся одинаковым.

#### Lua как glue

Generator Lua — отдельный headless sandbox, не `visage` UI state:

- загружает generator scripts через `demiurg`;
- регистрирует C++ primitives через generation building-block facade;
- собирает pass graph and parameters;
- выбирает templates/variants and repair strategy;
- не владеет тяжёлыми pixel/node/mesh buffers;
- не выполняет per-pixel Lua loop в горячем пути;
- не использует `math.random`, OS time, filesystem or unordered global state;
- имеет instruction/wall-time/memory/output budgets;
- компилируется/валидируется до запуска pipeline;
- входит script/resource hash в generator fingerprint.

Массовая обработка единицы генерации выражается typed C++ iterators:

```text
for_each_unit(kind)
  -> filter(predicate)
  -> group_by(key)
  -> map(native/script callback over compact record)
  -> stable_reduce(reducer)
```

Lua задаёт композицию и небольшие semantic predicates. Noise sampling, graph algorithms, simulation loops, raster transforms and mesh baking остаются native.

#### Шаблоны проходов

Общий host предоставляет pass templates:

- `source`: создать initial field/graph/contracts;
- `transform`: field/graph/record transformation;
- `simulate`: bounded iterative state evolution;
- `classify`: вычислить labels/categories;
- `segment`: разбить raster/space/graph на regions;
- `connect`: построить или изменить graph edges/corridors;
- `assign`: сопоставить semantic requests and spatial roles;
- `embed`: назначить coordinates/volumes;
- `place`: разместить modules/objects по ports/slots;
- `bake`: создать mesh/collision/nav/streaming artifacts;
- `validate`: выпустить structured issues;
- `repair`: применить bounded local transformation;
- `seal`: canonicalize, hash and publish package.

Template задаёт lifecycle/budgets/provenance, но project выбирает input/output types and implementation.

#### Обратная связь и repair

Pipeline обычно DAG, но сложная генерация требует возврата к предыдущему уровню. Это оформляется явно:

- validator создаёт issue with source artifact/pass/records;
- repair policy выбирает локальную transform;
- attempt id становится частью RNG/provenance;
- pass invalidates only declared descendants;
- maximum attempts/rewind depth жёстко ограничены;
- полный reject seed — последняя typed outcome, не exception из бесконечного retry.

Произвольная рекурсия Lua и неявная мутация уже sealed стадий запрещены.

#### Reference pipeline: 2D карта региона/мира

Один возможный pass graph:

1. `world_contract` — размер, topology, climate and gameplay constraints.
2. `continental_seeds` — крупные массы и условная базовая высота.
3. `continental_surface` — Perlin/другой noise добавляет поверхность и неоднородность.
4. `tectonic_simulation` — столкновение/расхождение плит формирует горы, низины, острова, вулканические зоны.
5. `water_and_moisture` — flow accumulation, basins, rivers, lakes, evaporation/moisture spread.
6. `biome_classification` — температура/влага/высота/соседство → biome labels.
7. `region_partition` — Voronoi/weighted segmentation с географическими границами.
8. `transport_and_function_graphs` — дороги, железные дороги, логистика and strategic nodes при необходимости.
9. `history_simulation` — bounded eras/events over regions/graphs.
10. `current_politics` — ownership/control/presence and active tensions.
11. `streaming_partition` — cells/authority/interest constraints.
12. `macro_validation` — connectivity, alternatives, slopes/corridors and strategic invariants.
13. `seal_world_package`.

Типы данных меняются по стадиям: raster fields → labeled regions → linked graphs → historical records → current political state. Политика и история project-owned; fields/partition/group/filter/simulation host reusable.

Для MMO-варианта один объект участвует в нескольких несмешиваемых графах:

- physical;
- road;
- railway;
- logistics;
- underground;
- strategic;
- network authority/interest.

Они используют одни stable node ids, но разные edge types and validation.

#### Reference pipeline: 3D приключение

Top-down pass graph:

1. `world/adventure_contract` — способности, budgets, обязательные/запрещённые сочетания.
2. `central_situation` — компактное ядро причинной проблемы.
3. `event_task_hierarchy` — иерархия событий, исходов and requirements.
4. `finite_task_expansion` — разбиение на конечные действия с preconditions/effects/spatial requests.
5. `region_graph` — области, обязательные/необязательные пути, gates and loops.
6. `content_node_graphs` — локальные места, события, позиции ролей and state variants.
7. `semantic_spatial_binding` — task requirements назначаются regions/nodes/slots.
8. `graph_embedding` — coordinates, volumes, approaches, vistas and corridors.
9. `functional_reservations` — movement, camera, combat, event, spawn and retreat volumes.
10. `landscape_and_modules` — terrain/fields, carve/CSG, deformation and authored module insertion.
11. `local_noise` — мелкая неоднородность, не нарушающая protected volumes.
12. `object_binding` — обязательные objects раньше ambient population.
13. `bake` — meshes, collision, navigation, visibility and streaming artifacts.
14. `reverse_validation` — от физической проходимости к task/world contract.
15. `bounded_repair` — slot/module/corridor/graph correction with explicit rewind.
16. `seal_world_package`.

В отличие от 2D pipeline, главные primitives здесь — semantic DAGs, typed graphs, constraints, volumes, module ports, SDF/CSG and baking. Общими остаются pass lifecycle, Lua glue, grouping/filtering, RNG, provenance, validation and repair.

#### Package, local generation and mutable state

Generator contract различает:

- immutable macro/world package;
- deterministic local generation recipe/artifacts;
- cache, который можно пересоздать;
- mutable gameplay override/journal.

Generator не перезаписывает mutable session state. Local regeneration получает package + protected constraints + override journal and produces expected hash.

#### Provenance

Каждый significant output record может хранить:

- generating pass/version;
- seed domain and attempt;
- source record ids;
- template/variant;
- parameters;
- applied transforms;
- validation/repair history;
- final artifact/hash.

Detail budget настраивается: debug build может хранить полную цепочку, shipping package — compact source ids and reports.

#### Concurrency and determinism

- независимые passes/units могут исполняться параллельно;
- unit id and stage id define RNG, не worker index;
- group/reduce order canonical;
- Lua не мутирует shared buffers из worker callbacks;
- external bake jobs tagged by generation epoch;
- cache key включает inputs, scripts, native tool versions and generator config;
- reference serial path сравнивается с MT bytes/hash.

#### Tooling

Generator contract напрямую питает ранний generated-content tooling:

- pass graph and current status;
- simplified raster/map/region/graph views;
- overlay linked graph types;
- protected corridors/volumes;
- selected output provenance;
- validation issues and repair attempts;
- seed/pass/version diff;
- batch reject reasons and distributions;
- jump/export selected artifact.

Это inspection/debugger, не level editor. Ручное изменение результата возможно только через explicit source constraint/template override, иначе воспроизводимость теряется.

#### Ownership

Engine:

- pass/artifact schemas and registry;
- generator host and execution/budget rules;
- deterministic Lua sandbox and native building-block facade;
- common raster/graph/constraint/volume primitives;
- grouping/filtering/reduction;
- provenance/issues;
- cache/invalidation hooks;
- tooling shell.

Project:

- pipeline composition;
- semantic artifact types;
- content templates;
- stage parameters;
- constraints and quality metrics;
- history/politics/events;
- repair policy;
- final package schema adapters.

Сложность:

- `L` — pass registry/host, Lua glue, basic raster/graph artifacts and provenance;
- `XL` — полезный 2D reference pipeline with validation/tooling;
- `XL–XXL` — 3D graph→geometry/modules→mesh/collision/nav pipeline;
- каждый новый family of native primitives оценивается отдельно.

### 7. Standard resource format layer

Назначение: дать внешним проектам стандартные resource types и importer adapters для распространённых asset formats, отделив внешний формат файла от canonical CPU representation и runtime GPU/animation/physics objects.

Статус: `PARTIAL`, составим из существующего примерно на 45%.

Уже есть:

- `demiurg::resource_interface` and staged CPU/external loading;
- stable resource handles;
- painter texture/mesh resources and GPU lifetime;
- font/sound import examples;
- resource module override and dependency graph.

Не хватает:

- общий importer interface с structured diagnostics, cancellation and size limits;
- разделение source/import artifact/runtime resource;
- canonical image/texture metadata;
- KTX/KTX2 loader с mip levels, arrays, cubemaps and compressed-format metadata;
- прямой upload поддерживаемых BC7/других block-compressed payloads;
- fallback/transcode policy, если GPU не поддерживает source format;
- canonical 3D mesh intermediate: streams, indices, submeshes, bounds, materials;
- adapters к выбранным mesh/scene formats, вероятный первый кандидат — glTF 2.0;
- skeleton/animation import seam без привязки core resource к одной сторонней animation library;
- offline validation/cooking and content-hash cache;
- import-version fingerprints для save/network compatibility.

Engine владеет resource interfaces, canonical intermediates, validation and runtime adapters. Third-party libraries декодируют KTX/KTX2, mesh/scene and animation formats. Project выбирает import profiles and semantic material conventions.

Сложность: `L` для KTX/KTX2 + одного mesh adapter; `XL` для полного texture/mesh/material/skeleton pipeline.

### 8. Generic command/preview/result shell

Назначение: унифицировать внешний запрос на изменение authoritative state, его проверку, preview, идемпотентный commit и структурированный result, сохраняя payload и правила внутри проекта.

Статус: `PARTIAL`, составим из существующего примерно на 75%.

Уже есть:

- `act::intent`;
- player intent queue/dedup;
- `resolve` pointer-free work;
- cardgame action reports;
- deterministic commit;
- UI read-only seam.

Не хватает:

- command id/sequence;
- expected state version;
- structured validation/rejection;
- preview token;
- idempotency;
- common tracing fields;
- serialization shell.

Payload and authoritative application остаются project-owned.

Сложность: `M`.

### 9. Localization and compiled text

Назначение: загрузить локализованные таблицы, выбрать правильную языковую форму, безопасно разобрать текстовые теги и скомпилировать шаблон с параметрами в стабильное представление, которое UI может layout/render без повторного парсинга исходной строки.

Статус: `MISSING/PARTIAL`, составим из существующего примерно на 35%.

Уже есть:

- loc-key как `utils::id` в `act::string`;
- demiurg modules/list resources/overrides;
- Lua/Nuklear UI;
- MSDF font resources;
- deterministic/resource fingerprint conventions.

Нужны слои:

```text
locale resources
  -> validated string templates
    -> compiled text program
      -> bind typed parameters and choose language forms
        -> UI shaping/layout
          -> glyph/styled/inline-object draw runs
```

#### Таблицы и locale lifecycle

- locale id, fallback chain and default locale;
- resource type для таблиц `loc_key -> template`;
- module override per key;
- canonical key/placeholder/tag registry;
- hot locale switch;
- cache invalidation by locale/catalogue/font revisions;
- missing-key fallback, pseudo-localization and coverage report;
- fingerprints for save/replay diagnostics, но сохранения хранят keys/parameters, не готовый перевод.

#### Языковые формы

- cardinal/ordinal plural categories;
- `select` для рода, одушевлённости, grammatical case or project-defined enum;
- locale-aware number/date/duration formatting;
- typed parameters and formatter registry;
- validation, что все required branches/placeholders присутствуют;
- явная fallback policy для языка без конкретной категории.

Реализацию правил стоит строить поверх проверенной сторонней Unicode/localization библиотеки, например ICU/ICU4X либо более узкого MessageFormat-compatible parser, а не поддерживать вручную таблицу правил всех языков.

#### Теги

Теги компилируются в bounded AST/bytecode, не в HTML и не в Lua:

- style spans: emphasis/color/class;
- semantic spans: actor/item/action/loc reference;
- inline icon/image;
- line/paragraph controls;
- optional interaction/link id;
- project extension tags через capability registry;
- nesting/depth/output limits;
- exact source diagnostics;
- plain-text/accessibility projection.

Тег не может исполнять gameplay effect или получать произвольный доступ к UI environment.

#### Compiled representation

Pointer-free `compiled_text_program` содержит:

- literals/string-table offsets;
- placeholder instructions;
- form/select branches;
- tag push/pop/inline-object instructions;
- source loc-key/revision;
- bounded static requirements.

Binding создаёт immutable `resolved_text`/run stream для одного parameter set. Gameplay передаёт loc-key + typed args; оно не конкатенирует финальный текст.

#### UI rendering

`visage` получает API, работающий с compiled/resolved text:

- shaping into glyph runs;
- bidi and script segmentation;
- font fallback per codepoint/run;
- wrapping, alignment, ellipsis and clipping;
- styled spans and inline images;
- selectable/hit-test spans;
- baseline/line metrics;
- layout cache keyed by `(locale revision, program, args hash, font stack, size, width, style)`;
- draw conversion в существующий UI render output;
- plain/debug view compiled instructions and final runs.

Для сложного текста вероятны third-party shaping/Unicode components, например HarfBuzz плюс Unicode line-break/bidi implementation. Nuklear остаётся widget/layout host, но не должен сам парсить localization markup.

Engine владеет locale resources, compiler, form/tag contracts, shaping/layout/cache and UI rendering. Project владеет строками, разрешёнными semantic tag handlers и typed values.

Сложность: `L` для таблиц/forms/tags/compiler; `XL` с полноценным Unicode shaping/bidi/fallback/layout and editor diagnostics.

### 10. 3D scene foundation

Назначение: предоставить единое runtime-представление transform hierarchy и scene instances между gameplay, physics и presentation, не превращая render object в authoritative entity.

Статус: `MISSING`, составим из существующего примерно на 25%.

Уже есть:

- math/dependencies;
- painter meshes/draw groups;
- input/window/camera publication patterns;
- ECS;
- spatial query toolkit;
- resource loading.

Не хватает:

- transform hierarchy;
- scene instances;
- world/local conversion;
- origin policy;
- render snapshot adapter;
- picking/debug representation.

Сложность: `L`.

### 11. Physics

Назначение: подключить collision/rigid-body/query simulation к ECS и streaming через стабильные engine handles, явные phase boundaries и headless lifecycle.

Статус: `MISSING`, составим из существующего примерно на 15%.

Уже есть:

- geometry/spatial primitives;
- ECS and phase execution;
- broker and snapshots.

Не хватает:

- backend selection/integration;
- shape/body/constraint handles;
- broadphase/narrowphase/solver;
- ray/shape/overlap queries;
- collision layers;
- character/moving platform contract;
- cooking/runtime geometry;
- deterministic/network policy;
- debug draw.

Базовый вариант — сторонний backend, наиболее вероятный кандидат сейчас Jolt Physics. В scope движка входят adapter, handles, ECS mapping, unit/layer policy, queries, cooking, streaming, debug draw and tests. Внутренний broadphase/narrowphase/solver Jolt переписывать не нужно.

Adapter должен допускать разные профили стоимости:

- `query-only` без активной simulation scene;
- простые static/dynamic bodies для `MMO`;
- character/controller and interaction physics для `PA`;
- constraints/vehicles/ragdoll для `BITS`;
- nested moving frames, items and environment coupling для `SC`.

Проект включает только нужные body/solver features. Отсутствие physics у `CG/MHM/TC` не должно требовать создания пустого physics world.

Сложность: `L–XL` для Jolt integration and character/query contracts; собственный solver был бы `XXL` и не планируется.

### 12. Navigation

Назначение: строить и запрашивать проходимость для разных agent profiles, связывать navigation data с streaming/topology versions и возвращать асинхронные paths без stale references.

Статус: `MISSING`, составим из существующего примерно на 20%.

Уже есть:

- spatial data structures;
- thread pool;
- resource pipeline;
- deterministic query conventions.

Не хватает:

- nav representation/backend;
- tile build/load;
- agent profiles;
- off-mesh links;
- dynamic obstacles;
- async path request/versioning;
- local avoidance;
- debug/edit tools.

Предпочтителен adapter-first подход поверх сторонней navmesh/pathfinding библиотеки хотя бы для части задач. Engine всё равно должен владеть nav resource/tiles, query handles, async request epochs, links/obstacles, streaming integration and diagnostics. Crowd/formation policy может остаться project-owned или использовать отдельный backend.

Navigation — family из трёх разных representations:

- abstract/strategic graph routing для `MHM` и macro/logistics частей `MMO`;
- deterministic integer grid queries для `TC`;
- spatial navmesh/volume routing для `BITS/PA/SC/MMO`.

Общими могут быть request/result ids, versions, budgets, cancellation, path diagnostics and overlays. Нельзя заставлять strategic graph и tactical grid проходить через navmesh backend.

Сложность: `L–XL` с готовым backend; собственный полный navmesh build/query stack существенно дороже.

### 13. Skeletal animation

Назначение: превратить импортированные skeleton/clips и gameplay animation state в pose, root-motion/notifies и GPU skinning data, сохраняя authoritative gameplay boundaries вне presentation runtime.

Статус: `MISSING`, составим из существующего примерно на 25%.

Уже есть:

- `flow` playback concepts;
- demiurg resources;
- mood/action ids;
- painter buffers;
- presentation checkpoints.

Не хватает:

- skeleton/clip import/resources;
- pose/blend graph;
- root motion;
- IK/notifies;
- skinning buffers/shaders;
- animation LOD;
- authoring/debug.

Предполагается сторонняя skeletal-animation runtime/library. Engine реализует adapter, resource bindings, ECS/playback state, notify/intents boundary, root-motion policy, renderer upload/skinning and debug tools. Собственный clip compression/pose solver не является целью без доказанной необходимости.

Runtime profiles:

- small clip/state playback для `MHM/MMO/TC`;
- network pose/equipment attachment при небольшом числе clips для `SC`;
- full blend/root-motion/IK для `PA`;
- full graph + physical pose/ragdoll handoff and recovery для `BITS`.

Ragdoll принадлежит physics integration, но animation adapter определяет transition `animated pose -> physical bodies -> sampled/recovered pose`.

Сложность: `L–XL` с готовой библиотекой; собственный полноценный runtime был бы `XXL`.

### 14. Tooling

Назначение: дать разработчику наблюдаемые и редактируемые представления данных проекта. Tooling делится на ранний дешёвый inspection генерируемого контента и поздний полноценный level editor, потому что у них разные ownership, interaction and transaction requirements.

Статус: `PARTIAL`, составим из существующего примерно на 55%.

Уже есть:

- visage Lua/Nuklear;
- catalogue logging/statistics;
- demiurg resource discovery;
- project metrics;
- headless tests.

#### 14A. Tooling генерируемого контента

Назначение — быстро отвечать «что генератор создал и почему», не редактируя вручную итоговую сцену.

Минимальные представления:

- simplified generated map/region/graph;
- POI/spawn/resource markers;
- connectivity, reachability and seam failures;
- heatmaps and aggregate statistics;
- seed/stage/version comparison;
- artifact/residency status;
- selected record inspector;
- batch report and export.

Это может быть отдельный headless report + простой `visage` viewer. Не нужны general scene gizmos, undo/redo or arbitrary object editing. Project регистрирует view adapters and overlays; engine даёт graph/map primitives, tables, filters, selection and report shell.

Сложность: `M–L`. Этот tooling нужен раньше artifact/residency scaling и должен появиться до полноценного level editor.

#### 14B. Полноценный level authoring

Назначение — создавать и менять authored scenes/levels, размещать объекты и метаданные, управлять сложными транзакциями редактирования и сохранять результат в resource formats.

Не хватает:

- editor application mode;
- scene hierarchy/outliner;
- stable selection/picking and transform gizmos;
- world-space debug draw;
- schema-driven inspectors;
- graph/timeline/table widgets;
- undo/redo, transactions and dirty state;
- project tool/inspector registration;
- nav/physics/streaming metadata authoring;
- prefab/resource dependency editing;
- save/resource diff;
- import/export and validation.

Сложность: `XL`. Делать после стабилизации 3D scene/resource/nav/physics contracts; иначе editor законсервирует неверные APIs.

### 15. Headless host, scenario and batch harness

Назначение: запускать authoritative project runtime без window/render/audio, используя тот же command/simulation/save path для dedicated server, deterministic regression и массовых автономных прогонов.

Статус: `PARTIAL`, составим из существующего примерно на 75%.

Уже есть:

- headless app paths;
- cardgame/tile_frontier tests;
- deterministic seeds;
- catalogue metrics;
- snapshots and hashes.

Не хватает:

- scenario interface;
- parallel independent run orchestration;
- common output schema;
- first-divergence capture;
- property/fuzz integration;
- soak limits;
- distribution export.

Главный consumer — networking: отдельный dedicated-server executable с server tick, config/module fingerprints, lifecycle, networking adapters, metrics, graceful shutdown and checkpoint hooks. Он не должен линковать или создавать Vulkan/window/audio device.

Второй consumer — автономные content runs: AI-vs-AI, generation validation, balance/statistics, fuzz/property and soak. Они переиспользуют scenario interface, но запускают множество независимых sessions and export aggregate reports.

Сложность: `M` для общего headless host/scenario interface; `L` вместе с production-oriented dedicated-server shell and operations hooks.

### 16. Advanced audio

Назначение: преобразовывать semantic sound events в слышимый результат с учётом источника, материала, действия, помещения, препятствий, listener profile и доступного voice budget.

Статус: `PARTIAL`, составим из существующего примерно на 45%.

Уже есть playback/resources/voices/listener/groups.

Нужны:

- semantic sound event resource;
- map `(material, action/impact, context tags) -> event/variant set`, например `metal + footstep -> metal_step`;
- surface/material query seam from physics/world;
- deterministic or presentation-local variant selection policy;
- pitch/volume/layer randomization with bounded voices;
- EFX-like environment graph independent of old OpenAL API;
- zones/portals and auxiliary sends;
- reverb, echo, low/high-pass and obstruction/occlusion parameters;
- smooth transition between environment states;
- priorities/virtual voices;
- authoring preview and debug of chosen rule/effect chain;
- lifetime pins for active resource data.

OpenAL EFX может служить моделью возможностей, но текущий miniaudio path требует backend-neutral DSP/effect contract либо интеграции подходящего стороннего DSP layer. Gameplay публикует semantic event/material/action; конкретные samples and environment processing — presentation.

`subprojects/playgrounds/AU01_spatial_audio` фиксирует результат изолированного manual A/B 2026-08-12:
production miniaudio `system` против direct OpenAL Soft на одинаковом deterministic mono S16 signal,
horizontal/vertical orbit,
matched front/up distance pulses и linear attenuation. Он сообщает actual device/sample rate/OpenAL HRTF, поддерживает
HRTF off/on и dry-run CTest. Реальное headphone A/B подтвердило: OpenAL HRTF on заметно лучше передаёт
направление, built-in miniaudio близок к OpenAL HRTF off, а above/below у miniaudio почти неразличимы.
Это согласуется с реализацией miniaudio 0.11.25: координата `Y` сохраняется в listener-space и участвует
в distance, но обычный stereo panner вычисляет gains относительно SIDE_LEFT/RIGHT speaker vectors с
нулевой `Y`-компонентой; равнодистанционные `+Y/-Y` поэтому дают одинаковый stereo output. Vertical
orbit держит radius 4, так что perceived OpenAL near/far на нём может быть directional coloration.
Сценарий расширен до 28 секунд: одинаковые front `-Z` и up `+Y` pulses `4→10→1→4` с явным radius
log проверили axis attenuation parity отдельно: front и up работают корректно, а OpenAL отличается
лишь очень небольшой direction-dependent coloration. Miniaudio выбран production backend; весь live
OpenAL path архивирован в `exclude`. Живая `AU02_directional_coloration` и `AUD-17` зафиксировали
optional default-off bounded coloration:
listener-relative high shelf с профилем behind `-2.25 dB`, above `+0.65 dB`, below `-0.85 dB`,
общим strength `[0,2]` и итоговым пределом `[-3,+1] dB`. Он помогает заметить заход за спину, но
ручная проверка признала elevation практически нечитаемым; это предел shared shelf, не дефект `Y`.
Steam Audio/HRTF отложен до pre-release
spatial evaluation проекта `submarine_coop`, где его цена будет проверяться на реальной сцене.

VoIP добавляет совершенно отдельные capture/codec/jitter/network concerns.

Сложность: `L–XL`; с VoIP — `XL`.

### 17. Networking

Назначение: соединить authoritative headless server и clients через versioned protocol, передавать commands/state в заданном bandwidth budget и поддерживать prediction, reconnect and failure diagnostics.

Статус: `MISSING`, из существующего собирается не более 20%.

Можно переиспользовать:

- headless runtime;
- versioned entity ids;
- typed messages;
- snapshots/hash;
- deterministic commands;
- resource fingerprints.

#### Состав networking family

Networking разделяется на четыре нижних инфраструктурных слоя и более высокий gameplay protocol:

```text
HTTPS control plane
  server directory / direct-peer rendezvous / session ticket / key bootstrap

connectivity provider
  direct endpoint / NAT punch / optional Steam or third-party relay

encrypted ENet data plane
  peers / channels / reliable-unreliable UDP packets / metrics

game protocol
  handshake / commands / replication / prediction / reconnect / voice
```

Эти слои координирует одна явная connection state machine. Они не должны скрыто вызывать друг друга через независимые callback webs.

#### UDP data plane

Игровые данные передаются через ENet:

- unreliable sequenced snapshots/input;
- reliable ordered control/transactions;
- peer/channel lifecycle;
- MTU/fragmentation and timeout behavior;
- packet ownership and bounded queues;
- RTT/loss/bandwidth metrics;
- disconnect/reconnect reason mapping;
- packet impairment/capture hooks.

Кандидаты — оригинальный ENet или `zpl-c/enet`. Выбор делается spike по maintenance, IPv6, CMake/packaging, thread model, intercept hooks, license and compatibility with encryption wrapper.

Engine adapter должен оставаться тонким: предоставить RAII/lifetime, typed result/error, metrics and host integration, но не повторять ENet channels, reliability, retransmission, fragmentation or congestion behavior.

#### HTTPS control plane

HTTPS используется отдельно от realtime UDP:

- получить список серверов от master server;
- опубликовать/обновить dedicated server entry;
- запросить connection metadata конкретного server/peer;
- получить session/rendezvous ticket;
- передать наблюдаемые public endpoint candidates;
- согласовать protocol/build/module/resource fingerprints;
- передать начальный key material или безопасно согласовать способ его получения;
- при peer-hosted session помочь двум сторонам найти друг друга.

HTTP/TLS client/server primitives должны приходить из сторонней поддерживаемой библиотеки. Движок не реализует TLS, certificate validation or HTTP parser самостоятельно.

Master server — control-plane service, а не authoritative gameplay server. Его недоступность не должна обрывать уже установленную игровую сессию. Direct endpoint/manual connect может быть отдельным policy.

#### Шифрование UDP

Вероятная схема:

1. HTTPS аутентифицирует master endpoint и выдаёт short-lived session ticket/key bootstrap.
2. Client и peer предъявляют ticket при ENet connection handshake.
3. После проверки стороны выводят/получают session keys.
4. ENet packet payload защищается authenticated encryption.
5. Packet sequence/nonce/replay window and rekey имеют явный owner.
6. Ключи удаляются при завершении session и не пишутся в logs/saves.

Нельзя писать собственную криптографию. Нужна сторонняя crypto library и стандартный AEAD primitive. Adapter определяет только key lifecycle, packet framing, associated data, nonce/replay policy and error metrics.

Шифрование не заменяет command validation, rate limiting, authentication/authorization и защиту master service от abuse.

#### NAT traversal и relay

Connectivity layer предоставляет capability interface:

- direct public endpoint;
- master-assisted UDP hole punching;
- optional local-network/direct connect;
- optional Steam Networking/Steam relay provider;
- optional другой managed relay/third-party provider;
- explicit failure/fallback result.

Простейший self-hosted path — rendezvous через master, обмен endpoint candidates и ограниченная последовательность simultaneous UDP probes. Не следует сразу писать полный ICE/STUN/TURN stack, если готовая Steam/сторонняя инфраструктура закрывает требуемые платформы.

Steam и direct ENet paths могут иметь разные возможности. Верхний protocol видит connection capabilities и identity, но не притворяется, что все providers имеют совершенно одинаковую transport semantics.

#### Gameplay protocol

После установления transport всё ещё отсутствуют:

- protocol schemas;
- baselines/deltas;
- interest management;
- prediction/reconciliation;
- lag compensation;
- join/reconnect;
- bandwidth control;
- network simulation/tools;
- authority transfer;
- persistent transactions.

ENet закрывает reliable UDP data plane, но не HTTPS discovery, identity, encryption, NAT traversal, replication, schema negotiation, interest management or prediction.

#### Главные риски

**Затенить ENet собственным transport framework.** Толстая «универсальная» abstraction способна повторно реализовать channel policy, reliability, packet lifetime and connection state, скрыть полезные ENet features и усложнить диагностику.

**Glue explosion.** HTTPS library, ENet, crypto library, NAT punching, Steam/relay and gameplay protocol имеют разные lifecycle, threading, error and ownership models. Попытка поженить их через россыпь взаимных callbacks быстро создаст больше велосипедного кода, чем собственно network logic.

**Ложная взаимозаменяемость providers.** Direct ENet, Steam relay and another third-party backend могут различаться identity, MTU, ordering, QoS and reconnect behavior. Наименьший общий знаменатель способен уничтожить полезные возможности каждого.

**Разрастание test matrix.** Direct/manual, master-assisted, NAT, relay, encrypted/failed-key, reconnect and version-mismatch paths должны тестироваться отдельно.

Сдерживающие правила:

- один connection/session owner and explicit state machine;
- три узких backend boundaries: HTTPS control, connectivity provider, ENet-like data plane;
- crypto только через стандартную library;
- не дублировать ENet reliability/channels/congestion;
- provider-specific capabilities остаются видимыми;
- сначала vertical spikes: direct ENet → HTTPS directory/ticket → encrypted payload → NAT/relay fallback;
- failure injection and structured trace с первого slice.

Нужна отдельная library family/program, не адаптация broker. Dedicated-server executable из предыдущего пункта — первый host этой family.

Сложность: `XXL`.

### 18. Platform web services, telemetry and sharing

Назначение: дать offline и online проектам opt-in доступ к внешним HTTPS services — telemetry/statistics, crash/session reports и явная отправка пользовательского screenshot — без зависимости от gameplay networking stack.

Статус: `MISSING/PARTIAL`, составим из существующего примерно на 20%.

Можно переиспользовать:

- будущий third-party HTTP/TLS adapter из networking control plane;
- painter/render ownership для screenshot request;
- demiurg/resource handles for metadata;
- catalogue metrics/logging;
- app settings and UI host facade.

Не хватает:

- asynchronous HTTPS request client with cancellation, timeouts, limits and structured errors;
- bounded upload/download queues and retry/backoff;
- privacy/consent categories;
- telemetry schema/version/session ids;
- local buffering and retention policy;
- screenshot GPU readback;
- image conversion/encoding and size limits;
- preview/confirm UI;
- external-provider authentication/token lifecycle;
- provider adapters and capability/version diagnostics.

Screenshot sharing flow:

```text
explicit user action
  -> render screenshot request/readback
    -> encode/stage bounded image
      -> preview and consent
        -> provider authentication
          -> asynchronous HTTPS/platform share
            -> success/error result to UI
```

X/Twitter, Facebook and platform APIs are volatile and may impose different authorization, review and media-upload flows. Engine owns generic screenshot staging, HTTP job, consent and provider interface. Each concrete integration remains a replaceable adapter and may instead invoke an OS/platform share dialog.

Telemetry and screenshot sharing are opt-in and separate:

- telemetry cannot silently attach screenshots or personal data;
- screenshot upload only follows explicit user action;
- tokens/secrets are not written to save/log;
- gameplay continues if service is unavailable;
- retry does not duplicate a successfully accepted upload;
- provider SDK/API failure cannot fault simulation.

Это не gameplay networking: feature не создаёт peers, replication, prediction or authority. Общий HTTP/TLS backend возможен, но lifecycles and permissions остаются раздельными.

Сложность: `M` для telemetry HTTP client; `M–L` для screenshot readback/encode/share shell; каждый provider adapter оценивается отдельно.

### 19. Knowledge, observations and explanation provenance

Назначение: не дать planner/UI случайно читать canonical truth и предоставить нескольким проектам общий
нейтральный shell для фактов, наблюдений, заявлений, причин решений и filtered read-only views.

Статус: `MISSING/PARTIAL`, составим из существующего примерно на 40%.

Можно переиспользовать:

- versioned ids и project components из aesthetics;
- immutable execution contexts and typed values из act;
- catalogue/resolve provenance patterns and append-only ranges;
- resource ids, localization keys and UI read-only seams;
- deterministic snapshots and serialization.

Не хватает:

- stable fact/observation/explanation record envelope;
- source, observed/received time, confidence, audience/access and supersession links;
- immutable versioned filtered view, не exposing canonical world API;
- explicit share/copy/withhold transaction;
- causal links candidate → evidence → score/constraint → decision;
- bounded retention/compaction and causal anchors;
- truth-vs-view diff/inspection widgets;
- replay of observation/claim propagation;
- loc-key + typed parameters without storing formatted text.

Engine не определяет, что считается истинным, как падает confidence, кто кому доверяет и какое
свидетельство законно. Эти semantics принадлежат `CMD/APQ/MHM/BITS/ZB`. Общая часть — records, access
boundary, provenance, storage/retention hooks and tooling.

Сложность: `L` для record/view/provenance shell; `L–XL` вместе с transfer, retention, replay and UI.

### 20. Layered spatial fields and mass flow toolkit

Назначение: дать coarse spatial simulations versioned CPU layers, brush/dirty operations, common field
algorithms, deterministic publication and visualization — без превращения toolkit в RTS/swarm policy.

Статус: `MISSING/PARTIAL`, составим из существующего примерно на 35%.

Можно переиспользовать:

- `utils::grid`, geometry, kd/aabb trees and deterministic PRNG;
- thread pool, aesthetics phase runner and catalogue semantic commit;
- painter textures/overlays для presentation;
- generator raster/field primitives после `GEN-03`.

Не хватает:

- typed field descriptor: extent/resolution/coordinate transform/version;
- immutable read snapshot and double-buffered publish;
- radius/polygon/corridor brush journal;
- dirty tiles/incremental recompute;
- sampling/gradient/distance/flood/diffusion/blur primitives;
- integration/flow fields, movement classes and obstacle epochs;
- congestion/capacity/local-avoidance adapter;
- serial-vs-MT canonical tests;
- provenance and heatmap inspector;
- CPU truth ↔ optional GPU texture publication contract.

`ZB` — первый mass-flow consumer. `CMD` может использовать threat/affordance overlays, `MMO` — macro
fields, generators — raster stages. Pressure, food, priority and retreat semantics остаются проектными.

Сложность: `L` для field host/basic algorithms; `XL` для flow/crowd/incremental tooling.

### 21. Planetary surface topology and globe presentation

Назначение: дать проектам с замкнутой планетой projection-independent surface ids, adjacency,
coordinates, picking and reusable globe/map-mode rendering seams, не стандартизируя форму мира и province
generation grammar.

Статус: `MISSING/PARTIAL`, составим из существующего примерно на 20%.

Можно переиспользовать:

- geometry/spatial containers and hashing;
- generator graph/artifact contract;
- painter runtime mesh/buffer path после его расширения;
- UI picking/debug overlays;
- logical ids/world-package resources.

Не хватает:

- выбранного canonical surface topology adapter;
- seam/pole-safe adjacency, distance and routing;
- stable surface cell + local coordinate model;
- polygon boundaries, label anchors and spatial index;
- screen ray → globe → surface/province picking;
- map projection adapters over the same topology;
- LOD/incremental geometry and map-mode buffers;
- terra-incognita filtered picking/rendering contract;
- circumnavigation/seam/property tests.

`APQ` — первый single-process globe/map consumer; `MMO` позже добавляет distributed coordinates,
streaming and authority. Эти требования нельзя сразу сливать в один MMO-scale subsystem.

Сложность: `XL` для reusable topology+picking+presentation proof; planet generator/runtime streaming
оцениваются отдельно.

## Общий backlog с приоритетами

### Tier 0 — закрепить существующие контракты

| ID | Задача | Ownership | Сложность | Зависимости |
| --- | --- | --- | --- | --- |
| `FND-01` | Canonical module/resource/build fingerprint API; ordered module-set slice `READY`, resource/build slices open | engine/demiurg | `M` | нет |
| `FND-02` | Common state hash/byte comparison and first-divergence test helpers | engine test utils | `S–M` | нет |
| `FND-03` | Structured fault/rejection/overflow record | engine shell | `M` | catalogue/resolve |
| `FND-04` | Explicit authoritative/derived/ephemeral component/data classification docs/helpers | engine + projects | `S` | aesthetics |
| `FND-05` | Channel delivery/backpressure metrics | engine/simul | `M` | broker |
| `BLD-03` | Audit and opt in to aligned default GLM gentypes | engine/build + ABI consumers | `S–M` | serialization/broker/GPU/foreign-layout audit and measured SIMD benefit |
| `MOD-01` | Installed module discovery + metadata/version/dependency catalog | engine/demiurg | `M` | `FND-01` |
| `MOD-02` | Ordered TAVL profiles + active-profile pointer + atomic persistence | engine/demiurg + app shell | `M` | `MOD-01`, atomic file transaction |
| `MOD-03` | Boot-time profile application and restart switch boundary | engine/simul | `M` | `MOD-02` |
| `MOD-04` | Module profile manager UI and save compatibility warning flow | engine UI shell + project skin | `M–L` | `MOD-03`, `PST-06/07` |

### Tier 1 — долговечность и дискретная симуляция

| ID | Задача | Ownership | Сложность | Зависимости |
| --- | --- | --- | --- | --- |
| `PST-01` | Save envelope, limits, checksum and fingerprints | engine | `M` | `FND-01` |
| `PST-02` | Atomic slots and metadata/index | engine | `M` | `PST-01` |
| `PST-03` | Section registry and migration graph | engine + project adapters | `L` | `PST-01` |
| `PST-04` | Profile/run/campaign transaction helpers | project-first, generic idempotency core | `M–L` | `PST-02` |
| `PST-05` | Replay artifact: intents/time/checkpoints/fingerprints | engine mechanism | `L` | `PST-01`, `FND-02` |
| `PST-06` | Save module manifest + compatibility classifier | engine/demiurg + persistence | `M` | `FND-01`, `MOD-02` |
| `PST-07` | Degraded load/opaque unknown-section policy | engine shell + project decision | `M–L` | `PST-03`, `PST-06` |
| `SIM-01` | Serializable due queue/calendar | engine | `L` | `PST-01` |
| `SIM-02` | Budgeted advance/stop/breakpoint diagnostics | engine | `M` | `SIM-01` |
| `SIM-03` | Persistent workflow conventions/host | project-first → engine | `M–L` | `PST-01` |
| `SIM-04` | Generic command/version/rejection shell | engine + project payloads | `M` | `FND-03` |
| `HLS-01` | Headless host/scenario interface for dedicated server and autonomous runs | engine/simul | `M` | `FND-01/02` |
| `KNW-01` | Stable fact/observation/explanation envelope + immutable filtered-view host | engine shell + project schemas | `L` | `PST-01`, `FND-03` |
| `KNW-02` | Provenance links, explicit share/copy transaction and bounded retention hooks | engine mechanism + project policy | `L` | `KNW-01` |
| `KNW-03` | Truth-vs-view/decision inspector and observation-log replay | engine tools | `M–L` | `KNW-01/02`, `HLS-01` |

Первые consumers persistence/workflow:

- `CG` проверяет save/run/profile/workflow;
- `TC` проверяет второй turn/resolve consumer;
- `MHM/APQ` проверяют calendar and multi-cadence headless simulation;
- `CMD` первым проверяет knowledge-filtered planner view and explanation provenance.

Для `HLS-01` главный целевой consumer — будущий networking dedicated-server executable. Второй — автономные content/balance/generation runs со статистическим export. Существующие cardgame/tile_frontier headless tests являются исходными proof cases, но не определяют конечную topology.

### Tier 2 — generators, logical world, resources и streaming

| ID | Задача | Ownership | Сложность | Зависимости |
| --- | --- | --- | --- | --- |
| `GEN-01` | Typed pass/artifact registry and execution host | engine/generation | `L` | `FND-01/02` |
| `GEN-02` | Deterministic headless Lua glue and generation building-block facade | engine/generation/bindings | `L` | `GEN-01`, demiurg |
| `GEN-03` | Basic raster/field/graph/unit group-filter-reduce toolkit | engine/utils/generation | `L–XL` | `GEN-01` |
| `GEN-04` | Provenance, validation issues, cache keys and invalidation | engine/generation | `L` | `GEN-01`, `FND-01` |
| `GEN-05` | Bounded repair/rewind and canonical serial-vs-MT tests | engine host + project policies | `L` | `GEN-03/04` |
| `GEN-06` | 2D region/planet reference pipeline | project-first (`MMO`/future strategy consumer) | `XL` | `GEN-02..05` |
| `GEN-07` | 3D adventure graph→volume/module→artifact reference pipeline | project-first (`PA`) | `XL–XXL` | `GEN-02..05`, `AST-*`, 3D/nav/physics |
| `GEN-08` | Closed-surface planet/culture/history/special-place package reference pipeline | project-first (`APQ`) | `XL–XXL` | `GEN-02..05`, `PLN-01`, `PST-01`; small saved surface first, full 3–4k uneven land provinces + larger sea zones only after profiling |
| `FLD-01` | Versioned layered-field host, immutable snapshots and deterministic publish | engine/utils/simulation | `L` | `FND-02`, utils grid |
| `FLD-02` | Brush journal, dirty tiles and basic distance/flood/diffusion/gradient toolkit | engine/utils | `L` | `FLD-01`, `GEN-03` |
| `FLD-03` | Flow-field, movement-class, obstacle-epoch and congestion/local-avoidance proof | project-first (`ZB`) → engine toolkit | `XL` | `FLD-01/02` |
| `FLD-04` | Field provenance, heatmap and cost/stuck diagnostics | engine tools | `M–L` | `FLD-01/02`, `UI-00` |
| `PLN-01` | Canonical closed-surface topology, stable ids, adjacency/distance and property tests | engine geometry + `APQ` proof | `L–XL` | `GEN-01`, `FND-02` |
| `PLN-02` | Projection adapters, ray/surface/province picking and seam/circumnavigation tests | engine painter/spatial | `L` | `PLN-01`, first saved `GEN-08` package; no full 3D action prerequisite |
| `PLN-03` | Globe LOD/map-mode buffers and terra-incognita-filtered presentation | engine painter/visage + project views | `XL` | `PLN-01/02`, first saved `GEN-08` package, `KNW-01`; no tactical renderer dependency |
| `WLD-01` | Persistent logical id registry | engine | `M` | `PST-01` |
| `WLD-02` | Logical↔resident mapping and epochs | engine | `L` | `WLD-01`, aesthetics |
| `WLD-03` | Materialization reserve/commit/reconcile lifecycle | engine + project codecs | `XL` | `WLD-02`, prefab |
| `WLD-04` | Hierarchical residency state machine | engine | `L` | `WLD-02`, demiurg/simul |
| `WLD-05` | Procedural artifact jobs/hash/cache | engine + project builders | `L–XL` | `GEN-01/04`, demiurg |
| `WLD-06` | Sparse generated-base override format | project-first → reusable pattern | `L` | `PST-03`, `WLD-01` |
| `WLD-07` | Residency/materialization inspector | engine tool | `M–L` | `WLD-03/04` |
| `AST-01` | Common importer/source-artifact/runtime-resource interfaces | engine/demiurg | `M–L` | `FND-01` |
| `AST-02` | KTX/KTX2 texture resource, compressed upload and fallback policy | engine/demiurg/painter | `L` | `AST-01` |
| `AST-03` | Canonical 3D mesh/material intermediate + first format adapter | engine/demiurg/painter | `L–XL` | `AST-01` |
| `AST-04` | Skeleton/animation import seam and cooked artifact fingerprints | engine/demiurg/flow | `L` | `AST-01` |

Первый consumer:

- `MMO`/ограниченный strategy fixture — 2D fields, regions and linked macro graphs;
- `PA` — semantic task/region/content graphs, 3D generated package/artifacts/overrides;
- `APQ` — closed-surface topology, immutable planet/culture/history package and globe picking;
- `ZB` — layered spatial fields, brush operations and mass-flow proof без обязательного world generator;
- `TC` — small floor + relatively large semantic quest graph without 3D artifact complexity;
- `SC` — medium streaming cave/route segments;
- `MHM` — aggregate↔named actor;
- `BITS` — 3D cell↔canonical city;
- `MMO` — только после локального single-process proof.

### Tier 3 — 3D action foundation

| ID | Задача | Ownership | Сложность | Зависимости |
| --- | --- | --- | --- | --- |
| `3D-01` | Transform hierarchy and scene instances | engine | `L` | aesthetics/painter |
| `3D-02` | Debug draw, picking and world-space markers | engine tools | `L` | `3D-01`, visage |
| `PHY-01` | Jolt-oriented physics backend spike and adapter/lifetimes/queries | engine + Jolt candidate | `L–XL` | `3D-01` |
| `PHY-02` | Kinematic character + moving platform contract | engine + project tuning | `L` | `PHY-01` |
| `PHY-03` | Dynamic rigid bodies, constraints and animation↔ragdoll handoff | engine + animation adapter | `L–XL` | `PHY-01`, `ANM-01` |
| `PHY-04` | Nested/local moving frames for vessel, characters and loose objects | engine mechanism + `SC` proof | `XL` | `PHY-02/03` |
| `CAM-01` | First/third-person camera rigs and camera intents | engine | `M–L` | `PHY-02`, input |
| `NAV-01` | Third-party navigation backend spike + data/resources/query/versioning adapter | engine + chosen backend | `L–XL` | `PHY-01`, `WLD-05` |
| `NAV-02` | Dynamic obstacles/off-mesh/local avoidance | engine | `L–XL` | `NAV-01` |
| `NAV-03` | Strategic region/road/access graph queries and hierarchical route planning | engine graph primitives + `MHM/APQ` policies | `L` | `WLD-01`, spatial query toolkit |
| `ANM-01` | Third-party skeletal runtime spike + skeleton/clip adapter and pose sampling | engine/flow + chosen backend | `L` | `AST-04` |
| `ANM-02` | Blending/root motion/notifies/IK | engine/flow | `XL` | `ANM-01`, `PHY-02` |
| `RND-01` | High-level 3D scene instance layer | engine/painter | `L` | `3D-01`; first bounded consumer `PF01` |
| `RND-02` | Transform/skinning frame buffers | engine/painter | `L` | `3D-01`; skinning waits for `ANM-01` |
| `RND-04` | Forward+ lighting path | engine/painter | `L–XL` | `RND-01/02`; `PF01` |
| `RND-22` | Directional/spot shadow maps and atlas | engine/painter | `L–XL` | `RND-04`; `PF02` |
| `RND-16/19/21` | Post compositor, first spatial effect and color chain | engine/painter | `L–XL` | `PF03`; temporal work remains separate |
| `RND-23` | Stencil effect path | engine/painter | `M–L` | depth/stencil baseline; `PF04` |
| `RND-03` | Skinned render path | engine/painter | `L–XL` | `ANM-01`, `RND-01/02` |
| `RND-06/07` | Visibility/LOD/HLOD/residency metrics | engine/painter | `XL` | `WLD-04`, live scale consumer |
| `INP-01` | Gamepad/axes/hotplug/control contexts | engine/input | `M–L` | нет |

Проверять не внутри полной игры, а в независимых engine laboratory scenes:

- `PF01` — Forward+ moving-light stress scene;
- `PF02` — moving light/caster and shadow-atlas inspection;
- `PF03` — post-effect gallery;
- `PF04` — stencil/masked effect gallery;
- отдельные будущие controller/camera obstacle course и one-skinned-actor labs;
- one streamed room/door/off-mesh link после выбора physics/navigation campaign;
- headless physics/nav tests;
- render snapshot detached from gameplay pointers.

### Tier 4 — project-first genre kernels

| ID | Задача | Первый owner | Возможная общая часть | Сложность |
| --- | --- | --- | --- | --- |
| `CG-01` | Run session/map/scenes | `CG` | persistent scene workflow | `L` |
| `CG-02` | Card instance/zones/profile | `CG` | idempotent collection transaction | `L` |
| `CG-03` | Headless GOAP run-policy/statistics fixture | `CG` | autonomous content-run harness | `M` |
| `TC-01` | Grid/path/LOS/shapes | `TC` | discrete grid query library после proof | `L` |
| `TC-02` | Initiative/reservations/group exit | `TC` | stable timeline primitives | `L` |
| `TC-03` | Small floor + semantic quest graph generator | `TC` | compact graph generator/validator consumer | `M–L` |
| `MHM-01` | Goal selector/operations/knowledge | `MHM` | bounded scorer/trace/workflow | `XL` |
| `BITS-01` | City operations/facts/crime | `BITS` | causal fact/operation tooling | `XL` |
| `PA-01` | World generator/event instance | `PA` | generation/workflow patterns | `XL` |
| `SC-01` | Vessel frames/topology/flood solver | `SC` | graph solver host/conservation tools | `XL` |
| `SC-02` | Streaming cave/route-segment generator | `SC` | medium generated-world/residency consumer | `L–XL` |
| `MMO-01` | Rail/logistics/strategic operations | `MMO` | transaction/graph primitives | `XL–XXL` |
| `ZB-01` | Priority-operation fields, swarm demand/allocation and pressure-front simulation | `ZB` | layered fields, flow solver, assignment/reservation trace | `XL` |
| `CMD-01` | Knowledge-bounded mission/route/tactical planner with partial replan | `CMD` | filtered views, persistent workflow, scorer/explanation and reservation primitives | `XL` |
| `APQ-01` | Fixed-graph daily campaign kernel: persistent orders, stable auto-WEGO pulse, stop/batch/save | `APQ` | calendar queue, budgeted advance and state comparison | `L` |
| `APQ-02` | Title/institution/seat/policy/claim vertical with one explained succession/hero-recognition chain | `APQ` | typed preview/rejection and evidence-backed explanation shell | `L–XL` |
| `APQ-03` | Holder knowledge: rumor/contact/route/survey, delayed report, map sale and false-map contradiction | `APQ` | filtered views, provenance transfer and truth-vs-view inspection | `L` |
| `APQ-04` | Persistent journey/army movement and multi-phase encounter/duel-stake process | `APQ` | workflow/route helpers and typed outcome/report adapters after proof | `L–XL` |
| `APQ-05` | Headless campaign encounter bridge: reserve → materialize/autoresolve → journal → exactly-once reconcile | `APQ` | logical identity, materialization transaction and audit tooling | `L` |
| `APQ-06` | Durable 150–200-year batch with migrations, retention, cadence/RNG budgets and worker identity | `APQ` | campaign save sections, headless harness and first-divergence tools | `L` |
| `APQ-07` | Short hero + 3–5 companions TBT vertical using the same campaign outcome schema | `APQ` | later 3D/animation/navigation consumers; no new generic combat semantics assumed | `XL` |
| `APQ-08` | Deed/witness/myth/Apate and special-place vertical | `APQ` | provenance/UI/generator stress; all semantic interpretation stays project-owned | `XL` |
| `APQ-09` | Late two-player authoritative-host daily-pulse/reconnect/shared-TBT lab | `APQ` | command/result/hash/session primitives after stable single-player slice | `L–XL` |

Ни один пункт этого tier не переносится целиком в engine.
`APQ-01..05` together define `apates_campaign_bridge_lab`; `GEN-08`/`PLN-*` are the next independent
planet/globe proof, not prerequisites. `APQ-07/09` remain explicitly later gates.

### Tier 5 — localization, audio and tooling

| ID | Задача | Ownership | Сложность | Зависимости |
| --- | --- | --- | --- | --- |
| `AUD-01` | Pin sound data for queued/active tasks and unload safety — `READY` | engine/sound | `M` | demiurg |
| `AUD-02` | Semantic events, priorities and virtual voices | engine/sound | `L` | `AUD-01` |
| `AUD-03` | Material + action/impact -> semantic sound-event mapping | engine/sound + project material tags | `M–L` | `AUD-02`, physics material seam |
| `AUD-04` | EFX-like environment DSP, zones, sends, occlusion and portals | engine/sound | `L–XL` | `PHY-01`, `WLD-04` |
| `AUD-17` | Bounded behind/above/below coloration over miniaudio — `READY`, optional/default-off; elevation limitation documented | engine/sound | `S` | miniaudio production path |
| `AUD-18` | Steam Audio/HRTF pre-release evaluation | `submarine_coop` + engine adapter | `M–L` | real submarine scene, target platforms and voice budget |
| `LOC-01` | Locale tables, fallback, coverage and pseudo-localization | engine/localization + demiurg | `M–L` | `FND-01` |
| `LOC-02` | Language forms, typed parameters and safe tag compiler | engine/localization | `L` | `LOC-01` |
| `LOC-03` | Compiled text program and resource fingerprints | engine/localization | `M–L` | `LOC-02` |
| `LOC-04` | Unicode shaping/bidi/font fallback/layout cache | engine/visage + third-party text libs | `XL` | `LOC-03` |
| `UI-00` | Early generated-content map/graph/heatmap/report viewer | engine tool shell + project adapters | `M–L` | `HLS-01`, `GEN-04` |
| `UI-01` | Data/schema/tree/timeline inspector widgets | engine/visage | `L` | нет |
| `UI-02` | World overlay/picking/debug draw integration | engine | `L` | `3D-02` |
| `UI-03` | Full level-editor host, scene transactions and undo/redo | engine | `XL` | `UI-01/02`, stable 3D APIs |
| `UI-04` | Render compiled localized text in panels/widgets | engine/visage | `L` | `LOC-03/04` |
| `UI-05` | Semantic graph/route/timeline viewer with confidence/provenance overlays | engine tool shell + project adapters | `L` | `UI-01`, `KNW-01` |
| `WEB-01` | Third-party HTTP/TLS client adapter, async jobs, limits and errors | engine/platform | `M` | no gameplay net dependency |
| `WEB-02` | Opt-in telemetry schema, batching, consent and retention | engine/platform + project events | `M` | `WEB-01` |
| `WEB-03` | Screenshot readback/encode/preview/share provider shell | engine/painter/visage/platform | `M–L` | `WEB-01`, render readback |
| `WEB-04` | X/Twitter, Facebook or OS/platform share adapters | provider-specific | `M` each | `WEB-03` |
| `VAL-01` | Resource dependency/schema validation framework | engine + projects | `M–L` | demiurg |
| `VAL-02` | Save/state diff and migration tool | engine | `M–L` | `PST-03` |

### Tier 6 — networking program

| ID | Задача | Сложность | Зависимости |
| --- | --- | --- | --- |
| `NET-00` | Dedicated-server executable shell | `M–L` | `HLS-01`, fingerprints/settings |
| `NET-01` | ENet fork evaluation + thin UDP data-plane adapter | `L` | `NET-00` |
| `NET-01A` | HTTPS master-server client: directory, registration, rendezvous and tickets | `L` | `NET-00`, third-party HTTP/TLS |
| `NET-01B` | Session-key bootstrap + standard AEAD wrapper for ENet packet payloads | `L` | `NET-01/01A`, third-party crypto |
| `NET-01C` | Master-assisted NAT punching spike | `M–L` | `NET-01/01A` |
| `NET-01D` | Steam/third-party relay provider and capability-aware fallback | `L–XL` | `NET-01C` |
| `NET-01E` | Unified connection state machine, structured trace and failure matrix | `L` | `NET-01`, `NET-01A..01D` |
| `NET-02` | Replication schemas/baselines/deltas | `XL` | `NET-01E`, stable ids |
| `NET-03` | Interest management/bandwidth budgets | `L–XL` | `NET-02`, spatial services |
| `NET-04` | Prediction/reconciliation/interpolation | `XL` | `NET-02`, physics |
| `NET-05` | Join/reconnect and persistent ownership | `XL` | `NET-02`, persistence |
| `NET-06` | Voice capture/Opus/jitter/routing | `XL` | `NET-01E`, sound |
| `NET-07` | Authority cells/epochs/handoff | `XXL` | `NET-01E`, `NET-02..05` |
| `NET-08` | Distributed journal/checkpoints/operations | `XXL` | `NET-07`, persistence |
| `NET-09` | Load bots, packet simulator and operations tooling | `XL` | `NET-01E`, `NET-02..08` |

Первый network proof — маленький `PA/SC`-подобный static-room co-op, не планета и не полная физика лодки. `MMO` начинается только после prediction, reconnect, persistence and handoff tests.

Порядок transport proof:

1. direct manual ENet connection без master;
2. HTTPS server directory and direct endpoint ticket;
3. authenticated encrypted UDP payload;
4. master-assisted NAT punch;
5. Steam/managed relay fallback;
6. только затем replication/prediction поверх стабилизированного connection lifecycle.

## Уникальные проектные системы

### `cardgame`

Уникально:

- card instance/zone/mutation semantics;
- directed run map rules;
- initiative/countdown;
- elemental/status/death/action-report semantics;
- card preview limits.

Переиспользуемо:

- run/session workflow;
- compact mid-operation save;
- profile transaction;
- headless batch tooling.

### `tower_crawler`

Уникально:

- square tactical rules;
- wounds/resources/ascent;
- initiative formula;
- safe group exit;
- tactical scoring.

Переиспользуемо после proof:

- integer grid queries;
- LOS/shape preview contract;
- reservation/timeline helpers;
- tactical trace widgets.

### `medieval_hero_manager`

Уникально:

- actor/area/polity semantics;
- political/economic transactions;
- historical constraints;
- motives and knowledge rules.

Переиспользуемо:

- calendar;
- persistent operation workflow;
- multi-cadence diagnostics;
- materialization audit;
- goal-scoring explanation shell.

### `zerg_brain`

Уникально:

- priority-operation semantics and approach corridors;
- food/upkeep/growth/control-capacity formulas;
- pressure/front/retreat policy;
- multi-capability swarm assignment;
- physiology and moving-hive rules.

Переиспользуемо после proof:

- versioned layered-field host and brush journal;
- flow-field/congestion diagnostics;
- capacity-aware assignment/reservation trace;
- mass simulation/render/animation LOD metrics;
- field heatmap and provenance inspector.

### `commander_simulator`

Уникально:

- mission/route/tactical template hierarchy;
- squad fact/confidence semantics;
- roles, support budgets and orbital actions;
- prepared-position and evacuation policy;
- user-facing military reports.

Переиспользуемо:

- immutable knowledge-filtered planner views;
- persistent plan/interrupt/reservation records;
- evidence-backed scoring explanation;
- semantic graph/route/timeline widgets;
- headless mission batch and no-progress/cycle diagnostics.

### `apates_quest`

Уникально:

- characters/houses/titles, governing institutions/seats/policies, claims, de-jure traditions,
  de-facto relation graph, bounded casus belli and succession/regency semantics;
- project-owned daily resolution phases, persistent order/interruption rules and strategic army-
  encounter/autoresolve/duel semantics;
- heroic readiness/recognition, deeds, witnesses, evidence, competing recognized myth versions and
  rights already granted from a version;
- terra-incognita/map-trade/report-delivery semantics and the rule that marching actors remain in an
  origin province without acting as ordinary local residents;
- uneven province/history grammar, wonders/great works and Apate archetype bindings;
- encounter stakes, player-side selection and political outcome interpretation shared by manual and
  abstract resolution.

Переиспользуемо:

- closed-surface topology, projection and globe picking;
- immutable generated world package;
- knowledge/provenance/filtered-view tooling;
- calendar/multi-cadence campaign host;
- persistent workflow/route inspection, logical identity and tactical reconcile audit;
- morphology-heavy localization fixtures.

### `bandit_in_the_shell`

Уникально:

- city/faction/crime/investigation model;
- jurisdiction semantics;
- crowd promotion;
- traffic and urban operation rules.

Переиспользуемо:

- dense 3D streaming;
- fact provenance tools;
- perception/debug overlays;
- crowd/animation/render LOD mechanisms.
- short replay capture/checkpoint tooling;
- opt-in telemetry and screenshot-sharing platform shell.

### `party_adventure`

Уникально:

- world grammar and central situation;
- POI/event constraints;
- companion/order semantics;
- world retirement/progression.

Переиспользуемо:

- deterministic generation host;
- artifact cache;
- sparse overrides;
- persistent event instances;
- region residency.

### `submarine_coop`

Уникально:

- nested vessel/world frames policy;
- compartment topology;
- flooding/pressure/air formulas;
- signature/sensor model;
- repair/error semantics.

Переиспользуемо:

- nested transform/velocity tools;
- bounded graph solver host;
- conservation diagnostics;
- audio portals/radio/VoIP;
- authoritative long-action transaction.

### `mmo_planet_shooter`

Уникально:

- literal planet coordinate/topology;
- rail/logistics economy;
- strategic operation allocation;
- cell deployment policy;
- optional neural scorer.

Переиспользуемо:

- networking only as a general engine/platform family;
- authority epoch/handoff;
- distributed transaction/journal;
- interest-management tooling;
- content-addressed procedural artifacts.

## Dependency graph

```text
fingerprints + state hashes
  -> save envelope
    -> migrations/slots
    -> replay/checkpoints
    -> calendar/workflows
    -> logical ids/materialization

stable records + owner sections
  -> knowledge/observation envelopes
    -> immutable filtered views
      -> evidence-backed planning/explanations
      -> truth-vs-view inspector and observation replay

grid/geometry + deterministic publish
  -> versioned layered fields
    -> brush/dirty/basic field algorithms
      -> flow/congestion/local avoidance
      -> heatmap/provenance tooling

transform/scene
  -> physics queries
    -> character/camera
    -> navigation
    -> audio occlusion
    -> network prediction

resources + external jobs
  -> standard format adapters/cooked artifacts
  -> generator native-tool registry + deterministic Lua glue
    -> typed pass graph
      -> provenance/validation/bounded repair
        -> sealed world package
  -> artifact cache
    -> world residency
      -> simulation LOD/materialization
      -> render/nav/collision streaming

closed-surface topology primitive
  -> APQ planet/province/history passes
    -> sealed immutable world package
      -> projection + globe picking/map-mode/terra-incognita presentation

typed commands + faults
  -> preview/results
  -> project genre kernels
  -> replication protocol

locale tables + form/tag compiler
  -> compiled text program
    -> shaping/bidi/font fallback/layout cache
      -> UI glyph/styled/inline-object runs

headless host + diagnostics
  -> dedicated server
    -> ENet UDP data plane
    -> HTTPS master/rendezvous/key bootstrap
      -> encrypted session
      -> NAT punch / Steam or third-party relay fallback
        -> replication/prediction/reconnect
  -> autonomous content/statistical runs
  -> every authoritative tier

third-party HTTP/TLS adapter
  -> networking HTTPS master control plane
  -> opt-in telemetry
  -> screenshot/provider sharing

render readback + image encode
  -> screenshot preview/consent
    -> provider share adapter

generator provenance/issues
  -> generated-content views
    -> early generator/artifact validation

stable scene/resource/nav/physics contracts
  -> full level authoring tools
```

## Практический порядок развития проектов

Это авторский порядок перехода от одного игрового проекта к следующему. Он основан не
только на количестве движкового долга, но и на ясности конечной картинки, понимании
реализации и уже накопленных заготовках. Поэтому это более полезный порядок производства,
чем рейтинг проектов по абстрактной технической сложности.

1. `cardgame`.
2. `submarine_coop`.
3. `tower_crawler`.
4. `zerg_brain`.
5. `apates_quest`.
6. `medieval_hero_manager`.
7. `party_adventure`.
8. `commander_simulator`.
9. `mmo_planet_shooter`.
10. `bandit_in_the_shell`.

Причины считать этот порядок основным:

- у более ранних проектов лучше определены игровой образ, границы первого среза и
  ожидаемый результат;
- `apates_quest` имеет ранний прототип, поэтому часть продуктовых и архитектурных
  вопросов уже можно проверять не только по дизайн-документу;
- ранний `submarine_coop` намеренно поднимает 3D, физику, звук и co-op раньше, чем это
  следовало бы из минимального графа движковых зависимостей: эти затраты оправданы ясным
  проектным ориентиром и затем окупятся в следующих 3D-проектах;
- позднее место проекта не означает, что его отдельные дешёвые playground-срезы нельзя
  использовать раньше для проверки общей системы.

Порядок задаёт приоритет полноценных project slices, но не требует заканчивать одну игру
до начала любой работы для следующей. Переиспользуемый движковый контракт следует выносить
тогда, когда его потребовал текущий проект и найден хотя бы второй правдоподобный consumer.

## Технический порядок proof consumers

Ниже — не альтернативный порядок разработки игр, а последовательность небольших
headless/playground-проверок, минимизирующая число одновременно неизвестных движковых
контрактов. Такой proof может быть сделан задолго до полноценного среза соответствующего
проекта.

1. `cardgame`: завершить run/save/profile/UI и headless GOAP run-statistics fixture. Это самый короткий путь проверить долговечность и автономные прогоны поверх уже готового combat kernel.
2. `tower_crawler`: второй discrete consumer для `turn_pipeline`/`resolve`; не обобщать grid до рабочего боя.
3. `commander_simulator`: маленькая headless graph mission как первый строгий consumer filtered knowledge views, persistent plans and truthful explanations.
4. `zerg_brain`: layered-field/mass-flow playground с 1k proxy actors, затем scale ladder; не начинать с production battle rendering.
5. `medieval_hero_manager`: calendar, workflow, multi-cadence and materialization в headless форме.
6. `apates_quest`: `APQ-01..05` — fixed-graph daily campaign bridge: persistent route orders,
   one title/institution rule chain, holder knowledge/report transfer, multi-phase army encounter and
   headless materialization/reconcile. Затем durability batch; spherical package/globe и TBT — отдельные proofs.
7. Engine 3D laboratory: transform/physics/controller/animation/nav без полной игры.
8. `party_adventure`: generation/artifacts/residency на более ограниченном мире.
9. `bandit_in_the_shell`: плотная authored 3D city/action integration, multi-LOD simulation, short replay and optional web/share shell.
10. `submarine_coop`: networked co-op, nested frames, graph solvers, advanced audio.
11. `mmo_planet_shooter`: distributed world only after all lower contracts have measured proof.

Этот технический порядок не задаёт, какую игру «нужно делать». Он лишь минимизирует число
новых неизвестных в одном проверочном срезе и подчинён практическому порядку проектов выше.

Параллельные infrastructure tracks:

- `LOC-01..03` можно начинать до 3D: locale resources/compiler нужны любому UI consumer;
- `AST-01/02` нужны до стабилизации texture pipeline, `AST-03/04` — перед 3D scene/animation;
- `GEN-01..05` формируют общий generator contract; `GEN-06..08` остаются reference pipelines проектов;
- `KNW-01..03` лучше доказывать на `CMD`, затем применять к `APQ/MHM/BITS`, не наоборот;
- `FLD-01/02` можно начать как headless `ZB` fixture независимо от 3D; `FLD-03` требует scale proof;
- `PLN-01` даёт topology маленькому `APQ` package; затем `GEN-08` сохраняет package, и только после
  этого `PLN-02/03` строят projection/picking/globe. `MMO` не должен диктовать первому proof distributed requirements;
- `UI-00` строится прямо на provenance/issues первого generator consumer и предшествует full editor;
- `HLS-01` проектируется прежде всего под `NET-00` dedicated server, затем переиспользуется batch/content runners.

## Ближайший практический пакет

Ближайший пакет — не набор независимых foundation-задач, а одна painter campaign:

1. дорастить запускаемый baseline `PF01`: room/free camera/HDR+depth готовы; остаются camera rail и target viewer;
2. bounded tile-light data и compute assignment готовы в первом варианте; добавить overflow diagnostics;
3. получить visible simple-forward/Forward+ comparison, heatmap, overflow и timings;
4. после DoD `PF01` перейти к отдельной `PF02_shadows`;
5. затем независимо развивать `PF03_post_processing` и `PF04_stencil_effects`;
6. собрать выбранные стабильные части в `PF05_submarine_light_room` и `PF06_party_environment`.

`FND-02`, persistence, module profiles, command shell, headless host, localization, knowledge, calendar и
layered fields остаются важным dependency pool. Они возвращаются в active work только вместе с
playground/project slice, которому дают конечный результат. Это не меняет их horizon priority, но
убирает случайное переключение между несвязанными маленькими контрактами.

## Критерии переноса project-кода в engine

Перед переносом задать пять вопросов:

1. Есть ли второй consumer с той же семантикой lifetime/order?
2. Можно ли назвать типы без слов конкретного жанра?
3. Сохраняется ли механизм без project component/resource definitions?
4. Есть ли focused contract test, который не запускает всю игру?
5. Не начинает ли существующая библиотека владеть чужой ответственностью?

Если хотя бы два ответа отрицательные, код остаётся в проекте.

## Глобальные Definition of Done

Движковая feature считается готовой, когда:

- имеет одного явного owner;
- не хранит raw pointers в persistent/cross-thread work;
- задаёт deterministic ordering там, где порядок влияет на gameplay;
- имеет hard budgets and explicit overflow/fault policy;
- поддерживает headless путь, если feature содержит authoritative logic;
- имеет save/resume boundary либо явно объявлена derived/ephemeral;
- экспортирует diagnostics;
- покрыта focused tests;
- доказана хотя бы одним живым consumer;
- не переносит project semantics в generic namespace.

Production-ready network/streaming/persistence дополнительно требуют failure injection, corrupt input handling, recovery and soak tests.
