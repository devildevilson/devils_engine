#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

#include <devils_engine/act/building_blocks.h> // декларативная регистрация building blocks: ds + act replay
#include <devils_engine/act/exec_context.h>
#include <devils_engine/act/function.h>
#include <devils_engine/act/packer.h>   // entity_handle/rng_source/pack — упаковщик обычных функций без exec_context
#include <devils_engine/acumen/astar.h> // astar<>::container для find_solution
#include <devils_engine/aesthetics/common.h>
#include <devils_engine/aesthetics/system_runner.h>   // run — общий barrier независимых систем
#include <devils_engine/aesthetics/template_system.h> // template_system — range map-примитив фаз
#include <devils_engine/aesthetics/worklist_system.h> // worklist_system + budget_clamp — think-фаза
#include <devils_engine/catalogue/deferred.h>         // fn_deferred_ptr + collect/elect executors
#include <devils_engine/catalogue/introspection.h>
#include <devils_engine/catalogue/logging.h>  // DE_LOG — perf-дамп в домен gameplay
#include <devils_engine/mood/runtime.h>       // mood::step / apply_transition — шаг FSM
#include <devils_engine/thread/atomic_pool.h> // MT-пул (distribute/thread_index/wait)
#include <devils_engine/utils/core.h>         // utils::error
#include <devils_engine/utils/prng.h>         // utils::mix
#include <devils_engine/utils/string_id.h>    // utils::string_hash
#include <devils_script/system.h>

#include "actor_simulation.h"

// glm-адаптеры сериализации + регистрация ВСЕХ компонентов в реестр serial (SERIALIZABLE_COMPONENT).
// Включён здесь, а не только в тестах, чтобы игровой бинарь сам умел save/load (регистрации линкуются
// в этот TU) и чтобы сторонний дампер скаляров (sim_globals с glm::vec2) видел adapter<glm::vec2>.
#include "core/actor_snapshot.h"
#include "entity_scope.h" // seed_entity_scope — засев root-скоупа скрипт-предикатов
#include "spawn_scope.h"  // scope_spawn_at — примитивный ds-спавн (регистрируется в building blocks)

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// ── Мост act ↔ aesthetics::world ───────────────────────────────────────────
// act намеренно ECS-агностичен: exec_context.w / entity_handle.w — НЕПРОЗРАЧНЫЙ тег (act::world нигде
// не определён). Шов здесь, где видны оба мира: сырой указатель кладёт make_ctx, достаёт world_of(handle)
// одним кастом. Горячий путь — каст указателя, без виртуалок; act не зависит от aesthetics.

// Строит контекст вызова для конкретной сущности. sink == nullptr ⇒ dry-run
// (планирование/think не мутирует мир). entity_id round-trip'ит полный packed id.
static act::exec_context make_ctx(
  const aesthetics::world& world, const aesthetics::entityid_t id,
  const uint64_t seed, const uint64_t tick, act::effect_sink* sink = nullptr,
  act::execution_scratch* scratch = nullptr) noexcept {
  act::exec_context ctx;
  ctx.scope[0] = act::entity_id{uint32_t(id)};
  ctx.scope_count = 1;
  ctx.w = reinterpret_cast<const act::world*>(&world);
  ctx.rng_seed = seed;
  ctx.rng_entity = aesthetics::get_entityid_index(id);
  ctx.rng_tick = tick;
  ctx.sink = sink;
  ctx.scratch = scratch;
  return ctx;
}

// Аксессоры fat-handle (act::entity_handle) — для функций, написанных БЕЗ exec_context (packer.h,
// ROADMAP п.16). Реинтерпрет act::world → aesthetics::world (упаковщик уже снял const с ctx.w при сборке
// handle) + распаковка id. Даёт эффекту ровно world+entity, без остальных внутренностей exec_context.
// const_cast для мутации определён: actor_world_slice::world_ реально мутабелен (const на ctx.w — лишь
// контракт exec_context для предикатов/dry-run; эффекты фазы commit мутируют мир напрямую до effect_sink).
static aesthetics::world& world_of(const act::entity_handle h) noexcept {
  return *reinterpret_cast<aesthetics::world*>(h.w);
}
static aesthetics::entityid_t ent(const act::entity_handle h) noexcept {
  return aesthetics::entityid_t(h.id.id);
}
static aesthetics::world& world_of(const entity_scope h) noexcept {
  return *const_cast<aesthetics::world*>(h.w);
}
static aesthetics::entityid_t ent(const entity_scope h) noexcept {
  return aesthetics::entityid_t(h.id);
}

// ── тюнинг мотиваций (drives) ──────────────────────────────────────────────
// Скорости в единицах/секунду; пороги в 0..1. hunger копится всегда; boredom копится пока
// актор стоит (думает), быстро спадает в движении → колебание think⇄wander. Значения
// подобраны на глаз для main_fps≈20 (dt≈0.05). Все крутилки — кандидаты в config позже.
static constexpr float hunger_rate = 0.08f;  // голод/сек (≈12с до сытого→голодного)
static constexpr float bored_rate = 0.14f;   // скука/сек пока стоит (думает)
static constexpr float bored_relief = 0.30f; // спад скуки/сек в движении
static constexpr float still_speed2 = 0.01f; // |vel|^2 ниже — считаем «стоит»
// Длительность поедания в µs ИГРОВОГО времени (0.9с = прежние 18 тиков при 20fps, scale 1).
static constexpr uint64_t eat_game_ticks = 900000;
static constexpr uint64_t sated_seconds = 10; // игровых секунд сытости после еды (флаг sated, голод не растёт)

// Радиус восприятия для kD-запроса (мировые единицы, tile_size=1, шаг спавна ~1).
// Ограничивает поиск «ближайшего крупнее/мельче» (иначе предикат-NN без кандидата
// деградирует в полный обход) и задаёт модель ограниченного зрения: вне радиуса — не видим.
// Меньше радиус → меньше площадь поиска → дешевле query (квадратично).
static constexpr float perception_radius = 4.0f;

namespace catalogue_domains {
constexpr size_t actor_update_perf = 1;
}

using actor_perf_domain = catalogue::domain<catalogue_domains::actor_update_perf>;
using build_actor_batch_perf = actor_perf_domain::fn_traits<&actor_batch::build, "build", "batch", "world", "tick">;
using build_actor_batch_fn_t = build_actor_batch_perf::loc_fn_t;

// Общий catalogue-стат: агрегаты (avg/min/max/last) + кольцо последних замеров на каждую
// обёрнутую функцию (окно под график в UI). Режим statistics (невиртуальный switch): в exit
// пишется function_id+elapsed в store, без построения arg_views.
static catalogue::statistics_store& actor_perf_stats() noexcept {
  static catalogue::statistics_store store(256); // окно 256 замеров/функцию ≈ 12.8с при 20fps
  return store;
}

static void ensure_actor_perf_introspection() noexcept {
  static const catalogue::introspection cfg{
    catalogue::introspection_mode::statistics, catalogue::log_domain::gameplay, &actor_perf_stats()};
  actor_perf_domain::set_introspection(&cfg);
}

// Публичный доступ к perf-стату (для UI-биндинга в simulation.cpp; тот же поток).
const catalogue::statistics_store& actor_perf_statistics() noexcept {
  return actor_perf_stats();
}

void reset_actor_perf_statistics() noexcept {
  actor_perf_stats().reset();
}

// Периодический дамп агрегатов по всем обёрнутым функциям апдейта актора.
static void dump_actor_perf_stats() {
  actor_perf_stats().for_each([](const catalogue::statistics_store::function_record& r) {
    DE_LOG(catalogue::log_domain::gameplay, flow,
           "perf {}: avg {:.1f} us (min {} / max {} / last {}), n={}",
           r.name, r.average_mcs(), r.min_mcs, r.max_mcs, r.last_mcs, r.call_count);
  });
}

static uint32_t mix32(uint32_t x) noexcept {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

static glm::vec2 direction_from_hash(const uint32_t h) noexcept {
  constexpr float diag = 0.70710678118f;
  static const glm::vec2 dirs[] = {
    {1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}, {diag, diag}, {-diag, diag}, {diag, -diag}, {-diag, -diag}};
  return dirs[h & 7u];
}

static float unit_from_hash(const uint32_t h) noexcept {
  return float(h & 0xffffu) / float(0xffffu);
}

static instance_layout::rgba8_color actor_color(const uint32_t index) noexcept {
  const auto pack = [](const float v) {
    return uint32_t(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };

  static constexpr float colors[][4] = {
    {0.05f, 0.30f, 1.00f, 1.0f}, // blue
    {1.00f, 0.08f, 0.05f, 1.0f}, // red
    {0.62f, 0.16f, 1.00f, 1.0f}, // purple
    {0.05f, 0.85f, 0.25f, 1.0f}, // green
    {1.00f, 0.85f, 0.05f, 1.0f}, // yellow
    {1.00f, 0.35f, 0.03f, 1.0f}, // orange
    {0.00f, 0.85f, 0.95f, 1.0f}, // cyan
    {1.00f, 0.12f, 0.62f, 1.0f}, // magenta
  };
  const auto& c = colors[index % std::size(colors)];
  return instance_layout::rgba8_color{
    (pack(c[0]) << 0) | (pack(c[1]) << 8) | (pack(c[2]) << 16) | (pack(c[3]) << 24)};
}

static float actor_size(const uint32_t seed) noexcept {
  return 0.34f + unit_from_hash(mix32(seed ^ 0x9e37u)) * 0.42f;
}

static instance_layout::rgba8_color pack_rgba(const float r, const float g, const float b, const float a) noexcept {
  const auto p = [](const float v) {
    return uint32_t(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  return instance_layout::rgba8_color{(p(r) << 0) | (p(g) << 8) | (p(b) << 16) | (p(a) << 24)};
}

// ── еда и препятствия (C2) ─────────────────────────────────────────────────
static constexpr float food_size = 0.2f; // < минимального актора (0.34) ⇒ еда всем «добыча»
static instance_layout::rgba8_color food_color() noexcept {
  return pack_rgba(0.20f, 0.95f, 0.35f, 1.0f);
} // ярко-зелёный
static instance_layout::rgba8_color obstacle_color() noexcept {
  return pack_rgba(0.38f, 0.38f, 0.44f, 1.0f);
} // серый

// Анимация масштаба = синусоида, частота/амплитуда зависят от состояния FSM. ВЫВОДИМАЯ
// величина (из state + tick + пер-акторной фазы) — НЕ хранится, не пишется в save. Частота в
// циклах/тик (на main_fps≈20: 0.03 ц/т ≈ 0.6Гц медленно … 0.28 ц/т ≈ 5.6Гц паника).
// «Думаю — медленная синусоида, гонюсь/ем — быстрая» (как просил автор).
static float animation_scale(const uint64_t state, const uint64_t tick, const uint32_t phase) noexcept {
  static const uint64_t h_think = utils::string_hash("think");
  static const uint64_t h_wander = utils::string_hash("wander");
  static const uint64_t h_seek = utils::string_hash("seek_food");
  static const uint64_t h_chase = utils::string_hash("chase");
  static const uint64_t h_flee = utils::string_hash("flee");
  static const uint64_t h_eating = utils::string_hash("eating");
  static const uint64_t h_eaten = utils::string_hash("eaten");

  float freq = 0.05f, amp = 0.10f; // дефолт
  if (state == h_think) {
    freq = 0.03f;
    amp = 0.12f;
  } // медленное «дыхание» раздумья
  else if (state == h_wander) {
    freq = 0.07f;
    amp = 0.10f;
  } else if (state == h_seek) {
    freq = 0.12f;
    amp = 0.12f;
  } else if (state == h_chase) {
    freq = 0.20f;
    amp = 0.16f;
  } // возбуждённо
  else if (state == h_flee) {
    freq = 0.28f;
    amp = 0.18f;
  } // паника
  else if (state == h_eating) {
    freq = 0.40f;
    amp = 0.22f;
  } // быстрая синусоида — кушаем
  else if (state == h_eaten) {
    freq = 0.55f;
    amp = 0.30f;
  } // жертва бьётся в захвате

  const float t = float(tick + uint64_t(phase));
  return 1.0f + amp * std::sin(6.28318530718f * freq * t);
}

// Биндинг «вход в состояние FSM → звук» (хеш имени предзагруженного звука; 0 = тихо). MVP:
// только пунктовые события — eating (чавк) и flee (тревога). walking/ambient зарезервированы.
static uint64_t sound_for_state(const uint64_t state) noexcept {
  static const uint64_t h_eating = utils::string_hash("eating");
  static const uint64_t h_flee = utils::string_hash("flee");
  if (state == h_eating) {
    return utils::string_hash("eating");
  }
  if (state == h_flee) {
    return utils::string_hash("fleeing");
  }
  return 0;
}

// ── Геймплейные функции act (мозг акторов) ─────────────────────────────────
// Предикаты ЧИСТЫЕ — читают actor_perception (наполнен sense-фазой) за O(1), их
// свободно зовёт планировщик A*. Эффекты мутируют скорость на apply-фазе.

// Актор уже ест (есть actor_eating) — guard для перехода FSM в eating: транзит только если
// хват реально удался (effect_eat выставил компонент). Иначе остаёмся в прежнем состоянии.
static bool predicate_is_eating(const act::entity_handle self) noexcept {
  return world_of(self).get<actor_eating>(ent(self)) != nullptr;
}

// Ставит скорость актора в направлении dir (нормализованном) * его speed.
static void set_velocity(aesthetics::world& world, const aesthetics::entityid_t self, glm::vec2 dir) noexcept {
  auto* vel = world.get<actor_velocity>(self);
  const auto* brain = world.get<actor_brain>(self);
  if (vel == nullptr || brain == nullptr) {
    return;
  }
  const float len2 = dir.x * dir.x + dir.y * dir.y;
  dir = len2 > 1e-12f ? dir * (1.0f / std::sqrt(len2)) : glm::vec2{0.0f, 0.0f};
  vel->value = dir * brain->speed;
}

// Поедание — ЧИСТАЯ МУТАЦИЯ пары (self = root scope, prey = ds-аргумент от функции `prey`).
// Контенция решена до тела: actor_eat_effect_strategy elect'ит один source на prey и гасит prey,
// которая сама является source. Тело идёт в serial_structural commit lane, поэтому create компонентов
// не параллелится и guard'ы конкуренции здесь не нужны. Скрипт-форма: `eat = prey`.
static void effect_eat(const entity_scope self_s, const entity_scope prey_s) noexcept {
  if (self_s.w == nullptr) {
    return;
  }
  auto& world = world_of(self_s);
  const auto self = ent(self_s);
  const auto prey = prey_s.valid() ? ent(prey_s) : aesthetics::invalid_entityid;
  if (aesthetics::is_invalid_entityid(prey) || !world.exists(prey)) {
    return; // добыча исчезла между claim (think) и commit (apply)
  }
  world.create<actor_eating>(self, prey, utils::game_duration{eat_game_ticks}); // countdown: телу не нужен tick
  world.create<actor_grabbed>(prey, self);
  set_velocity(world, self, glm::vec2{0.0f, 0.0f});
  set_velocity(world, prey, glm::vec2{0.0f, 0.0f});
  if (auto* bs = world.get<actor_state>(prey); bs != nullptr) {
    bs->state = utils::string_hash("eaten");
  }
}

// Случайный курс из нормализованного броска [0,1) — один building block для wander/seek_food.
// Бросок делает САМ скрипт через ds `chance` (детерминированный prng_state сеется в
// act::script_function из seed/entity/tick, per-callsite соль различает скрипты) — курс
// материализуется в record-фазе и едет в journal обычным double-аргументом; телу RNG не нужен.
// Курс живёт до следующего think (прежние 12/24-тиковые slot-окна ушли вместе с act::rng_source).
static void effect_set_course(const entity_scope self, const double roll) noexcept {
  if (self.w == nullptr) {
    return;
  }
  set_velocity(world_of(self), ent(self), direction_from_hash(uint32_t(roll * 8.0)));
}

// Script-facing forms keep the root scope as an owned value. Catalogue records that value during
// cognition and dereferences the stable world/entity handle only at commit. This first config slice
// covers local effects that need no extra scope or RNG; structural eat remains the direct typed
// action until its script contract can expose prey+rng without borrowing VM state.
static void effect_flee_script(const entity_scope self) noexcept {
  auto& world = world_of(self);
  const auto id = ent(self);
  const auto* pos = world.get<actor_position>(id);
  const auto* per = world.get<actor_perception>(id);
  if (pos != nullptr && per != nullptr) {
    set_velocity(world, id, pos->value - per->threat_pos);
  }
}

static void effect_chase_script(const entity_scope self) noexcept {
  auto& world = world_of(self);
  const auto id = ent(self);
  const auto* pos = world.get<actor_position>(id);
  const auto* per = world.get<actor_perception>(id);
  if (pos != nullptr && per != nullptr) {
    set_velocity(world, id, per->prey_pos - pos->value);
  }
}

static void effect_think_script(const entity_scope self) noexcept {
  set_velocity(world_of(self), ent(self), glm::vec2{0.0f, 0.0f});
}

// ── ds-аксессоры восприятия (bool building blocks для inline-метрик GOAP) ──
// Читают actor_perception/positions за O(1); null-мир/мёртвый скоуп ⇒ false (паритет с нативами,
// предикат не кидает). Регистрируются в ds через actor_building_blocks().

static bool scope_threat_present(const entity_scope s) {
  if (s.w == nullptr) {
    return false;
  }
  const auto* per = s.w->get<actor_perception>(aesthetics::entityid_t(s.id));
  return per != nullptr && per->has_threat;
}

static bool scope_prey_present(const entity_scope s) {
  if (s.w == nullptr) {
    return false;
  }
  const auto* per = s.w->get<actor_perception>(aesthetics::entityid_t(s.id));
  return per != nullptr && per->has_prey;
}

// ── generic-флаги (aesthetics::flag_set, countdown-модель) ─────────────────
// Тела deferred-блоков исполняются на commit без контекста времени ⇒ duration записывается как
// остаток, sweep (expire_flags) вычитает game-дельту. seconds — ИГРОВЫЕ секунды из скрипта;
// seconds <= 0 означает бессрочный флаг. create-on-demand компонента ⇒ set/clear идут в
// serial_structural lane (см. actor_flag_effect_strategy ниже).

static utils::id sated_flag_id() {
  static const utils::id id = utils::string_hash("sated");
  return id;
}

static void effect_set_flag(const entity_scope self, const std::string_view name, const double seconds) {
  if (self.w == nullptr) {
    return;
  }
  auto& world = world_of(self);
  const auto id = ent(self);
  if (!world.exists(id)) {
    return; // сущность исчезла между record (think) и commit (apply)
  }
  auto* flags = world.get<flag_set>(id);
  if (flags == nullptr) {
    flags = world.create<flag_set>(id);
  }
  const auto remaining = seconds <= 0.0
    ? flag_set::no_expiry
    : utils::game_duration{uint64_t(seconds * double(utils::timeline_ticks_per_second))};
  flags->set(utils::string_hash(name), remaining);
}

static void effect_clear_flag(const entity_scope self, const std::string_view name) {
  if (self.w == nullptr) {
    return;
  }
  auto& world = world_of(self);
  const auto id = ent(self);
  if (auto* flags = world.exists(id) ? world.get<flag_set>(id) : nullptr; flags != nullptr) {
    flags->remove(utils::string_hash(name));
  }
}

static bool scope_has_flag(const entity_scope s, const std::string_view name) {
  if (s.w == nullptr) {
    return false;
  }
  const auto* flags = s.w->get<flag_set>(aesthetics::entityid_t(s.id));
  return flags != nullptr && flags->has(utils::string_hash(name));
}

// Скоуп-функция «таргет из ближайших»: ближайшая добыча из восприятия как entity_scope. Скрипт
// передаёт её аргументом эффекту (`eat = prey`); явный typed-аргумент нужен арбитражу — elect
// видит prey в record-фазе, а не выкапывает её из компонентов внутри тела на commit.
static entity_scope scope_prey(const entity_scope s) {
  if (s.w == nullptr) {
    return entity_scope{};
  }
  const auto* per = s.w->get<actor_perception>(aesthetics::entityid_t(s.id));
  if (per == nullptr || !per->has_prey) {
    return entity_scope{};
  }
  const auto prey = per->prey_id;
  if (aesthetics::is_invalid_entityid(prey) || !s.w->exists(prey)) {
    return entity_scope{};
  }
  return entity_scope{s.w, uint32_t(prey)};
}

// Добыча в радиусе хвата (тот же eat_radius, что у хендшейка поедания).
static bool scope_prey_in_range(const entity_scope s) {
  if (s.w == nullptr) {
    return false;
  }
  const auto id = aesthetics::entityid_t(s.id);
  const auto* per = s.w->get<actor_perception>(id);
  const auto* pos = s.w->get<actor_position>(id);
  if (per == nullptr || pos == nullptr || !per->has_prey) {
    return false;
  }
  const glm::vec2 d = per->prey_pos - pos->value;
  constexpr float eat_radius = 0.9f;
  return (d.x * d.x + d.y * d.y) <= eat_radius * eat_radius;
}

// ── Catalogue deferred strategies ──────────────────────────────────────────
// Все локальные действия мутируют только source entity: collect группирует по self и позволяет
// commit разных entity-групп через worker pool. eat меняет пару сущностей + создаёт компоненты:
// elect выбирает младший source на prey, target_not_source сохраняет старое «intent beats grab»
// (если prey сама пытается eat — её не хватают), а тело идёт в serial_structural lane.
namespace {
using actor_local_effect_strategy = catalogue::mt::preset::parallel_collect<0>;
using actor_eat_effect_strategy = catalogue::mt::preset::structural_elect<
  1, catalogue::mt::conflict::target_not_source>;
// Флаги: collect (все вызовы, не победитель), но commit в ST structural lane — тело может
// создавать компонент flag_set по требованию (create не MT-safe, категория D).
using actor_flag_effect_strategy = catalogue::mt::collect<
  catalogue::mt::key::entity_arg<0>, catalogue::mt::order::source_then_sequence,
  catalogue::mt::commit::serial_structural>;

struct actor_local_effect_domain_id {};
struct actor_eat_effect_domain_id {};
struct actor_flag_effect_domain_id {};
using actor_local_effect_domain = catalogue::mt::domain<
  actor_local_effect_domain_id, actor_local_effect_strategy>;
using actor_eat_effect_domain = catalogue::mt::domain<
  actor_eat_effect_domain_id, actor_eat_effect_strategy>;
using actor_flag_effect_domain = catalogue::mt::domain<
  actor_flag_effect_domain_id, actor_flag_effect_strategy>;

using set_course_deferred = actor_local_effect_domain::fn_traits<
  &effect_set_course, "set_course", "self", "course">;
using set_flag_deferred = actor_flag_effect_domain::fn_traits<
  &effect_set_flag, "set_flag", "self", "flag", "seconds">;
using clear_flag_deferred = actor_flag_effect_domain::fn_traits<
  &effect_clear_flag, "clear_flag", "self", "flag">;
using eat_deferred = actor_eat_effect_domain::fn_traits<
  &effect_eat, "eat", "self", "prey">;
using flee_script_deferred = actor_local_effect_domain::fn_traits<
  &effect_flee_script, "flee", "self">;
using chase_script_deferred = actor_local_effect_domain::fn_traits<
  &effect_chase_script, "chase", "self">;
using think_script_deferred = actor_local_effect_domain::fn_traits<
  &effect_think_script, "think", "self">;

constexpr uint32_t max_deferred_effects_per_actor = 16;
} // namespace

const act::building_blocks& actor_building_blocks() {
  static const act::building_blocks blocks = [] {
    act::building_blocks b;
    // ds-словарь конфиг-скриптов: deferred-эффекты (record-only) + чистые аксессоры восприятия/spawn.
    b.effect<flee_script_deferred>();
    b.effect<chase_script_deferred>();
    b.effect<think_script_deferred>();
    b.effect<eat_deferred>();        // `eat = prey`: elect по prey-аргументу, тело в serial_structural lane
    b.effect<set_course_deferred>(); // `set_course = chance`: wander/seek_food из конфига
    b.effect<set_flag_deferred>();   // `set_flag = { name, seconds }`: generic флаг с countdown-сроком
    b.effect<clear_flag_deferred>(); // `clear_flag = name`: снять флаг досрочно
    b.pure<&scope_has_flag>("has_flag");
    b.pure<&scope_threat_present>("threat_present");
    b.pure<&scope_prey_present>("prey_present");
    b.pure<&scope_prey_in_range>("prey_in_range");
    b.pure<&scope_prey>("prey");
    b.pure<&scope_spawn_at>("spawn_at");
    // Исключение в обход ds: is_eating — FSM-гвард, скриптам не нужен. Конфиг-скрипт с тем же
    // именем упадёт громко на install() — фолбэков нет, конфликт решается удалением записи отсюда.
    b.native<&predicate_is_eating>("is_eating");
    // Арбитраж семантического имени: eat elect-ится эксклюзивно по target scope + self-claim
    // («intent бьёт grab»); дескриптор живёт у ИМЕНИ и переживёт перевод eat на script backend.
    b.reg_interaction("eat", act::interaction{});
    return b;
  }();
  return blocks;
}

struct deferred_effects {
  catalogue::mt::executor<actor_local_effect_strategy> local;
  catalogue::mt::executor<actor_eat_effect_strategy> eat;
  catalogue::mt::executor<actor_flag_effect_strategy> flags;

  ~deferred_effects() {
    if (actor_local_effect_domain::executor_instance() == &local) {
      actor_local_effect_domain::set_executor(nullptr);
    }
    if (actor_eat_effect_domain::executor_instance() == &eat) {
      actor_eat_effect_domain::set_executor(nullptr);
    }
    if (actor_flag_effect_domain::executor_instance() == &flags) {
      actor_flag_effect_domain::set_executor(nullptr);
    }
  }

  void begin_record(const size_t source_capacity, const size_t call_capacity) {
    actor_local_effect_domain::set_executor(&local);
    actor_eat_effect_domain::set_executor(&eat);
    actor_flag_effect_domain::set_executor(&flags);
    local.begin_record(source_capacity, max_deferred_effects_per_actor, call_capacity);
    eat.begin_record(source_capacity, max_deferred_effects_per_actor, call_capacity);
    flags.begin_record(source_capacity, max_deferred_effects_per_actor, call_capacity);
  }
};

void actor_batch::build(const aesthetics::world& world, const uint64_t tick) {
  instances_.clear();
  ids_.clear();
  instances_.reserve(world.count<actor_visual>());
  ids_.reserve(world.count<actor_visual>());

  // Один проход по всему рисуемому (актёры И будущие предметы). У кого есть actor_state —
  // масштаб анимируется синусоидой по состоянию; у предметов/препятствий (нет state) —
  // базовый размер. get<> по id — O(1) sparse-set, проход не децимируется, цена приемлема.
  for (auto [id, pos, visual] : world.view<actor_position, actor_visual>()) {
    float scale = visual->size;
    if (const auto* st = world.get<actor_state>(id); st != nullptr) {
      const auto* brain = world.get<actor_brain>(id);
      scale *= animation_scale(st->state, tick, brain != nullptr ? brain->phase : 0u);
    }
    ids_.push_back(uint32_t(id));
    instances_.push_back(actor_instance{pos->value, visual->texture, visual->color, scale});
  }
}

void actor_world_slice::setup_brain_registry() {
  if (brains_.is_hungry_program == nullptr || brains_.fsm_transitions == nullptr ||
      brains_.goap == nullptr || brains_.prefabs.empty()) {
    utils::error{}("tile_frontier: actor_world_slice requires script, FSM, GOAP, and prefab config");
  }

  // пересоздаём реестр с нуля: reg() ассертит на повторную регистрацию имени,
  // а init() может вызываться многократно.
  registry_ = act::registry{};
  goap_registry_ = acumen::registry{};
  fsm_registry_ = mood::registry{};
  goap_ = nullptr;
  fsm_ = nullptr;
  registry_.reg("actor.is_hungry", std::make_unique<act::script_function<bool>>(
                                     brains_.is_hungry_program, &seed_entity_scope));
  // ВАЖНО: на "is_eating" (внутри building blocks) ссылается СТРОКА FSM (mood) как гвард — парсер
  // mood не допускает точку в идентификаторах, поэтому имя dot-free (в отличие от acumen-метрик
  // "actor.*", которые в парсер mood не попадают и резолвятся по полному хешу строки).
  // Native-исключения + interaction-дескрипторы из декларативного списка; дубликат имени с
  // конфиг-скриптом упадёт громко в reg() (никаких тихих фолбэков).
  actor_building_blocks().install(registry_);
  build_goap_from_config(*brains_.goap);

  // FSM-исполнитель (mood): событие = выбранное GOAP действие, ведёт в одноимённое состояние из
  // ЛЮБОГО (any_state — wildcard). Движение остаётся эффектом GOAP в apply; FSM держит состояние
  // (выбор анимации в рендере) и даёт точку для on_entry-эффектов: звук — фаза D, поедание (guard
  // «добыча в радиусе» → состояние eating с длительностью) — фаза C. Пока чистые рёбра без эффектов.
  // Гварды/действия в строках — имена функций из registry_; mood резолвит их при сборке.
  fsm_registry_.add("actor", mood::system(&registry_, *brains_.fsm_transitions));
  fsm_ = fsm_registry_.get("actor");

  // Префабы слайса (перестраиваются вместе с реестром — общая точка init/load). C++-специи компонентов
  // (какие типы бывают + per-prefab on_construct для DERIVED) регистрируются ЗДЕСЬ; тексты префабов
  // приходят из обязательного конфига (prefab/*.tavl через brain_config.prefabs).
  //   food:  data food_item из конфига; визуал (зелёный, food_size) + позиция — derived в construct.
  //   actor: data actor_tuning (разброс скорости/голода/силы) из конфига; brain/visual/stats/… — derived
  //          в construct из per-instance зерна (spawn_args). GOAP/FSM остаются slice-level (не per-entity).
  prefab_ = devils_engine::prefab::prefab_registry<spawn_args>{};
  prefab_.data<food_item>("food_item");
  prefab_.data<actor_tuning>("actor_tuning");
  prefab_.on_construct("food", [](aesthetics::entityid_t id, aesthetics::world& w, const spawn_args& a) {
    if (w.get<food_item>(id) == nullptr) {
      utils::error{}("tile_frontier: required food_item is missing from prefab 'food'");
    }
    w.create<actor_position>(id, a.pos);
    w.create<actor_visual>(id, 0u, food_color(), food_size);
  });
  prefab_.on_construct("actor", [](aesthetics::entityid_t id, aesthetics::world& w, const spawn_args& a) {
    // DERIVED per-instance из зерна + тюнинга (формулы = историческому init-циклу, побайтовый паритет).
    const auto* tp = w.get<actor_tuning>(id);
    if (tp == nullptr) {
      utils::error{}("tile_frontier: required actor_tuning is missing from prefab 'actor'");
    }
    const actor_tuning t = *tp;
    const uint32_t tex = std::max(a.tex_count, 1u);
    w.create<actor_position>(id, a.pos);
    w.create<actor_velocity>(id, glm::vec2{0.0f, 0.0f});
    w.create<actor_brain>(id, a.seed, a.seed % 31u, t.speed_base + unit_from_hash(a.seed) * t.speed_var);
    w.create<actor_visual>(id, (a.index + 1u) % tex, actor_color(a.index), actor_size(a.seed));
    w.create<actor_perception>(id);
    w.create<actor_cognition>(id); // last_think=0 ⇒ все «созрели» на старте
    w.create<stats>(id, unit_from_hash(mix32(a.seed ^ 0xf00du)) * t.hunger_scale, 0.0f,
                    int64_t(mix32(a.seed ^ 0x57ab00du) % uint32_t(t.strength_mod)));
    w.create<actor_state>(id, utils::string_hash("think")); // стартуем «думающими»
  });
  const auto prefab_lc = devils_engine::prefab::prefab_load_context{&registry_};
  for (const auto& d : brains_.prefabs) {
    prefab_.add_prefab(d.name, d.text, prefab_lc);
  }
}

void actor_world_slice::build_goap_from_config(const acumen::goap_config& cfg) {
  // Метрики ОПРЕДЕЛЯЮТ свои предикаты инлайн-скриптами: регистрируем каждый как script_function<bool>
  // под ключом метрики в registry_ (перед сборкой acumen — он резолвит метрики по имени). Скрипт
  // исполняется на per-worker execution_scratch; засев root-скоупа — seed_entity_scope. Контейнеры живут в
  // goap_config (в реестре ассетов), поэтому заимствование &m.program валидно на всё время слайса.
  for (const auto& m : cfg.metrics) {
    registry_.reg(m.key, std::make_unique<act::script_function<bool>>(&m.program, &seed_entity_scope));
  }

  // Script-backed actions keep the semantic action name in act/acumen/FSM. Their ds programs call
  // fn_deferred_ptr building blocks, so cognition remains a record-only phase and apply still owns
  // every collect/elect commit lane.
  for (const auto& a : cfg.actions) {
    if (a.has_effect_program) {
      registry_.reg(a.name, std::make_unique<act::script_function<void>>(
                              &a.effect_program, &seed_entity_scope));
    }
  }

  // Единое пространство имён битов: метрики (0..N-1, порядок = конфиг) + символические биты
  // (next_state/goal, напр. "resolved") получают индексы ≥N при первой встрече. Линейный поиск — N крошечно.
  std::vector<std::string> bit_names;
  bit_names.reserve(cfg.metrics.size() + 4);
  for (const auto& m : cfg.metrics) {
    bit_names.push_back(m.key);
  }

  const auto bit_of = [&bit_names](const std::string& key) -> size_t {
    for (size_t i = 0; i < bit_names.size(); ++i) {
      if (bit_names[i] == key) {
        return i;
      }
    }
    bit_names.push_back(key);
    return bit_names.size() - 1;
  };

  // "key" / "!key" / "not key" -> (ключ, значение бита). Пробелы обрезаем.
  const auto parse_bit = [](std::string_view s) -> std::pair<std::string, bool> {
    const auto trim = [](std::string_view v) {
      while (!v.empty() && v.front() == ' ') {
        v.remove_prefix(1);
      }
      while (!v.empty() && v.back() == ' ') {
        v.remove_suffix(1);
      }
      return v;
    };
    s = trim(s);
    bool value = true;
    if (s.starts_with("!")) {
      value = false;
      s = trim(s.substr(1));
    } else if (s.starts_with("not ")) {
      value = false;
      s = trim(s.substr(4));
    }
    return {std::string(s), value};
  };

  const auto build_ss = [&](const std::vector<std::string>& keys) {
    acumen::scoped_state ss;
    for (const auto& raw : keys) {
      const auto [key, value] = parse_bit(raw);
      ss.set(bit_of(key), value);
    }
    return ss;
  };

  std::vector<acumen::state_metric> metrics;
  metrics.reserve(cfg.metrics.size());
  for (const auto& m : cfg.metrics) {
    metrics.emplace_back(m.key);
  }

  std::vector<acumen::action> actions;
  actions.reserve(cfg.actions.size());
  for (const auto& a : cfg.actions) {
    actions.emplace_back(a.name, build_ss(a.requirements), build_ss(a.next_state), build_ss(a.weight_state));
  }

  std::vector<acumen::goal> goals;
  goals.reserve(cfg.goals.size());
  for (const auto& g : cfg.goals) {
    goals.push_back(acumen::goal{g.name, build_ss(g.requirements), build_ss(g.goal)});
  }

  // резолвит предикаты (метрики) и эффекты (действия) по имени из registry_, кидает при промахе.
  goap_registry_.add("actor", acumen::system(&registry_, std::move(metrics), std::move(goals), std::move(actions)));
  goap_ = goap_registry_.get("actor");
}

// ── map-фазы на общем примитиве template_system + внешний run ──
// Параллельный обход запроса; process мутирует ТОЛЬКО «свою» сущность (обстоятельства — read-only)
// ⇒ потоки трогают непересекающуюся память, локов нет. dt ставится слайсом перед проходом.

// Движение позиции по скорости + жёсткое позиционное расталкивание препятствиями (как было в apply).
struct integration_system : devils_engine::aesthetics::template_system<actor_position, actor_velocity> {
  float dt = 0.0f;
  const std::vector<actor_world_slice::obstacle_disc>* obstacles = nullptr;
  explicit integration_system(aesthetics::world* w) noexcept : template_system(w) {}
  void process(const query_tuple_t& t, const size_t /*time*/) override {
    auto* pos = std::get<1>(t);
    const auto* vel = std::get<2>(t);
    pos->value += vel->value * dt;
    if (obstacles == nullptr) {
      return;
    }
    for (const auto& o : *obstacles) {
      const glm::vec2 d = pos->value - o.pos;
      const float d2 = d.x * d.x + d.y * d.y;
      if (d2 < o.radius * o.radius) {
        const float len = std::sqrt(d2);
        const glm::vec2 n = len > 1e-6f ? d * (1.0f / len) : glm::vec2{1.0f, 0.0f};
        pos->value = o.pos + n * o.radius;
      }
    }
  }
};

// Пассивная динамика мотиваций: голод копится всегда, скука растёт в простое / спадает в движении.
struct drives_system : devils_engine::aesthetics::template_system<stats, actor_velocity> {
  float dt = 0.0f;
  aesthetics::world* w = nullptr;
  explicit drives_system(aesthetics::world* world) noexcept : template_system(world), w(world) {}
  void process(const query_tuple_t& t, const size_t /*time*/) override {
    auto* dr = std::get<1>(t);
    const auto* vel = std::get<2>(t);
    // Сытость (флаг sated с countdown) замораживает рост голода — чтение СВОЕГО компонента, MT-safe.
    const auto* flags = w->get<flag_set>(std::get<0>(t));
    if (flags == nullptr || !flags->has(sated_flag_id())) {
      dr->hunger = std::clamp(dr->hunger + hunger_rate * dt, 0.0f, 1.0f);
    }
    const float speed2 = vel->value.x * vel->value.x + vel->value.y * vel->value.y;
    const float db = (speed2 <= still_speed2 ? bored_rate : -bored_relief) * dt;
    dr->boredom = std::clamp(dr->boredom + db, 0.0f, 1.0f);
  }
};

// think-фаза: worklist_system над «созревшими» акторами (отобранными select+budget_clamp). process
// зовёт decide_actor слайса на своей per-thread scratch-полосе (A*+cache+ds VM); тот под record_scope
// вызывает act::effect, чья deferred-обёртка append-ит вызов в dense journal.
struct cognition_system : devils_engine::aesthetics::worklist_system<acumen::execution_scratch> {
  actor_world_slice* slice;
  cognition_system(thread::atomic_pool* pool, actor_world_slice* s) noexcept
    : worklist_system(pool), slice(s) {}
  void process(aesthetics::entityid_t id, acumen::execution_scratch& scratch, const size_t time) override {
    slice->decide_actor(id, time, scratch);
  }
};

actor_world_slice::actor_world_slice() noexcept = default;
actor_world_slice::~actor_world_slice() = default;

void actor_world_slice::init(const uint32_t count, const glm::vec2 min_bound, const glm::vec2 max_bound, const uint32_t texture_count,
                             const brain_config& brains) {
  world_ = aesthetics::world{};
  // Кэш-системы держат query, подписанный на СТАРЫЙ world — сбросить, пересоздадутся против нового.
  integration_sys_.reset();
  drives_sys_.reset();
  sense_tree_sys_.reset();
  sense_tree_ready_ = false;
  select_sys_.reset();
  resolve_eating_sys_.reset();
  deferred_.reset();
  brains_ = brains;
  setup_brain_registry();
  calls_.reset(count); // предварительная ёмкость по числу акторов; растёт в cognition до index_capacity
  tick_ = 0;
  game_ticks_ = 0;

  // СОЗДАЁМ аллокаторы пулов поедания заранее на ГЛАВНОМ потоке. Две причины: (1) view<>
  // (в resolve_eating) кидает, если аллокатора нет, а компоненты появляются лишь при первом
  // хвате; (2) component_type_id — sequential_type_id (присваивается при первом обращении),
  // фиксируем его до любого MT-доступа. block_size как в world::create (sizeof(T)*250).
  static_cast<void>(world_.get_or_create_allocator<actor_eating>(sizeof(actor_eating) * 250));
  static_cast<void>(world_.get_or_create_allocator<flag_set>(sizeof(flag_set) * 250));
  static_cast<void>(world_.get_or_create_allocator<actor_grabbed>(sizeof(actor_grabbed) * 250));

  // C2: границы для респавна еды, целевое число еды, кэш препятствий.
  spawn_min_ = min_bound;
  spawn_max_ = max_bound;
  texture_count_ = std::max(texture_count, 1u);
  food_target_ = std::max(count / 8u, 32u);
  food_spawn_seq_ = 0;
  obstacles_.clear();

  const uint32_t tex_count = std::max(texture_count, 1u);
  const glm::vec2 extent = max_bound - min_bound; // только для стартовой раскладки
  const uint32_t columns = std::max<uint32_t>(uint32_t(std::ceil(std::sqrt(float(std::max(count, 1u))))), 1u);
  const uint32_t rows = std::max<uint32_t>((count + columns - 1u) / columns, 1u);

  // Спавн акторов через обязательный префаб «actor»: data-компонент actor_tuning из
  // конфига задаёт разброс, on_construct лепит seed-производные компоненты. Раскладка/зерно/индекс —
  // per-instance (сетка+джиттер) → в spawn_args. gen_entityid зовётся внутри spawn (порядок id сохранён).
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t x = i % columns;
    const uint32_t y = i / columns;
    const float fx = (float(x) + 0.5f) / float(columns);
    const float fy = (float(y) + 0.5f) / float(rows);
    const uint32_t seed = mix32(i + 0x1234u);
    const float jitter_x = (unit_from_hash(mix32(seed ^ 0xa5a5u)) - 0.5f) * 0.35f;
    const float jitter_y = (unit_from_hash(mix32(seed ^ 0x5a5au)) - 0.5f) * 0.35f;
    const glm::vec2 pos{
      min_bound.x + extent.x * fx + jitter_x,
      min_bound.y + extent.y * fy + jitter_y};
    prefab_.spawn("actor", world_, spawn_args{pos, seed, i, tex_count});
  }

  // Препятствия: статичные диски в детерминированных точках. Из восприятия исключены
  // (build_sense_tree их пропускает), рисуются (есть position+visual). Кэшируем в obstacles_
  // для дешёвой коллизии (O(N·M), M мало) в фазе интеграции.
  const uint32_t obstacle_count = std::min(std::max(count / 256u, 8u), 64u); // кап: коллизия O(N·M)
  obstacles_.reserve(obstacle_count);
  for (uint32_t i = 0; i < obstacle_count; ++i) {
    const uint32_t os = mix32(i * 2654435761u + 0x0b57ac1eu);
    const float fx = unit_from_hash(os);
    const float fy = unit_from_hash(mix32(os ^ 0x5bd1e995u));
    const float radius = 1.0f + unit_from_hash(mix32(os ^ 0xa53cu)) * 1.5f; // 1.0..2.5
    const glm::vec2 p{min_bound.x + extent.x * fx, min_bound.y + extent.y * fy};
    const auto id = world_.gen_entityid();
    world_.create<actor_position>(id, p);
    world_.create<actor_visual>(id, 0u, obstacle_color(), radius); // визуальный размер ≈ радиус
    world_.create<obstacle>(id, radius);
    obstacles_.push_back(obstacle_disc{p, radius});
  }

  // Стартовая еда до целевого числа (дальше поддерживается maintain_food каждый тик).
  for (uint32_t i = 0; i < food_target_; ++i) {
    spawn_food();
  }
}

actor_metrics actor_world_slice::update(const uint64_t game_delta_ticks, actor_batch& batch, thread::atomic_pool& pool) {
  // game-домен: аккумулируем игровое время слайса (deadlines/flags сравниваются с game_now()),
  // dt секунд для интеграции/drives выводится из той же дельты. Пауза даёт 0, scale масштабирует.
  game_ticks_ += game_delta_ticks;
  const float dt_seconds = float(game_delta_ticks) / float(utils::timeline_ticks_per_second);
  using build_sense_tree_perf = actor_perf_domain::fn_traits<
    &actor_world_slice::build_sense_tree, "sense.tree", "self", "pool">;
  using cognition_perf = actor_perf_domain::fn_traits<&actor_world_slice::cognition, "cognition", "self", "tick", "pool">;
  using apply_perf = actor_perf_domain::fn_traits<&actor_world_slice::apply, "apply", "self", "pool">;
  using maps_perf = actor_perf_domain::fn_traits<
    &actor_world_slice::integrate_and_update_drives, "integration+drives", "self", "dt_seconds", "pool">;
  using resolve_eating_perf = actor_perf_domain::fn_traits<
    &actor_world_slice::resolve_eating, "eating", "self", "game_delta_ticks">;
  using expire_flags_perf = actor_perf_domain::fn_traits<
    &actor_world_slice::expire_flags, "flags", "self", "game_delta_ticks">;
  using maintain_food_perf = actor_perf_domain::fn_traits<&actor_world_slice::maintain_food, "food", "self">;
  using gather_sense_tree_perf = actor_perf_domain::fn_traits<
    &actor_world_slice::gather_sense_tree, "sense.gather", "self">;
  using finalize_sense_tree_perf = actor_perf_domain::fn_traits<
    &actor_world_slice::finalize_sense_tree, "tail.sense+batch", "self", "pool">;

  using build_sense_tree_fn_t = build_sense_tree_perf::loc_fn_t;
  using cognition_fn_t = cognition_perf::loc_fn_t;
  using apply_fn_t = apply_perf::loc_fn_t;
  using maps_fn_t = maps_perf::loc_fn_t;
  using resolve_eating_fn_t = resolve_eating_perf::loc_fn_t;
  using expire_flags_fn_t = expire_flags_perf::loc_fn_t;
  using maintain_food_fn_t = maintain_food_perf::loc_fn_t;
  using gather_sense_tree_fn_t = gather_sense_tree_perf::loc_fn_t;
  using finalize_sense_tree_fn_t = finalize_sense_tree_perf::loc_fn_t;

  tick_ += 1;
  ensure_actor_perf_introspection();
  if (!sense_tree_ready_) {
    build_sense_tree_fn_t{}(*this, pool); // init/load fallback; normal ticks consume tail-built snapshot
    sense_tree_ready_ = true;
  }
  cognition_fn_t{}(*this, tick_, pool);
  apply_fn_t{}(*this, pool);            // collect MT + elect ST, затем FSM/звук
  maps_fn_t{}(*this, dt_seconds, pool); // две независимые map-системы, один общий barrier
  resolve_eating_fn_t{}(*this, game_delta_ticks);
  expire_flags_fn_t{}(*this, game_delta_ticks); // сроки флагов тикают game-дельтой (пауза/scale учтены)
  maintain_food_fn_t{}(*this);

  // Snapshot semantics are unchanged: positions at the end of tick N are positions at the start of
  // tick N+1. Gather after structural changes, then build the next sense tree concurrently with the
  // independent sim->render actor batch. finalize_sense_tree owns the shared pool barrier and waits
  // for both its subtree jobs and this already-submitted batch task.
  gather_sense_tree_fn_t{}(*this);
  pool.submit(
    [](actor_batch* out, const aesthetics::world* world, const uint64_t tick) {
      build_actor_batch_fn_t{}(*out, *world, tick);
    },
    &batch, &world_, tick_);
  finalize_sense_tree_fn_t{}(*this, pool);
  sense_tree_ready_ = true;

  if (tick_ % 100 == 0) {
    dump_actor_perf_stats(); // периодический дамп агрегатов (≈ раз в 5с при 20fps)
  }

  return actor_metrics{
    uint32_t(world_.count<actor_position>()),
    uint32_t(calls_.size()), // = сколько акторов реально думали в этот тик
    batch.count(),
    uint32_t(world_.count<actor_eating>()),
    tick_};
}

// build_sense_tree — init/load fallback для kD-дерева над ВСЕМИ таргетируемыми акторами.
// Обычный тик использует раздельные gather/finalize в хвосте предыдущего тика, чтобы параллельный
// O(N log N) median build перекрывался с actor_batch. Запросы делает cognition только для отобранных.
void actor_world_slice::build_sense_tree(thread::atomic_pool& pool) {
  using gather_perf = actor_perf_domain::fn_traits<
    &actor_world_slice::gather_sense_tree, "sense.gather", "self">;
  using build_perf = actor_perf_domain::fn_traits<
    &actor_world_slice::finalize_sense_tree, "sense.build", "self", "pool">;
  using gather_fn_t = gather_perf::loc_fn_t;
  using build_fn_t = build_perf::loc_fn_t;
  gather_fn_t{}(*this);
  build_fn_t{}(*this, pool);
}

void actor_world_slice::gather_sense_tree() {
  using kd = utils::kd_tree<perception_target>;
  sense_tree_.clear();
  // Скан-фаза как ЛЯМБДА-СИСТЕМА (ROADMAP п.11): reduce query<pos,vis> в общий kd-дерево. Однопоточно
  // ⇒ insert в захваченное дерево безопасен; фильтр (схваченные/препятствия) — в теле лямбды. clear/build
  // обрамляют проход (система делает только обход). Порядок query == порядок view (dense) ⇒ дерево то же.
  if (!sense_tree_sys_) {
    sense_tree_sys_ = aesthetics::make_map_system<actor_position, actor_visual>(
      &world_, [this](const auto& t, const size_t /*time*/) {
        const auto id = std::get<0>(t);
        if (world_.get<actor_grabbed>(id) != nullptr || world_.get<obstacle>(id) != nullptr) {
          return; // схваченную добычу / препятствие никто не таргетит
        }
        const auto* pos = std::get<1>(t);
        const auto* vis = std::get<2>(t);
        sense_tree_.insert(kd::point{pos->value.x, pos->value.y}, perception_target{vis->size, id});
      });
  }
  sense_tree_sys_->update(0);
}

void actor_world_slice::finalize_sense_tree(thread::atomic_pool& pool) {
  sense_tree_.build_parallel(pool);
}

// decide_actor — восприятие (kD-запрос) + GOAP для ОДНОГО актора. Пишет в actor_perception
// и actor_cognition этого актора (эксклюзивно — каждого обрабатывает ровно один поток), а
// план/кеш/выход берёт из переданных per-thread scratch. Зовётся из воркеров конкурентно:
// goap_->decide/compute_state и sense_tree_.nearest2 — const (чтение), мир читается + пишутся
// ТОЛЬКО поля этого актора (непересекающаяся память) ⇒ гонок нет.
void actor_world_slice::decide_actor(const aesthetics::entityid_t id, const uint64_t tick,
                                     acumen::execution_scratch& scratch) {
  using kd = utils::kd_tree<perception_target>;
  auto* pos = world_.get<actor_position>(id);
  auto* vis = world_.get<actor_visual>(id);
  auto* per = world_.get<actor_perception>(id);
  auto* brain = world_.get<actor_brain>(id);
  auto* cog = world_.get<actor_cognition>(id);
  if (pos == nullptr || vis == nullptr || per == nullptr || brain == nullptr || cog == nullptr) {
    return;
  }

  // восприятие: ближайший крупнее (угроза) + мельче (добыча) за один обход.
  const float s = vis->size;
  const uint32_t self = uint32_t(aesthetics::get_entityid_index(id));
  const kd::point q{pos->value.x, pos->value.y};
  const auto [threat, prey] = sense_tree_.nearest2(q, perception_radius, [s, self](const perception_target& t) {
    return uint32_t(aesthetics::get_entityid_index(t.id)) != self && t.size > s;
  },
                                                   [s, self](const perception_target& t) {
                                                     return uint32_t(aesthetics::get_entityid_index(t.id)) != self && t.size < s;
                                                   });
  per->has_threat = threat != nullptr;
  per->has_prey = prey != nullptr;
  if (threat != nullptr) {
    per->threat_pos = glm::vec2(threat->pos[0], threat->pos[1]);
  }
  if (prey != nullptr) {
    per->prey_pos = glm::vec2(prey->pos[0], prey->pos[1]);
    per->prey_id = prey->payload.id;
  }

  // GOAP: dry-run ctx → decide (hit кеша ⇒ без A*) → первое действие плана → deferred effect.
  const auto& goal_state = goap_->get_goals().front().goal;
  const uint64_t goal_id = utils::string_hash("resolved");
  const auto ctx = make_ctx(world_, id, brain->seed, tick, nullptr, &scratch.act);
  const acumen::state start = goap_->compute_state(ctx);
  std::array<const acumen::action*, 4> plan{};
  acumen::decide_params dp;
  dp.start = start;
  dp.goal = goal_state;
  dp.goal_id = goal_id;
  dp.scratch = &scratch.planner;
  dp.cache = &scratch.cache;
  const size_t n = goap_->decide(dp, plan);
  cog->last_think = game_ticks_; // решение принято (game-время; при пустом плане переобдумаем по расписанию)
  if (n == 0 || plan[0] == nullptr) {
    return;
  }

  const auto fn = utils::string_hash(plan[0]->name);
  catalogue::call_record rec;
  rec.fn = fn;
  rec.primary = uint32_t(id);
  rec.target = uint32_t(aesthetics::invalid_entityid);

  const auto* effect = registry_.effect(fn);
  if (effect == nullptr) {
    return; // невалидное GOAP-действие: нечего писать в deferred pipeline
  }

  // Проект знает, откуда взять target; стратегия знает, как его арбитрить. Дескриптор act
  // только заполняет scope; fn_deferred_ptr ниже запишет typed args в нужный executor.
  act::exec_context effect_ctx = ctx;
  bool has_required_scope = true;
  if (const auto* desc = registry_.interaction_of(fn); desc != nullptr) {
    has_required_scope = false;
    if (desc->target_scope >= act::exec_context::max_scope) {
      utils::error{}("actor interaction target_scope {} is outside act scope", desc->target_scope);
    }
    if (per->has_prey) {
      const auto prey = per->prey_id;
      if (!aesthetics::is_invalid_entityid(prey) && world_.exists(prey)) {
        rec.target = uint32_t(prey);
        effect_ctx.scope[desc->target_scope] = act::entity_id{uint32_t(prey)};
        effect_ctx.scope_count = std::max(effect_ctx.scope_count, uint32_t(desc->target_scope) + 1u);
        has_required_scope = true;
      }
    }
  }

  if (has_required_scope) {
    catalogue::mt::record_scope source(uint64_t(uint32_t(id)), aesthetics::get_entityid_index(id));
    effect->invoke(effect_ctx); // MT-фаза: обёртка только пишет в предаллоцированный буфер
  }

  // Отдельный однослотовый journal нужен только для FSM/звука после commit.
  calls_.record(aesthetics::get_entityid_index(id), rec);
}

// cognition — SELECT (однопоточно) + THINK (MT map по отобранным).
//   1) select: «созревшие» (matured) не-едящие/не-схваченные акторы → кандидаты с приоритетом
//      overdue (давность решения). budget_clamp усекает до think_budget_ самых просроченных
//      (детерминированный тай-брейк по индексу). Общий примитив aesthetics::budget_clamp.
//   2) think: cognition_system (worklist_system) раскидывает отобранных по потокам, каждый — в свою
//      scratch-полосу (A*+cache+ds VM), зовёт decide_actor → тот append'ит effects в dense journal.
//      Глобальный sequence id выводится из source_index + локального script ordinal; seal восстанавливает
//      total order независимо от scheduling.
// Стоимость восприятия+GOAP ∝ бюджету/числу_потоков. Отбор — O(N)-скан (на миллионах заменить
// на timing-wheel). sense_tree_ подготовлено хвостом предыдущего тика и только читается воркерами.
void actor_world_slice::cognition(const uint64_t tick, thread::atomic_pool& pool) {
  if (!cognition_sys_) {
    cognition_sys_ = std::make_unique<cognition_system>(&pool, this); // ленивое (пул известен тут)
  }
  // Всю память record-фазы готовим на главном потоке. calls_ остаётся индексным action journal;
  // typed collect/elect calls append-ятся в dense journal с ёмкостью от фактического think-budget.
  const size_t cap = world_.index_capacity();
  calls_.reset(cap);
  if (!deferred_) {
    deferred_ = std::make_unique<deferred_effects>();
  }

  // select: созревшие (skip едящих/схваченных — «коммит длительностью действия») + давность. Reduce
  // view<actor_cognition> в due_ ЛЯМБДА-СИСТЕМОЙ (ROADMAP п.11): однопоточно, лямбда захватывает this и
  // пишет в захваченный буфер due_ (порядок query == view ⇒ детерминированный отбор). time = tick.
  due_.clear();
  if (!select_sys_) {
    select_sys_ = aesthetics::make_map_system<actor_cognition>(
      &world_, [this](const auto& t, const size_t time) {
        const auto id = std::get<0>(t);
        if (world_.get<actor_eating>(id) != nullptr || world_.get<actor_grabbed>(id) != nullptr) {
          return; // едящих/схваченных не переобдумываем (держатся действия)
        }
        const auto* cog = std::get<1>(t);
        static_cast<void>(time);
        // Каденция в game-домене: overdue = сколько игрового времени актор ждёт решения.
        if (matured(cog->last_think, game_ticks_)) {
          due_.push_back(think_candidate{game_ticks_ - cog->last_think, id});
        }
      });
  }
  select_sys_->update(tick);

  // budget-clamp: дольше ждавшие впереди, тай-брейк по индексу сущности (детерминированно).
  aesthetics::budget_clamp(due_, think_budget_, [](const think_candidate& a, const think_candidate& b) {
    if (a.overdue != b.overdue) {
      return a.overdue > b.overdue;
    }
    return aesthetics::get_entityid_index(a.entity) < aesthetics::get_entityid_index(b.entity);
  });

  if (due_.size() > std::numeric_limits<size_t>::max() / max_deferred_effects_per_actor) {
    utils::error{}("actor deferred call budget overflow: {} sources * {} effects", due_.size(),
                   max_deferred_effects_per_actor);
  }
  const size_t deferred_call_capacity = due_.size() * max_deferred_effects_per_actor;
  deferred_->begin_record(cap, deferred_call_capacity);

  // think: work-list = отобранные; run раскидывает по потокам, decide_actor вызывает act::effect,
  // а fn_deferred_ptr append-ит inline payload + metadata, не мутируя world.
  auto& wl = cognition_sys_->worklist();
  wl.clear();
  wl.reserve(due_.size());
  for (const auto& c : due_) {
    wl.push_back(c.entity);
  }
  cognition_sys_->run(tick);
  DE_TRACE(catalogue::log_domain::gameplay, "cognition tick={} calls={} budget={}", tick, calls_.size(), think_budget_);
}

// apply — три барьерных шага: (1) collect-группы по self параллельно, (2) structural elect
// по target однопоточно, (3) action journal по source-index для FSM/звука. Внутри группы вызовы всегда
// упорядочены (source, local_sequence); elect берёт первый. Все эффекты читают позиции ДО integrate.
void actor_world_slice::apply(thread::atomic_pool& pool) {
  sound_emits_.clear(); // sim-звуки этого тика собираем заново
  if (!deferred_) {
    utils::error{}("actor deferred executors are missing before apply");
  }

  deferred_->local.seal();
  pool.distribute1(deferred_->local.group_count(), [this](const size_t start, const size_t count) {
    for (size_t i = start; i < start + count; ++i) {
      deferred_->local.dispatch_group(i);
    }
  });
  pool.compute();
  pool.wait();
  deferred_->local.finish_commit();

  // create/remove component остаются в явном ST-шаге. elect и target_not_source здесь уже sealed.
  deferred_->eat.seal();
  deferred_->eat.commit();

  // флаги — тоже ST structural lane (create<flag_set> по требованию); порядок после eat стабилен.
  deferred_->flags.seal();
  deferred_->flags.commit();

  calls_.dispatch([this](const catalogue::call_record& rec) {
    const auto id = aesthetics::entityid_t(rec.primary);
    // Схваченного В ЭТОМ ЖЕ commit гасим: eat уже заморозил его и поставил state="eaten".
    if (world_.get<actor_grabbed>(id) != nullptr) {
      return;
    }
    const auto* brain = world_.get<actor_brain>(id);
    const uint64_t seed = brain != nullptr ? brain->seed : 0u;
    // cognition завершён: caller-полосу 0 можно переиспользовать для серийного FSM-шага.
    const auto ctx = make_ctx(world_, id, seed, tick_, nullptr, &cognition_sys_->lanes().front().act);
    auto* st = world_.get<actor_state>(id);
    if (st == nullptr) {
      return;
    }
    const auto outcome = mood::step(*fsm_, st->state, rec.fn, ctx);
    if (outcome.result == mood::step_result::transitioned &&
        outcome.next_state != utils::invalid_id && outcome.next_state != st->state) {
      st->state = mood::apply_transition(*fsm_, st->state, *outcome.taken, ctx);
      if (const uint64_t snd = sound_for_state(st->state); snd != 0) {
        if (const auto* p = world_.get<actor_position>(id); p != nullptr) {
          sound_emits_.push_back(sound_emit{snd, p->value});
        }
      }
    }
  });
}

// integration + drives независимы после apply: обе читают velocity, первая пишет position, вторая
// stats. Сначала enqueue чанков обеих систем, затем ОДИН compute/wait в aesthetics::run.
void actor_world_slice::integrate_and_update_drives(const float dt_seconds, thread::atomic_pool& pool) {
  if (!integration_sys_) {
    integration_sys_ = std::make_unique<integration_system>(&world_);
  }
  if (!drives_sys_) {
    drives_sys_ = std::make_unique<drives_system>(&world_);
  }

  integration_sys_->dt = dt_seconds;
  integration_sys_->obstacles = &obstacles_;
  drives_sys_->dt = dt_seconds;
  aesthetics::run(pool, tick_, *integration_sys_, *drives_sys_);
}

// resolve_eating — завершить поедание у хищников с истёкшим сроком: наелся (голод в ноль),
// снять actor_eating (хищник снова «созреет» и переобдумает), удалить съеденную жертву. СКАН — лямбда-
// система (reduce view<actor_eating> в eat_finished_/eat_kill_; мутация СВОЕГО stats disjoint). Структурное
// удаление компонентов/сущностей — ПОСЛЕ прохода в обёртке (нельзя мутировать пул при обходе), однопоточно.
void actor_world_slice::resolve_eating(const uint64_t game_delta_ticks) {
  eat_finished_.clear();
  eat_kill_.clear();
  if (!resolve_eating_sys_) {
    resolve_eating_sys_ = aesthetics::make_map_system<actor_eating>(
      &world_, [this](const auto& t, const size_t time) {
        const auto id = std::get<0>(t);
        auto* eat = std::get<1>(t);
        // time = game-дельта тика: остаток тает игровым временем (пауза/скорость учтены).
        if (eat->remaining.ticks > time) {
          eat->remaining.ticks -= time; // self-мутация disjoint
          return;
        }
        eat->remaining.ticks = 0;
        if (auto* dr = world_.get<stats>(id); dr != nullptr) {
          dr->hunger = 0.0f; // наелся
        }
        eat_finished_.push_back(id);
        eat_kill_.push_back(eat->target);
      });
  }
  resolve_eating_sys_->update(game_delta_ticks);
  for (const auto pid : eat_finished_) {
    // Сытость — живая демонстрация флагов: пока флаг жив, голод не растёт (см. drives_system).
    // Ставится ЗДЕСЬ (post-commit факт доедания), а не в eat-скрипте: ветки составного скрипта
    // записываются независимо, и проигравший elect не должен получить sated.
    auto* flags = world_.get<flag_set>(pid);
    if (flags == nullptr) {
      flags = world_.create<flag_set>(pid);
    }
    flags->set(sated_flag_id(), utils::game_duration::from_seconds(sated_seconds));
    world_.remove<actor_eating>(pid);
  }
  for (const auto bid : eat_kill_) {
    if (world_.exists(bid)) {
      world_.remove_entity(bid);
    }
  }
}

// Sweep флагов: вычесть прошедшую game-дельту у всех flag_set (self-мутация, порядок обхода
// стабилен). Компонентов с флагами мало и записи крошечные — обычный ST-проход без пула.
void actor_world_slice::expire_flags(const uint64_t game_delta_ticks) {
  const utils::game_duration dt{game_delta_ticks};
  for (auto [id, flags] : world_.view<flag_set>()) {
    static_cast<void>(id);
    flags->advance(dt);
  }
}

// spawn_food — одна еда-сущность в детерминированной точке в пределах bounds. Маленький размер
// (< любого актора) ⇒ для всех «добыча»; своя яркая расцветка; без brain/cognition/velocity/state
// ⇒ статична, не думает, не анимируется, в integration не двигается.
void actor_world_slice::spawn_food() {
  const uint64_t h1 = utils::mix(0xf00du, food_spawn_seq_, tick_, 1u);
  const uint64_t h2 = utils::mix(0xf00du, food_spawn_seq_, tick_, 2u);
  ++food_spawn_seq_;
  const float fx = float(h1 & 0xffffffu) / float(0xffffffu);
  const float fy = float(h2 & 0xffffffu) / float(0xffffffu);
  const glm::vec2 p{spawn_min_.x + (spawn_max_.x - spawn_min_.x) * fx,
                    spawn_min_.y + (spawn_max_.y - spawn_min_.y) * fy};
  // Через префаб: food_item из конфига, позиция/визуал — derived в on_construct (см. setup_brain_registry).
  prefab_.spawn("food", world_, spawn_args{p});
}

// spawn_sink: примитивный спавн префаба по имени в точке (ds-натив spawn_at → сюда). Тот же путь, что
// spawn_food. Спавнеры-энтити/запросы/динамические точки — тех-долг.
aesthetics::entityid_t actor_world_slice::spawn_prefab(const std::string_view name, const glm::vec2 pos) {
  return prefab_.spawn(std::string(name), world_, spawn_args{pos});
}

// maintain_food — допополнить еду до целевого числа. Кап на тик, чтобы не было всплеска при
// массовом выедании. Детерминированно (spawn_food завязан на tick_ + счётчик).
void actor_world_slice::maintain_food() {
  const size_t have = world_.count<food_item>();
  if (have >= food_target_) {
    return;
  }
  size_t need = std::min<size_t>(food_target_ - have, 64); // не больше 64 спавнов/тик
  for (size_t i = 0; i < need; ++i) {
    spawn_food();
  }
}

// ── save/load ──────────────────────────────────────────────────────────────
// Сторонняя (не-ECS) структура состояния слайса: реплицируемые скаляры (tick/seq → детерминизм
// RNG и респавна) + конфиг (bounds/target/knobs), нужные для идентичного resume. Плоский агрегат
// ⇒ сериализуется тем же ядром serialize<T>; glm::vec2 — через adapter (см. actor_snapshot.h).
namespace {
struct sim_globals {
  uint64_t tick;
  uint64_t game_ticks; // накопленное игровое время (µs) — resume продолжает те же deadlines
  uint64_t food_spawn_seq;
  glm::vec2 spawn_min;
  glm::vec2 spawn_max;
  uint32_t food_target;
  uint32_t texture_count;
  uint32_t commit_game_ticks; // µs game-времени (окно коммита решения)
  uint32_t think_budget;
};
} // namespace

void actor_world_slice::rebuild_obstacle_cache() {
  obstacles_.clear();
  for (auto [id, obs, pos] : world_.view<obstacle, actor_position>()) {
    static_cast<void>(id);
    obstacles_.push_back(obstacle_disc{pos->value, obs->radius});
  }
}

std::vector<uint8_t> actor_world_slice::save(const aesthetics::serial::sink_policy& policy) const {
  namespace serial = aesthetics::serial;
  // payload = dump_world (компоненты) + свой дампер скаляров. Пред-resize по оценке мира + запас
  // на sim_globals ⇒ запись линейным memcpy, ensure() почти не срабатывает; усекаем по pos().
  std::vector<std::byte> raw;
  raw.resize(serial::estimate_size(&world_) + 128);
  serial::writer wr{raw};
  serial::dump_world(&world_, wr);
  const sim_globals g{tick_, game_ticks_, food_spawn_seq_, spawn_min_, spawn_max_,
                      food_target_, texture_count_,
                      uint32_t(commit_game_ticks_), uint32_t(think_budget_)};
  serial::serialize(wr, g);
  raw.resize(wr.pos());
  return serial::seal(raw, policy); // header + checksum + компрессия (+ скриншот — тут не задаём)
}

bool actor_world_slice::load(const std::span<const uint8_t> packet, const brain_config& brains) {
  namespace serial = aesthetics::serial;
  // чистый слайс: пересобираем реестр функций + GOAP/FSM (как init), но БЕЗ спавна сущностей.
  world_ = aesthetics::world{};
  // Кэш-системы держат query на старом world — сбросить (пересоздадутся против загруженного мира).
  integration_sys_.reset();
  drives_sys_.reset();
  sense_tree_sys_.reset();
  sense_tree_ready_ = false;
  select_sys_.reset();
  resolve_eating_sys_.reset();
  deferred_.reset();
  brains_ = brains;
  setup_brain_registry();
  calls_.clear();
  // как в init: аллокаторы пулов поедания заранее на главном потоке (view<> не кинет; type-id
  // фиксируется до MT). load_world и так тронет все зарегистрированные типы, но порядок важен.
  static_cast<void>(world_.get_or_create_allocator<actor_eating>(sizeof(actor_eating) * 250));
  static_cast<void>(world_.get_or_create_allocator<flag_set>(sizeof(flag_set) * 250));
  static_cast<void>(world_.get_or_create_allocator<actor_grabbed>(sizeof(actor_grabbed) * 250));

  std::vector<std::byte> raw;
  if (!serial::unseal(packet, raw)) {
    return false; // битый контейнер/checksum
  }
  serial::reader r{raw};
  if (!serial::load_world(&world_, r)) {
    return false; // magic/схема/обрыв → false
  }
  sim_globals g{};
  serial::deserialize(r, g);
  if (!r.ok) {
    utils::warn("slice load: truncated sim_globals");
    return false;
  }

  tick_ = g.tick;
  game_ticks_ = g.game_ticks;
  food_spawn_seq_ = g.food_spawn_seq;
  spawn_min_ = g.spawn_min;
  spawn_max_ = g.spawn_max;
  food_target_ = g.food_target;
  texture_count_ = g.texture_count;
  commit_game_ticks_ = g.commit_game_ticks;
  think_budget_ = g.think_budget;

  rebuild_obstacle_cache(); // кэш выводим из мира, не хранится в снапшоте
  return true;
}

} // namespace core
} // namespace tile_frontier
