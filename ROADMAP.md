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

Текущая лаборатория: [`PF03_post_processing`](subprojects/playgrounds/PF03_post_processing/README.md).
Предыдущая, [`PF02_shadows`](subprojects/playgrounds/PF02_shadows/README.md), закрыта: `RND-22/26/27` и
`shader_crafter::set_include_root` уехали в движок, а разрыв до референсного визуального стиля лежит не в
тенях, а в AO/тумане/экспозиции — это и определило порядок срезов `PF03`.

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
Bias A/B теперь использует raw raster controls, world-texel receiver normal offset, tap-level receiver-plane
derivatives, zero/default presets, наклонный receiver, тонкий contact caster и изоляцию light classes.
Hard/PCF/Poisson edge AA отделён от world-unit spot-PCSS emitter. Camera depth prepass и half-resolution
compute дают directional + четыре spot contact masks без temporal history; все masks имеют debug views и
отдельный GPU timing. Contact masks являются opt-in spatial proof: single-depth silhouette artifacts
консервативно reject'ятся, а полноценная hidden-surface/HZB/history reconstruction остаётся PF03.
Directional target уже переведён на четыре practical-split каскада в
`2×2` atlas через `draw_regions`; rotation-independent extent, texel snapping, debug tint и blend bands live.

Срез PF02 2026-08-17 (граница техник зафиксирована): цель лаборатории — сглаженные (edge AA), а не
физически мягкие тени, поэтому temporal, stochastic, HZB, полноценный PCSS и area lights уходят в `PF03`.
Правдоподобие набирается корректным bias, ненулевым ambient внутри тени и контактным затемнением; рост
мягкости с расстоянием допускается только дешёвой оценкой по одному центральному blocker-tap'у.
Закрыт `RND-26`: сравнивающие сэмплеры и specialization constants шага. Тиры качества PF02 переехали из
define'ов материала в `shader_constants` (`pcf_radius`, `contact_ray_steps`, `contact_refine_steps`).
Первые две техники тоже закрыты: sampling идёт через `sampler2DShadow` со взвешенным разделимым tent'ом
(один tap = билинейная доля, `3×3` даёт футпринт `6×6`), при этом сырой nearest-доступ к атласам сохранён
для hard-режима и blocker search PCSS; contact-вклад ограничивает карту через `min` вместо умножения,
апсемплится по ближайшей глубине (`contact_directional` стал `sf2`: маска + линейная глубина источника) и
гаснет по длине ray, глубине камеры и краю кадра. Все presets проходят валидацию чисто. Дальше по README
PF02: `guarded contact`, дешёвая оценка полутени, `dual-depth contact`, затем repeatable camera-rail и
directional caster culling.

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
| `RND-17` | Temporal rendering inputs and history lifecycle | `L` | **частично сделано в `PF03` 2026-08-19:** тонкий G-buffer (глубина + октаэдральная нормаль + motion в UV) одним проходом, motion от камеры через прошлую view-projection, репроекция прошлого кадра по этим векторам и явный сброс истории на resize/`R`. Проверено численно: при статичной камере ошибка репроекции ровно нулевая, поле motion масштабируется линейно со скоростью камеры (3.02 → 6.09 → 11.14 px при 0.5/1.0/2.0 rad/s), а при одинаковом виде ошибка с motion ниже наивной и разрыв растёт со скоростью (1.22× при 2.5 px/кадр → 2.17× при 20 px/кадр). **Per-object motion добавлен 2026-08-19:** трансформы объектов живут в per_frame ресурсе, а ПРОШЛЫЙ трансформ вершинный шейдер берёт из истории того же ресурса (`history = 1` на втором биндинге) — хост пишет только текущий, дублировать нечего. Опознавать объекты на экране для motion не нужно: фрагмент уже знает свой инстанс; идентичность объекта понадобится позже для отбраковки истории. Измерено при НЕПОДВИЖНОЙ камере и чистом переносе: наивная ошибка растёт со скоростью объектов (107 → 581 → 885 при 0.5/1.0/2.0), компенсированная заметно ниже (74 → 248 → 391), выигрыш 1.5× → 2.3×, а остаток лежит тонкой каймой ровно на силуэтах (disocclusion). Остаток задачи: camera jitter, depth/normal history и disocclusion-маска — идут вместе с `RND-18` |
| `RND-18` | Temporal anti-aliasing (TAA) | `L–XL` | **сделано в `PF03` 2026-08-19:** джиттер проекции по Халтону, репроекция накопленного кадра по motion-векторам, neighbourhood clamp, смешивание в обратимо сжатом диапазоне, disocclusion и passthrough при отсутствии истории. Измерено: сходимость к суперсэмплингу (RMSE `96.7` против среднего 8 джиттеренных кадров, у одиночного кадра `564.6`), подавление дрожания `1238 → 49.5` (25×), кромки `121.4 → 84.4`, clamp урезает шлейф вдвое (`2249 → 1058`), шум AO `44.4 → 36.7`. ГЛАВНЫЙ УРОК: TAA нельзя валидировать глазом — неверный знак вычитания джиттера из motion-векторов давал картинку, которая выглядела сглаженной и гасила дрожание, но была ДАЛЬШЕ от эталона, чем один кадр (`795` против `538`); поймал только численный тест на сходимость. Инвариант для проверки: на статичной сцене motion при включённом джиттере обязан быть нулём (было `1.624 px`, стало `0.008 px`). Разобран и ЗАМЕРЕН предел техники: ступенька на далёкой статике при движении камеры — не нехватка данных, а отбраковка недействительной истории с последующей пересходимостью (~12 кадров при весе 0.92). Ослабление отбраковки не помогает (гладкость `75.5/75.3/76.2` для min/max, дисперсии и клипа по скорости), а метрика гладкости для этого вопроса непригодна — она не отличает «сглажено» от «замылено». По эталону (среднее 8 джиттеренных кадров той же точки обзора): без TAA `404`, min/max+bilinear `596`, min/max+Catmull-Rom `536`, без отбраковки `788` — то есть самый гладкий вариант самый неточный, а при непрерывном движении честный аляйсящий кадр ближе к истине. Измеримое улучшение дал фильтр выборки истории: Catmull-Rom вместо bilinear `596 → 536` (bilinear переразмывает историю каждый кадр). Остаток: responsive masks для прозрачности/партиклов, sharpen после накопления, вес истории по скорости, правила сброса при cut/телепорте и — принципиально — больше входных сэмплов на кадр (MSAA+TAA либо TAAU) |
| `RND-19` | Screen-space ambient occlusion (SSAO) | `L` | **сделано в `PF03` 2026-08-19:** половинное разрешение (`screensize` + `scale`), depth-aware блюр и depth-aware апсемпл (глубина текселя в `.g`, контракт из PF02), число проб — specialization-константа шага с CLI-переопределением до сборки графа. AO умножает ТОЛЬКО ambient, поэтому в солнечно-доминированной сцене слаб по построению (появилась ручка `--ambient`). Оценщик переписан ПО ИЗМЕРЕНИЮ: версия на разнице глубин давала самозатенение открытого пола (`0.99 → 0.89` при росте радиуса), версия на восстановленной позиции затенителя с тестом касательной плоскости даёт `0.9999` на открытом полу при сохранённом сигнале в углах (`0.898`). Веер радиальных штрихов на скользящем полу снят ограничением радиуса в ЭКРАННЫХ единицах (`std 30.3 → 0.075`, сигнал в углу без изменений). Шум сырого AO `41.4 → 25.2` при `4 → 32` пробах, но после блюра выравнивается около `23` при любом числе — блюр часть техники. Остаток: temporal-накопление (идёт с `RND-18`) и presets |
| `RND-35` | Туман и аэроперспектива | `S–M` | **сделано в `PF03` 2026-08-19:** однократное рассеяние с экспоненциальным спадом плотности по высоте, оптическая глубина взята АНАЛИТИЧЕСКИ (одна `exp` на пиксель вместо марша, отдельная ветвь для почти горизонтального луча), направленность через фазовую функцию Henyey–Greenstein, небо исключено из тумана. Применяется в линейном HDR ДО экспозиции, поэтому замер яркости видит кадр уже с туманом. Проверено: `--fog=0` даёт пропускание ровно 1.000; `T` монотонно падает с расстоянием; контраст далёкой шахматки `28.05 → 17.68 → 8.71` при плотности `0/0.08/0.25` (мерить только при фиксированной экспозиции — авто возвращает контраст обратно); фазовая функция даёт 2.2× разницы между рассеянием вперёд и назад при солнце у горизонта. GOTCHA измерения: при `cos θ ≈ 0` HG симметрична по знаку `g`, поэтому тест фазовой функции нельзя ставить перпендикулярно солнцу |
| `RND-37` | Световые лучи (экранные) | `S` | **сделано в `PF03` 2026-08-19:** радиальное размытие от экранной позиции солнца по маске ярких пикселей, умноженной на признак отсутствия геометрии (шахты = свет мимо препятствий), затухание вдоль луча, число проб — spec-константа. Маска берётся с четверти bloom-пирамиды, то есть пирамида общая на две техники. ОГРАНИЧЕНИЯ метода честные: солнце обязано быть в кадре (за камерой буфер ровно нулевой), затенители за границей кадра не видны, и сцене нужны РАЗРЫВЫ в маске — размытие сплошного неба даёт засветку, а не шахты (в стенде для этого добавлены колонны). Полноценные шахты = марш по карте теней, отдельная задача |
| `RND-38` | Мип-цепочки у ресурсов render graph | `M` | **сделано 2026-08-19.** Ресурс объявляет `mips = N` либо `mips = auto` (цепочка до 1x1 от размера уровня 0, пересчитывается при resize) — это свойство ресурса, потому что определяет аллокацию; биндинг объявляет `mip = k`, отсутствие поля = вся цепочка. Правило из Vulkan, а не из вкуса: у `imageLoad`/`imageStore` нет LOD, поэтому storage-вид покрывает ровно один уровень, и `texel_write`/`texel_read`/`general`/вложения ОБЯЗАНЫ называть уровень (loud error), а `sampled` без уровня даёт вид на цепочку для `textureLod`. Реализовано: вид на цепочку плюс вид на каждый уровень у каждой копии, отслеживание layout ПО УРОВНЯМ (`resource_inst::usage_levels` — иначе невыразимо «уровень k в sampled, пока k+1 пишется»), проверка конфликта юсаджей по паре (ресурс, уровень), барьеры выводятся из биндингов со склейкой подряд идущих уровней в один барьер. Попутно исправлено молчаливое: `create_samplers` не задавал диапазон LOD, а при `maxLod = 0` выборка любого уровня выше нулевого возвращает нулевой; добавлены `mipmap = nearest|linear` и снятие верхней границы. Проверка: bloom-пирамида PF03 переведена с четырёх ресурсов на один с `mips = 4` — картинка ПОБИТОВО та же (`AE = 0`), минус три объявления ресурсов и два declare_values. Не поддержано: рисование в произвольный уровень (render target не умеет называть уровень, падает громко) и генерация цепочки блитами |
| `RND-39` | `usage = general` у картинки = read-write storage image | `S` | **исправлено 2026-08-19:** layout (`eGeneral`), image-usage (`eStorage`) и access-маска (read+write) у `general` уже были верные, а тип дескриптора выдавался буферный (`eStorageBuffer`), из-за чего шаг не мог одновременно читать и писать одну картинку. Добавлена перегрузка `convertdt(usage, is_image)`; аддитивный подъём bloom-пирамиды теперь обходится без ping-pong ресурсов на каждый уровень |
| `RND-40` | Выходной тракт: sharpen, дизеринг, линза | `S` | **сделано в `PF03` 2026-08-19.** Порядок применения следует физике: аберрация и виньетка — оптика (линейный свет), резкость — работа с изображением, зерно — плёнка (после кривой), дизер — подготовка к квантованию (последним, амплитудой в шаг). ИЗМЕРЕНИЕ ОПРОВЕРГЛО план: резкость не «лечит замыливание», а меняет одну ошибку на другую — относительно суперсэмплированного эталона нерезкое маскирование даёт `695 → 810 → 981` при силе `0/0.35/0.8` из-за overshoot. Защемление диапазоном соседей (RCAS) срезает добавленную ошибку вдвое (`0.8` стоит `746` вместо `981`), и тогда это честная ручка вкуса: энергия высоких частот `50.6 → 64.2` против эталонных `82.5` за ограниченную плату по точности. Дизер: `327 → 353` различимых уровня в градиенте без сдвига яркости (`220.80` против `220.79`), шум треугольный. Виньетка не трогает центр, гасит угол `199 → 135`. Зерно меняется по кадрам (RMSE `1991`) и не смещает яркость (`154.897` против `154.901`). Аберрация и зерно по умолчанию выключены как приёмы на вкус. GOTCHA измерений: эталон привязан к сцене и настройкам, а не только к точке обзора — после добавления колонн в срезе 6 старый эталон дал RMSE 4250 вместо ~700 |
| `RND-41` | Взрослый замер экспозиции | `M` | **сделано в `PF03` 2026-08-19:** 256-корзинная гистограмма (локальная в разделяемой памяти, группа 16x16 = по потоку на корзину и одно глобальное атомарное сложение на поток), разбор по перцентилям с частичным учётом краевых корзин, центровзвешенность, асимметричная адаптация и границы замера как вход для «тёмной комнаты». Три шага в одном пассе с РАЗНЫМИ юсаджами (обнуление `storage_write` → сбор `general` → разбор `storage_read`) — иначе барьеров между ними не будет вовсе, потому что движок выводит их из смены юсаджа. Измерено: верхний перцентиль убирает влияние яркого выброса (`0.031 → 0.000` стопа при панели ×1000) и управляет вкладом КРУПНОЙ яркой области (`−0.125/−0.345/−0.627` стопа при отсечении верхних `10/30/50%` на кадре с небом); нижний перцентиль — это СМЕЩЕНИЕ к средним тонам (`+0.38/+0.79` стопа при `25/45%`), а не устойчивость; центровзвешенность двигает замер к центру; асимметрия даёт отставание `+0.88` стопа при быстром привыкании к яркому и `−0.50` при обратном; границы замера дают барьер «тёмной комнаты» (тёмный угол читается `94` против `22` при ограничении снизу). Замер по сетке вдвое реже кадра даёт ТОТ ЖЕ результат (`+2.431`, 87 корзин против 93) при четверти работы. Остаток: exposure-волюмы как авторские данные — это прикладной слой, движку нужны только границы |
| `RND-48` | GPU-таймстемпы по пассам в площадках | `S` | **сделано 2026-08-19.** Механизм в движке уже был (`gpu_timestamp_profiler`), площадка его не подключала. Теперь `PF03` накапливает среднее/минимум/максимум на пасс за прогон (минимум — самая честная оценка, шум только добавляет) и печатает отчёт на выходе; результаты забираются сразу после `prepare_frame` и относятся к кадру, отправленному `frames_in_flight` назад. СРАЗУ ОПРОВЕРГЛО прежние оценки по времени кадра в обе стороны: замер экспозиции, подозреваемый в «около миллисекунды», стоит `0.144`; самым дорогим пассом оказалась компоновка (`0.956`), о цене которой не думали вовсе, и дорога она базой (дюжина выборок и запись полного кадра), а не ручками линзы (`0.906` с резкостью против `0.877` без). Полная цепочка: `3.967` мс среднее, `3.596` минимум на 1280×720 Iris Xe, тогда как время кадра `5–7` мс — то есть около двух миллисекунд вне GPU. Закрыло два висевших вопроса: SSAO `0.351` мс при восьми пробах против `0.950` при тридцати двух (а качество после блюра одинаково), замер `0.316/0.136/0.121` мс при сетке `1/2/4` с ИДЕНТИЧНЫМ результатом. Подтвердило числом и вывод аудита: `--ao=0` не экономит ничего, потому что пасс выполняется — тиры качества обязаны менять форму графа |
| `RND-42` | Depth pyramid (Hi-Z) | `M` | **СДЕЛАНО в `PF03` 2026-08-20.** Поверх мип-цепочек (`RND-38`): уровень хранит пару (минимум, максимум) глубины по блоку, шесть уровней одним ресурсом `mips = 6`, шесть шагов одним пассом (барьеры между уровнями выводятся из дескрипторов, группы submit'а не тратятся). В reverse-Z максимум это самая БЛИЗКАЯ поверхность блока, минимум — самая далёкая (небо = 0): по максимуму марш пропускает блок целиком за лучом, по минимуму — целиком перед ним. Единственное содержательное свойство — КОНСЕРВАТИВНОСТЬ, и она проверяется на GPU отдельным видом (интервал уровня обязан содержать глубину каждого покрытого пикселя, число нарушений обязано быть нулём). Измерено на трёх вариантах: как сделано 0 нарушений; без поправки на нечётный размер 14896 пикселей (1.62%) — уровень имеет floor(N/2) текселей, блоки 2x2 покрывают N-1 элементов, и последний столбец остаётся снаружи (для 1280x720 это переход 80x45 -> 40x22, портится полоса у нижнего края, а пирамида выглядит правильной); в ПОЛОВИННОЙ точности 356366 пикселей (38.7%) — шаг half у единицы около 5e-4 при округлении к ближайшему, то есть минимум округляется вверх, максимум вниз, и гарантия теряется. Отсюда формат `v2` (32 бита) и `texelFetch` вместо `texture` (интерполяция сглаживает границы внутрь интервала). Цена 0.368 мс минимум на 1280x720, и дорог нулевой уровень: 921600 текселей против 307000 во всех остальных вместе, то есть три четверти работы — копия глубины. Половинное разрешение уровня 0 — первый кандидат на оптимизацию, но решать после SSR. ПРОБЕЛ КОНФИГА: уровни перечисляются по одному (шесть шагов и шесть дескрипторов с `mip = k`), способа сказать «шаг на каждый уровень цепочки» нет, поэтому `mips = auto` в таком виде невыразим — тот же класс, что `RND-31`: не хватает композиции |
| `RND-43` | Screen-space reflections | `L` | **СДЕЛАНО в `PF03` 2026-08-20, с ОТРИЦАТЕЛЬНЫМ результатом про пирамиду.** Отражения стоят после тумана и до накопления (это свет: обязаны попасть в замер экспозиции и в bloom), читают затуманенный кадр, шум марша усредняет существующий TAA — отдельный денойзер не понадобился. Passthrough при `--ssr=0` ПОБИТОВЫЙ, эффект меняет 76% пикселей (RMSE 17.05). ГЛАВНОЕ: иерархический марш по пирамиде глубины НЕ окупился, и это измерено против честного эталона (линейный марш с шагом порядка пикселя, 512 шагов). При равной точности он в 1.5–2 раза дороже: линейный даёт отклонение 2.31 за 1.733 мс, иерархическому для того же нужно ~150 шагов (~3.2 мс); при одинаковом пределе 48 шагов он тратит 14.1 отсчёта против 10.9 у линейного, то есть делает БОЛЬШЕ шагов, а не меньше. Механизм: отражает большой почти плоский пол, скользящий луч не выходит из окрестности той самой геометрии, от которой отразился, каждая клетка содержит этот пол, «весь блок дальше луча» почти никогда не срабатывает, и выборка пирамиды — чистые накладные расходы. Пирамида окупается там, где луч летит через ПУСТОТУ (зеркало и далёкие объекты). Дефолт поэтому ЛИНЕЙНЫЙ марш, 128 шагов — выбран измерением. Попутно: первая версия марша шагала фиксированной длиной вместо шага до границы клетки — это был не тот алгоритм (шаг приземлялся посреди клетки, а «безопасность» относилась к клетке конца шага), пришлось переписать, чтобы отличить «алгоритм не окупается» от «реализация неверна». Ограничения измерены: 40.0% лучей уходят за кадр (цена метода, маршем не лечится), 33.9% находят поверхность, 3.9% не встречают ничего, 22.2% не трассируются; 29.9% упираются в предел шагов при бюджете 48. Небо для отражений — честный обман (основное небо задано градиентом по ЭКРАНУ, отражению нужен цвет по НАПРАВЛЕНИЮ; правильный ответ — environment probe, отдельная лаборатория). Шероховатости нет: в G-buffer площадки нет канала материала, `--ssr-roughness` только ослабляет отражение. Толщина поверхности обязательна и берётся в ЛИНЕЙНОЙ глубине |
| `RND-44` | Depth of field | `M` | планируется в `PF03` срезом 11: bokeh поверх мип-пирамиды, разделение near/far поля, борьба с протеканием через силуэты (та же depth-aware выборка, что у AO и контактных теней) |
| `RND-45` | Motion blur | `M` | планируется в `PF03` срезом 12 и стоит в конце СОЗНАТЕЛЬНО: автору эффект не нравится, но это единственный потребитель motion-канала помимо TAA, поэтому в рамках площадки он уместен — заодно проверяет per-object векторы под нагрузкой |
| `RND-46` | TAAU: накопление с апскейлингом | `L` | планируется в `PF03` срезом 13. Рендер в пониженном разрешении с реконструкцией: компромисс отставания против точности уже измерен в срезе 5, расширение естественное. Именно этим темпоральные техники окупают себя (отрицательная цена) |
| `RND-47` | Объёмный туман по froxel'ам | `L` | ОТДЕЛЬНАЯ будущая лаборатория, не `PF03`: «настоящие» световые шахты требуют марша по карте теней, то есть смыкания с `PF02`. Экранные лучи (`RND-37`) умеют только источник в кадре и не видят затенителей за его границей |
| `RND-20` | Общий screen-space effects toolkit | `L–XL` | depth pyramid, reconstruction, bilateral blur/denoise and reusable kernels for SSR, contact shadows, fog and outlines |
| `RND-21` | Базовая color post-processing chain | `L` | **экспозиция и tone mapping сделаны в `PF03` 2026-08-19:** отдельный пасс сводит кадр в одно число (геометрическое среднее яркости через log-усреднение, 4096 проб одной рабочей группой), адаптация идёт через историю того же 1×1 ресурса, экспозиция = ключ/среднее; операторы `none/reinhard/hable/aces` переключаются рантайм. Проверено: замер линеен по яркости сцены (солнце 1→6→30 даёт 2.51 и 2.38 стопа при теоретических 2.58 и 2.32), экспозиция его ровно компенсирует (средняя картинка держится 74–82 при 30× диапазоне, против 180→247 при фиксированной экспозиции), без кривой 1.2% кадра выжжено в плоский белый, с Reinhard/Hable — ноль. Отдельно ИЗМЕРЕНА передаточная функция тракта: blit в `B8G8R8A8Srgb` свопчейн кодирует sRGB сам (0.5 линейного → 187/255), поэтому кодировать в шейдере нельзя. **Bloom сделан 2026-08-19:** общая пирамида (четыре понижения тринадцативыборочным фильтром Jimenez, три аддитивных подъёма шатровым 3×3), мягкое колено порога. ВАЖНО: порог обязан быть в единицах ПОСЛЕ экспозиции — в линейных единицах он захватывал всю сцену (небо `19`, поверхности `3.6`), из-за чего пасс экспозиции переехал ПЕРЕД пирамидой; после правки в буфере остаётся только источник (средняя `0.536 → 0.021`), а порог выше источника гасит эффект полностью. Профиль свечения управляется весом подъёма (`9.3 → 3.6` при spread `0.4` против `70.0 → 20.4` при `0.95`). **Color grading и LUT сделаны в `PF03` 2026-08-20 (срез 9), задача закрыта.** Грейд применяется в scene-referred линейном HDR ДО кривой (баланс белого, цветной фильтр, контраст в стопах — это съёмка), порядок операций: баланс белого → фильтр → контраст вокруг среднего серого → ASC CDL slope/offset/power → насыщенность ПОСЛЕДНЕЙ (она определена относительно яркости, которую меняет каждый предыдущий шаг). Баланс белого — фон Крис в Bradford-LMS от цветности планковского локуса, с нормировкой на яркость нейтрали; наивное усиление каналов оставлено ручкой и ИЗМЕРЕНО: дрейф средней яркости 9.8% против 1.8% по прогону 3500..12000 K, монотонный, то есть систематически спорящий с автоэкспозицией. ВЫБОР ПРОСТРАНСТВА ГРЕЙДА измерен по операциям (отклонение scene- от display-referred, 8-битные уровни): насыщенность 1.15, температура 14.55, контраст 17.68 — то есть у операций типа усиления это 15–26 уровней, а не стилистика, и у баланса белого разрыв растёт с экспозицией (14.6 → 26.2). Таблица: полоса N*N x N (3D-ресурсов в графе нет, `RND-49`), log-shaper как кодировка ОБЛАСТИ ОПРЕДЕЛЕНИЯ, выход хранится в тех же кодированных координатах — тогда нейтральный грейд это тождество, трилинейная интерполяция воспроизводит линейную функцию точно, и любая измеренная ошибка означает баг. Измерено: тождество через таблицу ≤ 1 уровня (RMSE 0.41), аналитический путь 0.007% пикселей; размер таблицы 8/16/32/64 даёт 15.03/4.58/1.06/0.34 (квадратичная сходимость, пол 0.4 = точность хранения и фильтрации), поэтому 32³ по умолчанию; линейный shaper вместо log2 хуже В 83 РАЗА (87.4 против 1.06) с ошибкой в тенях; границы shaper'а нужны двусторонние (узкие обрезают: 6 стопов дают максимум 12 уровней; широкие теряют точность: MAE 0.081/0.167/0.243 при 12/22/44 стопах). ЛОВУШКИ, каждая проверена намеренной поломкой: полтексельный сдвиг внутри плитки обязателен, иначе билинейная фильтрация лезет в соседний синий срез и тождество уезжает в 33 раза (0.41 → 13.67) при правдоподобной картинке; nearest вместо linear — в 21 раз (8.60). Цена (минимум по 600 кадрам, 1280x720 Iris Xe): таблица 32³ +0.150 мс, аналитика +0.185 мс, то есть таблица дешевле всего на 19%, а при 64³ запекание (0.094) съедает выигрыш целиком — таблица не бесплатна, она стоит две выборки |
| `RND-22` | Directional/spot shadow maps and atlas | `L–XL` | **active in PF02:** spot + 4-cascade directional atlases, `draw_regions`, splits/snapping/blend/tint, world-texel + receiver-plane bias, independent hard/PCF/Poisson AA, world-unit spot-PCSS, half-res directional/spot contact masks and timings live; next camera-rail stability + culling; temporal reconstruction belongs to PF03, atlas lifetime/cubemaps later |
| `RND-23` | Stencil effect path | `M–L` | depth/stencil attachment lifetime, material front/back ops, masks/reference, visualization and ordinary graph consumers |
| `RND-24` | Present policy отдельно от frame pacing | `M` | базовое разделение уже есть; осталось overrun-resync, выбранный-mode metrics и явный MAILBOX/FIFO/IMMEDIATE fallback policy |
| `RND-25` | Вывод step usages/barriers из descriptor sets | `M` | **done 2026-08-15:** named `sets` → usages/read-write masks, dedup/conflict validation; pass/subpass attachment load/store остаются явными |
| `RND-29` | Контракт чтения предыдущего кадра | `M` | **done 2026-08-18, первый потребитель — `PF03`.** Число копий ресурса больше не задаётся руками: ПЕРИОД вращения — свойство счётчика (`per_frame` → `frames_in_flight`, `swapchain` → число образов; про host-счётчики движок не знает, там нужен явный `type`, он же остался override'ом), а ГЛУБИНА ИСТОРИИ — свойство техники-читателя и объявляется в binding'е дескриптора (`history = N`); ресурс берёт max по читателям, копий = `период + max(history)` (минимальность проверена перебором в `painter_temporal_history_test`). Из того же объявления ВЫВОДИТСЯ всё остальное: read-only фиксация копий в конце кадра (`temporal_fixate_instance` — именно в конце, потому что копию может тронуть более поздний пасс вроде blit в свопчейн), кросс-кадровый порядок пассов (`derive_history_ordering` + авто-семафор писателя, ожидание слота предыдущего кадра, срез хвоста ожиданий на первом кадре графа) и очистка копий на старте/после resize (`initialize_temporal_resources`, иначе первый кадр читает UNDEFINED-картинки). Индекс истории в шейдере — не данные и даже не индекс массива: `history = 1` даёт одиночный `sampler2D`. Ручной `wait_previous` у пасса остался escape hatch; `wait_for` теперь честно внутрикадровый (раньше локальные семафоры молча ждали предыдущий кадр) с loud error, если сигналящий пасс идёт позже. Попутно исправлено: `pSignalSemaphores` всех групп смотрели в один буфер, поэтому сигналить могла только последняя группа |
| `RND-36` | Семафора выведенного порядка на пару «писатель→читатель» | `S` | **исправлено 2026-08-19 в `PF03`:** вывод кросс-кадрового порядка создавал одну семафору на пасс-писатель, а бинарный семафор — это один сигнал на одно ожидание. Как только историю одного ресурса стали читать ДВА пасса (накопление TAA и отладочные виды компоновки), второе ожидание повисало на неотсигналенном примитиве (валидация: `no way to be signaled`). Теперь семафора создаётся на пару, писатель сигналит по одной на каждого читателя |
| `RND-34` | Смещения констант считались в словах вместо байтов | `S` | **исправлено 2026-08-19 в `PF03`:** `constant::offset` задаётся в БАЙТАХ (`commit_parsed_resources`), а память констант — `vector<uint32_t>`; `get_constant_data`/`write_constant_data` прибавляли смещение к `uint32_t*`, то есть уходили вчетверо дальше. Первая константа (offset 0) работала всегда, поэтому баг жил незамеченным до первого конфига с ДВУМЯ используемыми по значению константами: dispatch читал мусор (в валидации — `groupCountY = 1072693248`, то есть старшая половина double). Покрыто регрессом в `painter_temporal_history_test` |
| `RND-32` | Скриншоты в PNG | `S` | libpng уже в зависимостях. Нужен пользовательский инструмент, отдельный от отладочного readback: папка скриншотов (переопределяемая, с проверками и fallback), отдельный шаг с blit в host staging буфер, по кнопке отдать буфер libpng и сохранить на диск. Сырой PPM-дамп (`PF03 --dump`) остаётся для измерений: без кодека, побитово воспроизводим |
| `RND-33` | Период инстанс-буферов host-visible draw group | `S–M` | НАЙДЕНО в `PF03` 2026-08-19: инстанс/indirect буферы host-visible draw group жёстко привязаны к счётчику `per_update` и типу `doublebuffer`, а `per_update` в кадровом цикле не двигается — значит ВСЕ кадры в полёте читают одну и ту же память. Данные, меняющиеся каждый кадр (например трансформы для motion-векторов), туда класть нельзя: история совпадает с текущим кадром побитово, и это молча даёт «правдоподобную» неверную картинку. Обход в `PF03`: в инстанс-лейн положен только индекс объекта (константа), а сами трансформы — в обычный per_frame ресурс. Правильное решение — вывести период этих буферов из того, кто и как часто их пишет, тем же правилом, что и у остальных ресурсов (`RND-29`) |
| `RND-49` | Трёхмерные ресурсы render graph | `M` | **СДЕЛАНО 2026-08-20.** Найдено в `PF03` срезом 9 (таблица цветокоррекции — естественно трёхмерная) и оказалось глубже, чем «дописать глубину»: буферизация картинок сделана СЛОЯМИ одного образа (`layers += buffering`, копия j = `baseArrayLayer = layer_offset + j`), а Vulkan запрещает слои у трёхмерного образа (`VUID-VkImageCreateInfo-imageType-00961`) — то есть третья ось несовместима с тем способом, которым движок хранит копии. Решение: **у объёмного ресурса копия это ОТДЕЛЬНЫЙ образ** (контейнеры подряд, по одному на копию, ни с кем не делятся). Альтернатива «копии вдоль z одного образа» отброшена по существу: вид на 3D-образ не умеет выбирать поддиапазон глубины, значит номер копии пришлось бы передавать в шейдер — ровно то, что убирает контракт истории `RND-29`. Цена честная: объёмный ресурс дороже дескрипторами и аллокациями. Выводится, а не объявляется: тип образа из глубины (`extent::is_volume()`), тип вида из типа образа, мип-цепочка по ТРЁМ осям (у объёмной картинки уровень делит и глубину). Громко запрещено: объёмная картинка как вложение render target (у framebuffer'а нет третьей оси) и объёмный контейнер со слоями. Реализовано: `extent` с третьей осью вместо двух анонимных структур (заодно убран мёртвый `struct extent`), `compute_frame_size` возвращает три оси (`fixed_3d` больше не теряет z), `compute_mip_levels(extent)`, `imageType`/`viewType` из глубины, per-copy контейнеры, blit по трём осям. Проверка как у мип-цепочек: таблица PF03 переведена с полосы на куб 32³ — кадр без грейда ПОБИТОВО тот же (RMSE 0), тождество через таблицу 0.4094 против 0.4092, ошибка табулирования воспроизвелась по всем размерам (15.03/4.58/1.06/0.34 при 8/16/32/64), 3D против полосы отличается на 3.1% пикселей на один уровень (RMSE 0.10 — побитового совпадения быть и не могло: аппаратная трилинейная квантует восемь весов, ручная квантовала два билинейных и досчитывала третью ось во float). Валидация чиста на размерах 8/16/32/64. Дальше третья ось нужна froxel-туману (`RND-47`) и 3D-шуму |
| `RND-50` | Условный пасс: кэш в графе | `M` | **СДЕЛАНО 2026-08-20.** Оказалось не новым механизмом исполнения, а ТРЕТЬИМ ВИДОМ СЧЁТЧИКА. Счётчик остался тупыми данными: «делал ли я это поколение» — состояние ПАССА (`pass_observed_counter`), привязка объявляется ЯВНО (`counter = grade_cache` у пасса), чтобы читателю конфига было видно, что исполнение зависит от внешних событий, и чтобы пасс с ресурсом были связаны одним счётчиком. Двигает хост (`inc_counter` уже был) — условие инвалидации это его знание; GPU решать не может: результат приходит через frames_in_flight кадров, а структура графа выводится при сборке и обязана оставаться статической. Детали, каждая из которых могла стать молчаливым багом: сравнение по НЕравенству, а не `<` (swapchain-счётчик выставляется номером образа и не монотонен), сентинел `UINT32_MAX`, а не стартовое значение (нуль у swapchain законен), решение принимается в `prepare_frame`, а не при записи команд (запись получает graphics_base по const-ссылке — и правильно), наблюдённое значение фиксируется ПОСЛЕ успешного submit (кадр, не дошедший до отправки, не должен закрывать поколение), сброс при смене графа и resize — зануление памяти ПАССА. Пасс не пишет команд, но группа ОТПРАВЛЯЕТСЯ: пустой submit сигналит семафоры сам, поэтому цепочка ожиданий цела, таймстемпы на месте и число измеряемых пассов не меняется по кадрам — отдельное понятие «пасс не исполнялся» профилировщику не понадобилось. Частота сдвигов ВЫВОДИТСЯ из числа копий: живых поколений не больше, чем копий, отсюда `разрыв = ceil((frames_in_flight-1)/(копий-1))` — три копии при трёх кадрах в полёте разрешают сдвиг каждый кадр, две копии — раз в два кадра, одной копии не хватает никогда (`singlebuffer` у кэша запрещён громко). Обещание проверяется в рантайме, а не принимается на веру: ровно на таком молчании `RND-33` дал историю, побитово равную текущему кадру. Измерено в PF03: цена пасса 0.021 (каждый кадр) → 0.010 (раз в два) → 0.004 (раз в четыре) → 0.000 (однократно), а картинка при всех частотах ПОБИТОВО та же (RMSE 0) — ротация копий верна. Проверено намеренными поломками: ресурс на чужом счётчике, одна копия, слишком частый сдвиг — все три падают громко. ИСТОРИЯ ПОКОЛЕНИЙ РАЗРЕШЕНА (уточнение правила среза 2, а не обход): запрет `history` вне `per_frame` стоял из-за НЕОПРЕДЕЛЁННОСТИ (у host-счётчика «предыдущая копия» = предыдущий апдейт, а счётчик между кадрами может не сдвинуться вовсе либо сдвинуться дважды), а у условного счётчика неопределённости нет — индекс копии выводится из значения, поэтому `history = 1` это ровно предыдущее поколение. Ломался только выведенный кросс-кадровый порядок: читатель ждёт семафор писателя ПРЕДЫДУЩЕГО кадра, а поколение писал более ранний. Решение: порядок берётся из ГОРИЗОНТА ФЕНСА — при объявленной истории движок требует у счётчика разрыв не меньше frames_in_flight, тогда прошлое поколение записано кадром, чей фенс текущий уже дождался, и семафор не нужен (в логе flow: 'ordered by the fence horizon, not by a semaphore'). Заодно исправлена ошибка в самой формуле разрыва: считать надо от ПЕРИОДА, а не от периода+истории — копии истории заняты прошлыми поколениями и для записи не свободны, то есть объявление истории ослабляло ограничение вдвое. Проверено живым потребителем (переход между поколениями таблицы в PF03): скачок в кадре запекания 12.98 → 0.99 (в 13 раз) при переходе за 20 кадров, конечная точка та же (12.29 = ровно 19/20 пути), passthrough при выключенном переходе ПОБИТОВЫЙ, цена +0.09 мс только на время перехода |
| `RND-30` | MSAA в конфиге графа | `M` | makers уже умеют `rasterizationSamples`/`attachmentSamples`, но в конфиге нет `samples` у ресурсов, resolve-attachment у render target, выбора sample count из таргета и depth resolve. Нужен как полноценный путь: у части планируемых проектов рендер простой, и там MSAA уместнее TAA. Делать forward-only и как A/B против TAA, а не поверх: MSAA не помогает экранным эффектам и плохо сочетается с G-buffer |
| `RND-31` | Профили качества как документ переопределений | `S–M` | **конструкция уточнена по итогам PF03 (2026-08-19):** у качества картинки пять рычагов, различающихся ЦЕНОЙ смены — форма графа (пересборка pipeline графа), размеры целей `declare_values`+`scale` (пересоздание ресурсов), глубина мип-цепочки (пересоздание ресурсов), specialization-константы шага (пересборка pipeline шага) и числа UBO (бесплатно, живьём). Пресет обязан помечать уровень каждого переопределения, иначе движок не может честно сказать «нужен перезапуск». ГРАНИЦА: числа UBO в пресет НЕ входят — движку нечего знать про «радиус AO», он владеет ресурсами, pipeline'ами и графами, а не смыслом эффекта; прикладные числа живут в настройках проекта. Отдельно: `passthrough` числом в UBO (как в площадке) годится для A/B, но не для тиров — на низком качестве пасс не должен выполняться вовсе, а это выбор графа. | `declare_values` парсит `presets`/`scale_presets`, но НИКОГДА их не применяет. Замена per-value пресетам: отдельный документ-профиль переопределяет любые поля конфига (размеры, `shader_constants` шагов, `definitions` материалов), применяется до сборки графа, читается целиком как «что значит high», допускает пользовательские имена, композицию `база + дельта` и группировку по эффектам. Каждое переопределение помечает свой уровень (ресурс/pipeline/UBO), чтобы движок мог честно сказать «требуется перезапуск» — смыкается с `LEG-03` |
| `RND-28` | Общий бюджет теней на все источники | `M` | политика раздачи регионов атласа источникам внутри кадра: важность/приоритет, переиспользование между кадрами, честный отказ при перерасходе. Механизм размещения уже есть (`RND-27`), не хватает политики; первый consumer — сцена с несколькими shadow-casting источниками |
| `RND-27` | Atlas allocation contract | `S–M` | **done 2026-08-18:** `painter::allocate_atlas_regions` детерминированно размещает квадратные регионы (first-fit по сетке с шагом НОД), `atlas_region_uv`/`atlas_occupancy` дают трансформ и занятость, переполнение = `false` + громкое сообщение вызывающей стороны; раскладка стала данными (`uv_scale_offset` в записях, число регионов и размер атласа в UBO), шейдеры больше не знают сетку; покрыто `painter_atlas_layout_test`. Первый потребитель — PF02: до шести каскадов с неравными размерами регионов |
| `RND-26` | Сравнивающие сэмплеры и specialization constants шага | `S–M` | **done 2026-08-17:** `sampler.compare = <compare_op>` (аппаратный `samplerXDShadow` PCF) и `step.shader_constants = [ name = "value" ]`; `constant_id`/тип/размер из reflection SPIR-V, форма `id_<N>` для констант без `OpName`, имя→id снимается отдельной debug-сборкой (spirv-opt снимает `OpName`), неизвестное имя = loud error со списком доступных; покрыто `painter_shader_prepare_test` |

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
