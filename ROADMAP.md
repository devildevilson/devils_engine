# devils_engine — рабочий план развития

Этот документ — оперативная очередь движка. Все незавершённые задачи находятся в верхней части,
завершённые задачи и закреплённые контракты — в архиве в самом низу.

`ROADMAP_ULT.md` отвечает на вопрос «что в итоге потребуется всем проектам»; этот файл отвечает на
вопрос «что можно брать в работу и закрывать». Проектная семантика остаётся в проектах: в engine
переносятся ownership, lifetime, ordering, budgets, diagnostics и переиспользуемые primitives.

Текущий integration playground — `subprojects/tile_frontier`; `subprojects/cardgame` — второй живой
gameplay consumer. Целевой внешний проект заполняет движковые registries/resources/scripts и добавляет
тонкую C++-обвязку, не копируя app lifecycle, threading, loading, persistence или tooling infrastructure.

## Легенда

| Сложность | Ориентир | Смысл |
| --- | --- | --- |
| `S` | 2–5 рабочих дней | локальный контракт, adapter, diagnostics или focused tests |
| `M` | 1–3 недели | ограниченная подсистема с первым consumer и тестами |
| `L` | 1–2 месяца | новый owner/kernel либо несколько связанных подсистем |
| `XL` | 3–6 месяцев | крупная программа с runtime, resources и tooling |
| `XXL` | 6–18+ месяцев | отдельное направление разработки и эксплуатации |

`project-first` означает: первый implementation живёт в проекте, а в engine переносится только после
второго consumer либо когда уже доказана чистая общая граница.

## Ближайшие задачи, которые можно закрыть

Это не перечень самых больших возможностей движка, а ограниченные slices с понятным Definition of Done.
Порядок внутри таблицы рекомендуемый; одновременно лучше держать не более двух задач.

| Порядок | ID | Результат закрытия | Сложность | Почему сейчас |
| --- | --- | --- | --- | --- |
| 1 | `RES-01` | demiurg экспортирует canonical ordered module list и fingerprint; порядок покрыт тестом override-модулей | `S–M` | prerequisite save/replay/network handshake и уже нужен replay-плану |
| 2 | `UTL-02` | atomic replace helper: temp file, flush, rename, recovery/error result; есть failure-path tests | `S–M` | маленькая общая основа для settings/save/artifact index |
| 3 | `AUD-01` | active sound task удерживает resource data до остановки/завершения; unload race покрыта тестом | `M` | закрывает конкретный lifetime-риск в существующем runtime |
| 4 | `FSM-01` | inspector/serialization view показывает state, candidate transition, guards и settle result | `S` | маленький самостоятельный tooling slice над стабильным `mood` |
| 5 | `CAT-03` | phase metadata/write policy регистрируются декларативно и доступны diagnostics | `S–M` | упрощает последующий общий trace UI без изменения executor semantics |
| 6 | `CAT-01` | единый bounded statistics service принимает phase timing/rejection/overflow и показывает first divergence | `M` | объединяет уже существующие catalogue domains и бюджеты |
| 7 | `SIM-04` | у broker-каналов видны capacity, high-water mark, rejected/dropped и stalled-consumer diagnostics | `M` | полезно текущему multithreaded `tile_frontier` и будущему dedicated host |
| 8 | `ACT-03` + `ACT-04` | command/preview/rejection имеют versioned envelope, typed code, loc-key, parameters и state token | `M` | нужен UI-командам без dry-run мутации мира |
| 9 | `LOC-01` + `LOC-02` | загружаются locale manifest/table, работает fallback и module override, coverage проверяется headlessly | `M–L` | первый вертикальный срез новой локализации без shaping и сложных forms |
| 10 | `GEN-01` + `GEN-02` | typed pass/artifact descriptors и registry исполняют два dummy passes с проверкой входов/выходов | `L` | минимальный proof нового generator contract до Lua и реальных карт |

После каждого пункта обновлять этот список: закрытая строка переносится в нижний архив, а её место
занимает следующий ограниченный slice из полного backlog.

## Открытый системный backlog

### `libs/aesthetics` — ECS, snapshots и долговечное состояние

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `ECS-01` | Migration старых component schemas | `L` | versioned migration registry; конкретные migrations принадлежат owner-компонентам |
| `ECS-02` | Sectioned owner-level save format поверх `dump_world` | `L` | campaign/run/profile owners сохраняют независимые bounded sections |
| `ECS-03` | Logical persistent ids отдельно от resident ECS ids | `L` | стабильный logical id и проверяемый logical↔resident mapping |
| `ECS-04` | Compact delta/change tracking | `L–XL` | dirty/change epochs без превращения ECS в netcode |
| `ECS-05` | Transaction journal | `L` | bounded committed operations для persistence/recovery consumers |
| `ECS-06` | Replication codecs и baselines | `XL` | отдельный owning networking layer использует ECS primitives |
| `ECS-07` | Materialization registry | `L–XL` | project codecs materialize/dematerialize logical records |
| `ECS-08` | Явная classification `authoritative / derived / ephemeral` | `M` | metadata, validation и save/load policy |

### `libs/simul` — runtime, workflow и процессы

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `SIM-01` | Persistent scene/run stack | `L` | serializable scene ids/cursors; presentation objects не попадают в save |
| `SIM-02` | Calendar scheduler | `L` | canonical due ordering, cancellation, catch-up и budgets |
| `SIM-03` | Generic persistent workflow records | `M–L` | serializable stage/cursor owner рядом с маленьким `turn_pipeline` |
| `SIM-04` | Delivery/backpressure diagnostics всех broker-каналов | `M` | capacity/high-water/rejection/stall metrics |
| `SIM-05` | Reusable checkpoint/save coordinator | `M–L` | owner quiescence, section collection, atomic commit and resume |
| `SIM-06` | Server process/session topology | `L–XL` | dedicated executable shell, sessions, shutdown/restart boundaries |
| `SIM-07` | Cross-process transport seam | `L` | transport adapter рядом с broker, но broker не становится socket API |

### `libs/demiurg` — ресурсы, модули и artifacts

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `RES-01` | Export canonical ordered module list and fingerprints | `S–M` | stable save/replay/network handshake input |
| `RES-02` | Per-section resource schema migration metadata | `M` | version/fingerprint/migration owner для resource sections |
| `RES-03` | Priority, cancellation и budgets для procedural CPU artifact jobs | `L` | bounded job lifecycle через существующие staged transitions |
| `RES-04` | Content-addressed artifact cache | `L` | key/index/storage/eviction и corrupt-entry recovery |
| `RES-05` | Explicit artifact epochs | `M` | stale results не публикуются после invalidation/reload |
| `RES-06` | Общий importer/runtime-resource contract | `M–L` | source → canonical CPU intermediate → cooked/runtime resource |
| `RES-07` | KTX/KTX2 texture resource | `L` | mip/layer/cubemap metadata, BC7 upload, fallback/transcode policy |
| `RES-08` | Canonical 3D mesh/scene/animation formats | `L–XL` | единый CPU intermediate независимо от конкретного importer |
| `RES-09` | Dependency/resource graph inspection UI | `M–L` | origins, overrides, edges, states, fingerprints and failures |
| `RES-10` | Safe unload contract для active sound data | `M` | координируется с `AUD-01`; ресурс не исчезает у active task |

### `libs/catalogue` — trace, budgets и deferred execution

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `CAT-01` | Общий service/UI для phase timings, rejection/overflow и first divergence | `M` | bounded сбор и единый inspection API |
| `CAT-02` | Bounded codecs для новых доказанных signatures | `M` | добавлять только вместе с реальным consumer; не делать generic serializer |
| `CAT-03` | Удобная регистрация phase metadata/write policy | `S–M` | identity, reads/writes, strategy и budget видны tools/tests |

Не добавлять в `catalogue`: replay format, RPC/network transport, world persistence и gameplay event ontology.

### `libs/act` — gameplay function and command seam

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `ACT-01` | String marshalling native↔devils_script | `M` | bounded ownership и lifetime, без ad-hoc Lua strings |
| `ACT-02` | Object/vector marshalling и только необходимые custom codecs | `M–L` | реализовать по фактическим spawn/player/preview signatures |
| `ACT-03` | Versioned generic command envelope around project intents | `M` | common header/status; payload остаётся project-owned |
| `ACT-04` | Structured rejection/preview result | `M` | reason loc-key, params, state version and optional preview refs |
| `ACT-05` | Capability metadata для authoring/security scopes | `M` | pure/effect/read/write/allowed-root metadata; enforcement tests |
| `ACT-06` | Stable disk/network codecs для command envelopes | `L` | отдельный persistence/network owner кодирует только доказанные payload schemas |

`ACT-06` строится в owning persistence/network layer, а не внутри registry.

### `libs/acumen` — GOAP и многоуровневое планирование

| ID | Задача | Сложность | Ownership |
| --- | --- | --- | --- |
| `GOAP-01` | Goal candidate selection and utility explanation | `L` | project-first; в engine позже bounded scorer/trace |
| `GOAP-02` | Duration/commitment/interruption owner | `L` | отдельный persistent workflow, не A* node |
| `GOAP-03` | Spatial action generation | `L` | project-first над navigation/spatial queries |
| `GOAP-04` | Multi-agent reservations | `XL` | project-first; общими могут стать token/lease primitives |
| `GOAP-05` | Hierarchical/strategic planning | `XL` | project-first (`MHM`/`BITS`); политика не входит в `acumen` core |
| `GOAP-06` | Budget inspector | `M` | expanded nodes/cache hits/cutoff/replan explanation |

### `libs/mood` — FSM

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `FSM-01` | Diagnostics/serialization views | `S` | state, transitions, guards/actions, settle и failures |
| `FSM-02` | Capability/tooling around state graphs | `M` | graph browser, unreachable/ambiguous transition validation |

Persistent multi-day event или repair action хранится в `SIM-03`, а не внутри `mood`.

### `libs/resolve` — нейтральное разрешение взаимодействий

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `RSL-01` | Generic diagnostic export | `S–M` | provenance/frontier/fault/outcome summaries без project variants |
| `RSL-02` | Дополнительные neutral route/outcome helpers | `M` | только после второго consumer (`tower_crawler`) |
| `RSL-03` | Common fault/report adapters | `M` | bounded host-facing diagnostics; grouping остаётся проектным |

Не добавлять в `resolve`: cards, initiative, grid movement, elements/status/death policy и realtime physics hits.

### `libs/prefab` — construction и materialization

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `PFB-01` | Logical persistent identity policy | `M` | prefab construction принимает/создаёт stable logical identity |
| `PFB-02` | Materialization/dematerialization codecs | `L` | project component rows ↔ logical record через registry |
| `PFB-03` | Validation tooling and dependency browser | `M` | inheritance/reference/callback origins and failures |
| `PFB-04` | Staged construction rollback при частичном failure | `M` | entity не остаётся частично построенной |

### `libs/utils` — базовые сервисные primitives

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `UTL-01` | Service ownership/versioning вокруг spatial containers | `M` | immutable snapshot/version/query facade без единой world semantics |
| `UTL-02` | Atomic file transaction helpers | `S–M` | temp/write/flush/replace/recover и explicit errors |
| `UTL-03` | Более ясные error/`std::expected` APIs | `M–L` | сначала file/background/resource paths, затем по consumers |
| `UTL-04` | Canonical priority queue serialization helpers | `M` | stable key/order/cancel token restore |
| `UTL-05` | Artifact cache/index primitives | `M` | content hash, metadata, atomic index and verification |
| `UTL-06` | Generation primitives: noise, Voronoi/Delaunay, flood/distance, graph transforms | `L–XL` | deterministic reusable algorithms, не world grammar |
| `UTL-07` | Canonical parallel group/filter/reduce для generation units | `L` | semantic ordering независимо от worker count |
| `UTL-08` | Canonical byte/hash comparison test utilities | `S` | serial-vs-MT and save/reload identity helpers |

### `libs/painter` — high-level rendering gaps

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `RND-01` | High-level 3D scene instance layer | `L` | typed instances, lifetime, snapshots and batches |
| `RND-02` | Transform/skinning buffers | `L` | frame-owned uploads and stable instance mapping |
| `RND-03` | Skeletal rendering | `L–XL` | bone palettes, bounds and skinned draw path |
| `RND-04` | Lighting/shadows | `XL` | tiered scene path; 2D/2.5D не платят полный cost |
| `RND-05` | Decals/transparency | `L–XL` | ordering, lifetime and material integration |
| `RND-06` | Visibility/occlusion | `XL` | CPU/GPU culling с diagnostics |
| `RND-07` | Material/mesh LOD and HLOD | `XL` | selection, residency, transitions and metrics |
| `RND-08` | Runtime-generated mesh lifecycle | `L` | CPU artifact → GPU publish/reuse/unload with epochs |
| `RND-09` | KTX/KTX2 compressed upload path | `L` | supported GPU blocks не декомпрессируются на CPU |
| `RND-10` | Canonical mesh/material/skeleton upload interfaces | `L` | importer-independent runtime boundary |
| `RND-11` | GPU timings/residency/debug capture | `M–L` | project/tool-readable metrics and captures |

### `libs/flow` — skeletal animation integration

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `ANM-01` | Adapter к выбранной сторонней skeletal-animation library | `L` | engine handles/resources поверх внешнего runtime |
| `ANM-02` | Skeleton/clip resources | `L` | canonical ids, validation, fingerprints and hot lifetime |
| `ANM-03` | Pose sampling | `M–L` | deterministic game-time input and render snapshot output |
| `ANM-04` | Blending/crossfade | `L` | bounded layers/masks and transition state |
| `ANM-05` | Root motion contract | `L` | animation delta ↔ character/physics authority |
| `ANM-06` | IK | `L–XL` | solver seam and project constraints |
| `ANM-07` | Animation graph | `XL` | resource graph, parameters, transitions and inspector |
| `ANM-08` | GPU bone matrices/skinning integration | `L–XL` | координируется с `RND-02/03` |
| `ANM-09` | Animation LOD | `L` | update/sample/skin cadence and bounds policy |

### `libs/input` — устройства и control schemes

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `INP-01` | Gamepad/joystick backend | `M` | canonical buttons/axes and focused tests |
| `INP-02` | Control contexts/schemes | `M–L` | stack/priority for character/vehicle/menu |
| `INP-03` | Device hotplug | `M` | stable device identity and reconnect policy |
| `INP-04` | Axis/deadzone/curve model | `M` | data-driven transforms and serialization |
| `INP-05` | UI focus navigation | `M` | directional focus, activate/back and context handoff |
| `INP-06` | Conflict-aware live rebinding | `M` | capture, conflict policy, feedback and save/reload |

### `libs/sound` — audio runtime

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `AUD-01` | Resource/task lifetime pinning | `M` | active voice удерживает data; unload race test |
| `AUD-02` | Parameterized semantic sound events | `M–L` | event id + typed parameters отдельно от file id |
| `AUD-03` | Map `(material, action/impact, context) -> sound event` | `M–L` | шаги/удары/трение; project supplies semantic materials |
| `AUD-04` | Priorities/virtual voices | `L` | bounded voice budget and deterministic grant policy where needed |
| `AUD-05` | EFX-like environment processing | `L–XL` | zones, sends, reverb, filters and obstruction parameters |
| `AUD-06` | Occlusion/portals/reverb integration | `L` | physics/scene queries feed audio environment |
| `AUD-07` | Per-listener mixes | `M–L` | split-screen/spectator/listener-specific buses |
| `AUD-08` | Capture device | `M–L` | device lifecycle, permissions and bounded buffers |
| `AUD-09` | Opus/VoIP/jitter | `XL` | codec, packet loss, jitter buffer and talk state |
| `AUD-10` | Network voice routing | `XL` | session/interest/privacy policy поверх networking |

### `libs/visage` и `libs/bindings` — UI/tool host

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `UI-01` | Gamepad focus/navigation | `M` | consumer `INP-05`, не второй input backend внутри UI |
| `UI-02` | World-space overlays/markers | `L` | stable marker ids, clipping and camera projection |
| `UI-03` | Picking/debug draw | `L` | render/world query results exposed read-only |
| `UI-04` | Large-list virtualization | `M` | stable rows, selection and bounded allocation |
| `UI-05` | DPI/layout units | `M–L` | px/dp/percent/relative sizing and resize restore |
| `UI-06` | Editor host/panels | `L–XL` | docking/panel lifecycle/transactions; full level editor later |
| `UI-07` | Generic data/schema/tree/table inspectors | `L` | reusable read-only models and validation issues |
| `UI-08` | Render compiled localized text | `L` | styled runs, inline objects, wrapping, clipping and hit spans |
| `UI-09` | Shaping/bidi/fallback-font cache | `L–XL` | key = locale/font/width/parameters; bounded invalidation |
| `UI-10` | Multi-context policy | `L` | UI/editor/headless contexts without global `nk_context*` |

Generator scripts используют отдельный headless Lua environment (`GEN-04`); UI-owned visage sandbox
не переиспользуется как mutating generation backend.

## Открытый legacy debt, не продублированный выше

Это незавершённые пункты старого `ROADMAP.md`, которые не вошли в перечень «Не хватает» из
`ROADMAP_ULT`. Они сохраняют низкий приоритет или consumer-gate, но остаются видимыми задачами.

### Gameplay/runtime/config

| ID | Задача | Сложность | Условие |
| --- | --- | --- | --- |
| `LEG-01` | Flag/modifier payloads и calendar/turn expiration | `M–L` | начинать с проекта, которому недостаточен `flag_set` countdown |
| `LEG-02` | Spawn-point filters/pick/cooldown/capacity | `L` | project-first; generic spawner manager только после второго consumer |
| `LEG-03` | Формальная schema живых и restart-only settings | `M` | apply/reject/restart-required result и UI indication |
| `LEG-04` | Ownership общего deferred phase pipeline | `L` | только после второго gameplay pipeline; не строить runtime DAG заранее |

### Дополнительный долг существующих библиотек

| ID | Задача | Сложность | Условие/результат |
| --- | --- | --- | --- |
| `ACT-07` | Config/module registration format для gameplay functions | `M` | только когда native building-block facade перестанет быть достаточным |
| `ACT-08` | Number unit/tag metadata | `M` | first consumer для money/distance/percent; не менять arithmetic type |
| `ACT-09` | Reusable target-selection helpers | `M` | project-first; не встраивать combat target policy в `act` |
| `ACT-10` | Metric recomputation budgets и cache policy | `M` | diagnostics сначала, persistent cache только с proof |
| `ACT-11` | Dynamic symbolic-state capacity | `M` | только если `bitset<256>` реально ограничит consumer |
| `ECS-09` | Debug contract structural mutation during iteration | `S` | loud checks/documented policy |
| `ECS-10` | Random/forward deletion optimization | `S–M` | только после профиля; возможен dense-owner sidecar |
| `ECS-11` | Auto-create allocator для `view<T>` | `S` | убрать ручной precondition без скрытой гонки |
| `CAT-04` | Multi-participant reserve/write-set | `XL` | дальний backlog после реального atomic multi-target consumer |
| `CAT-05` | Standard scalar/value rendering in trace | `S` | bool/integer/float/enum/string/view bounded formatting |
| `RES-11` | Immutable packaged engine resource module | `L` | после стабилизации resource/app schemas |
| `RES-12` | Assets coordinator fan-out в общий worker pool | `M–L` | запрет nested dispatch/wait, join-before-publish and shutdown tests |
| `RES-13` | Strict zip-before-parse and append-collision contract | `S` | canonical diagnostics вместо skip ambiguity |
| `ANM-10` | 2D animation time/data model and ECS playback components | `M` | вместе с первым реальным 2D asset consumer |
| `ANM-11` | Mapping mood state → flow animation | `M` | переходы/crossfade и resource validation |
| `ANM-12` | Animation callback thread boundary | `S–M` | presentation callback vs gameplay intent/effect |
| `ANM-13` | 2.5D directional sprites | `M` | первый `MHM/MMO/TC` visual consumer |
| `ANM-14` | Flow loops/callback/thread/ECS tests | `S–M` | сопровождает `ANM-10..13` |
| `INP-07` | Event queue/frame snapshot contract | `M` | если raw callbacks перестанут удовлетворять consumers |
| `INP-08` | Remove static global input state | `L` | только при нескольких windows/input domains |
| `INP-09` | Events/key-name/state-machine tests | `S` | focused regression suite |
| `FSM-03` | Inline expressions/dotted names | `M` | только с config consumer; parser ambiguity tests |
| `FSM-04` | Explicit limit/parser-position diagnostics | `S` | guards/actions caps and idle-loop cap |
| `FSM-05` | Blocked/on-entry/on-exit/internal-transition tests | `S` | закрыть edge cases стабильного FSM core |
| `BLD-01` | Determinism flags as separate interface targets | `M` | fp/fast-math/denormals/FMA/precise policy |
| `BLD-02` | CPU/build/sanitizer/release presets and linkage audit | `M` | documented supported configurations |
| `RND-12` | GPU command coverage audit | `M` | убрать stubs только у реально используемых draw/dispatch/transfer paths |
| `RND-13` | Strict render `.tavl` schemas and graph validation | `M–L` | errors/examples/tests for buffers/textures/shader preparation |
| `RND-14` | Device-local draw groups and GPU indirect path | `L` | profiling-backed consumer |
| `RND-15` | Mips/arrays/MSAA/render-target role formats | `L` | согласовать с `RES-07` и resource validation |
| `SIM-08` | Runtime channel growth policy | `M` | после `SIM-04`; growth не должен скрывать overload |
| `SIM-09` | Stop-token/jthread-friendly low-level API | `S` | common cancellation path |
| `SIM-10` | Partial-init/shutdown and delivery tests | `S–M` | all optional worker combinations |
| `AUD-11` | PCM policy: подключить decoder либо удалить формат | `S` | no advertised empty decoder path |
| `AUD-12` | Semantic source type in play command | `S` | корректный volume bus вместо implicit `sfx` |
| `AUD-13` | Velocity/doppler feed | `M` | только после проверки positional audio в live 3D scene |
| `AUD-14` | Remove/archive OpenAL implementation | `S` | после окончательного закрепления miniaudio path |
| `AUD-15` | Start/after/underrun/device-fallback/snapshot tests | `M` | audio regression suite |
| `UTL-09` | Allocator/spatial/string-pool/compression/serializer tests | `M` | risk-oriented batches, не coverage ради coverage |
| `UTL-10` | Split heavyweight `core.h` and clarify serializer ownership | `M–L` | diagnostics/math/paths/unicode/crc boundaries |
| `UTL-11` | Reorganize thread/allocator/spatial public layout | `S–M` | после public/experimental audit; no API churn alone |
| `UI-11` | `screen_px_range` as push constant | `S` | remove runtime/global coupling |
| `UI-12` | Shared constexpr GPU API header for C++ and shaders | `M` | one verified layout/constant source |
| `UI-13` | Composite/heraldry image type | `L` | real project consumer and bounded layer model |
| `UI-14` | Demiurg resource as image source | `M` | stable resource handle and hot lifetime |
| `UI-15` | Full descriptor indexing beyond current small slot path | `L` | device capability/fallback policy |
| `UI-16` | Remove legacy `endf`/draw-resource/draw-stage API | `S` | migration complete and no live references |
| `UI-17` | Runtime UI reload | `M` | state/error/cache lifecycle |
| `UI-18` | Lua 5.4 closable-variable integration | `S–M` | deterministic cleanup of scoped UI resources |
| `UI-19` | Generic runtime debug overlay | `M` | consumer of catalogue/resource/broker diagnostics |

## Новые общие системы

### Локализация и compiled text

Локализация — отдельный engine concern. Проекты поставляют string tables, допустимые semantic tags и
parameter values, но не собственные parser, language-form resolver, shaping или layout pipeline.

| ID | Задача | Сложность | Результат |
| --- | --- | --- | --- |
| `LOC-01` | Locale manifest and fallback chain | `M` | canonical locale ids, fallback order and switch lifecycle |
| `LOC-02` | Таблицы `loc_key -> source template` с module override | `M` | demiurg resource, stable handles, hot reload and origin diagnostics |
| `LOC-03` | Compile-time validation locales/keys/placeholders | `M` | coverage report and type-compatible placeholders |
| `LOC-04` | Plural/cardinal/ordinal/select/gender/case forms | `L` | rule backend + locale data; form absence fails predictably |
| `LOC-05` | Safe bounded tag grammar | `L` | whitelist, depth/output budgets and typed tag payloads |
| `LOC-06` | Compiled pointer-free text program | `L` | immutable instructions/runs/params suitable for UI cache |
| `LOC-07` | Unicode normalization, shaping, bidi, line breaking and font fallback | `XL` | adapters over established Unicode/text libraries |
| `LOC-08` | Typed parameter binding without ad-hoc gameplay strings | `M–L` | values bind to compiled program by schema |
| `LOC-09` | Locale switch and cache invalidation | `M` | epoch-based invalidation across localization and UI caches |
| `LOC-10` | Missing-key/tag/form diagnostics and pseudo-localization | `M` | headless validation plus runtime debug views |

`UI-08/09` являются обязательным rendering consumer этой системы: UI получает compiled string, а не
повторно разбирает локализованный текст в каждом widget.

### Контракт генераторов

Общий генератор — typed pass host и toolkit, а не единая world grammar. C++ предоставляет bounded
алгоритмы и типы artifacts, Lua связывает проходы, проект определяет pipeline, semantic schemas,
constraints и repair policy.

| ID | Задача | Сложность | Результат |
| --- | --- | --- | --- |
| `GEN-01` | Typed artifact/pass descriptors | `M–L` | ids, schemas, required/optional inputs, outputs, versions and budgets |
| `GEN-02` | Pass/tool registry and execution host | `L` | validate → prepare → execute → validate → publish lifecycle |
| `GEN-03` | Reusable C++ building-block facade | `L` | noises, fields, Voronoi, graphs, grouping/filter/reduce through stable registration |
| `GEN-04` | Dedicated deterministic headless Lua environment | `L` | generator-only API, instruction/time/memory budgets, no UI globals |
| `GEN-05` | Lua pipeline glue and typed artifact handles | `L` | scripts compose passes without owning native artifact memory |
| `GEN-06` | Pass templates | `L` | raster/graph/unit/volume templates with explicit iteration and ordering |
| `GEN-07` | Deterministic scheduler | `L–XL` | semantic seal/order, deterministic RNG domains and serial-vs-MT identity |
| `GEN-08` | Provenance, cache keys and invalidation | `L` | seed/module/pass/tool/input fingerprints trace every artifact |
| `GEN-09` | Validation, bounded repair/rewind and failure reports | `L–XL` | failed constraints do not silently publish broken artifacts |
| `GEN-10` | Generated-content inspection tooling | `M–L` | simplified maps/graphs/heatmaps, pass timings and batch reports |
| `GEN-11` | 2D region/planet reference pipeline | `XL` | project-first `MMO`; continents→climate→biomes→regions→history/politics |
| `GEN-12` | 3D adventure reference pipeline | `XL–XXL` | project-first `PA`; tasks→graphs→terrain/modules→mesh/collision/nav artifacts |

Первые дополнительные consumers: `TC` — small floor + large semantic quest graph; `SC` — streaming
cave/route segments. `BITS`, `CG` и `MHM` не получают generator dependency без реального требования.

## Остальные открытые cross-system программы

| ID | Задача | Сложность | Условие старта/граница |
| --- | --- | --- | --- |
| `PST-01` | Durable save envelope, slots, metadata and atomic commit | `L` | `UTL-02`, `RES-01`, owner sections |
| `PST-02` | Schema migration registry and save inspection tool | `L` | `ECS-01/02`, `RES-02` |
| `RPL-01` | Versioned intent/input log + exact `game_delta_ticks` | `M–L` | camera becomes separate intent provider |
| `RPL-02` | Checkpoint and resource/config/build fingerprints | `M–L` | `SIM-05`, `RES-01`, `PST-01` |
| `RPL-03` | First-divergence hash/bisect diagnostics | `M` | `CAT-01`, `UTL-08` |
| `RPL-04` | Short replay artifact and optional presentation track | `L` | после `RPL-01..03`; первый продуктовый consumer — `BITS` |
| `TIME-01` | Local/per-entity time multiplier policy | `M–L` | только с consumer; определить влияние на physics, flags, AI and replay |
| `AI-01` | Runtime GOAP/FSM profile switch | `L` | deterministic tick boundary, capability validation and cache invalidation |
| `CFG-01` | Save simple UI Lua state | `M` | bounded data-only table, nested tables, no functions/userdata |
| `API-01` | Header documentation and public/experimental API audit | `M–L` | проход по owner libraries; старые dispatcher/loader remnants архивировать отдельно |

Replay остаётся неактивной большой вертикалью до трёх prerequisites: ordered runtime module list,
camera как отдельный intent provider и точная запись `game_delta_ticks` каждого sim tick.

## Открытые project-first задачи `cardgame`

Этот блок остаётся в `ROADMAP.md`, потому что `cardgame` — текущий второй gameplay consumer общих
контрактов. Контентный объём сюда не входит.

| ID | Задача | Сложность | Возможная движковая дельта |
| --- | --- | --- | --- |
| `CG-01` | Architecture/code review существующего combat kernel перед следующей вертикалью | `S` | зафиксировать границы `resolve`/project и список доказанных extensions |
| `CG-02` | Resource-loaded card → beats → effect/targeter schema | `M–L` | demiurg schema/validation stress; semantic schema project-owned |
| `CG-03` | Реальные follow-up rule lists и status pulse programs | `M–L` | только neutral helpers после доказанного повторения |
| `CG-04` | Остальные typed emitters/outcomes и content validation | `M–L` | расширение typed envelope остаётся project-owned |
| `CG-05` | Ресурс инициативы и preview state `S+1` | `L` | общий command preview shell только после proof |
| `CG-06` | UI interaction toolkit: selection, targeting, reorder, FSM state | `L` | reusable list/focus primitives в `visage` |
| `CG-07` | Стихийные метки и reaction table | `M` | project data/rules над существующим resolver |
| `CG-08` | Battlefield tooltip and 2D depth scene | `M–L` | picking/tooltip/depth primitives для painter/visage |
| `CG-09` | Единый player-facing preview | `L` | использует `ACT-03/04`, но world projection project-owned |
| `CG-10` | Run map/scene workflow | `L` | persistent workflow и reusable hex/grid math после proof |
| `CG-11` | Mid-run resume | `M–L` | первый consumer `PST-01`/`SIM-05` |
| `CG-12` | Persistent profile/collection | `M–L` | profile owner и idempotent collection transactions |
| `CG-13` | Headless player policy/GOAP for statistical runs | `M–L` | generic scenario/statistics harness после live proof |

Кросс-платформенный fixed-point для `cardgame` снят с roadmap: требуется воспроизводимость только в
пределах одной ОС и одного build fingerprint.

## Правила выбора следующей задачи

1. Сначала устранять correctness/lifetime risk в уже работающей системе.
2. Затем закрывать маленький контракт, который разблокирует минимум два следующих шага.
3. Крупный kernel начинать только с первого живого consumer и focused laboratory scene/test.
4. Не переносить project semantics в engine ради красивой абстракции.
5. Для каждого изменения требовать bounded inputs, ownership/lifetime, deterministic ordering там, где
   это gameplay state, diagnostics и failure-path tests.

## Архив завершённых задач и закреплённых контрактов

Ниже только закрытые slices. Если у подсистемы появляется новый gap, он добавляется в верхний backlog,
а уже доказанный контракт здесь не переписывается задним числом.

### Базовые project/engine seams

- [x] `act::registry` закреплён как typed facade native/devils_script gameplay-функций.
- [x] `act::building_blocks` стал единой декларативной точкой регистрации native building blocks.
- [x] `aesthetics::serial` хранит authoritative components; derived state восстанавливается после load.
- [x] FSM/GOAP/prefab/script resources загружаются через demiurg без production C++ fallback.
- [x] `prefab` поддерживает rows, inheritance, references, callbacks и config-defined spawn.
- [x] Generic stats accessors и `aesthetics::flag_set` с game-time expiration доказаны consumer-тестами.
- [x] Relations закреплены как full versioned entity-id fields с `exists()`-check и без auto-cleanup.
- [x] Событийная граница разделена на deferred calls, intra-tick message buffers, replayed intents и
  presentation side outputs; отдельной event-entity нет.

### App shell, runtime и resources

- [x] Window/settings/UI lifecycle и optional render/sound/assets workers вынесены в `simul::game_host`.
- [x] Runtime scene composition стала config-driven через state и scene manifests.
- [x] `tile_frontier::simulation.cpp` сокращён до host/wiring; project policy принадлежит
  `tile_frontier_game` и project systems.
- [x] Broker заменил старый cross-actor dispatcher/actor-ref path.
- [x] Headless и no-render/no-sound topologies поддерживаются общим runtime.
- [x] demiurg поддерживает module discovery/override, list resources, dependency gating, stable handles,
  staged CPU/external loading и Lua read-only access.
- [x] GPU texture/mesh hot unload имеет fence-safe lifetime, bindless slot cleanup и reuse tests.
- [x] Snapshot load уведомляет queries; созданные до load query/lazy-query сразу видят восстановленный мир.

### Время, ввод, UI и звук

- [x] Game time использует exact integer `game_delta_ticks`; speed задаётся рациональным multiplier.
- [x] Pause/time scale проходят через общий timeline contract; gameplay countdowns используют game time.
- [x] Settings-owned keyboard/mouse action bindings поддерживают live reload/save.
- [x] Первый player intent queue и anti-spam dedup доказаны `spawn_food` consumer-ом.
- [x] Read-only UI seam к pure act predicates/numbers/strings/describe работает без mutating Lua backend.
- [x] UI instruction/wall-time/GC/Nuklear convert budgets и failure streak policy вынесены в settings.
- [x] Positional/non-positional sound, listener, attenuation и live volume groups работают.
- [x] `rng_state + int` и отключаемый sound worker с динамическим reserved-worker count закрыты.

### MT execution и diagnostics foundation

- [x] `catalogue::mt` поддерживает collect/elect/structural strategies, bounded inline journals,
  semantic seal/order и deterministic 1/2/4/8-worker tests.
- [x] Identity domain отделён от reusable strategy policy; добавлены neutral presets.
- [x] `aesthetics::system_runner` объединяет независимые systems под один pool barrier без скрытого DAG.
- [x] `tile_frontier` cognition/effects и integration+drives используют доказанные MT paths.
- [x] kD-tree deterministic parallel build снял первый измеренный bottleneck без изменения world hash/bytes.
- [x] Catalogue domains уже собирают локальные timings; открытый общий service/UI вынесен в `CAT-01`.

### `cardgame` и общий resolver

- [x] `simul::turn_pipeline` и presentation checkpoints поддерживают animated/headless execution и resume.
- [x] `libs/resolve` владеет pointer-free provenance, bounded journals, target grouping, host-paced frontier,
  neutral damage route и hard non-recursive retaliation contract.
- [x] Cardgame grouped effect program, stable target snapshots/bindings и typed outcome envelope работают.
- [x] Damage route разделяет shield и residual health leaves без double resistance или synthetic outcome.
- [x] Retaliation исполняется отдельным nested cue/result/finished cycle и не рекурсирует по lineage.
- [x] devils_script combat effect, retaliation, execution report и follow-up scopes работают через stable
  resource hashes и pointer-free stores.
- [x] Action report visibility/reset, player/enemy party passes, ActorStateTick, theft, forced execution и
  resume boundaries покрыты scenario tests.

### Снятые или сознательно ограниченные направления

- [x] `catalogue` не является replay, serializer, RPC или netcode layer.
- [x] `resolve` не владеет card/grid/element/status/death semantics.
- [x] Lua остаётся UI/guest backend; deterministic generator Lua получит отдельный environment.
- [x] Cross-platform fixed-point для текущего `cardgame` снят; достаточен same-OS/same-build determinism.
