# Project Memory

This repository is the author's experimental game engine / framework. It is a large WIP codebase where many engine ideas are being tested together, so prefer reading the nearby implementation and preserving existing exploratory structure over imposing a new architecture too quickly.

## Current Focus

- Main integration playground: `subprojects/tile_frontier`.
- Goal for `tile_frontier`: draw a large tile map, support world/resource streaming, and run many AI actors to stress multithreading.
- GPU asset lifetime (2026-07-19): hot texture unload waits the owning graphics frame fences, rewrites the bindless slot to the default/null texture, destroys VMA image/view and returns the slot to `empty`; mesh unload removes the slot from every draw-group pair before destroying buffers and returning it to reuse. `painter_asset_lifetime_test` covers state transitions and cross-group unregister.
- Snapshot query repair (2026-07-19): `aesthetics::snapshot_loaded_event` is common, and both `world::query_t` and `lazy_query_t` subscribe and rebuild their cached entity containers after `load_world`. Queries created before a load immediately see the restored world.
- First player/spawn slice (2026-07-19): `actor_world_slice` owns one serialized `player_controller` plus an ephemeral deduplicating `player_intent_queue`. `spawn_food` (default `mouse_right`) converts screen to world and emits typed `act::intent_kind::spawn_prefab`; gameplay consumes it at the next tick boundary. Food maintenance uses semantic `spawn_point` entities/group `food`; ds exposes `spawn(prefab, group)`, while `spawn_at` remains the coordinate escape hatch.
- UI budgets (2026-07-19): app/user settings carry Lua instruction/wall-time, incremental GC, Nuklear convert time/output and consecutive-failure limits. `visage::system` skips a failed/over-budget frame and disables after the configured streak; settings reload resets it. `lua.update` and `nuklear.convert` feed a catalogue statistics domain.
- Replay remains inactive until three prerequisites: ordered runtime module list/priority loading in demiurg, camera as its own intent provider with a replay-only presentation track (not necessarily network input), and exact per-tick `game_delta_ticks` recording. The eventual replay artifact is a versioned intent/input log plus checkpoint and resource/config/build fingerprint, not catalogue serialization.
- Generic interaction resolution kernel (2026-07-22): new header-only `libs/resolve` owns pointer-free work provenance, bounded MT journals with semantic seal/id assignment, target grouping (parallel between targets, serial within one target), host-paced `frontier_state begin/advance`, neutral damage route/outcome data and the hard retaliation contract. Retaliation deduplicates by `(triggering_instance, rule)`, may occur once per hit/rule, and neither retaliation nor any descendant of its lineage can retaliate. Atomic append order has no gameplay meaning; ambiguous provenance and every deterministic budget overflow fail loudly. There is deliberately no fundamental `run_to_completion`: an FPS host loops `advance` within a sim tick, while a turn FSM paces the same boundaries with presentation checkpoints. Card ordering, elemental tables, status storage/stacking and death policy remain project-owned. Detailed contract: `libs/resolve/README.md`; `resolve_pipeline_test` covers 1-vs-4 identity, target order, frontier faults, retaliation and minimum-HP guard without resurrection.
- Cardgame grouped combat slice (2026-07-23): `subprojects/cardgame` is the first live `libs/resolve` consumer and owns the pointer-free hierarchy `effect_program → beat → authored_effect/effect_ref → emitted effect_instance_ref → typed outcomes`. Every beat freezes all target snapshots before cue; authored cues share one gameplay barrier, scripts execute in stable order, and one aggregated result per authored effect carries its outcome range. Targeters are target/random/all; effects select independently by default, while an explicit nonzero binding key reuses one snapshot and mismatched reuse fails loudly. Death is checked after every damage/status outcome and gates only the next beat. Primary hits still use an owned frontier; elemental children and thorns retaliation retain resolve provenance. The serializable project cursor runs ActionBegin/card execution → player/enemy party passes → source ActorStateTick → explicit countdown boundary → a fresh mirrored enemy execution/party passes/tick → ActionEnd. Its append-only `action_report` seals one segment per group: each group reads only the prefix sealed before it, appends zero or more complete `resolution_work` executions, and exposes those outputs only to the next explicit group. Thus player follow-ups see card work, enemy follow-ups see card+player work, ActorStateTick sees card+both passes, and the report resets before enemy execution; the mirrored sequence builds a separate report. A stolen card contributes one `executed=false` execution segment, skips party passes, still ticks its actor and advances countdown. Forced end-turn enemy executions receive their own monotonic action-cycle token and skip player ActionEnd. Party order is frozen from `(combat_seed, action_token, side_domain, stable entity id)`. The current native attack listener intentionally emits zero work and ActorStateTick only records its consumed prefix; real rule programs and DoT/negative/positive work remain next. `cardgame_action_pipeline_test` covers staged report visibility/reset plus normal/theft/mirrored/forced/resume order.
- Cardgame typed effect envelope and routed damage (2026-07-23): attack/healing/shield/agility-damage/status stores feed one semantic-order `outcome_ref {kind,index}` without a thick variant; sign never changes kind (`attack(-N)` remains damage, `healing(-N)` remains healing, `shield(-N)` remains shield removal), and death is checked after every typed outcome. Authored `emit_shield` is a first-class DS leaf: it adds a signed amount to the shield stat, clamps at zero, produces its own report range/category/presentation result and survives pre-invocation resume. One unrouted damage root gathers modifiers/resistance exactly once into `damage_preparation`, then emits at most two bounded provenance children in fixed order: shield leaf followed by residual health leaf. Only leaves mutate stats/become outcomes; shield-only hits have no synthetic HP outcome and terminal shield/HP drives elemental continuation once. Every committed damage leaf—shield, health, zero, negative, primary, reaction or periodic—offers subscribed retaliation rules the same immediate DS boundary; only `resolve::may_emit_retaliation` is a hard C++ gate, so retaliation and every descendant remain non-recursive. Routed leaf IDs share the frontier allocator, presentation exposes `shield_damage`, and each emitted response keeps its separate cue. `cardgame_typed_effect_test` covers shield-only and negative-attack retaliation, two rule invocations for shield→HP overflow, vulnerability without double resistance, signed healing, self targeting, attribute resistance, report/death boundaries and animated/headless identity; `cardgame_combat_script_test` covers authored shield emission.
- Cardgame DS reaction contract (2026-07-23): retaliation is an immediate continuation of one committed instance inside its outer execution; neither retaliation nor its descendants can retaliate. Follow-up is instead a fixed FSM group over a frozen `action_report` prefix. It cannot recursively observe output from its own party pass, but its completed executions are sealed into the ledger and may enable the next opposing-party group. A retaliation always owns a nested authored attack cue/result/finished (no inline merge) and a full attack route, but receives retaliation lineage, no action/countdown/tick and no separate follow-up group. Cardgame DS is deliberately two-phase: a rule/program prepare scope runs before cue and materializes targets/authored-effect requests; an authored-effect emit scope runs after the gameplay marker and records typed instances only. `emit_*` never resolves synchronously, lifecycle channel is invocation-owned, and direct reflected `add_*` stat mutation is forbidden because it bypasses outcomes/death/presentation. Preserve the existing mutable `register_stats`; add a separate read-only registration entry point for combat scopes.
- Cardgame DS leaf/resource slice (2026-07-23): devils_script is pinned to v1.2.1. `act::register_stats_readonly` sits beside unchanged mutable `register_stats` (plus separable reader/writer registration). `combat_effect_scope` executes a compiled ds script against a transient writer; its `each_target` iterator traverses the frozen authored target snapshot in project order and installs one `combat_target_scope`. Each `emit_attack/healing/shield/attribute_damage/status` call appends exactly one bounded pointer-free instance plus a semantic plan ref directly into `resolution_work`; target-major callback order is gameplay order, signed literals such as `emit_healing = -2` are native syntax, and attack root IDs remain unresolved until the resolver selects the plan item. `combat_effect_script_compiler` is the project adapter for generic `act::script_resource` roots `combat_effect` and `retaliation_rule`; authored recipes snapshot only stable demiurg hashes, while `combat_effect_script_resources` resolves externally owned compiled containers. `retaliation_rule_scope` exposes rule stacks plus the trigger's requested/delta/before/after/clamped, element/tags, destination/channel/death/id/root/source/target and bounded `emit_retaliation_attack`; shipped `scripts/thorns_retaliation` now owns the thorns response decision instead of a native HP-loss predicate. `cardgame_combat_script_test` covers both roots and pre-invocation resume.
- Cardgame execution-report DS view (2026-07-23): generic `act::script_resource` also accepts the read-only `execution_report` root. A transient `execution_report_view_context` rebinds a frozen pointer-free report to the `resolution_work` that owns its typed stores; no pointer enters snapshots, and every plan/outcome range plus typed ref is checked before dereference. DS sees execution/actor/selected-target/executed/category metadata, typed counts and stable `each_attack/damage/healing/shield/attribute_damage/status` iterators with route/result accessors. The scope has deliberately no emitter. The owning `action_report` ledger now retains all prior executions through party passes and ActorStateTick; the next follow-up prepare scope must iterate its sealed input prefix and append bounded output only after each rule invocation. `cardgame_execution_report_script_test` covers resource compilation, metadata, typed fields, snapshot-style rebinding and corrupt-range failure.
- Cardgame retaliation presentation (2026-07-22): each retaliation response now opens its own nested `returned_damage` cue/gameplay/result/finished checkpoint immediately after the triggering attack subtree. The response outcome is excluded from the outer authored-effect presentation result, so it is not visually merged twice. Response task ids stay transient; resume at the retaliation cue drops presentation and continues the serialized `response_commit` exactly once. `cardgame_headless_smoke` covers the separate cycle and resume boundary.
- The cross-actor contract question (main/gameplay, render, sound, assets) is now ANSWERED: a single lock-free **broker** holds every inter-thread channel (see "Message broker" below). The old `utils::actor_ref` / `actors.h` path was archived under root `exclude/` on 2026-07-13; `message_dispatcher<T>` and advancer `actor`/`get_actor()` remnants are no longer present in the live tree.
- `tile_frontier` core is split by concern under `subprojects/tile_frontier/{include,src}/core/`: project-specific `config`, `messages`, `render_system`, `assets_system`, `actor_simulation`, resources and `tile_frontier_game`; shared broker/topology/runtime pieces live under `libs/simul`. `simulation.cpp` is now only the thin `game_host` adapter/state+worker wiring. `tile_frontier_game` owns chunk request/receive/apply, map/camera + UBO, tile/actor publication, actor update, presentation-sound culling and project metrics/UI values. It consumes generic host outputs (`scene_binding`, `phase_gate`, framebuffer/presence) without moving any game policy into engine.
- Runtime scene composition is config-driven: `states/*` owns UI `script`/`default_font`/allowlist and points at a standard `scenes/*` manifest. `simul::scene_manifest_resource` expands `{id,target,group,alias,startup}` into scope grants, resource transitions and target-aware loading progress; without render, external targets stop at the pre-external level. `tile_frontier` interprets only manifest groups/aliases and reads chunk/grid/actor/brain settings from its CPU-only `worlds/*` descriptor. The texture/sound id arrays and default-font hook are gone from `simulation.cpp`.
- Scene group semantics matter: `worlds/*.tile_texture_group` is the terrain palette and currently contains only `grass`, `grass1_0`, and `grass3`. `grad1`, `grad2`, and `quad` belong to `aux_textures`; putting them into the tile group makes mock chunk generation use UI/test textures as terrain.
- Generic gameplay config resources live with their owner libraries: `mood::fsm_resource`, `acumen::goap_resource` (including merge/flatten), `prefab::prefab_resource`, and `act::script_resource`. Embedded devils_script compilation crosses one `act::script_compiler` seam; `tile_frontier::script_environment` supplies the concrete `entity_scope` dispatch. The project assets system only registers these types and native building blocks.
- Actor gameplay config is fail-fast and has no production fallback: `load_required_brain_config` requires the scene-selected script/FSM/GOAP resources plus both `actor` and `food` prefabs; `actor_world_slice::init/load` require that assembled config. `world_scene_config` has no C++ value/id defaults. Resume/config/MT tests use `test_brain_fixture`, which loads the shipped module resources instead of reproducing graphs or prefab text in C++. Config-defined GOAP metrics/actions become `act::script_function` facades; the duplicate native predicate/flee/chase/think table was removed. As of 2026-07-18 ALL six actions are config-only: `eat = prey` (target scope via the `prey` object function over perception) and `seek_food`/`wander` = `set_course = chance` (ds's own deterministic RNG).
- FSM disk syntax is native TAVL rather than quoted mood lines: `state + event [guard0, guard1] / (action0, action1) = next_state`. Parentheses around actions are required so TAVL emits a tuple. `mood::fsm_resource` parses directly into owner-level `mood::transition_config`; `mood::system` has a structured constructor and does not reparse synthetic strings. The legacy string constructor remains for C++ fixtures. `fsm_config_test` covers guards/actions and `tile_frontier_config_effect_smoke` loads the shipped six-transition FSM with 1-vs-4 identity.
- `tile_frontier` actor update phase timing now uses `libs/catalogue` instead of `utils::perf`: `actor_simulation.cpp` wraps sense gather/tail build, cognition, apply, combined `integration+drives`, structural tail and `actor_batch::build` with `catalogue::domain<...>::fn_traits`. `tile_frontier_mt_benchmark` resets the store after warmup and prints avg/min/max plus wall-tick shares. Release 4096×120 measurements identified the serial kD build as the first bottleneck; `utils::kd_tree::build_parallel` now partitions a deterministic top frontier and builds disjoint subtrees in the pool. The next-tick tree is gathered after structural changes and built at the current tick tail concurrently with `actor_batch::build`; init/load uses an explicit fallback. The optimization snapshot for 1/2/4/8 was 1.304/1.248/0.937/0.865 ms/tick (speedup 1.00/1.05/1.39/1.51); a longer 4096×600 control measured 1.438/1.360/1.085/0.920 (1.00/1.06/1.33/1.56). Both retained identical hashes and bytes across worker counts.
- `libs/aesthetics/system_runner.h` owns the minimal multi-system phase contract: `run(pool,time,systems...)` submits independent range-query systems and explicit worklists; a plain serial `update(time)` or `single(fn)` becomes one indivisible pool task. It then performs one shared `compute/wait`. It is deliberately not a dependency graph: callers guarantee disjoint writes/order independence, indivisible `update` tasks must not perform nested dispatch/wait, and structural commits remain later explicit steps. Legacy `template_system_mt::update()` and `worklist_system::run()` delegate to it. The first live group is tile_frontier `integration + drives` (shared velocity read; disjoint position/stats writes), replacing two pool barriers with one.
- Catalogue MT effects (live tile pipeline, 2026-07-17): gameplay effects remain ordinary free `void(Args...)` functions; devils_script/act register `catalogue::mt::domain<IDENTITY_TAG, STRATEGY>::fn_traits<&fn,...>::fn_deferred_ptr`. Identity owns the static executor binding, while reusable policy types can be shared safely; neutral presets are `preset::{parallel_collect,serial_elect,structural_elect}`. Existing `catalogue::domain<auto>` remains the orthogonal trace value-domain. `mt::executor<Strategy>` phases are `begin_record → MT record → seal → commit` or parallel `dispatch_group` + `finish_commit`. `record_scope(source_id,dense_index)` owns TLS provenance/local ordinal; executor derives global sequence as `source_index * sequence_capacity + local_ordinal`. Storage is a dense budget-sized journal with fixed 128-byte inline payload; oversized/over-aligned signatures fail compile-time, and only nested dynamic values such as `std::string` contents may still allocate. Atomic append position has no gameplay meaning; `seal()` restores `(key, source, local_sequence, function)`. Source uniqueness is a producer/worklist invariant. Implemented policies are collect/elect with `parallel_groups`, `serial`, `serial_structural` and optional `target_not_source`; multi-participant reserve/write-set is distant backlog. tile_frontier runs local effects as parallel collect groups and eat as ST structural elect before FSM/sound. `tile_frontier_resume_smoke` checks resume and 1-vs-4 identity; `tile_frontier_mt_benchmark` requires identical hash+bytes for 1/2/4/8 workers. Catalogue is NOT replay/RPC/netcode/on-disk serialization.
- Deferred ds-effect semantics (agreed 2026-07-17): each true effect branch records independently and routes by that function's strategy; a collect `add_strength` still commits if a neighboring `eat_prey` loses elect. Multi-effect atomic/all-or-nothing groups are future explicit syntax, never inferred from neighboring ds blocks. `devils_script::on_effect` is a gameplay reaction to the recorded call, not an MT hook and not proof of successful commit; it may enqueue later work. Success-dependent reactions inspect actual post-commit components/state in a later gameplay step. `on_effect` itself may use a catalogue strategy wrapper.
- Config-loaded void actions are live: GOAP action objects accept optional `effect = <devils_script>` co-parsed into an owned container. `actor_simulation` registers an `act::script_function<void>` under the semantic action name, while `script_environment` exposes the same catalogue `fn_deferred_ptr` building blocks. All six shipped actions use this path (2026-07-18): `flee`/`chase`/`think`, `eat = prey`, and `seek_food`/`wander` = `set_course = chance`. `act::script_function::invoke/describe` seeds `vm->prng_state = utils::mix(rng_seed, rng_entity, rng_tick)`, so ds `chance`/`random` blocks are deterministic per (seed, entity, tick) with per-callsite parse salt; `act::rng_source` stays a provenance carrier for the native packer path only. The `prey` scope function materializes the target as a typed ds argument so elect arbitration sees it at record time; `actor_eating` switched from an absolute `until_tick` to a `ticks_left` countdown so the eat body needs no tick. `tile_frontier_config_effect_smoke` loads the shipped `goap/actor` resource, requires 6 scripted actions, 300-tick 1-vs-4 worker byte identity, and a nonzero eating peak (the config eat pipeline actually grabs prey).
- Registration facade (2026-07-18): `act::building_blocks` (libs/act/building_blocks.h) is the single declarative registration point for native gameplay building blocks. `effect<Traits>()` registers a catalogue `fn_deferred_ptr` into `ds::system`; `pure<&fn>(name)` registers a plain ds function; `effect_native<Traits>()`/`native<&fn>(name)` are the exceptional act-only path bypassing ds; `reg_interaction(name, desc)` attaches arbitration to the semantic name regardless of backend. The ds side registers once via `register_ds(sys)` (owner of `ds::system` calls it in its ctor); the act side replays via `install(registry&)` on every registry rebuild, and a name collision with a config script fails loudly inside `registry::reg` (no silent fallback). tile_frontier keeps one static list in `actor_simulation.cpp` (`actor_building_blocks()`); `script_environment` calls `register_ds`, `setup_brain_registry` calls `install` before `build_goap_from_config`. Covered by `act_building_blocks_test`. Note: test sources now live under `subprojects/tests/` (the old top-level `tests/` paths in this file are historical).
- Game time & generic flags (2026-07-18). Speed model is VARIABLE dt (author's decision, model B): the sim tick stays one per host frame and `game_delta_ticks` (µs of game time, already scaled/paused by `utils::timelines`) is the gameplay input. `actor_world_slice::update(uint64_t game_delta_ticks, ...)` accumulates its own `game_now()` (serialized in `sim_globals` so countdowns survive resume) and derives float dt for integration/drives. Live speed control: Lua `app.set_game_speed(1.2)`/`app.game_speed()` — the double exists only at the UI boundary; `game_host::set_game_speed(double)` converts it ONCE to a milli-precision rational (1.2 → 6/5 via llround+gcd) and the exact `set_game_speed(num, den)` core stays integer, so `timelines`' remainder-carry keeps the game clock an exact ratio of the engine clock (no per-frame float rounding drift; the recorded replay input is the rational). Multiplier sits over the nominal `settings.time` mapping (reset to nominal on runtime settings reload; zero/NaN forbidden — stopping is pause; the two-arg overload deliberately has no default denominator so a single-int C++ call is not ambiguous). Tick-based gameplay was converted to game time: `actor_eating.remaining` is a game-duration countdown and cognition cadence is `commit_game_ticks_` (150ms game = the old 3 ticks at 20fps); `think_budget_` deliberately stays a per-tick CPU budget. Consequence: at small dt (1/60s smokes/benchmark) actors re-think less often per tick, so the MT benchmark dropped to ~0.77 ms/tick (hash 0x6314d39ac4e9f875, 1/2/4/8 identical).
- `aesthetics::flag_set` — engine-generic per-entity flags with expiry (flag = string hash bound to the entity): an aggregate with a public sorted `entries` vector of `{utils::id flag, utils::game_duration remaining}` (aggregate so reflect/SERIALIZABLE_COMPONENT serialize it; `std::vector` fields work). COUNTDOWN model, not absolute deadlines — deferred ds-block bodies run at commit with no time context and reader predicates need none either; pause (dt=0) and time scale act through the game delta automatically. API: set (refresh overwrites; <=0 remaining is dead until sweep), has (expired == absent even before sweep), remove, advance(dt) (saturating subtract + erase exhausted). tile_frontier: ds blocks `set_flag = { name, seconds }` (seconds <= 0 ⇒ permanent) / `clear_flag = name` in a dedicated collect+serial_structural lane (`create<flag_set>` on demand is structural), `has_flag(name)` pure predicate; `expire_flags(game_delta_ticks)` phase after resolve_eating; live consumer: resolve_eating sets `sated` for 10 game seconds on the finished eater (post-commit fact — NOT in the eat script, since compound-script branches record independently) and drives_system freezes hunger growth while it lives. Covered by `aesthetics_flag_set_test` + config_effect_smoke (mid-run speed ×3 → pause → ×0.5 with 1-vs-4 identity, flag block parse checks).
- Events/triggers/relations contract (fixed 2026-07-18, ROADMAP session item 3). There is NO separate "event" entity (author's decision). Four forms: an effect is a catalogue deferred call; intra-tick system-to-system communication is `aesthetics::message_buffer`/`message_registry`; gameplay input (player/network) is an `act::intent` — the only replayed input; a presentation side-output follows the `sound_emit` pattern (ephemeral, never read by the sim, re-emitted on replay). Cross-tick events wait for the deferred-call queue (deferred). Relations rule: a relation is an entityid component field holding the FULL versioned id; readers must `exists()`-check; no automatic cleanup (versioned ids make dangling safe) — `actor_eating.target`/`actor_grabbed.by` are the pattern and serial already resolves references on load; generic relation containers (entity-entity, entity-faction-entity lazy matrix) are deliberately deferred as game-type-specific. Planned UI intent queue (build when a player entity exists): a UI action button emits an `act::intent` into the player intent queue with mandatory anti-spam dedup by action type (re-emitting the same type while one is pending is ignored).
- Read-only UI seam to act (2026-07-18): `actor_world_slice::ui_predicate/ui_number/ui_string/ui_describe(name, entityid)` invoke pure act categories by name in a dry-run ctx on the main thread (dedicated `ui_scratch_`, no MT lanes); the effect category is intentionally unreachable (Lua is not a mutating backend). Lua side: `app.act_predicate/act_number/act_string(name, id)` (nil = missing/wrong category/dead entity; act_string returns the loc-key hash as a 64-bit Lua integer) and `app.act_describe(name, id [, callback])` — with a callback it streams execution nodes one by one (ds `container::describe` node names; the intent is for Lua to build a small execution-graph tooltip), without it returns nodes joined with newlines. Covered headlessly in config_effect_smoke.
- Per-entity GOAP/FSM refs (2026-07-19): components `goap_ref`/`fsm_ref` hold utils::id hashes of BRAIN NAMES (resource id minus prefix), assigned at spawn via `prefab::reference<C>` rows (`goap = prey`, loud validation) and immutable (runtime switch deferred). Scene config declares brain SETS via `fsm_prefix`/`goap_prefix` (each resource = a named acumen/mood system in the slice registries); decide/apply resolve per entity, miss = loud error. Metric/action names are a slice-global act namespace: same-name registrations from multiple flattened goap configs dedup by `origin` (new goap_config field; same origin = same script, different = loud error). acumen `plan_key.system_salt` (set by registry::add) keeps shared per-thread solution_caches sound across multiple systems — without it plans (action INDICES) would leak between brains. Live world spawns `actor_prefab_cycle = [prey, prey, prey, actor]`; prey = actor_base with `disable_actions = [chase, eat]`, shared fsm/actor proves the two refs are independent.
- Positional sound (2026-07-19): `sound::task.max_distance > 0` => spatialized voice (linear attenuation in [min_distance, max_distance] world units), `0` (default) => spatialization DISABLED (UI/music play flat). Voices are pooled, so the toggle is set on every voice grant. `command_sound_play` carries volume/pitch/pos/min/max_distance; `command_sound_listener` (mailbox `sound_listener`, top-down default orientation) feeds the ma_engine listener each frame — tile_frontier publishes camera center as listener (also during pause) and emits sim sounds with min=0.25*audible, max=audible(=1.5*half_width); the old radius cull is now just a send budget aligned with the audibility edge.
- Input mapping from settings (2026-07-19): `input::bindings_config` (libs/input/bindings.h) is a tavl section `input = { actions = { camera_up = [key_w] } }` in the project settings schema (btree_map -> sorted deterministic file). The engine applies it OVER `bind_default_actions` on window creation and on reload_settings (listed action is rebound wholesale, empty list = unbind, unknown key name = warn+skip); `save_settings` dumps the effective live map back (guarded by `events::has_bindings`). Key names: keyboard canonical table + mouse_left/right/middle/mouse_4..8 + `scancode_N` lossless fallback. Lua: `app.save_settings()` / `app.reload_settings()`.
- WASD camera (first live input case, 2026-07-18): `bind_default_actions` binds the generic `camera_up/left/down/right` vocabulary to WASD (full config-driven bindings remain libs/input debt); `tile_frontier_game::move_camera` moves the camera at `half_width * 1.5` world units/sec using real frame time (presentation-plane: works during gameplay pause, unaffected by game speed) and clamps the camera point to the tile-world box (`world_extent_`); screen-up is -y in world space (Vulkan clip-Y points down — verified live). World enlarged to 8x8 chunks (128x128 tiles) in `worlds/tile_frontier.tavl`. Actors are clamped to the same world box in `integration_system` (after obstacle pushout). Camera is interpolated on the render thread: main publishes `command_draw_camera` snapshots (center/half_width/aspect/fb size + frame_time), render blends prev->cur with `nominal_clock` real-time alpha and composes the `camera_buffer` global UBO itself every render frame (main no longer writes it via write_buffer) — camera and actors share the one-snapshot presentation lag, so they stay visually in sync.
- `libs/simul/game_state.h` owns the standard main/game-thread state inherited by projects: loading/lifecycle, window/input, framebuffer policy, clocks/calendar/pause, broker ownership, visage/font handles and UI sound/device state. `game_host` owns standard UI startup from the active runtime-state, engine Lua bindings, entry-module load, config-selected default font, font GPU-view updates, device logging and sound-state merge. `tile_frontier::simulation_init` now contains only the assets-worker seam plus one `tile_frontier_game`; UI hooks forward project `perf_stats` and scene counters to that facade.
- `tile_frontier` lifecycle rule: main `simulation::init()` loads `resources/engine/config/app.tavl` (via engine demiurg registry), creates the broker, creates subsystem objects, wires `set_broker(br)` on each BEFORE starting their `std::jthread`s (no window required). Window creation is a late platform event controlled by `window.create_on_start`; when a window exists, main publishes `command_window_recreation` into the broker. Shutdown stops subsystem advancers, joins threads, destroys the window before GLFW termination, tolerates partial init.
- First render slice in `tile_frontier`: `render_simulation` owns Vulkan instance/device, `graphics_base`, `assets_base`, and `graphics_ctx`. It can bootstrap instance/device/basic render resources before a window if `main_device.tavl` exists; otherwise it waits for `command_window_recreation`, creates a surface, chooses/caches a physical device, creates swapchain/render graph, registers a test triangle mesh, fills instance/indirect buffers, and draws four test triangles using the same render config/shaders as root `main.cpp`.
- `graphics_base::recreate_basic_resources()` expects its config folder argument to end with `/` because parser paths are built as `path + "resources/"`, `path + "render_graphs/"`, etc. `tile_frontier` normalizes `render.config_folder` with a trailing slash before passing it to painter.
- Vulkan bootstrap note: with `VK_NO_PROTOTYPES` and Vulkan-Hpp dynamic dispatch, `painter::load_dispatcher1()` must initialize `VULKAN_HPP_DEFAULT_DISPATCHER` from a real `vkGetInstanceProcAddr`. `libs/input` now exposes this through GLFW as `input::get_instance_proc_addr()`. A null dispatcher here crashes as an address-0 segfault during `vk::createInstance()`.
- Vulkan device bootstrap note: `painter::device_maker::create()` initializes `VULKAN_HPP_DEFAULT_DISPATCHER` with the newly created `vk::Device` before any debug-utils object naming. `painter::set_name()` is intentionally a no-op if debug names are disabled or `vkSetDebugUtilsObjectNameEXT` is unavailable; object naming must never crash renderer startup.
- Render shutdown order matters: stop/join render thread first, explicitly drain render work in the render system, destroy `graphics_base`/swapchain, destroy surface, destroy device, then destroy GLFW window/input. `graphics_base` is a heavy resource container but does not own `VkDevice`/`VkQueue`; avoid hiding strong device/queue stalls in its destructor.
- `graphics_base` now separates parsed render-config storage from active graph instances. `set_startup_graph` seeds `resident_graphs_`; `add_resident_graph`/`set_resident_graphs` define graphs that may live at the same time. `commit_parsed_resources` builds active masks over resident graphs, creates only their resources/descriptors, and unions Vulkan usage from active graph steps plus active descriptor layouts. More than 256 graphs/resources/descriptors hard-errors via `std::bitset<256>` masks until a larger policy is designed.
- Render graph switching is staged: `create_render_graph_instance(index)` builds a new graph instance off to the side, `change_render_graph(index)` builds the target, then syncs with **`wait_all_fences()`** (per-frame fences of this `graphics_base`, NOT device/queue-wide waitIdle — fences are created signaled so the initial call is instant), swaps the active `execution_graph`, and clears the old graph-local objects. No resource recreation on swap: the used-set is the UNION of resident graphs, so pipelines rebuild (from assets-prepared SPIR-V + cache) but resources/descriptors stay. A few-frame pause is acceptable. Runtime swap is exercised via `command_set_active_graph` (config `render.demo_graph_toggle_ms` toggles graphics1↔menu1 for demo). `render_graph_instance::clear()` frees command buffers and graph-local semaphores/steps; `graphics_base` shutdown clears graph/descriptors/resources before command pool/swapchain/cache.
- `tile_frontier` render modes are runtime config driven: `render.enabled = false` skips render thread creation entirely, while `render.headless = true` allows a no-present render state without window/surface attachment. Window creation and `command_window_recreation` are only sent when render is enabled and not headless.
- Headless Vulkan bootstrap must not touch GLFW/input. `painter::load_dispatcher1(false)` loads `vkGetInstanceProcAddr` from the system Vulkan loader directly and `painter::get_required_extensions(false)` omits GLFW surface extensions. Headless device selection uses `system_info::choose_physical_device_headless()` and does not require presentation support or `VK_KHR_swapchain`.
- Root CMake defaults single-config generators to `CMAKE_BUILD_TYPE=Debug` when the user does not specify a build type. It also enables `CMAKE_POSITION_INDEPENDENT_CODE` globally so static third-party dependencies can link into shared libraries in Debug.
- New resource/render-prep contract tests: `tests/demiurg_resource_loader_test.cpp` covers CPU prepare vs external GPU commit and dependency gating; `tests/painter_shader_prepare_test.cpp` covers `glsl_source_file` SPIR-V preparation cache. Useful checks: `ctest --test-dir build-debug -R "(painter_shader_prepare_test|demiurg_resource_loader_test)" --output-on-failure` and `cmake --build build-debug --target tile_frontier`.

### Act scratch, generic stats, timelines/pause, and AI registries (2026-07-13)

- `act::call_context` has exactly ds's standard 8 scalar argument slots inline (`static_assert == 8`),
  plus reusable named lists/result. `act::execution_scratch` owns one ds VM + call frame and is embedded
  per worker rather than allocated locally or distributed globally/TLS. `acumen::execution_scratch`
  composes it with A* container + solution cache; tile_frontier holds one stable deque lane per pool slot.
  Script bind/collect works for bool/integer/number/entity basics; string/object/vector policy is pending.
- Generic numeric aggregate stats use real ds scopes, not prefixes. `register_stats<T,Parent,Getter,Domain>`
  registers the named scope getter and reflected local `field`/`add_field` functions. Scripts use
  `stats.hunger`, `stats = { add_hunger(…) }`, `combat_stats.abc`; ds overloads identical field names by
  returned `stat_scope<T>`. `initialize_stats<T>()` selects C++ default member initializers; its callback
  overload assigns every reflected numeric field. `stats_script_test` covers both modes and two aggregates.
- Time has four orthogonal coordinates: engine (never paused), presentation (real-rate world animation,
  pausable), game (pausable + rationally scaled), and turn (discrete). `game_time_scale` maps DURATIONS
  only; absolute timestamp conversion is intentionally absent because pause is non-bijective. Config
  convention: unqualified duration means nominal real/engine time; gameplay loaders convert through the
  project scale, while explicit game/calendar/turn values keep their domain. `simul::pause_state` owns
  gameplay/presentation masks; tile_frontier exposes Lua `app.set_paused/paused`, configures the ratio as
  `time.game_seconds / time.real_seconds`, and feeds actor systems the scaled game-clock delta. Calendar is
  a separate typed domain: immutable `calendar_clock` selects `time.calendar.source = game_time|turn` once
  at project startup; turn steps support seconds/days/months/years and are derived from the configured epoch.
- `acumen::registry` and `mood::registry` independently own stable immutable systems. There is no combined
  `brain_ref`: FSM may exist without GOAP and GOAP without FSM. `acumen::goap_resource` supports a
  single `base`, same-key replace-in-place, append, `disable_metrics/actions/goals`, cycle detection and
  flatten before `acumen::system` construction; `goap/actor` now inherits `goap/actor_base`. Compatibility
  contracts and runtime GOAP profile switching remain long-running. Tests: `act_call_context_test`,
  `stats_script_test`, `timeline_test`, `simul_lifecycle_test`, `goap_config_test`, `acumen_test`.

### Prefab & spawn (tile_frontier ↔ `libs/prefab`, 2026-07-12)

- `libs/prefab` (`devils_engine::prefab`): engine mechanism `prefab_registry<SpawnArgs>` = recipe
  of components. Forms: `data<C>` (tavl-deserialize, field-level inheritance via `base`), `list<C,Item>`,
  `callback<C>` (name→`act` fn hash), `reference<C>` (name→component via project resolver — fsm/goap by name),
  `custom` (project builder sees raw value text — inline-ds, custom list-inserters). Global + per-prefab
  `on_construct(name, fn)` for DERIVED components; `spawn(name, world, args)` = spawn_at (args → construct).
- tile_frontier: `food` and `actor` both spawn through the registry. Prefab TEXT comes from disk via
  owner-level `prefab::prefab_resource` (`prefab/*.tavl`; collected by `load_required_brain_config` → `brain_config.prefabs`);
  C++ registers component specs + construct in `setup_brain_registry` (shared init/load point); missing prefab
  resources or the required `actor_tuning`/`food_item` data fail loudly. `actor_tuning` (data component, NOT serialized) carries config knobs (speed/hunger/strength
  ranges); construct derives seed-based brain/visual/stats. Determinism: `SERIALIZABLE_COMPONENT` pins type_ids at
  static-init so the extra unregistered `actor_tuning` neither shifts registered ids nor enters `dump_world`.
- **GOTCHA:** a `//​---` literal in a `prefab/*.tavl` comment triggers demiurg's list splitter (`resource_manifest`
  splits `.tavl` on `//​---`) — the file breaks into sections and the prefab name becomes `name:0`. Never write
  three dashes after `//` in tavl comments.
- Primitive `spawn_at` is registered in `devils_script`: native `spawn_at(prefab, x, y)` over `spawn_scope`
  (carries a mutable `spawn_sink`, implemented by `actor_world_slice::spawn_prefab`). Bareword prefab → `string_view`.
  `tests/spawn_script_test.cpp` covers it via a mock sink. Seeding `spawn_scope` into live event/trigger scripts
  comes with the spawner work below.
- **TECH DEBT (deferred, author-confirmed 2026-07-12 — build later):**
  1. **per-entity system refs** — common independent acumen/mood registries are ready, but tile_frontier
     still selects one GOAP and one FSM for the whole slice. Do NOT introduce a combined brain_ref: use
     independent optional goap_ref/fsm_ref only where needed. FSM is normally fixed by entity type; GOAP may
     also belong to a squad/abstract thinker with no FSM. Compatibility validation + runtime GOAP switching
     (including per-profile caches/action indices) are a separate long-running task.
  2. **spawner entities + query (explicitly deferred again 2026-07-13)** — real spawn "where" = spawner ECS entities (static designer points OR dynamic
     rules), many of them, queried spatially (reuse `kd_tree` like perception) + predicate-filtered (tags, cooldown,
     capacity). `spawn_at(point)` stays the primitive; a selection/placement layer resolves intent → point.
     Determinism: spawn = catalogue effect on sim rng+state; "off-camera" is presentation (MP: sim must not read a
     local camera — use a replicated region/all-players notion).
  3. **ds `spawn`/`filter`/`pick`** — ergonomic script composition (`spawn(horse, pick(filter(spawners, mount, off_camera)))`);
     triggers = per-entity reference-list of spawners + on-enter ds script; dynamic spawner = point source is a ds/native fn.

### Message broker (inter-actor channels)

- All inter-thread messaging goes through one `core::broker` (`broker.h`), owned by main, created in `simulation::init` BEFORE subsystems and handed to each via `set_broker(br)` BEFORE its thread starts (ordering: create → set_broker×3 → threads). Each channel is strictly 1-producer/1-consumer. Primitives live in `libs/utils/include/devils_engine/thread/`:
  - `mailbox<T>` — latest-wins triple-buffer: producer fills `write_slot()` IN PLACE (T's capacity, e.g. a `std::vector`, is reused across the 3 slots → no per-frame alloc), `publish()`; consumer `consume() -> const T*|nullptr`. drop-oldest is normal. One atomic. Use for snapshot/latest channels.
  - `spsc_queue<T>` — fixed-capacity FIFO ring, move-aware, overflow = drop-newest (`try_push` returns false). Use for reliable commands + lossy bursts.
  - `payload_channel<Msg>` — `spsc_queue<Msg>` + `byte_ring` for messages with a byte payload. `write(size, fill)` where `fill(region,pos)->Msg` (channel owns alloc+push atomicity: queue-full check BEFORE `byte_ring::alloc`, else arena leaks); `drain(handler)` where `handler(const Msg&, span<const std::byte>)` then FIFO-release. Msg MUST carry `int64_t pos; uint32_t size;`.
  - `byte_ring` — SPSC bip-buffer, MONOTONIC positions (padding at wrap self-reclaims), single atomic (`tail`); payload-byte visibility piggybacks on the MESSAGE queue's release/acquire (payload written before `push`). Reclaim is a monotonic cursor because consumption order == allocation order — arena is PER-CHANNEL, never per-edge (multiplexed edges break the cursor). Payload valid only until `release`; copy out if kept longer.
- Channel policy by semantics (each channel in `broker`): latest-wins → `mailbox` (window_recreation, set_active_graph, draw_tiles, draw_actors, shaders_prepared, sound_state); reliable payload → `write_buffer_channel` (camera/UI); reliable/lossy FIFO → `spsc_queue` (gpu_transition/gpu_done/prepare_shaders/load_resource/load_chunk/chunk_loaded/sound_play/stop/update/devices/recreate_sound). Budgets are FIXED in the broker ctor (256 reliable, 64 sound-lossy, 8 one-shot, write_buffer 64 msgs + 1 MiB arena).
- Payload budget note: the two biggest per-frame payloads (draw_actors ~92 KB, camera+UI via write_buffer) now allocate ZERO per frame — mailbox slot vectors and the byte_ring are preallocated and reused.
- Tests for the primitives: `tests/thread_general_test.cpp` (byte_ring wrap/reclaim/overflow, payload_channel FIFO+overflow, mailbox latest-wins + slot-capacity reuse), alongside the existing `spsc_queue` tests.
- Deferred (see memory `message-broker-design`): segmented runtime growth for reliable channels (current overflow is drop-newest at fixed capacity). The dead actor/dispatcher baseline cleanup is complete; `actor_ref`/`actors.h` remain locally inspectable under ignored `exclude/`. Deviation: assets `gpu_transition` producer now unconditionally `try_push`es (benign — loader marks jobs in-flight so ≤1 push/resource; with render off resources just stay warm).

### Tile map / world slice (current WIP)

- First world/tile slice is now wired through the existing actors. `tile_map.{h,cpp}` owns the main-side CPU model: `tile`, `tile_grid`, `chunk_coord`, `tile_chunk`, `camera2d`, `visible_tiles`, plus `generate_mock_chunk` and `apply_chunk`. It is intentionally painter-free.
- Mock world streaming uses broker channels, not direct calls: main pushes `command_load_chunk{x,y,size,texture_count}` into `broker.load_chunk`; assets generates a deterministic CPU `tile_chunk` on the assets thread and pushes `command_chunk_loaded` into `broker.chunk_loaded`; main drains it and applies into the 4x4 chunk grid (16x16 tiles each). (Replies go to fixed broker channels — no `reply_to` field anymore.)
- Main-side render path for the map: `simulation::update()` computes `visible_tiles(cam, grid, margin=1)`, `tile_batch` packs `tile_instance{world_center, texture}` as layout `v2ui1` directly into `broker.draw_tiles.write_slot().bytes` (in-place, reused), writes `global_ubo_t` to `camera_buffer` via `broker.write_buffer`, then `broker.draw_tiles.publish()`. draw_tiles/draw_actors are latest-wins **mailboxes** (render `consume()`s the freshest snapshot; stale ones drop) — see Message broker.
- Render-side tile path: render config has `dg_tiles` (host_visible, layout `v2ui1`) and `draw_tiles` before `draw_ui`; shaders are `tests/shaders/tile.vert.glsl` and `tile.frag.glsl`, material is `tests/test_render_config/materials/tile.tavl`. `render_create_tile_draw()` creates one GPU `tile_quad` mesh and registers one pair with max 5000 instances; `render_update_tile_draw()` writes BOTH per_update instance/indirect buffers each update.
- Tile textures currently reuse the asset texture descriptor `textures`; `tile_instance.texture` is the texture array index. This is still a bootstrap path, not the final terrain/material system.
- Minimal actor slice is visible on screen. `actor_simulation.{h,cpp}` owns the first lightweight ECS slice: `actor_position`, `actor_velocity`, `actor_brain`, `actor_visual`; deterministic brains produce sorted `actor_move_intent`s, apply mutates positions, and `actor_batch` packs `actor_instance{pos, texture, color, size}` as layout `v2ui1c4v1` (`color` is packed `c4` RGBA8, `size` is 32-bit float via `v1`).
- Main publishes `command_draw_actors` into `broker.draw_actors` (mailbox) every update after tiles, filling the slot's `bytes`/`ids` in place (reused). Render config has `dg_actors` (host_visible, layout `v2ui1c4v1`) and `draw_actors` after `draw_tiles` and before `draw_ui`; shaders are `tests/shaders/actor.vert.glsl` and `actor.frag.glsl`, material is `tests/test_render_config/materials/actor.tavl`. `render_create_actor_draw()` creates one GPU `actor_triangle` mesh and registers one pair with max 5000 instances; `render_update_actor_draw()` writes BOTH per_update instance/indirect buffers.
- Actor render gotchas already hit: if `actor.vert.glsl` declares an input such as `in_tex` but shader optimization removes it, validation reports `Vertex attribute at location N not consumed by vertex shader`; keep declared vertex inputs observably used. Actor quads were also invisible when their world Z was `0.08`: with the current `glm::ortho(..., -1, 1)` / Vulkan clip convention they clipped behind the tile layer. Use the same world Z as tiles (`0.0`) unless the camera depth convention is deliberately fixed.

### Assets / resource pipeline (tile_frontier ↔ demiurg ↔ painter)

- **Stable handles (2026-07-06, `11a4e93`/`8ce23c1`):** the durable resource currency is now `demiurg::resource_handle` = `{const resource_system*, utils::id hash}` (hash of the logical id, NOT a pointer); `handle.get()` = `system->get(hash)` (O(1) `resources_by_hash`), `get<T>()` checks `is_type(type_id<T>())` = exact `type_id` OR `loading_type_id` (split 2026-07-06: `type_id` = exact C++ identity, always set by `register_type`; `loading_type_id` = loader-dispatch key, may be a BASE — so `get<visage::font_resource>()` and `get<painter::gpu_texture_resource>()` both work on a font), `operator bool`. It SURVIVES `clear()`+re-`parse_resources()` — the same id re-resolves to the freshly-instantiated object (`rebuild_hash_index()`/`register_hash_key()` run at the end of parse/append; `register_hash_key` errors on a true hash collision, warns+keeps-first on duplicate id). Make one via `resource_system::handle(id)`/`handle(hash)`/static `resource_hash(id)`. Consumers migrated: `flow::image_ref`/`sprite_sample` hold a handle; tile_frontier's inter-thread commands (`command_sound_play`/`command_load_resource`/`command_gpu_transition`/`command_gpu_done`) carry a `resource_ref` wrapper (`messages.h`) = handle + `direct` raw-pointer fallback (factories `from_handle`/`from_direct`/`from_system`; `from_system` only keeps `direct` if the id doesn't round-trip). `direct` exists for SYNTHETIC resources built outside any registry (none in-tree since fonts moved into the assets registry 2026-07-06). A bare `resource_interface*` is still fine for same-thread, same-lifetime reads (assets registry is built once, read-only), but cross-thread/persistent references should ride a handle/`resource_ref`.
- Resource currency was `demiurg::resource_interface*` (NOT a string id); see stable-handle bullet above. State machine in atomic `_state` is a GENERIC LEVEL ladder `0..top_state()` (was fixed cold/warm/hot). Defaults: cold(0)/warm(1)/hot(2), `top_state()`=hot. A resource with more steps overrides `top_state()` + `load_step(from)`/`unload_step(from)` (default dispatch to named `load_cold`/`load_warm`/`unload_*` for the 3 base rungs) + `is_external_step(from)` (which transition runs on the render/GPU thread). `state()` returns the raw int32 level (NO warm→hot remap anymore); `usable()` = `_state >= final_state()`; `final_state()` = `warm` if flag `warm_and_hot_same` else `top_state()`. Example multi-step: `tile_frontier::font_resource` (4-state, ttf→MSDF→GPU) — see font bullet below.
- `demiurg::resource_interface::dependencies` (a `std::vector<resource_interface*>`, filled by the resource type via `add_dependency`) — NOT the ring-lists (those are for mod override chains). The loader drives the whole transitive closure and gates a resource's UP-movement until all its direct deps are `usable()` (transitive correctness holds by induction since a dep isn't usable until ITS deps are; shared deps dedupe). INVARIANT: a resource with deps MUST be driven through the loader, not `res->load()` directly (direct load bypasses gating). Assumes a DAG.
- `demiurg::resource_loader` (lives ONLY in the assets actor) is a reconciler: `request(res, target)` sets desired LEVEL (clamped to `res->final_state()`; also recursively `request`s deps) ; `update(out)` steps each ready resource one transition toward its target. A transition is local (disk/CPU, run inline) unless `res->is_external_step(from)` → emitted as `external_job{res, load}` for the render actor (GPU table is render-owned — avoids cross-thread sync). Currently SINGLE-THREADED per tick (one step per ready resource); planned future: inject a `thread_pool` for level-load bulk fan-out + a per-tick budget for gentle background loading (API already accommodates both). Heavy render resources should be staged: assets may do CPU-heavy prepare/compile in local steps, while external steps publish/commit render-owned GPU handles. Creating `VkPipeline` off-thread is technically possible with `VkDevice`/cache sync, but the default design keeps `VkPipeline`, layouts, render-pass dependencies, and destruction on the render side.
- Contract: main holds `resource_interface*` from the assets registry (built once in `assets_simulation::init` via `resource_system::parse_resources`, then read-only) and pushes `command_load_resource{res, res->final_state()}` into `broker.load_resource`. assets runs local (CPU) steps, pushes `command_gpu_transition{res,load}` into `broker.gpu_transition` for external steps; render runs `res->load(safe_handle_t(&gpu_load_context))` → the external step (e.g. `load_warm`/`load_step`) does the GPU upload, writes `gpu_index`, frees the CPU copy, advances `_state`, acks `command_gpu_done`. main observes `usable()` + reads `gpu_index`. (Send `final_state()`, NOT `state::hot` — multi-step resources like font have a higher top level.)
- Common resource loaders now live in their OWNER libs, not tile_frontier: **painter** has `gpu_texture_resource` (base: RGBA `memory`+w/h+`gpu_index` + `load_warm`/`unload_*` GPU upload; `load_cold` no-op), `texture_resource : gpu_texture_resource` (png/stb `load_cold`; painter gained the stb-src include + `stb_image_impl.cpp`), `mesh_resource`, `gpu_load_context.h`. **visage** has `font_resource : painter::gpu_texture_resource` (ttf→MSDF CPU steps fill base `memory`, GPU step reuses base `load_warm`; NO longer `: texture_resource` — png/stb not needed; visage gained a `demiurg` dep). Generalization: textures register as `register_type<gpu_texture_resource, texture_resource>(...)` so `loading_type_id` is the BASE — render's texture check, `texture_set`, and `get<>` all work through `gpu_texture_resource` (only need `gpu_index`), unaware of the concrete png decoder. `gpu_index` = the asset's slot in `assets_base`. tile_frontier keeps only app-specific `app_config_resource` + `texture_set`.
- demiurg GOTCHA: `resource_system::find_proper_type` matches a registered type NAME against a path SEGMENT of the resource id — resources MUST live in a type-named folder. e.g. type `register_type<mesh_resource>("mesh","mesh")` needs the file at `<module>/core/mesh/test.mesh` (id `mesh/test`); a bare `core/test.mesh` is skipped. Module layout: `module_system(root)` looks for `root/core/` (or `core.zip`); call `resource_system::parse_resources(module_system*)` (it does open/parse/close + finalize), not `module_system::parse_resources`.
- `copy_directory` in CMake POST_BUILD copies but does NOT delete files removed from source — stale assets linger in the build output (e.g. a moved mesh). Clean the output dir if a phantom file warning appears.
- **Engine (non-replaceable) registry** — SEPARATE `resource_system`+`module_system` from the game/mod registry (Q2: don't mix; mods get free path namespace). Root `resources/engine/`, loaded as a module directory via `module_system::load_modules({list_entry{"engine",...}})` (no new demiurg API needed). `tile_frontier::simulation::init` builds it and preloads: `app_config_resource` (`config/app`, CPU-only, `load_cold` parses `app.tavl` via tavl → replaces old raw-`file_io` `load_app_config`, removed) and `painter::render_config_source` (`render_config/*`, CPU-only text of each render-graph tavl file). CMake copies `tests/test_render_config` into `resources/engine/render_config/` (source unmoved — shared with fast_test/tests).
- **Render-graph loads through demiurg** (not a folder scan): `graphics_base::recreate_basic_resources` has two overloads — `(folder)` (fs, fast_test) and `(const demiurg::resource_system*, prefix)` (engine registry). `structures.cpp` `parse_data` is now content-source-agnostic (a `config_source{read_file,read_folder}` seam with fs + demiurg builders); `parse_config_content<T>(text)` unchanged. tile_frontier passes the engine registry + `"render_config/"` prefix; render config resources are preloaded to `warm` on main at init (render thread only reads them). `commit_parsed_resources` holds the shared post-parse tail.
- **Prepared shader/pipeline path (first slice)**: shader compilation moved to assets-side preparation. `painter::glsl_source_file` keeps source `memory` plus prepared `spirv`/`spirv_shader_kind`; `prepare_spirv(reg, kind)` compiles with `shader_crafter` and demiurg include resolution. Assets drains `command_prepare_shaders{registry,prefix}` from the broker, compiles engine GLSL resources, then publishes `command_shaders_prepared` into the broker mailbox; render delays `change_render_graph()` until that ack and then only creates `VkShaderModule`/`VkPipeline` from prepared SPIR-V. `load_shader_module()` still has a synchronous render-thread compile fallback with a warning, but normal `tile_frontier` startup should use the assets path. This is not yet a full `prepared_pipeline_resource`: material/geometry/render-pass pipeline assembly remains render-owned.
- **`visage::font_resource`** (`: painter::gpu_texture_resource`, moved from tile_frontier, replaced `font_atlas_resource`) — the reference multi-step resource: `top_state()`=3, `load_step` 0→1 reads ttf via `module->load_binary(path)`, 1→2 runs `font_atlas_packer` (MSDF atlas RGBA into base `memory` + glyph `font_t`), 2→3 reuses base `load_warm` (GPU); `is_external_step` true only for 2→3. Since 2026-07-06 fonts are REGULAR assets-registry resources: `register_type<painter::gpu_texture_resource, visage::font_resource>("fonts","ttf")` (files in `resources/modules/core/fonts/`, moddable like textures; `loading_type_id` = base → render treats the atlas as a texture, exact `type_id` → `handle.get<font_resource>()` works). The runtime-state allowlist drives their CPU/GPU steps; `game_host` creates visage from the selected default font's CPU metrics and `standard_game_state::ui_fonts` tracks when render writes each `gpu_index` so host can stamp `font_t::texture_id`. Lua selects a font by HANDLE: `nk.push_font{ font = request("fonts/crimson.italic"), ... }` — the old string-name registry (`add_font`/`resolve_font`/`fonts_`) was removed from `visage::system`. Legacy `visage::load_font(painter::host_image_container*,…)` + its freetype/msdf helpers were removed; the obsolete `arbitrary_image_container` pair is archived in `exclude/`.
- **Engine cache registry merged (2026-07-04):** the pipeline cache is no longer a separate `resource_system`; its MODULE stays separate (rooted at `<cache>/painter/`) but its resources are appended into the shared engine registry via new `demiurg::resource_system::append_resources(module_system*)` (ingest + add only new to the sorted index, dedup via `get()`; does NOT re-run override/ring dedup — for disjoint sub-registries). Needed because `parse_resources` clears and `cache_folder` is only known after the config parse.

### Texturing / render-graph descriptors (painter)

- `painter::sampler` is a render-graph type (config `samplers/*.tavl`: name, filter, address; VkSampler created in `graphics_base::create_samplers` before descriptor layouts). `descriptor.layout` entries are `(slot, usage, sampler_index, shader_stages)`; when an entry has a sampler the binding becomes `eCombinedImageSampler` with an IMMUTABLE sampler baked into the layout (so no per-frame sampler buffering needed). Config descriptor layout entries are `{resource, usage, sampler, stage}` structs (stage parses `fragment|vertex|...|all`).
- Asset textures are NOT render-graph resources. A descriptor declares a `texture_count`/`texture_sampler`/`texture_stage` asset-texture binding (combinedImageSampler ARRAY, binding index = `layout.size()`); `update_descriptors` skips it and the render actor fills views from `assets_base.texture_slots` (slot index = `gpu_index`). A draw step binds descriptors via its `sets = [name]` config list (set 0-based in list order); `dg<n>.descriptor` is NOT auto-bound, instance data is a VERTEX BUFFER (binding 1).
- Bindless table is sized 4096 (`descriptors/textures.tavl` `texture_count`), clamped in `create_descriptor_set_layouts` to device limits (min of maxPerStageDescriptorSampledImages/Samplers + set-level). `graphics_base::recreate_descriptor_pool()` (called in commit before `create_descriptor_sets`) sizes the pool from the ACTUAL descriptors (fixed 256/type couldn't hold 4096×frames → `vk::OutOfPoolMemoryError`). **Every array element must be a valid view or validation fires `VUID-vkCmdDrawIndirect-None-08114` on the first draws (before textures load).** Fix: `assets_base::default_texture` — a permanent 1×1 magenta placeholder created at render init (OUTSIDE `texture_slots`); `render_bind_textures` fills ALL N elements with it once at graph-ready, and each texture load does a POINT update (`render_bind_texture_slot`, `dstArrayElement=slot,count=1`) instead of rewriting the whole array.
- `assets_base` slot state is a PLAIN `asset_state` (atomics removed 2026-07-04): `assets_base` lives strictly on the render thread; cross-thread index publication rides `demiurg::resource::_state`, so the per-slot atomics were dead weight.

### Painter gotchas hit while wiring textured draws (Linux/Intel windowed path was undertested)

- `descriptor_set_layout_maker::combined` stores `pImmutableSamplers = samplers.data()` — the sampler vector MUST outlive `create()` (keep it in a container, don't pass a loop-local temporary, or the driver derefs freed memory and crashes).
- `image_acquire` used a 1ms `acquireNextImageKHR` timeout → near-certain `Timeout` crash under vsync (~16ms/frame); now a generous finite timeout.
- Texture upload barrier in `assets_base::populate_texture_storage`: transitioning to ShaderReadOnly needs a dst stage that supports `eShaderRead` (`eAllCommands`), not `eTransfer` (valid only when graphics+transfer share a queue family).
- Draw-group instance/indirect buffers for a host_visible draw group are DOUBLE-buffered (per_update). Static data written asynchronously while the render loop is running can land in the wrong buffer after `update_event()` shifts parity — write to BOTH buffers (`get_current_*_resource_frame(pair, off)` for off in {0,1}).
- `register_pair(dg, mesh, max_count)` strides each pair's indirect offset by `max_count * indirect_buffer_size`; the indirect buffer is small, so a large `max_count` across multiple pairs overflows it (VUID-vkCmdDrawIndirect-00487). Keep `max_count` modest, or enlarge the indirect buffer / stride by one command per pair.
- Descriptor buffering gotcha: use `resource::compute_buffering(base)`, not `static_cast<uint32_t>(resource.type)`, when creating descriptor set layouts or writing descriptor arrays. The enum value is not the runtime buffering count. `VkDescriptorBufferInfo::range` also must be clamped to the actual remaining bytes in the packed `resource_container` from `subbuffer.offset`, otherwise validation reports VUID-VkDescriptorBufferInfo-range-00342.
- Render-graph buffer packing now distinguishes logical frame size from aligned frame stride. `graphics_base::create_resources()` aligns buffer suballocation offsets by final VkBuffer usage: uniform buffers use `minUniformBufferOffsetAlignment`, storage buffers use `minStorageBufferOffsetAlignment`, and texel buffers use `minTexelBufferOffsetAlignment`; descriptor `range` stays the logical `subbuffer.size`.
- Render-graph layout strings support `mat4` and `mat44` aliases. `parse_layout` expands either alias into four `v4` atoms; they are layout macros, not real `VkFormat`s/vertex attributes. Use them for buffer layouts such as `mat4mat4v4v4v4v4`.
- Render-pass barrier gotcha: `vkCmdPipelineBarrier` is illegal inside a render pass subpass unless the render pass has a matching self-dependency. `execution_pass_instance::process()` must apply `pass.barriers[0]` BEFORE `beginRenderPass()`. Avoid adding ordinary `step.barriers` to draw steps inside render passes unless painter grows a pre-pass barrier phase or self-dependencies; `draw_ui` already intentionally skips `make_barriers1()` for this reason.
- KNOWN-OPEN (non-fatal warnings): present semaphore is indexed per-frame-in-flight, not per swapchain image (VUID-vkQueueSubmit-00067 — fix: per-image present semaphores); swapchain images created with only TRANSFER_DST but a view needs attachment usage (VUID-04441). (`VUID-vkCmdDrawIndirect-None-08114` was FIXED via the default placeholder texture — see Texturing section.)

### Sound / miniaudio contract

- `libs/sound` is being migrated from OpenAL to miniaudio. The old `sound::system` / OpenAL-oriented helpers still exist; do not remove or rewrite them casually. New work is centered on `sound::system2`.
- `sound::system2` owns a miniaudio context/device/engine plus precreated mono spatial and stereo non-spatial voice pools. Mono voices are intended for positional `sfx` / `talk_pos`; stereo voices are intended for UI/music/non-spatial playback. Positional sound with miniaudio should generally use mono sources.
- `system2` uses a custom miniaudio data source as a PCM ring stream. That data source should stay dumb: byte/frame ring-buffer state, read/write cursors, `frames_read_total`, `frames_written_total`, underrun count, and format info. It must not know about `task_id`, `after`, or higher-level sound scheduling.
- Higher-level sound state lives in `system2::sound_task`: task id, resource view, decoder/converter, segment begin frame, segment length, decoded frames, and started/initialized flags. `stat_sound` and `snapshot` report `progress` as the ABSOLUTE normalized position in the SOURCE `[0,1]`, i.e. `(start_frame + local_frames) / source_frames_count` where `start_frame = source_frames_count - stream_frames_count`; before playback it is `task.start`. (It used to be segment-relative `local/stream_frames_count`, which made a sound started at `start=0.5` report 0% and advance at 2× — fixed 2026-07-01; absolute position is what a player seek bar needs.)
- `task.after` means gapless continuation on the same voice. The sound thread registers the next segment only after the previous segment has been fully decoded into the shared stream, so the callback continues reading PCM without task-aware branching. Late `after` messages can still miss the practical gapless window if the stream underruns.
- `system2` supports `task.start` as normalized `[0,1]` start position, `update_sound(task_update)` for long positional sounds that need refreshed position/direction/velocity, `snapshot(vector<task_status>&)` for a full current task slice, and basic setup filters (invalid id/resource/type, duplicate task id, positional sounds beyond max distance).
- Decode work per sound update is budgeted by `decode_frames_per_update` and also bounded by ring free space, remaining segment frames, and scratch cache capacity. `stream_buffer_seconds` controls precreated ring-buffer duration; if the sound actor runs slower than the audio buffer coverage, underruns are expected.
- `system2::playback_devices(out)` enumerates playback device names. `system2(device_name, ...)` tries to create that device; if the requested name is not present in the current list, it logs `utils::warn` with the requested name and the default playback device name, then creates the default device.
- **Sound is a demiurg resource (2026-07-04, staged/compressed):** `sound::sound_resource : demiurg::resource_interface` (in `libs/sound`, which gained a `demiurg` dep) — CPU-only (`warm_and_hot_same`), `load_cold` reads bytes via module + infers `data_type` from extension, exposes `resource2 view()` (id + type + span). Managed by the ASSETS thread like mesh/texture; sound files MOVED to `resources/modules/core/sounds/<sub>/`, registered `register_type<sound::sound_resource>("sounds","mp3,flac,wav,ogg")`. (TODO: full PCM decode of short sounds needs a mixer PCM branch — `make_decoder(pcm,…)` is a null stub and `resource2`/`task` lack PCM format.)
- Sound messages (`messages.h`): `command_sound_play{taskid,after,res,start}` (`res` is a `demiurg::resource_interface*` handle — main resolves name→resource), `command_sound_stop{taskid}`, `command_sound_update{taskid,pos/dir/vel}`, `command_sound_devices`, `command_recreate_sound_system` (main→sound); `command_sound_state{vector<sound_state_entry>}` (sound→main). Play/stop are separate types on purpose. All flow through the `broker` (sound_* are `spsc_queue`, sound_state is a `mailbox`).
- `sound_system` (sound actor) NO LONGER preloads or stores sounds: it plays from `static_cast<sound_resource*>(cmd.res)->view()`, gracefully skipping (warn) when the resource isn't warm yet. **main** owns a `name_hash → sound_resource*` map (`sound_by_name`), requests the sounds to `warm` through the assets loader, and resolves it for both the UI binding (`app.play_sound`) and gameplay sim-emits. (Startup ~2s window: sim-sounds fired before the async load completes are "not ready"/skipped, then play once loaded.)
- `sound::resource2` is a non-owning view (`id`, `type`, `span<const char>`); callers must keep id/data alive while a sound task uses them. Its default type is `data_type::undefined` and empty data.

### UI sound API & cross-thread state delivery (tile_frontier)

- Lua UI scripts reach sound through a host-registered `app` namespace in the visage sandbox (older `tf_*` globals are being phased out): `app.play_sound(res [, {start=0..1, after=handle}])` — as of `d2ce7fe` `res` is a demiurg `resource_handle` from `request(...)`, NOT a string name (also one-table form `app.play_sound{ resource=…, start=, … }`) → opaque `sound_handle`; `app.stop_sound(handle)`; `app.sound_state(handle)` → `progress(0..1)` or `nil` (a number means "in progress", nil means "gone"). Each is just a normal message to the sound thread; presentation sounds are NOT in the replay log.
- visage stays UI-agnostic: it exposes `system::script_state()` / `script_env()` so the HOST registers its own usertypes/functions (sound, later assets) into the UI Lua state/env. `sound_handle` is a usertype with no arithmetic metamethods — a Lua script can't do math on the id or pass a stray number where a handle is expected. The C++ message contract stays a plain `size_t` (the wrapper lives only at the Lua boundary).
- **Cross-thread state is PUSH, not pull.** Each background system publishes its full simplified state periodically; main reads the latest. Now realized as a `broker` **`mailbox`** (`sound_state`): sound fills `broker.sound_state.write_slot()` in place (slot vector reused) and `publish()`s; main `consume()`s the freshest. This replaced an earlier pull scheme (raw `atomic_bool*` + output-vector pointer, rejected). Same pattern planned for assets (in-flight resources → loading progress bar).
- main merges the published `command_sound_state` into a single table `std::vector<ui_sound_state_entry{taskid, progress, deadline}>` (NOT a second container). The publish is authoritative for live sounds (`deadline=0`); `app.play_sound` also inserts an OPTIMISTIC entry (`deadline = frame + grace`) so a just-requested sound reads as `0`, not `nil`, during the 1-2 frame request→register→publish latency — otherwise a Lua player sees the startup as "finished" and resets its handle. consume rebuilds: publish entries (confirmed) + still-young optimistic entries the publish doesn't yet know; a confirmed entry that drops out of the publish = finished. A scratch `sound_state_next` + `swap` avoids per-frame allocation. `app.sound_state` is then a plain lookup (expired-optimistic ⇒ nil).
- `tile_frontier` still has a temporary sound smoke scenario: main asks sound for playback devices, logs all names, then after roughly 45 ticks sends recreate-device followed by a play command. The sound actor processes recreate before play so the new sound is queued into the new `system2`.

### Window management, UI control & app-state FSM (tile_frontier, 2026-07-05)

Three debts closed together; all changes additive over the broker/actor model. Built rc=0, 37 tests green, live run confirms `boot→loading→game` + fonts + focus events, no VUID/lua errors.

- **GLFW window management.** `libs/input` gained `framebuffer_size`/`window_focus`/`window_iconify`/`window_maximize` callbacks (dedicated setter names — those signatures collide with existing `window_size`/`cursor_enter`, so overloading `set_window_callback` would be ambiguous), plus helpers `framebuffer_size`/`window_focused`/`window_iconified`/`maximize_window`/`restore_window`/`set_window_monitor`/`window_pos`/`set_window_size`/`set_should_close`. Main accumulates window events in a file-local `g_window_events` (same no-atomics pattern as `g_ui_input` — callbacks fire in `poll_events` on main). A C++-tunable `window_policy` (`draw_when_unfocused`/`draw_when_minimized`/`mute_when_unfocused`/`focused_/unfocused_master_gain`) drives reactions; effective `active = focused && !iconified` (minimize = focus loss).
- **Resize path is SWAPCHAIN-ONLY.** New `command_window_resize{w,h}` (mailbox, main→render); render's `render_resize_swapchain` = `wait_all_fences()` + `graphics_base::resize_viewport(w,h)` (which already does recreate_swapchain + screensize resources + graph viewport — the SAME path `render_try_create_graph` uses at startup, so it's proven). `recreate_swapchain` asserts `extent!=0`, so main NEVER publishes resize on a 0×0 (minimized) framebuffer. **Projection bug fixed:** `ui_proj`/`misc`/`cam.aspect` now derive from the LIVE framebuffer size, not static `config.window.*`.
- **Focus/minimize reactions.** `command_sound_set_master_gain{gain}` (spsc, main→sound) → sound actor calls the pre-existing `system2::set_master_volume` (survives device recreate via a stored `master_gain`). `command_render_set_active{draw}` (mailbox) gates render's draw block (`draw_active`). Fullscreen via `apply_fullscreen` (glfwSetWindowMonitor; saved windowed rect for restore) — mode change emits a framebuffer_size event → normal resize path.
- **UI control.** `stop_predicate()` now reads a real `std::atomic_bool quit_requested` + `input::should_close` (the 200-frame `test_counter` is GONE). Host `app.*` namespace (alongside `app.play_sound`): `quit_game`/`maximize`/`restore`/`set_fullscreen`/`is_fullscreen`/`set_master_volume`/`set_resolution`/`set_sound_device` (the last three are the "poke setting → diff → message" pattern: resolution→`set_window_size`→resize path, device→`command_recreate_sound_system`). `app.action_pressed/clicked(name)` query `input::events` (the named-action layer, now wired: `events::init/set_key(escape→quit, f1→toggle_menu)` in window setup, `events::update_key` in the key callback, `events::update` each frame). `app.state()`/`app.loading_progress()` for the FSM.
- **Multi-font (C++ side).** (UPDATED 2026-07-06: the name registry is GONE — `visage::system` keeps only `default_font`; `add_font`/`resolve_font`/`fonts_` removed.) Lua selects the base font by demiurg HANDLE: `push_font{ font = request("fonts/..."), size=, ... }` — the binding resolves it via `handle.get<font_resource>()`, silently falls back to default while `font()` metrics aren't ready (transient), warns ONCE if the value isn't a font handle at all. `sized_font` is keyed by `(base font, height)`.
- **Nuklear styling.** `nk.style_default()` = `nk_style_from_table(ctx, nullptr)`; `nk.style_from_table{ text=, window=, button=, ... }` seeds nuklear's default color table (values duplicated in `seed_default_theme` since `NK_COLOR_MAP` is only visible in the NK_IMPLEMENTATION TU) then overrides named entries and applies — a stateless "load a theme" contract (`nk_style` has no flat `colors` array).
- **App-state FSM.** Lightweight `enum class app_state{boot,loading,game}` in the orchestrator (NOT `mood` — that's per-entity/act). `boot→loading` when the UI font atlas is `usable()` (text renders); `loading→game` when the whole `startup_resources` set (grass textures + both fonts) is `usable()` AND mock chunks applied. Loading progress is computed MAIN-SIDE from `startup_resources` `usable()` counts (main holds the pointers; no assets-side publish — the planned `command_assets_state` push was dropped to avoid dead code). Single render graph throughout (graphics1); state GATES publishing (`draw_tiles`/`draw_actors` + actor sim only in `game`) but the camera buffer / `ui_proj` is written ALWAYS (UI must render on splash/loading). `entry.lua` picks screen by `app.state()`: splash (italic-font logo) / loading (`nk.progress` bar) / game (existing panels + Quit/Fullscreen buttons); Esc→`app.quit_game`.
- **visage `update` now takes `(time, timestamp, rng_state)`** (was `(time)`) and forwards them to the lua entry `function(time, timestamp, rng)`. `timestamp` = monotonic time mark (main accumulates `ui_timestamp += time`, starts at 0) for pinning UI-animation starts; `rng_state` = a per-frame PRNG seed — main advances a dedicated 256-bit `utils::xoshiro256starstar` state (`ui_rng`, seeded `string_hash("visage_ui")`) each frame and passes `value()`. Goal: decouple UI randomness from real math.
- **`bindings::rng_state`** (env.h: `struct rng_state{uint64_t s;}`) is an opaque PRNG-state usertype in the UI lua sandbox (registered in `basic_functions`, under `base`). `base.prng64(rng)→rng` (next state, splitmix hash-step; also keeps the legacy `prng64(int)→int` via `sol::overload`), `base.value(rng)→[0,1)` and `base.value(rng,n,m)→int[n,m]` (also as methods `rng:next()`/`rng:value()`), `base.rng(seed)→rng`, and `+` metamethod = `utils::mix` hash-mix (`local s3 = s1 + s2`) — NOT arithmetic. visage passes `bindings::rng_state{seed}` to the entry. 32-bit `prng32*` free functions kept as-is (legacy). Verified live: game UI calls the whole rng API every frame with zero lua errors.

### libs quick tech-debt pass (block A, 2026-07-05)

Batch of small closures across libs (see `ROADMAP.md` «Тех-долг»). Build rc=0, tests green (except bundled cpuinfo `init-test`), tile_frontier reaches `game` with no lua errors. API-affecting bits:
- **`libs/input/events`**: mouse buttons are now FIRST-CLASS bindable — synthetic scancode range (`mouse_button_scancode(button) = (1<<30)+button`), `events::update_mouse_button(button,state)` / `events::set_mouse_button(id,button,slot)`. The whole `set_key`/`check_event` machinery works over them unchanged; tile_frontier's mouse callback feeds `update_mouse_button`. Wheel stays in `auxiliary` (analog).
- **`libs/painter` clear**: `transfer_clear_color`/`transfer_clear_depth` implemented (were `assert(false)`). Value comes from the step's CONSTANT: `v4`=float32×4 memcpy'd into `ClearColorValue` (also ui4/i4), `c4`=rgba8 unpacked /255 → float; depth = first float, optional 2nd component = stencil. tile_frontier adds `command_update_constant{name,bytes}` (broker→render → `find_constant`+`write_constant_data`+`update_constant_memory`) so external code can update a constant (e.g. clear color).
- **`libs/mood`**: new `mood::settle(sys, cur_state, event, ctx, max_idle_iters=8)` runtime helper (event→apply→settle-idle completion loop with cap; stops on no-transition/internal/self-loop). Parser now bounds-checks guards/actions (>8 → error, was silent overflow) and reports token POSITION in errors. Tests added in `utils_general_test.cpp` (blocked/internal/settle/limits).
- **`libs/act` registry**: `reg()` distinguishes duplicate-NAME re-registration from a true hash-collision of distinct names (tracks `names_` for load-time diagnostics).
- **`libs/simul` advancer**: `run(std::stop_token, wait_mcs)` overload — jthread destruction cooperatively stops the loop; old `run(wait_mcs)` delegates with an empty token. tile_frontier's subsystem jthreads pass their stop_token.
- **`libs/sound`**: `.pcm` removed from `type_from_ext` (raw headerless `.pcm` FILES aren't loadable — no format metadata). `command_sound_play` carries `type` (sound::type; `UINT32_MAX`→sfx); sound actor no longer hardcodes sfx.
- **`libs/sound` PCM path (2026-07-05, follow-up):** `resource2` gained audio metadata (`sample_format`/`channels`/`sample_rate`/`frames_count`), filled ALWAYS in `sound_resource::load_cold` (a temp decoder reads the header). SHORT sounds (`frames_count < 5s·sample_rate`) are decoded WHOLE into PCM there: `data` becomes raw frames, `type` becomes `data_type::pcm`. `system2` plays `pcm` via a dedicated branch (`pcm_decoder` passthrough built from the resource2 metadata) instead of `make_decoder`. So PCM = the in-memory decoded-data type (not a loadable file). Verified: eating/fleeing/walking decode to f32 PCM with exact byte sizes, ambient (52s) stays compressed, no decoder errors. `pcm_decoder` still pulls OpenAL headers (legacy) but its `void*` `get_frames` path is AL-free.
- **`libs/bindings`**: `rng_state + int` overload = advance state N steps (`meta::addition` = mix for rng+rng, advance for rng+int). Nuklear end function renamed `nk.endf` → **`nk.fin`** (all end-variants: group/chart/popup/combo/contextual/menubar/menu); `entry.lua` updated.
- **`libs/visage`**: dead `draw_resource`/`draw_stage` files deleted (were commented out of the build).
- **`libs/catalogue`**: first active introspection slice added in `catalogue/introspection.h`. Use `catalogue::domain<domain_value>::fn_traits<&fn,"name","arg"...>::fn_ptr` to get a `constexpr` wrapper pointer with a concrete mirrored signature (`Ret(*)(Args...)`, member methods as `Ret(*)(T&, Args...)`, const methods as `Ret(*)(const T&, Args...)`, structural functor NTTPs with simple non-overloaded `operator()`). `fn_traits::loc_fn_t` is the source-location functor form: `using f = traits::loc_fn_t; f{}(args...)` captures call-site `std::source_location` into `call_info.file/line`; for methods object is still the first arg. `domain_value` is still an `auto` NTTP (tests cover both `constexpr size_t` and enum values). Runtime policy lives on `catalogue::domain<domain_value>::intro_i` via `set_introspection(ptr)` and is non-owning/non-atomic for now. If the pointer is `nullptr`, the wrapper fast-path calls the original function directly and does not build `call_info`/argument strings. Built-ins: `trace_introspection`, `timing_introspection`, `dry_run_introspection`, `statistics_introspection<N>`. `function_id` is constexpr and uses `utils::murmur_hash64A(Name)`; `argument_names` is constexpr array on `fn_traits`. `noexcept` is accepted but not preserved on the wrapper pointer; thrown exceptions currently skip `exit()`. Complex args log as `<type_name>` placeholder, bounded to the 64-byte local value buffer (try full type, then strip `devils_engine::`, then truncate with `...`). `call_info.arguments` spans wrapper-local storage, valid only during the synchronous `enter`/`exit`/`skipped` call; `argument_view.value` is a non-owning `string_view` into either the original string-ish arg or a wrapper-local fixed buffer (`to_chars` for numbers), so copy values if retaining them. Wrapper internals use `std::invoke` + forwarding refs so member functions with noncopyable refs (`atomic_pool&`, `world const&`) work. TODO: per-domain function registry keyed by `function_id`.
- **`libs/demiurg` loader**: `request()` now warns on a true dependency CYCLE (DFS `visiting_` path set; distinct from an already-queued independent request). NOT done: stricter zip-type-before-parse contract (existing path already warns+skips; deferred pending a concrete rule).
- **tile_frontier config**: `simulation.sound_enabled` (topology toggle, restart-required like `render.enabled`); disabling render/sound frees a reserved core → `+1` worker thread (dynamic `worker_threads_reserved`).

### Logging (catalogue domains, 2026-07-06)

Domain-scoped logging lives in `libs/catalogue/logging.h` (`DE_LOG`/`DE_TRACE` macros + `catalogue::logs()` registry). Two orthogonal axes plus spdlog severity:
- **Base always-on layer** = plain `utils::info`, reserved for a HANDFUL of lines only: `Using cpu …`, `Using sound device …` (libs/sound), `Using '<type>' device …, driver version` (painter `system_info.cpp`), `Using monitor …`. Everything else must NOT be `utils::info` — it belongs to a domain. `warn`/`error` (spdlog severity) stay always-on (importance, not depth).
- **Domains** = `catalogue::log_domain::` **`constexpr` constants** (NOT an enum): `main/assets/sound/render/ui/gameplay/resource` + `engine_count`. New logging layer = new constant + `register_domain(id,name)`. Projects continue numbering past `engine_count`.
- **Depth** (`catalogue::log_depth`): `off/info/flow/trace`. Default **off** (invisible). `flow` = important transitions + periodic slices; `trace` = full pipeline detail. `DE_LOG(domain, DEPTH, fmt, …)` gates on a relaxed atomic load — near-zero cost when off (release too). `DE_TRACE(domain, fmt, …)` = trace depth + captures call-site `file:line` (path compressed via `utils::make_sane_file_name`, like `utils::error{}`).
- **Runtime toggle** (works in release): `app.set_log_level("sound","trace")` (lua/app) or `logging` section in `app.tavl` (per-domain `off/info/flow/trace` + `console`/`file`). `catalogue::init_logging(file, console)` sets up spdlog console + rotating file (5×8 MiB); called in `simulation::init` right after config load (base layer also hits the file).
- **libs now dep `devils_engine::catalogue`** for `DE_LOG`: painter (strides→render/trace, graphics_base setup→render/flow, texture/mesh→resource/flow), visage (font→ui/flow), sound (sound_resource→resource/flow, data-source callbacks→sound/trace). No dep cycle (catalogue→utils/options only).
- **Automatic function tracing (auto-linked to log level)**: `catalogue::introspection` is a non-virtual mode switch (`off/logging/statistics/tracing/dump`) installed via `domain<D>::set_introspection(const introspection*)`. The config carries `{mode, log_domain, statistics_store* stats}`. The EFFECTIVE mode = `max(base mode, trace-if-log-domain-on-trace)` — so `app.set_log_level("gameplay","trace")` auto-escalates that domain's wrapped functions to `tracing` (enter/exit + compressed `file:line` + elapsed), on top of whatever base mode (e.g. perf `statistics`). No NTTP realignment needed — the config's `log_domain` field is what gates. arg_views built lazily only for `dump`. tile_frontier's actor phases run at base `statistics` (perf UI) and escalate to `tracing` when gameplay hits trace.
- **Debugging workflow**: enable a domain at runtime/config, run, read `logs/*.log` to inspect the flow you need — instead of adding throwaway prints.

## Subsystems

- `libs/acumen`: GOAP planner plus `goap_resource` config parser/merge/flatten. It uses `act` predicates to compute bitset state, A* over symbolic actions, and caller-owned scratch/cache; it returns plans but does not mutate world state.
- `libs/act`: shared gameplay-function registry (`devils_engine::act`) — typed-by-return functions (effect/predicate/number/string/object), immutable `exec_context`, `intent` seam, `registry`, and generic `script_resource`/`script_compiler` boundary. `native_function` and devils_script-backed `script_function` are live; Lua backend remains pending. `acumen` and `mood` consume act contracts.
- `libs/bindings`: Lua/sol2 binding layer for sandbox env, `base` utilities, deterministic `rng_state`, reflect-based table conversion, and large Nuklear bindings. No local README yet; root README summarizes it from code/CMake.
- `libs/catalogue`: active code is function-call introspection/tracing/timing/statistics, the older in-memory `call_log`, and typed deterministic MT strategy wrappers/executors in `deferred.h`. The first live ECS integration is tile_frontier's cognition→collect/elect→structural/FSM pipeline, including config-loaded void actions over the same deferred building blocks. Dense 128-byte inline payload storage is live; remaining nearby work is member/custom bounded codecs and executor ownership after a second consumer. The obsolete `act::effect_sink` seam was deleted on 2026-07-19. Older recording/replay/RPC/channel files are archived under `exclude/` and are not to be revived as catalogue's direction. Replay/network persistence, if ever needed, belongs to a separate layer.
- `libs/demiurg`: module/resource registry and staged resource loader. The durable cross-system handle is now `demiurg::resource_handle` (hash of logical id, survives registry rebuild — see "Assets / resource pipeline" and "Demiurg ↔ Lua resource API"); raw `resource_interface*` is fine only for same-lifetime same-thread reads. Loaders must honor dependency gating and external render/GPU steps. tavl list-pattern (`path:name`/`path:index`) and dependency-cycle diagnostics are done.
- `libs/flow`: first active 2D/2.5D/UV animation slice. It is now a CMake target `devils_engine::flow` with `flow::library`, `flow::state`, `flow::playback`, `flow::animation_resource`, directional image selection, action events, and UV accumulation/truncation. 3D/skeletal/blending remain future work.
- `libs/input`: GLFW window/input wrapper, Vulkan surface/proc helpers, key-name registry, and abstract input-event state machine.
  - Key-name contract now has three distinct layers in `input/key_names`: canonical names for config/storage (`key_w`, `minus`, `f10`, `right_super`, `kp_1`), US/QWERTY display names (`W`, `-`, `F10`, `Right Super`, `Num 1`), and local names via `glfwGetKeyName` plus platform fallback. Prefer canonical names in configs; parse them with `key_from_canonical`, which returns `(glfw_key, scancode)` in the same order as the GLFW key callback.
  - Input configs should look like `action = [ key_w, minus ]`: parse each canonical token to `(glfw_key, scancode)`, call `events::set_key(event, scancode, glfw_key, slot)`, and update runtime key state from the GLFW callback scancode. Runtime key state is still keyed by scancode.
  - `events` event ids are `utils::id` hashes from `utils::string_hash`, not `std::string_view` keys. Hot-path code should precompute `events::event_id` once with `events::make_event_id("use")` and call id overloads (`check_event(id, ...)`, etc.); string overloads remain as convenience wrappers that hash then forward.
  - Hash collision checking intentionally lives in `events::set_key(std::string_view, ...)`: `event_map` stores the original event name and errors if the same hash is registered with a different name during binding load/setup. `key_mapping` stores event ids per scancode and removes stale scancode entries when a key has no events left.
- `libs/painter`: active Vulkan/render-graph layer (`graphics_base`, `render_config_storage`, `render_graph_instance`, `assets_base`) plus demiurg resources for render config, shaders, pipeline cache, meshes and textures. Older painter files remain beside the active path.
- `libs/sound`: sound prototype, currently migrating toward miniaudio (`system2`) while older OpenAL code remains for reference/compatibility.
- `libs/utils`: broad utility library. It also owns the former `libs/thread` utilities under `libs/utils/include/devils_engine/thread` and `libs/utils/src/thread`; keep `devils_engine::thread` as an alias to `devils_utils` for compatibility instead of reintroducing a separate `libs/thread` target.
- `libs/aesthetics`: ECS storage/view/query/events/snapshot implementation; still exploratory and performance-sensitive.
  - Current storage is `world::sparce_dence_set<T>` in `libs/aesthetics/include/devils_engine/aesthetics/world.h`: `sparce_set` maps entity index → dense component index + version; component payload lives in `component_array<T>` rather than a raw `std::vector<T>`. `component_array` is the compromise that lets const `view()` / `lazy_view()` keep membership const while returning mutable `T*` payloads without `const_cast`.
  - The old public-ish `sparce_dence_set::entities` dense entity list was removed. `world::raw_itr` walks `sparce_set`, skips invalid entries, and reconstructs `entityid_t` from sparse index + stored version. This keeps create cheap and avoids storing owner data in the hot component payload path.
  - Removal currently has a known tradeoff: when removing a non-tail dense component, the moved tail component's owner is found by scanning `sparce_set` (`entity_at_dense_index`). This preserves low memory overhead and create speed, but random/forward deletion can be much slower than reverse/tail deletion. If this becomes a real bottleneck, the likely fix is an internal sidecar dense-owner array (`entityid_t` or just entity index, +4 bytes/component with current `uint32_t` ids) rather than putting owner inside each component slot.
  - `view<T...>` means all requested components and iterates the smallest component storage via `raw_itr`; construction is cheap and `construct+iterate` is mostly traversal. `lazy_view<T...>` gathers a hash-set union of entity ids (any requested component), so construction is relatively expensive. `query<T...>` is a live all-components query updated by create/remove events; `lazy_query<T...>` is the live any-components variant. Keep this semantic difference explicit in benchmarks and API discussion.
  - Query maintenance cost grows with the number of active query receivers for a changed component type. Each create/remove emits events to relevant query receivers, which do `has/get_tuple` checks and vector insert/erase. If many queries and high mutation rates appear, prefer phase rebuilds / dirty query updates over per-mutation live maintenance.
  - Tests and benchmarks: `tests/aesthetics_world_test.cpp` covers sparse storage without `entities`, version reconstruction, mutable payload through const views, query updates, and lazy-query removal. `tests/aesthetics_world_benchmark.cpp` is a manual benchmark target (`aesthetics_world_benchmark`) grouped into all-component traversal, lazy/any traversal, and component mutation. Benchmark arguments are `[entities] [iterations] [mutation_count]`; mutation includes raw vs warm create/remove and forward vs reverse removal to expose deletion-order risk. It is intentionally not registered with CTest.
  - Near-term design direction: keep `libs/aesthetics` focused on storage/view/query contracts. Efficient per-entity handoff between systems should be designed later as batched pipeline/scratch channels, not as per-entity mailbox traffic in the ECS hot path.
- `libs/mood`: FSM built from owner-level structured `mood::transition_config` records (`devils_engine::mood`); a legacy transition-description string constructor remains for C++ fixtures. Split into two layers (deliberate): `mood::system` (`system.{h,cpp}`) is a DUMB, stateless, fast STORE — it holds nothing at runtime, reacts to nothing, and does NOT understand the naming conventions; it only returns the candidate `transition` list for a `(state, event)` pair in O(1), preserving source order within a group (critical for top-down guard evaluation). `mood/runtime.{h,cpp}` is the CONVENTION + stepping layer (free functions): it owns the standard names (`any_state` = wildcard fallback, `idle` = standard "no event", `on_entry`/`on_exit` = entry/exit pseudo-events), `step()` (top-down guard eval → `step_outcome{transitioned|blocked|no_transition}`, effects NOT executed — that is the caller's apply phase), `find_with_fallback()`, and `validate()` (load-time graph warnings). See "FSM mood internals" below.
- `libs/simul`: shared app/runtime layer. It owns `interface`/`advancer`, `app_runtime<Traits>`, standard broker/messages/systems, jthread lifecycle and window/settings/loading/render/sound/assets helpers. `app_runtime` owns an extensible erased `worker_systems<Broker>` rather than fixed render/assets/sound slots, wires one broker before init, exposes optional workers through the typed `runtime_system<T>()` registry, and joins by explicit shutdown order. `make_standard_workers` builds optional render/sound/assets (disabled assets now create neither object nor thread); the topology callback resizes the task pool from the actual standard+project worker count. `tile_frontier::runtime_traits` is reduced to bootstrap/broker/main types plus one worker factory; project-local broker channels and gameplay policy remain project code.
  - **Turn-based pipeline scaffold (2026-07-22, engine side of ROADMAP CG-1)**: `simul/turn_pipeline.h` is a reusable header-only, host-templated resumable step machine for turn-based / discrete-event gameplay, modeled on `lifecycle_controller` (single writer of the cursor). `presentation_barrier` is a TRANSIENT set of typed `{presentation_task_id, gameplay|finished}` checkpoints with an engine-tick watchdog; it is deliberately NOT serialized, and headless steps register nothing and resolve inline. A visible beat is `start animation → gameplay checkpoint → deterministic main-thread commit/result → finished checkpoint → next step`. The host advances the serializable cursor BEFORE each wait, so resume drops in-flight presentation and continues at a deterministic boundary without double commit. `turn_pipeline<Cursor>` owns no gameplay coordinates: turn, player-action, countdown-pulse and forced-generation counters belong to the project cursor/state because none can be derived from another. A watchdog timeout latches `faulted` and reports exactly once. Covered by `turn_pipeline_test` (5 cases/40 assertions).
  - **First `cardgame` consumer (2026-07-22)**: `subprojects/cardgame` now contains a small project-owned combat FSM and project-specific player intent. It proves player card cue/commit/finish, enemy countdown/attack, forced end-turn pulses, snapshot before the gameplay marker, and animated-vs-headless identity across three completed turns. `quick_strike` is deliberately a player action without a countdown pulse; forced end-turn pulses are deliberately pulses without player actions. `cardgame_headless_smoke` ends at identical `{turn=4, actions=5, pulses=6, player_hp=24, enemy_hp=89}`. The hard-coded cards are scaffolding for the next project-owned ds beat recorder, not the content model.
  - **Cardgame atomic resolution slice (2026-07-22)**: the project combat FSM now delegates a materialized, serializable `resolution_work + resolution_cursor` to a small inner resolver; it is NOT another `turn_pipeline` or `mood` system. `attack_instance` and `damage_instance` are distinct: initial card attacks are all gathered first, while `resolution_work.plan` preserves card-text ordering between attack/effect entries; attacks then resolve one by one because an earlier hit may change shields/resists/elements/death for the next. Each attack runs cue/gameplay → resistance modifier journal → shield/HP apply → result/finished, then its elemental reactions, returned-damage instances and death boundary before the next attack. Reaction/returned damage use explicit channels, pass through the common damage resolver, and are non-recursive by default. `thorns` appends one response per eligible attack rather than recursively invoking resolution. Status effects live in a project-owned `combatant_state.effects` list, NEVER `aesthetics::flag_set`; effect application yields explicit `added/updated/immune/invalid_target/rejected` outcomes. Current limits are intentional scaffolding: resistance collection is a native stand-in for a future project ds script, only a minimal elemental collision is present, stacking policy is simple add+refresh, and destructive death effects/cleanup are pending. The smoke covers complex animated/headless identity, resist on attack+reaction, non-recursive per-hit return, effect immunity/update and resume after primary commit with queued reaction/response/effect work.
- `libs/utils/include/devils_engine/utf`: UTF string conversion support.
- `libs/visage`: Lua/Nuklear UI runtime plus MSDF fonts and POD render-output buffers consumed by painter's `draw_ui` path.
  - Host-binding seam: `system::script_state()` (sol::state&) and `script_env()` (sol::environment&) let the host register its own usertypes/functions into the UI Lua sandbox without visage knowing about gameplay (used for the `app.*` sound API in tile_frontier). visage itself stays UI-only.
  - Dynamic font size for `nk_style_push_font` lives in `visage::system` (per-size `nk_user_font` cache `sized_fonts_` + private `sized_font(float)` helper), NOT in `font_t`. `font_t` keeps only the base `nkfont`, and `font_t::set_texture_id` now updates just that base font. `sized_font` refreshes `texture.id` from `nkfont` on every push, so there is no stale-id race when `push_font` runs before the atlas reaches HOT (the old per-font `sized_fonts` fixup loop is gone). Multiple fonts are selected by demiurg handle in `push_font{ font = request("fonts/...") }` (the `add_font` string registry was removed 2026-07-06); the demiurg↔Lua resource API (`request`/`require`/`find`/`filter`, below) drives UI resource access.
  - **UI images (Stage 1, 2026-07-05):** `visage::image` (`image.h`) is a POD lua-facing handle — `{texture_id (bindless slot), w, h, region[4]}`, no nuklear/vulkan dep so the HOST builds it. `visage::system` registers (same seam as `push_font`/`style_*`, NOT in bindings which can't depend on visage): the `image` usertype, an `nk.placement` bitmask table (`fill`/`stretch`/`scale_ratio`(=fit)/`center`/`left`/`right`/`top`/`bottom` + `mirror_u`/`mirror_v`), and `nk.image(img [, placement] [, color])` — takes the widget slot via `nk_widget(&bounds,ctx)`, computes the target rect (`image_placement_rect`: fit-by-min-scale + align, default center), draws `nk_draw_image`. Overrides the dead `bindings` `nk.image` stub. tile_frontier host: `app.image(handle)` → `visage::image` from a loaded `gpu_texture_resource` (`gpu_index`+`width`/`height`), `nil` until `usable()`. (As of `d2ce7fe` `app.image` takes a `resource_handle` from `request(...)`, not a string name — the `image_by_name` map was removed; see "Demiurg ↔ Lua resource API".) Constraint: usable slots 0–7 (`tex[8]` clamp) until the huge-descriptor pass.
  - **UI stencil effects (Stage 2, 2026-07-06) — cooldown + 4-blend via mask texture.** `gui_draw_mode` += `cooldown=4`/`mix=5`; `gui_draw_command_t`/`ui_push_t`/`ui_command_wire` replaced the named SDF fields with a generic `uint32 payload[6]` (48/28 B; config `ui_push` = 7×`ui1`) interpreted BY TYPE in `ui.frag` (msdf→SDF fields; cooldown→`[0]=mask_index [1]=fill`; mix→`[0]=mask_index [1..4]=comp0..3 [5]=is_image bits`). Params ride the SAME per-draw channel as SDF text (`effect_arena` + nk `userdata` offset; new `ui_image_effect` struct; `convert()` dispatches by `tex_id::type_of`). `nk.image_gradient{img,mask,fill}` (icon revealed where `mask.r<=fill`, unrevealed ×0.35) and `nk.image_mix{comps={≤4 image-or-color},mask}` (weights `mask.rgb` + `1-r-g-b`, per-comp image-vs-color by `is_image` bit, `mask.a`=opacity). Mask + comps are just indices into the SAME bindless `tex[]` (no new descriptor). `ui.frag` `tex[8]`→`tex[16]` (Vulkan guaranteed min; dynamic indexing, UI id uniform per draw). **Nearest masks → BINDLESS V2 (2026-07-06):** replaced the whole combined-image-sampler asset model. The `textures` descriptor is now a SEPARATE `SAMPLED_IMAGE` array (binding L, written by render) + an immutable `SAMPLER` POOL (binding L+1, config `sampler_pool = [linear, nearest]` → `samplers/nearest.tavl`). Shaders combine on the fly: `sampler2D(tex[index], samp[sampler_id])`. `sampler_id` is a 4-bit field in `tex_id` (`[20..23]`); UI content uses the id's sampler (default 0=linear), the MIX mask uses `samp[NEAREST]` (crisp seams) and the COOLDOWN gradient uses `samp[LINEAR]` (smooth) as shader constants. This works where per-slot immutable samplers can't (samplers are FEW + static → bakeable pool; slots are MANY + dynamically assigned at load → paired by index at sample time). ALL asset-sampling shaders converted (`fragment1`/`tile`/`ui`; `actor.frag` doesn't sample → untouched); the old `mask_textures` 2nd descriptor was deleted. **GLSL gotcha:** a `sampler2D` built from separate image+sampler may ONLY appear AT POINT OF USE (in `texture()`/`textureSize()`) — it cannot be returned from a function, stored in a local var, or passed as a function arg. So `S(idx,sid)` is a `#define` (not a function) and `screen_px_range(uint index, uint sid)` constructs the combined sampler inside its own `textureSize`. **Stale-asset gotcha hit:** deleting `mask_textures.tavl` from source left a stale copy in the build output (POST_BUILD copies, never deletes) → had to `find build-debug -name ... -delete`. **Tile-set gotcha:** the tile terrain gather (`texture_set::gather`) is a SUBSTRING match — putting `grad*/quad` under `textures/` pulled them into the terrain; narrowed the tile gather to `"textures/grass"` (masks don't contain "grass"). TODO reminder: generate a perfect single-channel gradient in CODE (grad*.png are placeholders with edge artifacts).
  - **UI texture-id encoding (2026-07-06) — TYPE/MIRROR/INDEX packed in one word.** `render_output.h` `namespace tex_id`: `[0..13] index (14b, 16384) | [14] mirror U | [15] mirror V | [16..19] type (4b, = `gui_draw_mode`) | [20..23] sampler_id (4b, index into the descriptor sampler pool) | [24..30] free (7b) | [31] unused (nk `handle.id` is signed `int` → keep bit 31 clear)`. `pack(type,index,mirror_u,mirror_v,sampler_id)` / `index_of`/`type_of`/`sampler_of`/`mirror_*_of`. This REPLACED the mode-inference heuristics: `convert()` is now a passthrough of the packed id (dropped `is_font_texture` + the `gui_draw_command_t.mode` field), `font_t::set_texture_id` packs `type=msdf`, `nk.image` packs `type=image`+mirror, nuklear shapes carry `texture.id==0` → decode `type=0=solid` naturally. `ui.frag` decodes index/type/mirror from the pushed `tex_id` (masks kept in sync with `render_output.h`), branches by type, and flips uv per mirror (mirror correct for whole-texture images; sub-region mirror needs the region uv-rect — TODO). Painter `ui_push_t`/`ui_command_wire` dropped `mode` (44→40 B; config `constants/list.tavl` `ui_push` 24→20 B). Free bits + `composite` type value reserved for Stage 2 (heraldry/effects). Verified: text (msdf via packed font slot), grass image + horizontally-mirrored copy all render, no VUID.

## Data/Config Direction

- The project is actively migrating from JSON to the custom `tavl` configuration format.
- `subprojects/tile_frontier/resources/engine/config/app.tavl` (MOVED from `resources/config/`) is the app/runtime config (window, subsystem FPS, worker threads, render cache paths/GPU preference, metrics). Loaded through the engine demiurg registry as `app_config_resource`, NOT raw file_io (see Assets section).
- `subprojects/tile_frontier/CMakeLists.txt` builds a self-contained project folder at `build/subprojects/tile_frontier/tile_frontier/`; the executable and bundled runtime libraries live in `tile_frontier/bin/`, while resources/config/render data sit beside that `bin/` folder. It copies `tests/test_render_config` into `resources/engine/render_config/` (was top-level `render_config/`; source dir unmoved, still shared with fast_test/tests), and copies `tests/shaders` to both `shaders/` and `tests/shaders/` because painter currently has both path assumptions (`utils::project_folder() + "shaders/"` and `utils::project_folder() + "tests/shaders/"`).
- `libs/painter/src/painter/system_info.cpp` now uses `main_device.tavl` for cached physical-device data instead of `main_device.json`.
- Render graph description already has tavl test data under `tests/test_render_config`.
- tavl config convention: when a value is a `pair`/`tuple`, write it with parentheses — `key = (a, b)` (or `(a, b, c)`) — rather than chained operators like `key = a = b`. tavl accepts both, but the parenthesized form is unambiguous and matches the serializer output. Applied to render-graph subpasses (`{ albedo_res = (undefined, clear) }` = `map<string, tuple<string,string>>`).
- DECIDED, not yet implemented — canonical multi-aggregate file convention: a resource file is a sequence of DOCUMENTS separated by `//---` (tavl's `document_separator`, in `ext.h`); each document is a braceless root aggregate (like `app.tavl`). Singleton = 1 document, list = N documents; NO top-level braces (`{}` only for nested aggregates, `[]` for value arrays). The resource TYPE decides exposure: singleton → id `path`; list → each document as `path:name` (name from a `name` field; positional fallback), enabling per-entry mod override. NB: this is a NEW convention — current render-config DATA files still use consecutive `{...}` blocks (tavl `deserialize_next` over values, a DIFFERENT mechanism); migrating them to `//---` is a pending small slice. UPDATE 2026-07-06 (`5013101`/`7c03758`): demiurg now DOES expand `//---` list files into `path:name`/`path:index` subresources, and painter's `structures.cpp` `parse_data` can CONSUME a list-pattern render-config category (subresources resolved by internal `name`) — so the capability landed on both sides; only the shipped data files haven't been moved over. `:name` is for game data (monsters/spells), NOT render config (the graph consumes whole files and cross-refs by internal `name`).
- The shared gameplay-function registry now exists as `libs/act` (`devils_engine::act`) — see "Gameplay function layer (`libs/act`)" below. It replaced the placeholder callbacks in `acumen` and `mood` (DONE). **acumen** now mirrors mood's pattern: `acumen::system(const act::registry*, metrics, goals, actions)` resolves each `state_metric` to a `const act::predicate_function*` and each `action` to a `const act::effect_function*` BY NAME (`registry->predicate/effect(string_hash(name))`), caching the typed pointer at build (lookup leaves the A* hot path). A `state_metric` is one GOAP state bit (bit index = the metric's position in the metrics vector — dense, implicit) computed by its predicate; `compute(const act::exec_context&)`/`system::compute_state(const act::exec_context&)`. `action.effect` is NOT executed during planning (the plan only picks actions; the effect runs on apply via intent); an empty action name means no effect (pure symbolic transition). `acumen::system`'s constructor is NOT `noexcept` (it throws `utils::error` on a missing/wrong-category function — a noexcept ctor calling the throwing `utils::error` would `std::terminate`; same fix applied to `mood::system`'s ctor). `tests/acumen_test.cpp` builds a tiny GOAP (draw_weapon→attack) over a real `act::registry` and asserts compute_state + find_solution + the load-time throw. Fixed a pre-existing off-by-one in `find_solution` while testing. `find_solution` is now ALLOCATION-FREE: `size_t find_solution(sys, container, start, goal, std::span<const action*> out)` writes the plan (actions in execution order) into the caller's buffer and RETURNS THE FULL plan length — if `> out.size()` the plan was truncated to a prefix (grow the buffer; caps ~32-64 make this practically unreachable). The plan EXCLUDES the start node (its action is null) and INCLUDES the goal node's action (the one that achieved the goal); `start` already satisfying the goal returns 0 (empty plan). No internal `std::vector` — the caller passes a stack `std::array` (mirrors `astar::solution(node**, max_nodes)`). (The old version returned a `std::vector` with a leading `nullptr` and dropped the final action.) `mood::table` was DELETED — it was literally "the shared function table" the author wanted, which is now `act::registry`; `mood::system(const act::registry*, transition_config)` resolves guard names → `registry->predicate(string_hash(name))` and action names → `registry->effect(...)`, caches typed pointers in each `transition`, and `transition::is_valid/process` take `const act::exec_context&`; a legacy lines overload remains for fixtures. The old `int32_t`/`void*` error-return channels are gone (act backends throw via `utils::error`; predicate returns bool, effect returns void). Both libs link `devils_engine::act` PUBLIC; `libs/act` is added in root CMake BEFORE `mood`/`acumen` so the alias exists. `tests/utils_general_test.cpp` mood test builds an `act::registry` of `native_function<void>`/`native_function<bool>`; `fsm_config_test` covers the native TAVL adapter and structured constructor. `mood` was further refactored into a dumb store + a runtime/convention helper layer — see "FSM mood internals" below. `tile_frontier` supplies the live `exec_context` and executes selected effects in its apply phase.

## Gameplay function layer (`libs/act`, `devils_engine::act`)

Shared registry for small-grain gameplay functions over one entity (or a few linked), used by GOAP
(`acumen`), FSM (`mood`), gameplay glue scripts, AND the Lua UI (e.g. a string function → loc key).
The layer is wired into `devils_plane`; `native_function` and `script_function` are live, while the
Lua function backend remains pending. `script_compiler` is the type-erased config-parse seam used by
`act::script_resource` and `acumen::goap_resource`; projects provide concrete root-scope dispatch.

- **Functions are SPLIT by return type, NOT unified into one `value invoke()`.** Categories mirror
  `devils_script::user_function_type`: `effect`(void) / `predicate`(bool) / `number`(real_t) /
  `string`(`utils::id` = loc-key hash) / `object`(`entity_id`). The return type IS the contract and
  also encodes purity (effect = mutating, the rest = pure), so no separate `purity`/signature
  metadata. Purity matters: GOAP A* calls pure functions freely during search; effects are NOT run
  during planning (the GOAP action only emits an `intent`, see below).
- **No combinatorial category×backend explosion.** Category = template `function<RetT>` (aliases
  `effect_function`/`predicate_function`/…); backend = template impl: `native_function<RetT>`
  (raw `RetT(*)(const exec_context&)`, no `std::function` on the A* hot path), `script_function<RetT>`,
  `lua_function<RetT>`. Common base `function_base` (carries `category` tag) for generic storage +
  `describe`.
- **`describe(ctx, callback)`** — run a function WITHOUT applying effects and STREAM useful text into
  a callback (`using describe_callback = std::function<void(std::string_view)>` — temporary/any type
  for now). For UI tooltips ("why can't I", "+5 from X, −2 from Y", effect preview, predicate/number
  breakdown). `devils_script` will stream its compiled-container introspection nodes here.
- **`exec_context` is IMMUTABLE.** Concrete struct, same for all backends, flows by reference into
  `invoke` (`const exec_context&`), NEVER a global (global = silent races under the actor model).
  Fields: fixed `entity_id scope[8]` + count ([0]=this, [1]=target, …), `const world*`, immutable RNG
  inputs (`rng_seed/entity/tick`) and a caller-owned `execution_scratch*`. **PRNG state is NOT held in the
  context** — inputs come from external systems and each call passes a `purpose` explicitly:
  `random(purpose) = utils::mix(seed, entity, tick, purpose)`. This is more deterministic than an
  auto-increment draw counter (order- and count-independent). Mutable VM/call data lives in scratch;
  mutating effects use catalogue deferred wrappers rather than a mode bit in the context.
- **No `fat_handle`.** Gameplay functions work over a finite entity set (essentially one `entity`);
  "type" is distinguished by ECS COMPONENTS, not a tag on the handle. Over disk/network it is a bare
  `entity_id` (uint32) or a context-dependent index. `value::handle` holds a raw uint64. (Note:
  `utils::type_id<T>()` is a constexpr name hash — stable within a build / across builds of the same
  compiler, differs only across compilers — but handles don't need it.)
- **`value`** = slim POD tagged union (none/boolean/integer/number/vector/handle/string), matching
  devils_script categories. NO LONGER the return type (returns are typed by the function class); it is
  only for generic boundaries (devils_script arg marshalling and generic trace/debug). `string` is a
  hash, not inline. GOTCHA: `vec3` has member-initializers (non-trivial ctor) → as a union member it
  deletes `value`'s default ctor; an explicit `value() : kind(none), inum(0) {}` fixes it and zeroes
  the union (no uninitialized bytes → friendly to deterministic checksums). `number`/`vec3` ride
  `using real_t = double` — float→fixed is a one-line change when determinism is taken up.
- **`effect_sink` was removed (2026-07-19).** It had no implementation or emit calls. Pure/read-only
  consumers resolve only pure act categories; mutating ds/native calls use catalogue deferred wrappers;
  replay will use a separate owning input/log layer.
- **`intent`** = the thinker→ECS seam. GOAP/FSM/script do NOT mutate the world; they emit a compact
  `intent` of base verbs (`move_to`/`turn_to`/`call_function`/`fsm_event`, extensible) that ECS systems
  consume in a DETERMINISTIC apply phase (sort by `actor.id`, not message-arrival order). One typed
  command seam instead of millions of scattered mutations; persistent serialization is a separate concern.
  Carries `source_action` provenance ("why", not just "what").
- **`registry`** — one `gtl::flat_hash_map<fn_id, unique_ptr<function_base>>`; `fn_id = string_hash(name)`
  (function names need no dense index). Typed checked accessors `predicate(id)`/`number(id)`/`effect(id)`/
  `string_fn(id)`/`object(id)` (category mismatch → nullptr). `reg()` only in the single-threaded load
  phase (asserts on hash collision via `utils::error`); `get()`/`call()` thread-safe after. Consumers
  cache the typed `const function<RetT>*` at plan/table build → lookup leaves the A* loop.
- **Id numbering (resolved): per-system, not one rule.** Dense/monotonic index ONLY where a system
  genuinely needs it: GOAP state flags = dense (bit position in `bitset<256>`, via `utils::string_pool`),
  a large logic-heavy flag registry (e.g. internal-politics flags) = dense. Function names, effect names,
  per-actor flags = plain hash (`utils::string_hash`); per-actor flags needing extra data like an expiry
  store `(date, hash)` sorted. The two numberings are separate namespaces — do NOT conflate.
- **Throw via `utils::error{}("msg {}", args...)`** (captures source_location, logs + throws), not
  `assertf(false, ...)`. `mix` lives in `devils_engine::utils` (`utils::mix`), not a `prng` namespace.

## FSM mood internals (`libs/mood`, `devils_engine::mood`)

Grammar of one line: `state [+ event] [[guards...]] [/ effects...] [= next_state]`. ORDER MATTERS:
within a `(state, event)` group lines are evaluated TOP-DOWN — a later line's guards are only reached
after earlier lines fail. Order of the initial states does NOT matter. Author's hard design rule:
`mood::system` is a DUMB, stateless, fast STORE that knows NONE of the naming conventions; conventions
+ stepping + validation live in free helper functions in `mood/runtime.h`.

- **Runtime works on hashes, not strings.** Each `transition` precomputes `current_hash/event_hash/
  next_hash` (`utils::string_hash`); the `string_view`s remain only for diagnostics/`describe`. An FSM
  cursor is a `utils::id`, not a string.
- **`system` lookup is O(1) via a hash index.** Construction stable-sorts `m_transitions` by
  `(current_hash, event_hash)` (stable ⇒ source order preserved within a group) and builds
  `gtl::flat_hash_map<uint64_t, range>` keyed `mix(state_hash, event_hash)` → `{offset, count}` into the
  contiguous vector. `find_transitions(state_hash, event_hash)` returns a `span`; string overloads hash+
  forward. (Replaced the old sorted-vector `lower_bound` + string-compare comparator + backward-expand
  loop.) Duplicate check (same state+event+guard-subset) runs once over each sorted group.
- **Conventions live in `mood/runtime.h`, NOT in `system`** (`mood::conv::` hashes): `any_state` =
  wildcard fallback, `idle` = standard "no event", `on_entry`/`on_exit` = entry/exit pseudo-events.
  `find_with_fallback(sys, state, event)` probes `(state,event)` then `(any_state,event)`. NOTE: in the
  test fixture `any_state` is currently only reached via this helper fallback — `system` itself treats it
  as a literal, so `find_transitions("begin","attack")` is empty but `step(...,"begin","attack")` succeeds.
- **`step()` returns a structured `step_outcome`, not a bool.** `step_result` distinguishes
  `transitioned` (a guard-passing edge found; `next_state`/`taken` set), `blocked` (edges exist but ALL
  guards failed — NORMAL gameplay, e.g. ragdoll not settled), `no_transition` (no edge even via
  `any_state` — likely a typo/programmer bug). `step()` only DECIDES; it does NOT run effects. Diagnostics
  are returned, not formatted on the hot path (no per-frame string-building for millions of actors).
  `step_outcome` fields are ordered by decreasing size (uint64, ptr, 2×u16, then the 1-byte enum LAST) to
  avoid padding — `static_assert(sizeof==24)`. This is a project-wide rule for new structs.
- **`apply_transition(sys, cur_state, taken, ctx)` MUTATES (the decide/apply split's apply half).** For an
  external transition (has `= next`): runs on_exit of the old state → the transition's own effects →
  on_entry of the new state (each of on_exit/on_entry is itself a `(state, on_exit|on_entry)` group from
  which the first guard-passing line's effects run). For an internal transition (no `=`): runs ONLY the
  transition's effects (the state is not left, so no exit/entry). Returns `taken.next_hash` (`invalid_id`
  ⇒ caller keeps `cur_state`). Mutating transition actions are deferred catalogue calls committed by
  the owning gameplay pipeline.
- **The per-entity apply loop (design): event then settle idle this same frame.** Consume the entity's
  `fsm_event` intent (else `conv::idle`), `step()`, and if `transitioned` call `apply_transition` and write
  `cur_state`. Then KEEP stepping `conv::idle` until it stops transitioning (UML completion transitions:
  an idle/guarded edge fires the moment its guard holds, which may already be true the same frame after
  entering the new state), with an iteration CAP to break idle A→B→A cycles. Run the whole thing in the
  ordered apply phase (sort by entity_id); guards there observe intra-tick mutations from lower-id
  entities — deterministic because the order is fixed (vs GOAP planning which reads a start-of-tick
  snapshot). The FSM cursor is a component holding `utils::id cur_state` (a hash, NOT a string_view) plus
  a which-FSM `def_id` (multiple `mood::system` tables exist). A `run()`/`settle()` helper wrapping this
  loop is a likely next addition.
- **Typo detection without a hand-maintained registry.** `validate(sys)` (load-time, convention-aware)
  derives the valid state/event sets from the table ITSELF and `utils::warn`s on dead-end states
  (`next_state` with no outgoing edges → terminal or typo) and unreachable states (`current_state` never
  a `next_state`, excluding `any_state`) with a fuzzy "did you mean" (mini Levenshtein over candidate
  names). In the fixture it correctly flagged `prepare_weapon1`→`prepare_weapon` and `melee_attack2`→
  `melee_attack` as likely typos, and `begin`/`initial_state` as initial states.
- **State→animation binding and "blocking" state properties (DESIGN, not in mood).** Author rule:
  states are NOT 1:1 with animations; entering a state runs an `act::effect` that swaps the entity's
  current animation. Per-state capability data (can-move, lock-input, interruptible, …) lives in the
  ENTITY's ECS components, NOT in `mood`. Big-engine pattern (Unreal GAS / Souls-like): the FSM PUBLISHES
  capability tags/bits to a per-entity place and each system gates ITSELF by reading them (FSM never
  calls the movement system). Mapping: "can't move" = a capability bit an on_entry effect writes
  (ref-counted to survive overlap), read by the movement system; "no multi-hit" = same bit or simply no
  outgoing edge for the event; "interruptible mid-anim" = an actual transition edge exists for the
  interrupt event; "ragdoll not settled" = a GUARD predicate on the get-up transition. Timed effects
  (fireball at frame 18) are ANIMATION NOTIFIES emitting intents; "animation finished" is a notify that
  emits an event back into the FSM (that is what `melee_attack + idle = melee_attack_end` really means).

## Gameplay layer → ECS bridge & apply phase (MVP BUILT in tile_frontier)

**MVP wired in `subprojects/tile_frontier` (`actor_simulation.{h,cpp}`, build rc=0, 2026-06-29).** Forks resolved:
**A = reinterpret seam** (`act::world` stays an opaque tag; cast in one `world_of(ctx)` helper),
**B = neither mood nor a GOAP planner yet** (triangles have a single state — a plan/FSM carries no signal):
the "brain" is one native `act::number_function "wander.direction"` (returns a direction index 0..7, reads
`actor_brain` through the bridge, randomness via `utils::mix` on the ctx's immutable inputs), **C = in
tile_frontier**. Concretely: `actor_move_intent` was DELETED → the buffer is now `std::vector<act::intent>`;
`actor_world_slice` owns an `act::registry` + a cached `const act::number_function*`; `think` builds a
dry-run ctx (`sink==nullptr`), invokes the brain fn, emits an `intent_kind::move_to` (payload.target =
dir*speed as a VELOCITY vector for now, `source_action = string_hash("wander")`), sorts by
`get_entityid_index(actor.id)`; `apply` walks the sorted buffer, switches on `kind`, and `move_to` mutates
velocity/position + bounce. No CMake change (`devils_plane` already links `devils_engine::act`). TODO next:
`call_function` intents through the typed catalogue commit pipeline; acumen/mood once actors gain real multi-state behavior;
then `script_function`/catalogue. The original design below stays valid for those next layers.

**ACUMEN BEHAVIOR LAYER ADDED (2026-06-29, build rc=0).** Actors now flee bigger actors / chase smaller
ones via GOAP. New pieces in `actor_simulation.{h,cpp}`: (1) a **target-search layer** `sense()` — naive
O(N²) pass filling a new `actor_perception` component (nearest-bigger = threat_pos/has_threat,
nearest-smaller = prey_pos/has_prey; tie-break by entity index; **grid/quadtree is the obvious next step,
N=4096**); (2) **act functions**: predicates `actor.threat_present`/`actor.prey_present` (read perception,
O(1), pure) and effects `flee`/`chase`/`wander` (mutate velocity via a `mutable_world_of(ctx)` that
`const_cast`s the world — a historical pre-catalogue MVP shortcut); (3) a 1-step **acumen GOAP** (`std::optional
<acumen::system> goap_`): metrics = the two predicates (bits 0/1), a virtual `resolved` bit (2) set by every
action, requirements encode priority (flee if threat; chase if prey & no threat; wander otherwise), goal =
`resolved`. `think` runs `compute_state` + `find_solution` (fresh `astar::container` per actor) and emits a
`call_function` intent carrying the chosen action's effect fn_id; `apply` phase 1 invokes effects in id
order (read positions pre-integration), phase 2 integrates `pos += vel*dt` (movement UNBOUNDED now — the old
min/max clamp + `actor_move_intent`'s velocity-in-`move_to` path are gone). The old `wander.direction`
number-fn was replaced.

**PERF PASS (2026-06-29, build rc=0, acumen_test still green).** Two fixes for the obvious hot spots:
(1) **O(N²) → a simple uniform grid.** `sense()`'s `find_nearest` became a `spatial_grid` class (`gtl::
flat_hash_map<int64 cell_key, vector<entry>>`, cell = `detection_cell = 3.0` world units, rebuilt once/tick);
`query()` scans the 3×3 neighbor cells — both an accelerator (O(actors-per-cell)) and a limited-vision model
(actors perceive within ~one cell; lone actors wander). Hash-map keys ⇒ arbitrary/negative coords (movement
is unbounded). Exact nearest via ring expansion is a later refinement. (2) **Fresh `astar::container`
per-actor → ONE reused** (`actor_world_slice::plan_container_`). This required an acumen LIB change:
`find_solution` now calls `a.free_solution()` after extracting the plan (success path) — the solution chain
(start..goal) that `free_unused` leaves allocated in `node_pool` is returned to the pool, so blocks stay warm
and the container is clean for the next solve. The copied plan stays valid (`action` pts into
`system->actions`, not the freed nodes). The failure path doesn't reach it (early return; `free_all` already
ran). `acumen_test` still green (it makes a fresh container per call — the free is harmless before dtor).

**PROFILE + SCALE PLAN (2026-06-30).** `perf(label, fn, args...)` wrapper (std::invoke, logs µs) wraps the
update phases. Debug 4096 actors: think 113ms / sense 42 / apply 4 / build 1.3 → 6fps. Release: think 8.0 /
sense 6.2 / apply 0.2 / build 0.07 ≈ 14.4ms, capped at `main_fps=20` — **release is healthy; 6fps was a
debug artifact.** Lesson: debug distorted RELATIVE weights (think optimized 14×, sense 7× → in release
they're near-equal, not 70/26) — profile in the target config. Principle: AI cost should scale with the
number of DECISION CHANGES, not actor count. Agreed plan (toward "millions"): **(1) memoize decisions in
acumen → (2) scheduler decimation (think on event/timeout + per-frame budget; count-budget for determinism)
→ (3) kD-tree + AABB tree in utils for sense → (4) MT think across workers.**

**ACUMEN MEMOIZATION DONE (2026-06-30, acumen_test 2/2).** A decision is a pure function of (system, goal,
relevant state bits) and A* is deterministic → EXACT memoization. New `acumen::solution_cache` (cache.h,
header-only): key `plan_key{goal_id, relevant bits projected into array<u64, state_words>}`, value
`cached_plan{array<u16 action-index, max_plan>, length, full_length}`, BYTE budget → entry cap (insert is a
no-op when full → solve live, no churn), `merge()` to share a warm table across threads, hit/miss stats; NOT
thread-safe by design (one-per-thread + merge between frames). `max_plan = 8` (`#ifndef`-define
`DEVILS_ENGINE_ACUMEN_MAX_PLAN` in common.h). `system` computes `relevant_mask_` = ∪ of all action/goal masks
in its ctor (bits outside it can't affect the plan → not in the key → actors in the same relevant state share
one solve). `system::decide(start, goal, goal_id, cache, scratch, out)`: hit → copy plan; miss →
`find_solution` into scratch + insert; `assert(goal.mask ⊆ relevant_mask)` for soundness. Wired into
tile_frontier `think` (`plan_cache_` member, `goal_id = string_hash("resolved")`). Once-warm cost = hash +
probe instead of A*. RESULT (release 4096): **think 8ms → ~350µs**; sense (~6ms) became the new dominant.

**kD-TREE FOR sense (2026-06-30, build rc=0, utils_general_test 5/5).** New reusable
`utils::kd_tree<T, Scalar=float, Dim=2>` (header-only, `libs/utils/.../kd_tree.h`): payload-agnostic static
k-d tree (implicit, in-place median `nth_element` split), `clear()` keeps capacity (per-frame arena reuse),
`insert/build`, `nearest(q, max_radius, pred)` (distance-pruned NN with predicate filter), `radius(q, r,
pred, visit)`. NOTE: nearest REQUIRES a radius — a predicate-NN with no match (e.g. "nothing bigger") else
degrades to a full scan (best=∞ ⇒ no pruning); the radius both prunes and models limited vision. tile_frontier:
the hash grid is gone, `actor_world_slice::sense_tree_` (member), `perception_target{size, entityid}` payload,
`perception_radius = 4.0`; `sense` rebuilds once/tick and runs one `nearest2` query/actor
(threat=bigger, prey=smaller).
Test in utils_general_test compares nearest/radius vs brute force (by distance, not id — tie-robust), the
radius bound, exclude-self, empty tree. Tie-break is "first found by distance" — add a total order for strict
determinism later.

**kD QUERY OPT + SCHEDULER (2026-06-30, build rc=0; historical baseline, superseded by 2026-07-17 measurements).** Split timing then showed sense ≈ build 0.55ms + **query
~5ms** (query dominated at that stage; later decimation/MT made build the producer floor). Incremental kD
modification "breaks balance" since balance is a one-time median property — incremental belongs to grid/DBVH,
not kD). Added `kd_tree::nearest2(q, r, predA, predB)` — both nearest in ONE traversal with shared pruning
(descend the far branch only if closer than BOTH bests) — and dropped `perception_radius` 8→4. sense query
~5ms → ~3.3ms; test `nearest2 == 2×nearest` added. (For uniform-dense dynamic + fixed radius a cell list
beats kD, deferred; kD kept for non-uniform/cull/raycast — grid raycast = DDA/Amanatides-Woo, needs no AABB.)
Then the **cognition scheduler** — the structural lever so AI cost ∝ BUDGET not actor count: new
`actor_cognition{uint64 last_think}` component; `update` split into sense snapshot/build (kD over ALL)
+ `cognition(tick)` + `apply`. `cognition`: (1) scan `view<actor_cognition>`, collect "ripe" actors
(last_think < tick), priority = staleness (overdue = tick − last_think, longest-waiting first, id tie-break →
deterministic), `nth_element`-cap to `think_budget_` (=512, ~1/8 of 4096, a member knob); (2) only the
selected get a kD perception query + GOAP decide → intent, then `last_think = tick`. Unselected coast on their
last velocity. `apply` phase 2 still integrates ALL every tick (cheap, smooth motion). count-budget (not
time) keeps it deterministic; `metrics.intents` now = thinkers/tick. Hooks for later: LOD (weight priority by
camera distance — needs the camera passed into `update`), events/dirty (`last_think` far in the future =
"asleep until an event"), decision-point timestamps. The O(N) selection scan and integrate-all are fine at
4096; at millions → timing-wheel/buckets + active-set culling.

**COMMIT WINDOW + MT + 64k STRESS (2026-06-30, release).** (1) **Commit window**: `actor_cognition` is gated
by `commit_ticks_ = 3` (a stand-in for action/animation duration) — an actor re-decides every 3 ticks;
`think_budget_` raised 512→2048 (above demand N/commit so the commit window, not the budget, is the binding
constraint → lag ~60ms not 150). (2) **MT cognition**: selection/cap stays single-threaded (cheap); the heavy
`due_` sweep is fanned via `pool.distribute(count, job)` (the `atomic_pool` from `container->pool`); the
scratch slot = `pool.thread_index(this_thread)` (0 = caller, 1.. = workers; arrays sized pool.size()+1) so
per-thread `plan_containers_`/`plan_caches_`/`intent_buffers_` are exclusive; `decide_actor` runs per actor;
`pool.wait()`; then merge buffers → `intents_` → sort by id. DETERMINISM HOLDS (selection total order, A* +
per-thread caches deterministic, final sort by id). Safe: each actor handled by exactly one thread (disjoint
perception/cognition writes); `world.get`/`sense_tree.nearest2`/`goap.decide` are read/const; all allocators
created in init (no lazy create under MT). `atomic_pool` is NOT std::function-based — `task_t<F,Args>` lives
in a fixed arena (`stack_pool`), captures ≤128B with no heap. cognition 2.5ms → ~0.5ms (~5×; ceiling = the
serial Amdahl fraction: scan + nth_element + merge/sort). (3) **64k actor stress**: update ~13-16ms (still
capped at main_fps 20). **`sense.tree` ~8.5-14ms DOMINATES** — the kD build is O(N·log N) over ALL actors,
single-threaded = the undecimated "producer floor". `cognition` ~0.9-1.9ms (budget+MT keep it low); `apply`
~0.5-1.1ms and `build` ~0.7-1.1ms are O(N) over 64k SINGLE-THREADED and jumped more than cognition →
candidates for deterministic MT / incremental structures. Confirms: at scale the producer (sense.tree) and
single-threaded O(N) phases bite, the decimated consumer (cognition) does not.

**kD PARALLEL BUILD + TAIL SNAPSHOT (2026-07-17).** This resolves the single-threaded producer floor for
the current slice without making the tree incremental. `kd_tree::build_parallel(pool)` partitions a small
deterministic top frontier, then builds disjoint subtrees with the existing recursive algorithm. Actor sense
gathers the next snapshot after structural changes and builds it at the tick tail concurrently with
`actor_batch::build`; positions at end N equal positions at start N+1, so cognition semantics are unchanged.
Init/load uses a synchronous fallback snapshot. Release 4096×120 improved from 1.479/1.532/1.316/1.165 to
1.304/1.248/0.937/0.865 ms/tick for 1/2/4/8 pool workers; a longer 4096×600 control measured
1.438/1.360/1.085/0.920. Hash/bytes remain identical within every worker-count sweep.

**Perf cleanup (2026-07-13).** The interim `utils::perf` helper has no live consumers and was archived
under root `exclude/`. tile_frontier actor phases now use catalogue domain/introspection wrappers directly;
future service-channel/UI graphs should extend that path instead of reviving the one-shot timer.

### Spatial structures & geometry primitives (libs/utils)

Reusable, dependency-free (no glm) spatial acceleration in `libs/utils/include/devils_engine/utils/`:
`geometry.h` (primitives + predicates), `kd_tree.h`, `grid.h` (`dense_grid` + `hash_grid`), `aabb_tree.h`
(dynamic BVH). All header-only. kd_tree is rebuild-per-frame (arena reuse); the grids and the BVH support
BOTH incremental `insert`/`remove`/`update` by stable handle AND bulk rebuild.

- **Vector type is a template parameter `Vec`, NOT `(Scalar, Dim)` + std::array.** `Vec` is the math
  vector type (default `std::array<float, Dim>`; the author will later feed `glm::vec4` regardless of
  2D/3D). Access is via `operator[]`; `Dim` = number of SIGNIFICANT axes (may be < Vec's component count —
  a `vec4` with `Dim=2` uses only `[0],[1]`); `Scalar` is deduced from `Vec` (`geom::scalar_of<Vec>`).
  `UpAxis` (template, default 1 = Y) is the "up" axis for the 3D `up_cylinder` query only.
- **One geometry contract drives all three structures.** Every shape provides `geom::overlaps(shape, aabb)`
  (node pruning + box-leaf test) and `geom::contains(shape, point)` (point-leaf test). Because `up_cylinder`
  needs `UpAxis`, structures call the dispatch wrappers `geom::test_overlaps<UpAxis>` / `test_contains<UpAxis>`
  (they forward `UpAxis` for `up_cylinder`, ignore it for all other shapes). `geom::query_bounds<UpAxis>(shape)`
  returns the shape's iteration AABB (±inf for unbounded shapes) for grid structures.
- **Unified query API: one overloaded `query(shape, visit)`** on every structure. `visit(const T& payload)`.
  Shapes: `aabb / sphere / up_cylinder / ray / cylinder / obb`. Primitives: `aabb{min,max}`,
  `sphere{center,radius}`, `up_cylinder{center,radius}` (infinite along UpAxis — 3D "radius column"; in 2D
  a radius query is just a `sphere`), `ray{origin, dir(normalized), tmax=inf}`, `cylinder{origin, dir, length,
  radius}` (a finite "ray with radius", FLAT caps: `0<=dot(p-o,dir)<=length && perp_dist<=radius`),
  `obb{center, half, axis[Dim]}` ("AABB + orientation"; axis = orthonormal world-space local axes).
- **Point stores vs box store.** `kd_tree`, `dense_grid`, `hash_grid` store POINTS (leaf = `contains(shape,
  point)`). `aabb_tree` stores EXTENDED objects, each with its own AABB (leaf = `overlaps(shape, box)`).
  Consequence: a `ray` query is degenerate on point stores (measure zero — `contains(ray, point)` is always
  false; use a `cylinder` = ray-with-radius instead) but MEANINGFUL on `aabb_tree` (ray vs bodies).
- **Per-object bounds on a point store → `geom::inflate(shape, margin)`** (Minkowski sum with a ball radius
  `margin`): "a body of radius r overlaps `shape`" ⟺ "its center ∈ `inflate(shape, r)`". Exact for spheres
  (sphere/aabb grow exactly), CONSERVATIVE for cylinder/obb (rounded caps/corners → superset; add a precise
  per-payload filter in `visit`). Inflate by the MAX object radius. `inflate(ray, m)` returns a `cylinder`
  radius `m` (a thick ray). For HETEROGENEOUS per-object bounds use `aabb_tree` (stores each object's AABB).
- `kd_tree` keeps its existing specialized `nearest`/`nearest2`/`radius` (predicate NN with distance pruning,
  used by tile_frontier sense) AND gained the generic `query`. The `Vec` migration is backward compatible —
  `kd_tree<T>` still defaults to `std::array<float,2>`, so tile_frontier's `sense_tree_` is unchanged. kd_tree
  is static (parallel rebuild once per frame) — no incremental remove.
- **Grids share `detail::grid_pool`**: ONE flat entry pool (no per-cell `std::vector`) with a free-list
  (stable indices, holes reused) + DOUBLY-LINKED per-cell chains → `insert`/`remove`/`update` are O(1).
  Returned `grid_handle{index, gen}` survives neighbour add/remove (`gen` catches a stale handle on a reused
  slot); `remove`/`update` on a stale handle are safe no-ops. `dense_grid(origin, dims, cell_size)` = bounded
  grid, arithmetic cell index `cy*width+cx` (no hashing); out-of-bounds points clamp to edge cells and edge
  cell bounds are open to ±inf so pruning never drops a clamped object. `hash_grid(cell_size)` = unbounded
  `gtl::flat_hash_map` by integer cell coord (arbitrary/negative coords); `query` clamps `query_bounds` to the
  occupied range and picks cheaper of {iterate cell box} vs {iterate occupied cells}. GRID GOTCHA: a new
  `hash_grid` cell head must init to `-1` via `try_emplace(c, -1)` — `heads_[c]` (operator[]) value-inits to
  `0`, a VALID entry index → corrupts the chain.
- `aabb_tree` = DYNAMIC BVH (Box2D `b2DynamicTree` style): node pool + free-list, `insert(box,payload)->
  bvh_handle` (best-sibling by SAH surface-area cost + refit ancestors + AVL-like rotation balancing),
  `remove(handle)` (collapse parent, refit), `update(handle, box)` = detach+attach (handle PRESERVED). Leaf
  stores the EXACT object AABB (no fat margin) → query exact. `rebuild()` re-optimizes but INVALIDATES handles.
  Convention: leaf ⇔ `child1==null`; free node ⇔ `height==-2` (threaded in free list). BVH GOTCHA hit:
  `remove` must decrement `leaf_count_` (detach alone keeps `size()` stale though queries stay correct).
- `cylinder`×AABB `overlaps` is CONSERVATIVE (segment bbox expanded by radius) — over-visits, never
  under-visits; the exact leaf test (`contains(cylinder,point)` / body AABB) removes false hits. Tighten with
  a segment/box distance test later if it matters. `obb`×AABB is exact SAT (2D: 4 axes; 3D: 15 axes with edge
  cross products, degenerate crosses skipped). Tests + brute-force cross-validation of all shapes across all
  four structures (2D+3D), plus incremental add/remove/update sequences and `inflate`, in
  `tests/utils_general_test.cpp` (kd_tree/dense_grid/hash_grid agree on points; aabb_tree agrees on boxes).

Next (agreed): **gameplay/animations** — also the real commitment source to generalize the scheduler (see the
AI scheduler notes). Perf picture is closed for now; further synthetic tuning is diminishing returns. Deferred
perf targets if needed: MT for apply/build + sense.tree (or an incremental cell-list), cell list/AABB in utils.

**GAMEPLAY/ANIMATION/SOUND LAYER BUILT (2026-06-30, tile_frontier; debug+release rc=0).** Author decisions:
brain = **GOAP-arbiter → FSM-executor** (acumen picks an action by priority → that action's hash IS the FSM
event → mood holds state/animation/commit/sound); save model = **snapshot of the replicated set** (phase E,
not started). All apply-phase mutations stay in entity-id order (determinism). Phases:
- **A — drives + priority ladder.** `actor_drives{hunger,boredom}` (0..1, REPLICATED). Predicates
  `actor.is_hungry`/`actor.is_bored`. Ladder via acumen requirements (complete disjoint partition, 1-step plan):
  `threat→flee`, `hungry&prey&in_range→eat`, `…&!in_range→chase`, `hungry&!prey→seek_food`, `bored→wander`,
  else `think`. Drive dynamics passively in apply for ALL actors/tick: hunger always rises; boredom rises while
  still (thinking), falls while moving → think⇄wander oscillation.
- **B — mood FSM + animation.** `actor_state{uint64 state-hash}` (REPLICATED; uint64 to keep string_id out of
  the header). `mood::system` from `"any_state + <action> = <state>"` lines. In apply, after the movement effect:
  `mood::step(event = intent.payload.call.fn)` (the action hash == mood's event_hash) → on a REAL state change,
  `apply_transition`. Animation = `animation_scale(state,tick,phase)` sinusoid in `actor_batch::build` (slow
  think … fast eating/flee) — **DERIVED, not stored** (a clean derived-vs-replicated example for save). `build`
  now takes `tick`; the pass is heterogeneous (food/obstacles have no `actor_state` → base size).
- **C — eating handshake (= the REAL commitment source).** `actor_eating{target,until_tick}` + `actor_grabbed{by}`.
  Perception stores the prey's FULL entityid (`prey_id`; kD payload `perception_target{size,id}`). Metric
  `prey_in_range` splits chase/eat. `effect_eat` (apply, id-order → deterministic: lower id grabs first; guards:
  prey exists / not grabbed / not itself eating / in range). FSM guard `[is_eating]` (name MUST be dot-free — the
  mood parser rejects '.' in identifiers; acumen metric names `actor.*` are fine, they never hit that parser)
  gates entry to `eating` to a successful grab. cognition SKIPS eating + grabbed actors → commit by action
  duration, retiring the `commit_ticks` stand-in for eaters. `resolve_eating` (after apply): hunger=0, drop
  `actor_eating`, `remove_entity` the prey (kill-list AFTER the view loop). apply also skips a grabbed actor's own
  queued intent; build_sense_tree excludes grabbed. `eat_duration=18`. Verified working (population drops, eaten
  count grows).
- **C2 — food items + obstacles.** `food_item` = small (0.2) static green entity; in the sense tree it reads as
  "prey" and is consumed by the SAME eat handshake (reliable — it never flees), **respawned** by `maintain_food`
  to `count/8` (cap 64/tick) → fixes depopulation. Zero new GOAP states. `obstacle{radius}` = gray disc, EXCLUDED
  from the sense tree, positional collision resolved in the integration pass (push-out; flat `obstacles_` cache,
  count capped 64).
- **D — sim sounds (HAS AN OPEN BUG).** `sound_for_state` binding (eating→chomp, flee→alert) emits
  `sound_emit{name,pos}` on FSM state ENTRY (EPHEMERAL, NOT replicated). Presentation bridge in `tile_frontier_game.cpp`
  culls by listener (camera) proximity + caps 8/tick → `command_sound{name}`. The sound actor preloads a named
  set from `resources/sounds/<type>/` and plays by `name` one-shot (explicit volume/pitch=1 — `sound::task` is a
  POD). **OPEN BUG: only ONE sound is audible in a run** — suspected sound-system issue, deferred, to debug with
  logging. test.mp3 was removed by the author (it was licensed). The sound dividing line (sim/replay-log vs
  presentation; the sound actor as a dumb mixer; 3 layers resource/trigger/policy) is documented — sim sounds are
  deterministic & replay-for-free, presentation sounds (UI click, ambient player) go straight to the actor and are
  never in the replay log; resources are owned by the asset system (handles), the UI owns playback policy only.

Tuning: hunger_rate 0.08, hungry_threshold 0.5, bored_rate 0.14 / relief 0.30 / threshold 0.5, eat_radius 0.9,
eat_duration 18, perception_radius 4.0, commit_ticks 3, think_budget 2048.

Also tidied `libs/acumen` (before the phases): `system::decide(const decide_params&, std::span<out>)` — inputs
bundled into a struct, `cache` optional (nullptr ⇒ no memoization), `out` a separate trailing arg; `find_solution`
demoted to a file-local static in system.cpp (decide is the sole public entry); hashes moved to a new
`libs/utils/.../hash.h` (fmix64/fmix32/hash_combine/wyhash64/splitmix/murmur_hash3_32). `murmur_hash64A` stays in
type_traits.h (tied to `type_id`).

REMAINING: D-UI (button click `ui_effect` + a Nuklear ambient player — presentation→sound directly, the player is
UI state); phase E (save = replicated-set snapshot + a replicated/derived classification; needs a
`world::for_each_pool` serialize hook in aesthetics — not present yet).

Next step after acumen/mood were ported to `act`: run GOAP/FSM over live `aesthetics::world` entities
and apply their decisions deterministically. NOTE: `subprojects/tile_frontier`'s `actor_world_slice`
(`actor_simulation.{h,cpp}`) ALREADY does a mini version of this — `think(tick)` → `intents_` (sorted
`actor_move_intent`) → `apply(dt)` mutates components. So this is an EVOLUTION of that code (replace the
hardcoded brain with acumen/mood, generalize `actor_move_intent` → `act::intent`), not greenfield, and is
prototyped in tile_frontier first (library-first: formalize into a lib later).

Planned layers:
- **`exec_context` ↔ `aesthetics::world` bridge (lives in tile_frontier, NOT in act — act stays
  ECS-agnostic).** `exec_context.w` is typed `const act::world*` which is an OPAQUE TAG (`act::world` is
  never defined — see the forward-decl comment in `exec_context.h`). The bridge is a reinterpret seam:
  build a ctx per entity with `c.w = reinterpret_cast<const act::world*>(&aesthetics_world)`, `scope[0] =
  act::entity_id{ uint32_t(entityid) }` (the FULL packed aesthetics id — version bits included — round-trips
  through the uint32), and a `world_of(ctx)` helper = `*reinterpret_cast<const aesthetics::world*>(ctx.w)`.
  Native predicates/effects are authored in tile_frontier (where both act and aesthetics are visible) and
  reinterpret `ctx.w` back to the concrete world. Hot path = pointer cast, no virtuals; act keeps zero
  dependency on aesthetics.
- **"think" system** (per entity with a brain/agent component): build ctx with `sink == nullptr`
  (dry-run — planning must not mutate), `system.compute_state(ctx)`, `find_solution(...)` into a stack
  `std::array`, take the first action → emit an `act::intent` (`call_function{fn}` / `move_to{target}`).
  mood analog: `fsm_event` → `mood::step` → if transitioned, emit an intent.
- **intent buffer** = `std::vector<act::intent>` sorted by `actor.id` (the generalized `intents_`). This is
  the "fast event buffer" — a sorted-by-entity array with an advancing cursor.
- **apply system** walks the buffer in id order and mutates each entity: `move_to`→velocity/position;
  `fsm_event`→`mood::apply_transition`; `call_function`→record/commit through the typed catalogue
  pipeline. Deterministic because provenance and commit order are fixed.

Validated facts grounding the design:
- **The cursor-advance model works because `aesthetics::world` iterates in ASCENDING id order**: `raw_itr`
  walks `sparce_set` (sparse index == entity index == id), skipping invalid. So an id-sorted intent buffer
  and the ECS view advance in lockstep — one pass, no per-entity lookup needed.
- **The live pipeline has one gameplay intent consumer plus the player queue.** Typed catalogue strategy
  executors own deferred gameplay effects; replay is not a catalogue consumer responsibility.
- think reads the world (pure predicates) ⇒ parallelizable; apply mutates in fixed id order ⇒ the
  deterministic barrier. Matches the "think in any order, apply in fixed order" rule.

OPEN QUESTIONS / forks (author to decide before building):
- **A. Bridge mechanism:** reinterpret seam (recommended — act decoupled, pointer cast) vs an abstract
  `act::world` interface with virtual accessors (slower, couples act to a world API).
- **B. Which backend to demo first on live actors:** mood (recommended — shortest loop event→transition→
  effect, fastest visible result; GOAP plugs in second by emitting `fsm_event`/`call_function`) vs acumen
  (GOAP planner first) vs the full GOAP→intent→FSM chain at once.
- **C. Where to build:** in tile_frontier's `actor_world_slice` first (recommended — library-first
  prototype), formalize into a lib later.

## Intent persistence/replay notes (superseded ownership; historical design)

The notes below predate the 2026-07-17 boundary decision. Their determinism advice (stable apply order,
per-entity RNG, fixed timestep, checksums) remains useful, but catalogue is no longer the owner of
replay/RPC/binary persistence. If persistence becomes real work, move those responsibilities into a
separate library and reassess the old channel design rather than restoring it into live catalogue.

**Intent as the thinker→ECS seam.** GOAP output is funneled through one compact `intent` struct of
base actions ECS systems consume (turn toward, move, call script, send FSM event, ...). This is the
deterministic-command / Quake-`usercmd` pattern: the single struct type is what makes observability
*tractable* — one serialization point instead of millions of scattered scripts. Hold onto this seam.

**Historical proposal only (superseded):** catalogue once had the following bones:
`INPUT_CHANNEL_ID / MUTATOR_CHANNEL_ID / META / SERVICE` (`channel_data.h`), an abstract `consumer`
(≤8 per channel), `rpc_function`/`mutator` wrappers.
- `intent` → INPUT channel (this is "the input" in catalogue's "log inputs, replay the rest" model).
  Log intents ALWAYS — compact, feeds replay + text export.
- mutations-after-apply → MUTATOR channel, but FILTERED/opt-in (specific entity + tick range) for
  focused debugging. Do NOT log mutations for millions of actors every frame — replay reconstructs
  them in the deterministic case.
- disk writer = just a `consumer` subscribed to the channel.
- **Authoritative on-disk format = binary (varint/zpp_bits), not text.** Text every frame × millions
  = I/O death. Text is an on-demand DECODE of a tick/entity slice; `tavl::serialize<T>` of the intent
  struct is the natural human-readable export (we're adopting tavl anyway).
- **Provenance for "why" not just "what":** the intent (or a META record) must carry which
  `action`/`goal` produced it (maybe plan id) so a trace reads as intent → action → goal. Without it
  you see commands, not decisions.

**Determinism (author is committed to pushing this hard).** Already done: seeds everywhere,
devils_script built around them, bindings hardwire PRNG as pure "next-from-previous".
- **The #1 real risk here is mutation ORDER under multithreading, not float bits** — tile_frontier is
  explicitly a millions-of-actors multithreading stress test. Determinism = deterministic order of
  state mutation first, bit-exact arithmetic second. The intent seam is the fix: think in parallel /
  any order, but APPLY intents in one barrier phase in a fixed order (sort by entity_id, never
  "whoever's message arrived first"). Under the actor model this means an explicit apply phase, not
  apply-on-message-arrival.
- **PRNG must be per-entity sub-streams**, not one global stream: derive `hash(global_seed, entity_id,
  tick, purpose)` (counter-based / splitmix style). Pure next-from-previous on a single shared stream
  still cascades — changing how many draws entity A makes shifts B and C, and order matters. Per-entity
  derivation makes RNG robust to iteration order and to adding/removing a draw. Check whether bindings'
  PRNG is currently one-per-context (the trap) and revisit before scripts pile up.
- **Float flags checklist:** kill `-ffast-math`/`-funsafe-math-optimizations` (the #1 killer, breaks
  determinism even same-binary across opt levels); `-ffp-contract=off` (FMA fusion of `a*b+c`); stay
  on SSE2, avoid x87/`-mfpmath=387`/32-bit; `sqrt` is IEEE-correct but `sin/cos/pow/exp` are NOT
  portable across libm versions/platforms (only matters cross-machine).
- **Pick the bar deliberately.** Same-binary/same-machine determinism (cheap: flags + pinned toolchain)
  fully covers the stated goal — replay, demos, "why did it decide that" debugging. Cross-machine
  determinism (own transcendentals / fixed-point / strict FMA discipline) is expensive and only needed
  for lockstep netcode. Do NOT gold-plate cross-platform float unless netcode is a real requirement.
- **Verify with state checksums** (Factorio / lockstep-RTS technique): hash full world state per tick
  (or every N) → a checksum stream; run the same intent log twice and diff; the first diverging tick
  localizes the bug. Ladder: same binary twice (catches threading order) → debug vs release (catches
  fast-math/reassociation) → cross-machine (catches libm/FMA). This rides catalogue directly: periodic
  full snapshot + checksum into META/SERVICE = the rollback design from the serialization notes, and a
  free desync detector on replay. A CI test that replays a recorded demo and asserts a golden final
  checksum is the regression guard.
- **Sim must run a fixed timestep** (time accumulator), decoupled from render FPS — a frame-varying
  `dt` silently breaks replay. Confirm the gameplay tick in `app.tavl` is fixed-step, not "elapsed".

## Demiurg ↔ Lua resource API (IMPLEMENTED 2026-07-06, first slice)

**Shipped (`d2ce7fe`).** Host-side in `subprojects/tile_frontier/src/core/simulation.cpp` (registered on the
visage sandbox env — demiurg itself has no Lua dep). Four GLOBAL keywords (no namespace), plus the
engine fully OWNS `require`:
- `request(id)` → `demiurg::resource_handle` (or `nil`). Single-resource accessor.
- `require(id)` → runs a `lua_script_resource` and returns its module. No `package.searchers`; resolves
  the handle, requires type lua (`luaL_error` "is not a lua script" otherwise), lazily `load()`s if not
  `usable()`, then `safe_script(text, env, chunk="@"+id)`. Has a `require_cache` keyed by absolute id
  (cycle guard: `true` before running, return value after; reset on error) and a `require_stack` so
  relative paths resolve against the current module.
- `find(prefix)` → Lua array of handles by PATH PREFIX (`resource_system::find`), across BOTH the engine
  and assets registries. `filter(substring)` → Lua array by SUBSTRING (`resource_system::filter<>`).
- `resource_handle` is a Lua usertype (`sol::no_constructor`): methods `valid/id/hash/state/usable/`
  `final_state/top_state`, each reads through `handle.get()` and returns `nil` if unresolved.
- Path resolution: `absolute_resource_path(current_module, path)` — trim, `\`→`/`, split `:selector`,
  strip leading `/`, resolve `.`/`..` vs `resource_parent_path`, drop extension, collapse segments.
  `lookup_resource_handle` searches engine registry first, then assets. (FIXED 2026-07-06: the
  segment-collapse loop as shipped in `d2ce7fe` never advanced `pos` past the final segment and spun
  forever — the FIRST `request()` from `entry.lua` hung main; `pos = end + 1` unconditionally now.)
- **Fonts through the same API (2026-07-06):** `nk.push_font{ font = request("fonts/crimson.italic"), … }`
  — the `font` field takes a `resource_handle`; visage resolves `handle.get<font_resource>()` and falls
  back to default while metrics aren't ready. String font names / `add_font` are gone. Backed by the
  `type_id`/`loading_type_id` split (exact identity vs loader-dispatch base) in demiurg — see
  "Stable handles" bullet.
- New resource type `lua_script_resource` (tile_frontier, `warm_and_hot_same`, text): `load_cold` reads
  script text (`ensure_text_loaded`, supports `path:name`/list-section entries), `unload_warm` drops it.
  Registered `engine_resources->register_type<lua_script_resource>("ui","lua")`.
- Lua-facing shift: `visage::system::load_entry_point(path)` REMOVED → `set_entry_point(const sol::object&)`
  (validates `sol::function`). `init()` bootstraps UI via `require("ui/entry")` and passes the returned
  function to `set_entry_point`. `entry.lua` builds `resources = { grass = request("textures/grass"), … }`
  (handles, not names). `app.image(...)`/`app.play_sound(...)` now take a `resource_handle` (or table with
  `resource=`/`res=`); the old `image_by_name`/`sound_by_name` string maps are GONE (`app.image` downcasts
  to `painter::gpu_texture_resource`, `nil` until `usable()`; `command_sound_play.res` = `resource_ref::from_handle`).
- **Did NOT land** (still design/open below): the "type IS the path" import-rule table (longest-segment
  type match, `trait/icon`→blit rules) — type is still `find_proper_type`'s segment match; `find` here is
  PREFIX-based, not by-kind; `:name` for lua modules is still effectively undefined.

Original design rationale (kept for the open items). Demiurg is fundamental enough that these are GLOBAL
keywords (no namespace), alongside Lua's `require`; the engine fully OWNS `require` (no `package.searchers`
hook) because every script comes from demiurg/mods (there is no plain-lua-from-disk case).

- **Four globals, two orthogonal axes (what to return × how many):**
  - `require(path | resource)` → runs and returns the lua MODULE. Script-only; a passed resource
    handle returns its module when the resource type is lua.
  - `request(path)` → the resource HANDLE itself (inspect size/source/metadata). The single-resource
    accessor (`get<T>`-equivalent). Note: `request`/`require` differ by 2 chars — typo-prone.
  - `filter(prefix)` → collection by PATH PREFIX (by location).
  - `find(type)` → collection by TYPE (by kind) across all paths/mods. This work ALSO fixes the
    dangling-ref bug (`view<>::operator[]`/`find<T>` currently returns a dangling reference).
- **Type IS the path (kills per-resource config):** the directory structure declares the type, and
  one `type → import rule` table in demiurg drives processing (`trait/icon` → blit 32×32,
  `spell/icon` → 256×256, `*/scripts/*` → load as lua). This table replaces scattered
  `{type,path,size}` configs and is what makes `require` "smart" (sees type=lua → returns a module).
  - name / cache key = full normalized path + filename without extension.
  - type = LONGEST registered contiguous-segment match anywhere in the path (`trait/icon/abc` beats
    `trait/icon`); position-independent — `effect/icon` matches both `/act2/spells/effect/icon/abc`
    and `/monsters/abilities/effect/icon/monster123/def`; everything around the type is just name.
  - leaf (last path segment) is ALWAYS excluded from type matching — it is the filename.
- **`:` selector — multiple data entries per file:** `path:index` or `path:local_name` indexes INTO a
  file. Cache the file ONCE by base path; selectors are cheap indexes into the loaded collection (not
  reloads). Sub-resources inherit the file's type and should be enumerable in `filter`/`find`. `:name`
  for lua-type resources is undefined (return `module.name`? or unsupported — `:` is for data/atlases).
- **Load ordering (for correctness):** register type rules → register+resolve all resources (mod
  override = a later mod shadows the same logical path) → THEN run scripts; so by the time `require`
  runs the path already points to the final overridden version and cache-by-path is correct.
- **Open (next session):** segment-boundary matching (whole segments, not substrings —
  `trait/icon` ∌ `traits/iconology`); tie-break when equal-length type matches at different positions;
  behavior when NO type matches (raw/untyped resource vs load error).

## Build/Layout Notes

- Root CMake builds an interface aggregate target `devils_engine::devils_plane`.
- Public includes are under `include/devils_engine/...` inside each `libs/*` directory.
- The root target and `subprojects/tile_frontier` use C++23 via `devils_engine::options`.
- `CODE_STYLE.md` is the canonical project style guide. Text uses UTF-8/LF, own code uses
  2-space indentation and snake_case, macros use SCREAMING_SNAKE_CASE, and third-party naming/layout
  is preserved. `.gitattributes`, `.editorconfig` and `.clang-format` encode the mechanical subset;
  do not run whole-tree formatting together with behavior changes.
- Dependencies are mostly fetched via CMake `FetchContent`, including `tavl`, `devils_script`, `miniaudio`, Vulkan-related libraries, Nuklear, msdfgen, glm, Catch2, etc.
- Root `README.md` was replaced on 2026-07-05 with a human-oriented map of `libs/` only. It is intentionally organized as separate sections per library: role, current shape, relationships, and status. Keep future `tests/` documentation separate unless explicitly asked.
- Documentation split requested by the author: root `README.md` should stay Russian for now and serve as a human-oriented overview for the author; `AGENTS.md` is the place for agent memory, technical gotchas, exact contracts, shutdown/order notes, and implementation details. Future README may become English later, but do not preemptively switch it.
- `libs/flow` is now an active CMake target. Current contract: animation = chain/graph of states; state has `duration_mcs`, `next` index, `images` as `demiurg::resource_handle + mirror_state` (migrated from raw `resource_interface*` in `8ce23c1`), `action` as `utils::id` (`invalid_id` = none), and `uv` delta. Runtime emits action messages; it must not call `act` effects directly from render/flow thread. `libs/bindings` has a CMake target but no local README, so its root README section is reconstructed from headers/sources.
- `libs/catalogue` current contract: use it as a small utility wrapper around selected function calls, not as `act` replacement, serializer or replay engine. Public active pieces are `devils_engine/catalogue/introspection.h`, logging, `call_log` and `deferred.h` (`mt::executor`, `record_scope`, collect/elect, `fn_deferred_ptr`). Old `core.h`/`registry.h`/`channel_data.h`/`rpc_function.h`/`demo.h` are archived under ignored `exclude/` and must not be revived into catalogue merely to add replay.
- Current `libs/` layering from CMake: `options` is the common interface build contract; `utils` is the low-level base; `act` feeds `mood` and `acumen`; `demiurg` feeds resource-backed `sound`/`painter`/`visage`; `input` feeds Vulkan/GLFW integration for `painter`; `bindings` and `visage` are tightly coupled for Lua/Nuklear UI.
- Be explicit about legacy/prototype code in docs and changes: `utils::actor_ref`, old painter renderer/image-container files and abandoned sound source classes are archived under ignored `exclude/`; `sound` still links/includes the mixed old OpenAL path next to current miniaudio `system2`; `catalogue` is still a prototype, not stable netcode.
- Linux portable runtime bundling is handled by `cmake/devils_portable_runtime.cmake`: it sets local RPATH to `$ORIGIN` and runs `file(GET_RUNTIME_DEPENDENCIES)` after build. The dependency copy script writes resolved `.so` files into the executable's `bin/`, removes stale/symlink `.so*` entries, names copied libraries by SONAME when available, and deliberately excludes the ELF loader plus core glibc-family libraries (`ld-linux`, `libc`, `libm`, `libdl`, `libpthread`, etc.).
- `vulkanmemoryallocator-hpp` is header-only from this project's point of view but brings Vulkan headers/VMA targets through CMake; keep those targets explicit in consumers such as `tile_frontier` instead of assuming a system Vulkan SDK layout. `msdf-atlas-gen` is not header-only; `artery-font` support is intentionally not needed.
- Current focused contract tests live in `tests/thread_general_test.cpp`, `tests/utils_contract_test.cpp`, `tests/sound_system_test.cpp`, and `tests/catalogue_introspection_test.cpp`. `thread_general_test` covers `atomic.h`, `atomic_pool.h`, `lock.h`, legacy `queue1`, and the new `thread::spsc_queue`; `utils_contract_test` covers `memory_pool`, `stack_allocator`, and `fixed_pool_mt` size/alignment checks; `sound_system_test` covers sound math/format helpers, task/resource defaults, playback-device enumeration, and a guarded `system2` construction smoke test; `catalogue_introspection_test` covers mirrored wrapper pointers for free functions, methods, const methods, structural functors, dry-run, and rolling stats. Newer: `tests/flow_test.cpp` (animation parse/playback/sampling), `tests/painter_shader_prepare_test.cpp` (assets-side GLSL→SPIR-V prepare + demiurg include resolution + list-pattern render config), and `tests/demiurg_resource_loader_test.cpp` (external GPU step + dependency gating + **stable `resource_handle` survives `clear()`+re-parse**).

## Collaboration Notes

- The author is comfortable with Russian comments and design notes in code; do not remove them just for cleanup.
- Keep changes scoped. Many comments are active design thinking, not dead noise.
- When changing architecture, first identify the existing exploratory direction and make the smallest concrete step that clarifies a contract.
- Avoid large rewrites unless explicitly requested.
