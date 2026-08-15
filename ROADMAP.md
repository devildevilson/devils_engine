# devils_engine — рабочий план развития

Этот документ — каталог движковых контрактов и зависимостей для активных playground/project slices.
Он больше не является очередью, по которой следует брать мелкие задачи сверху вниз. Завершённые
задачи и закреплённые контракты остаются в архиве в самом низу.

`ROADMAP_ULT.md` отвечает на вопрос «что в итоге потребуется всем проектам»; `PLAYGROUNDS.md` выбирает
текущую campaign и наблюдаемый результат; этот файл отвечает на вопрос «какие engine contracts могут
понадобиться для его закрытия». Проектная семантика остаётся в проектах: в engine переносятся ownership,
lifetime, ordering, budgets, diagnostics и переиспользуемые primitives.

`subprojects/tile_frontier` остаётся главным сложившимся integration playground, а `subprojects/cardgame`
— вторым живым gameplay consumer. Текущий новый фокус — painter campaign под
`subprojects/playgrounds/`, начиная с `PF01_forward_plus`.

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

## Активный срез

Campaign: [Painter visual stack](PLAYGROUNDS.md#текущий-фокус--painter-visual-stack).

Текущая лаборатория: [`PF02_shadows`](subprojects/playgrounds/PF02_shadows/README.md).

| Этап лаборатории | Наблюдаемый результат | Возможные engine blockers |
| --- | --- | --- |
| shadow baseline | directional map + движущиеся casters + depth viewer | минимальные части `RND-01/02/11/13/15/22` |
| spot atlas | несколько light views в одном target, региональные viewport/scissor/bias и caster spans | `RND-06/12/13/22` |
| visible diagnostics | bias/occupancy/culling overlay, GPU timings и atlas lifetime | `RND-11/22` |
| shadow quality | contact/sloped bias fixtures + hard/PCF/Poisson/spot-PCSS A/B | `RND-22` |

Срез 2026-08-14: room/free-camera/HDR/reversed-Z depth, 96 движущихся point lights в решётке `8 × 4 × 3`
и config-defined depth → compute tile assignment → Forward+ → present уже запускаются. Сцена имеет нулевой
ambient и жёсткий radius cutoff. Попутно исправлен общий контракт
`painter` config constants — parsed defaults теперь активны до первого кадра, поэтому constant dispatch больше
не превращается в `(0,0,0)`; staging runtime writes по-прежнему публикуются только через `update_event()`.
Срез 2026-08-15: light culling переведён с одного последовательного invocation на tile на workgroup `8 × 8`;
near-plane/camera-crossing spheres больше не исчезают при наклоне камеры. Screen-space projected bounds были
отброшены после false negatives на боковых видах: финальный тест идёт напрямую против четырёх view-space
плоскостей tile-frustum. Их scale берётся из чистой projection matrix: прежнее чтение диагонали
`projection * view` делало tile bounds зависимыми от yaw/pitch и создавало 16-pixel cutoff полосы. Tile
capacity поднята с 64 до всех 96 лабораторных lights, чтобы до появления явного overflow counter результат
не зависел от молчаливого усечения. Dispatch следует реальному viewport вместо постоянных 120×68 groups.
PF01 снова использует mailbox-first present policy и отдельный common deadline/`sleep_until` limiter на
60 FPS; `--uncapped` отключает только limiter и оставляет mailbox stress mode. Первый Visage-overlay через
обычные `draw_ui` buffers показывает описание/controls и сглаженные FPS/frame time.
`RND-25` закрыт: named descriptor sets автоматически добавляют свои usages в step barriers/read-write masks,
одинаковые usages дедуплицируются, конфликтующие fail-fast; PF01 разделяет read/write SSBO descriptor views и
больше не повторяет set resources вручную. `painter_shader_prepare_test` закрепляет inference.

Аудит `RND-24`: основной runtime уже независимо пейсит main/render/sound/assets workers через
`simul::advancer` absolute deadline + `sleep_until`, а Painter выбирает MAILBOX с гарантированным FIFO fallback.
Остаток задачи: явная platform/user fallback policy (включая IMMEDIATE), публикация фактически выбранного
present mode/метрик и resync общего `advancer` при сильном overrun вместо catch-up burst.
Отложенный хвост PF01 — naive-forward A/B, heatmap/overflow и repeatable camera rail/timings; он не
перебивает текущий PF02-срез.

Срез PF02 2026-08-15: fixed `2×2` spot atlas больше не размножает material/step на каждый light. Общая
команда `draw_regions gpu_data host_commands` принимает обычный целиком bound uniform/storage buffer и
host-visible versioned stream. На регион stream задаёт viewport/scissor, dynamic depth bias, `data_index`
push constant и spans `{pair, first_instance, instance_count}`; main-поток единолично понимает сцену,
консервативно отбирает casters и пакует instance lanes. Render graph остаётся layout/resource manager и
fail-fast проверяет stream до записи первого draw. Material `dynamic = [ depth_bias ]` включает
`vkCmdSetDepthBias`; при отсутствии dynamic state поддержан прежний static raster bias. PF02 использует один
spot material/step вместо четырёх материалов и восьми steps; validation-layer и визуальный прогоны чистые.
Bias A/B теперь использует отдельные raster/receiver constant/slope controls, zero/default presets,
наклонный receiver, тонкий contact caster и изоляцию directional/spot lighting. Runtime hard/PCF/rotated
Poisson/spot-PCSS фильтры имеют общий softness control; PCSS пока исследовательский и для directional map
переходит на расширенный Poisson. Directional target уже переведён на четыре practical-split каскада в
`2×2` atlas через `draw_regions`; rotation-independent extent, texel snapping, debug tint и blend bands live.
Следующий quality-срез — repeatable camera-rail проверка стабилизации, cascade-aware bias и caster culling.

`RND-*` из этой таблицы не требуется закрывать целиком до запуска сцены. Исправляется только конкретный
blocker текущего этапа; найденная более широкая работа остаётся в backlog. `UTL-08`, module profiles,
broker diagnostics, localization и generators поставлены на паузу до campaign, которой нужен их
наблюдаемый результат.

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
| `ECS-07` | Materialization/reconcile registry | `L–XL` | reserve logical participants/items → project request → typed outcome journal → version check → once-only atomic reconcile/rollback; codecs and outcome semantics stay project-owned |
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
| `RES-02` | Per-section resource schema migration metadata | `M` | version/fingerprint/migration owner для resource sections |
| `RES-03` | Priority, cancellation и budgets для procedural CPU artifact jobs | `L` | bounded job lifecycle через существующие staged transitions |
| `RES-04` | Content-addressed artifact cache | `L` | key/index/storage/eviction и corrupt-entry recovery |
| `RES-05` | Explicit artifact epochs | `M` | stale results не публикуются после invalidation/reload |
| `RES-06` | Общий importer/runtime-resource contract | `M–L` | source → canonical CPU intermediate → cooked/runtime resource |
| `RES-07` | KTX/KTX2 texture resource | `L` | mip/layer/cubemap metadata, BC7 upload, fallback/transcode policy |
| `RES-08` | Canonical 3D mesh/scene/animation formats | `L–XL` | единый CPU intermediate независимо от конкретного importer |
| `RES-09` | Dependency/resource graph inspection UI | `M–L` | origins, overrides, edges, states, fingerprints and failures |
| `RES-14` | Installed-module discovery catalog | `M` | сканирует project `mods/`/module root, folder/`.zip`/`.mod`, читает metadata/version/dependencies и возвращает стабильный UI read model без загрузки ресурсов |
| `RES-15` | Ordered TAVL module profiles | `M` | несколько именованных профилей рядом с user settings; ordered selected module ids, active-profile pointer, atomic save/recovery через `UTL-02` |
| `RES-16` | Boot-time active module profile | `M` | active profile выбирается до построения game resource registry; core обязателен, missing/duplicate/dependency/version diagnostics loud, actual loaded set даёт `RES-01` fingerprint |
| `RES-17` | Module-profile switch boundary | `S–M` | меню может выбрать другой профиль; базовый контракт — применить при следующем полном runtime start/restart, без неявного hot-unload мира |

### `libs/catalogue` — trace, budgets и deferred execution

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `CAT-01` | Возможная aggregation shell для нескольких diagnostic sources | `M` | deferred до двух consumers с одинаковым snapshot/reset lifecycle; не объединять timings, semantic rejection, overflow и divergence в общий variant заранее |
| `CAT-02` | Bounded codecs для новых доказанных signatures | `M` | добавлять только вместе с реальным consumer; не делать generic serializer |

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

### Knowledge/provenance — truth и ограниченные представления

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `KNW-01` | Stable fact/observation/explanation envelope и immutable filtered-view host | `L` | holder-facing planner/UI физически не получает canonical truth; project задаёт fact kinds/confidence semantics |
| `KNW-02` | Provenance links, explicit transfer/copy и bounded retention | `L` | передача одной версии знания не раскрывает всё состояние источника; causal anchors переживают compaction |
| `KNW-03` | Truth-vs-view/diff inspector и observation replay | `M–L` | показывает source/evidence/supersession и первый момент расхождения без превращения engine в rumor/law ontology |

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
| `UTL-03` | Более ясные error/`std::expected` APIs | `M–L` | сначала file/background/resource paths, затем по consumers |
| `UTL-04` | Canonical priority queue serialization helpers | `M` | stable key/order/cancel token restore |
| `UTL-05` | Artifact cache/index primitives | `M` | content hash, metadata, atomic index and verification |
| `UTL-06` | Generation primitives: noise, Voronoi/Delaunay, flood/distance, graph transforms | `L–XL` | deterministic reusable algorithms, не world grammar |
| `UTL-07` | Canonical parallel group/filter/reduce для generation units | `L` | semantic ordering независимо от worker count |
| `UTL-08` | Canonical byte/hash comparison test utilities | `S` | serial-vs-MT and save/reload identity helpers |
| `UTL-12` | Перенести общий шаблонный сериализатор из `aesthetics` в `utils` | `M` | `utils` владеет neutral serialization templates/codecs; `aesthetics` оставляет ECS schema/component adapters и compatibility facade на время миграции |

### `libs/painter` — high-level rendering gaps

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `RND-01` | High-level 3D scene instance layer | `L` | typed instances, lifetime, snapshots and batches |
| `RND-02` | Transform/skinning buffers | `L` | frame-owned uploads and stable instance mapping |
| `RND-03` | Skeletal rendering | `L–XL` | bone palettes, bounds and skinned draw path |
| `RND-04` | Forward+ lighting path | `L–XL` | bounded cluster/light lists, compute assignment, point/spot data, heatmap/overflow; tiered path so 2D/2.5D не платят полный cost |
| `RND-05` | Decals/transparency | `L–XL` | ordering, lifetime and material integration |
| `RND-06` | Visibility/occlusion | `XL` | CPU/GPU culling с diagnostics |
| `RND-07` | Material/mesh LOD and HLOD | `XL` | selection, residency, transitions and metrics |
| `RND-08` | Runtime-generated mesh lifecycle | `L` | CPU artifact → GPU publish/reuse/unload with epochs |
| `RND-09` | KTX/KTX2 compressed upload path | `L` | supported GPU blocks не декомпрессируются на CPU |
| `RND-10` | Canonical mesh/material/skeleton upload interfaces | `L` | importer-independent runtime boundary |
| `RND-11` | GPU timings/residency/debug capture | `M–L` | **partial PF02:** opt-in per-pass/full-graph timestamp profiler live; next reusable publication, residency and captures |
| `RND-16` | Post-processing graph/compositor foundation | `L` | ordered fullscreen passes, transient/persistent images, resize/format policy, enable/disable and debug views |
| `RND-17` | Temporal rendering inputs and history lifecycle | `L` | camera jitter, motion vectors, depth/normal history, disocclusion, reset on cut/resize/teleport |
| `RND-18` | Temporal anti-aliasing (TAA) | `L–XL` | reprojection, neighborhood clamp, responsive masks, ghosting diagnostics and no-history fallback |
| `RND-19` | Screen-space ambient occlusion (SSAO) | `L` | depth/normal sampling, denoise/temporal accumulation, quality presets and lighting integration |
| `RND-20` | Общий screen-space effects toolkit | `L–XL` | depth pyramid, reconstruction, bilateral blur/denoise and reusable kernels for SSR, contact shadows, fog and outlines |
| `RND-21` | Базовая color post-processing chain | `L` | HDR exposure, tone mapping, bloom, color grading and output-color-space policy |
| `RND-22` | Directional/spot shadow maps and atlas | `L–XL` | **active in PF02:** spot atlas, 4-cascade directional atlas, `draw_regions`, practical splits/snapping/blend/tint, CPU caster packing, bias fixtures/controls, hard/PCF/Poisson/spot-PCSS A/B and timings live; next camera-rail stability + cascade-aware bias/culling; atlas lifetime and point cubemaps later |
| `RND-23` | Stencil effect path | `M–L` | depth/stencil attachment lifetime, material front/back ops, masks/reference, visualization and ordinary graph consumers |
| `RND-24` | Present policy отдельно от frame pacing | `M` | базовое разделение уже есть; осталось overrun-resync, выбранный-mode metrics и явный MAILBOX/FIFO/IMMEDIATE fallback policy |
| `RND-25` | Вывод step usages/barriers из descriptor sets | `M` | **done 2026-08-15:** named `sets` → usages/read-write masks, dedup/conflict validation; pass/subpass attachment load/store остаются явными |

Порядок post-processing: `RND-16` создаёт общий host, `RND-17` — temporal data/history contract;
после них независимо проверяются `RND-18`, `RND-19` и `RND-21`. Переиспользуемые depth/reconstruction/
denoise primitives из первых живых эффектов постепенно собираются в `RND-20`, а не проектируются
полностью заранее.

Текущий порядок proof задают лаборатории, а не номера: `PF01` тянет минимальные `RND-01/02/04/11`
и только реально встреченные command/schema/format blockers; `PF02` доказывает `RND-22`, `PF03` —
`RND-16/19/21` и последующие самостоятельные temporal slices, `PF04` — `RND-23`. `PF05/06` выбирают
зафиксированное подмножество готовых возможностей вместо зависимости от полного состояния этих labs.

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
| `CFG-02` | Вынести настройки логгирования из общих настроек в отдельный файл/resource | `S–M` | отдельная schema для уровней, domains, sinks, filters и rotation; загрузка доступна до обычного app-config и имеет безопасный fallback |

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
| `BLD-03` | Aligned GLM gentypes ABI/layout audit | `S–M` | перед `GLM_FORCE_DEFAULT_ALIGNED_GENTYPES` проверить serialization, broker payloads, vertex/instance layouts и foreign API boundaries; SIMD benefit подтвердить profiling/assembly |
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
| `AUD-15` | Start/after/underrun/device-fallback/snapshot tests | `M` | audio regression suite |
| `AUD-18` | Steam Audio/HRTF evaluation for `submarine_coop` | `M–L` | дальний pre-release gate: real scene/listening target, platform/deployment cost, fixed-block latency, 1/8/32 voice CPU budget и fallback; не текущий engine dependency |
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
| `UI-20` | Малый Visage overlay для playground common | `S–M` | **first slice 2026-08-15:** Lua/Nuklear + MSDF font, описание/controls/FPS/frame time; расширяется только доказанными общими widgets |

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
| `GEN-13` | Closed-surface planet/culture/history package | `XL–XXL` | project-first `APQ`; immutable topology/province/route/climate/history/special-place/Apate bindings + validation report and fingerprints, не seed-only save |

Первые дополнительные consumers: `TC` — small floor + large semantic quest graph; `SC` — streaming
cave/route segments. `BITS`, `CG` и `MHM` не получают generator dependency без реального требования.

### Closed-surface topology и globe presentation

| ID | Задача | Сложность | Граница/результат |
| --- | --- | --- | --- |
| `PLN-01` | Canonical closed-surface topology | `L–XL` | projection-independent stable cell/province ids, seam-free adjacency/distance and poles/seams/circumnavigation properties |
| `PLN-02` | Projection/ray/surface picking adapters | `L` | saved `APQ` package → screen ray/surface/province without making a full 3D action scene prerequisite |
| `PLN-03` | Globe LOD/map-mode buffers | `XL` | incremental boundaries/routes/labels and holder-filtered terra-incognita presentation with no geometry leak |

## Остальные открытые cross-system программы

| ID | Задача | Сложность | Условие старта/граница |
| --- | --- | --- | --- |
| `PST-01` | Durable save envelope, slots, metadata and atomic commit | `L` | `UTL-02`, `RES-01`, owner sections |
| `PST-02` | Schema migration registry and save inspection tool | `L` | `ECS-01/02`, `RES-02` |
| `PST-06` | Save module manifest and compatibility report | `M` | каждый save хранит ordered module ids/versions/fingerprints; load сравнивает exact/reordered/missing/changed/extra с active profile до десериализации |
| `PST-07` | Explicit degraded-save policy | `M–L` | missing/changed module не всегда fatal: engine показывает structured warning, project решает allow/deny; неизвестные owner sections сохраняются opaque либо следующий save явно помечается как destructive |
| `RPL-01` | Versioned intent/input log + exact `game_delta_ticks` | `M–L` | camera becomes separate intent provider |
| `RPL-02` | Checkpoint and resource/config/build fingerprints | `M–L` | `SIM-05`, `RES-01`, `PST-01` |
| `RPL-03` | First-divergence hash/bisect diagnostics | `M` | `CAT-01`, `UTL-08` |
| `RPL-04` | Short replay artifact and optional presentation track | `L` | после `RPL-01..03`; первый продуктовый consumer — `BITS` |
| `TIME-01` | Local/per-entity time multiplier policy | `M–L` | только с consumer; определить влияние на physics, flags, AI and replay |
| `AI-01` | Runtime GOAP/FSM profile switch | `L` | deterministic tick boundary, capability validation and cache invalidation |
| `CFG-01` | Save simple UI Lua state | `M` | bounded data-only table, nested tables, no functions/userdata |
| `API-01` | Header documentation and public/experimental API audit | `M–L` | проход по owner libraries; старые dispatcher/loader remnants архивировать отдельно |
| `UI-20` | Module profile manager screen | `M` | список установленных модулей, enable/order, create/rename/delete/select profile, dependency/conflict feedback и save-compatibility warning перед запуском/загрузкой |

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

## Открытые project-first задачи `apates_quest`

Нормативная последовательность начинается с `apates_campaign_bridge_lab` на фиксированном замкнутом
графе из 12–20 провинций. Полный глобус и 3D-тактика не являются prerequisites этого proof. Engine
получает только нейтральные lifetime/order/persistence/view mechanisms; персонажи, титулы, право,
знание, миф, армейские правила и interpretation outcomes остаются проектными.

| ID | Задача | Сложность | Definition of Done / возможная движковая дельта |
| --- | --- | --- | --- |
| `APQ-01` | Суточное campaign-ядро на fixed graph | `L` | persistent orders проходят stable `snapshot intents → movement/routes → contacts → mandatory actions → due events → reports/stop` pulse; auto-advance/one-day/until-date/until-event и save/resume дают одинаковый state hash |
| `APQ-02` | Title/institution/policy/claim vertical | `L–XL` | character/house/title отделены от institution/seat; одна succession/hero-recognition chain имеет authority, evidence requirements, preview, structured rejection, два политических outcome и explanation provenance; отменённые universal binary culture flags не возвращаются |
| `APQ-03` | Holder-specific knowledge и evidence | `L` | rumor/contact/mapped-route/surveyed/current-intelligence records, delayed expedition report, explicit map sale и false-map contradiction работают через filtered view без утечки canonical geometry |
| `APQ-04` | Journey, army и multi-phase encounter | `L–XL` | route queue/current transition/progress/ETA/interruption переживают save; marching actor канонически остаётся в origin, но не действует как local defender; встречный переход создаёт boundary encounter; army contact/manoeuvre/decisive/pursuit поддерживает autoresolve и offer/accept/refuse duel with typed stakes |
| `APQ-05` | Campaign encounter bridge без 3D | `L` | один persistent character/items проходит reserve → immutable materialization request → headless/manual-stub или autoresolve → единый project typed outcome journal → version validation → atomic exactly-once reconcile/release; death/wound/capture/time/witness/evidence покрыты faults/resume |
| `APQ-06` | Durable generation-scale campaign batch | `L` | owner-section save/migration, bounded calendar/history/knowledge retention, domain RNG и 150–200-летний headless batch имеют budgets/stop reasons/first divergence и одинаковый результат serial/1/2/4/8 workers |
| `APQ-07` | Малый личный TBT vertical | `XL` | после выбора локальных правил: герой + 3–5 спутников, 6–10 meaningful rounds, retreat/surrender/lethal contracts/objectives beyond killing; manual и autoresolve возвращают schema `APQ-05`, глобальный календарь на время сцены остановлен |
| `APQ-08` | Myth/Apate/special-place layer | `XL` | deed truth, witnesses, competing recognized versions, granted rights и один Apate archetype связывают politics/religion/adventure; вулкан и незавершённая great work проверяют physical state, title/control/access, multi-stage hazard и historical reinterpretation |
| `APQ-09` | Late authoritative-host network lab | `L–XL` | только после устойчивого single-player slice: два участника, simultaneous daily commands, authoritative pulse/result hash, reconnect snapshot и одна общая короткая TBT; transport/product multiplayer не входит в первый prototype |

`APQ-01..05` вместе закрывают первый `apates_campaign_bridge_lab`. После `APQ-06` отдельно идут
`PLN-01 → GEN-13 → PLN-02/03`: сохранённый immutable package предшествует globe renderer. Полная цель
мира — 3–4k неравномерных сухопутных провинций плюс более крупные морские зоны, но первый generator
proof обязан быть маленьким и проверять seam/circumnavigation, связность и отсутствие knowledge leak.

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
- [x] `RES-01`: demiurg экспортирует versioned canonical ordered module list/fingerprint; folder
  fingerprints независимы от абсолютного root, archive hashes совместимы с module-list JSON,
  override-order/content changes покрыты focused test.

### Atomic persistence primitives

- [x] `UTL-02`: `file_io::atomic_file_transaction` даёт exclusive same-directory temp,
  streamed write, file flush, atomic replace, parent-directory flush, RAII abort, stale-temp recovery
  и stage/error/committed diagnostics; success/abort/recovery/validation/replace-failure paths покрыты.

### Время, ввод, UI и звук

- [x] Game time использует exact integer `game_delta_ticks`; speed задаётся рациональным multiplier.
- [x] Pause/time scale проходят через общий timeline contract; gameplay countdowns используют game time.
- [x] Settings-owned keyboard/mouse action bindings поддерживают live reload/save.
- [x] Первый player intent queue и anti-spam dedup доказаны `spawn_food` consumer-ом.
- [x] Read-only UI seam к pure act predicates/numbers/strings/describe работает без mutating Lua backend.
- [x] UI instruction/wall-time/GC/Nuklear convert budgets и failure streak policy вынесены в settings.
- [x] Positional/non-positional sound, listener, attenuation и live volume groups работают.
- [x] `AUD-01`/`RES-10`: immutable shared sound generations pin-ятся producer-ом до broker publish
  и живут через queued/active task; `unload_warm()` снимает resource owner без invalidation playback,
  unload lifetime покрыт focused test.
- [x] `AUD-LAB-01` tooling: `subprojects/playgrounds/AU01_spatial_audio` документирует завершённый
  A/B; архивный executable исполнял один deterministic mono S16 reference signal/trajectory через
  production miniaudio `system` или direct OpenAL Soft и печатал
  actual device/rate/HRTF, имеет HRTF off/on, headless dry-run и ручной comparison table. Реальное
  headphone A/B 2026-08-12: OpenAL HRTF on заметно улучшает понимание направления; built-in miniaudio
  звучит близко к OpenAL HRTF off; у miniaudio точки выше/ниже почти неразличимы. Проверка исходников
  miniaudio 0.11.25 показала не потерю `Y`, а ограничение stereo panner: default SIDE_LEFT/RIGHT
  directions имеют `y=0`, поэтому равнодистанционные `+Y/-Y` получают одинаковые channel gains.
- [x] `AUD-LAB-02` tooling: сценарий расширен до 28 секунд одинаковыми front `-Z` и up `+Y`
  distance pulses `4→10→1→4`; runtime/dry-run печатают фактический radius. Vertical orbit явно
  помечен constant-radius directional test, поэтому HRTF coloration больше не смешивается с
  distance attenuation. Ручной verdict: front и up attenuation работают одинаково; OpenAL даёт
  только очень небольшую полезную direction coloration. Miniaudio выбран production backend.
- [x] `AUD-14`: OpenAL runtime/reference path удалён из live tree. Legacy OpenAL implementation,
  helpers, decoder buffer overloads, CMake/DLL dependency и A/B executable архивированы под
  `exclude/`; `libs/sound` теперь miniaudio-only, PCM helpers живут в backend-neutral `common.h`,
  а production class переименован из `sound::system2` в канонический `sound::system` без alias.
- [x] `AUD-17`: optional listener-relative high-shelf принят как дешёвый bounded front/back cue.
  Финальный профиль `-2.25/+0.65/-0.85 dB`, общий strength `[0,2]`, default-off; headphone verdict
  подтвердил, что elevation почти не читается и остаётся задачей будущего HRTF, а не усиления shelf;
  live executable находится в `subprojects/playgrounds/AU02_directional_coloration`.
- [x] `rng_state + int` и отключаемый sound worker с динамическим reserved-worker count закрыты.

### MT execution и diagnostics foundation

- [x] `catalogue::mt` поддерживает collect/elect/structural strategies, bounded inline journals,
  semantic seal/order и deterministic 1/2/4/8-worker tests.
- [x] Identity domain отделён от reusable strategy policy; добавлены neutral presets.
- [x] `aesthetics::system_runner` объединяет независимые systems под один pool barrier без скрытого DAG.
- [x] `tile_frontier` cognition/effects и integration+drives используют доказанные MT paths.
- [x] kD-tree deterministic parallel build снял первый измеренный bottleneck без изменения world hash/bytes.
- [x] Catalogue domains собирают локальные timings в caller-owned `statistics_store`; project UI
  читает предметный store напрямую без обязательного общего aggregation service.
- [x] `FSM-01`: отдельный opt-in `mood/diagnostics.h` строит owned graph snapshot, guard-by-guard
  step trace и фактический settle trace с точной причиной остановки. Обычные `step/settle` и
  `runtime.cpp` не инструментированы; diagnostic settle исполняет actions ровно один раз.
- [x] `CAT-03`: passive constexpr phase descriptors выводят arbitration/commit/conflict из
  существующей strategy и явно показывают owner, reads/writes, write policy и fixed/dynamic budgets.
  Executor ничего о metadata не знает; caller-owned registry создаётся только tooling/test consumer-ом.
  Первый живой набор — local/eat/flag effect phases `tile_frontier`.
- [x] Общий `CAT-01` service сознательно отложен: локальные timings остаются в `statistics_store`,
  а semantic rejection/overflow/divergence принадлежат предметным owners до доказанного совпадения
  lifecycle хотя бы у двух consumers.

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
