#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#include <devils_engine/act/exec_context.h>
#include <devils_engine/act/function.h>
#include <devils_engine/acumen/astar.h> // astar<>::container для find_solution
#include <devils_engine/aesthetics/common.h>
#include <devils_engine/catalogue/introspection.h>
#include <devils_engine/catalogue/logging.h>  // DE_LOG — perf-дамп в домен gameplay
#include <devils_engine/mood/runtime.h>       // mood::step / apply_transition — шаг FSM
#include <devils_engine/thread/atomic_pool.h> // MT-пул (distribute/thread_index/wait)
#include <devils_engine/utils/core.h>         // utils::error
#include <devils_engine/utils/prng.h>         // utils::mix
#include <devils_engine/utils/string_id.h>    // utils::string_hash

#include "actor_simulation.h"

// glm-адаптеры сериализации + регистрация ВСЕХ компонентов в реестр serial (SERIALIZABLE_COMPONENT).
// Включён здесь, а не только в тестах, чтобы игровой бинарь сам умел save/load (регистрации линкуются
// в этот TU) и чтобы сторонний дампер скаляров (sim_globals с glm::vec2) видел adapter<glm::vec2>.
#include "core/actor_snapshot.h"
#include "entity_scope.h"  // seed_entity_scope — засев root-скоупа скрипт-предикатов
#include "goap_resource.h" // goap_config — GOAP-описание из tavl

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// ── Мост act::exec_context ↔ aesthetics::world ─────────────────────────────
// act намеренно ECS-агностичен: exec_context.w — НЕПРОЗРАЧНЫЙ тег (act::world
// нигде не определён). Шов здесь, где видны оба мира: храним сырой указатель на
// настоящий aesthetics::world и кастуем обратно одним хелпером. Горячий путь —
// каст указателя, без виртуалок; act не зависит от aesthetics.
static const aesthetics::world& world_of(const act::exec_context& ctx) noexcept {
  return *reinterpret_cast<const aesthetics::world*>(ctx.w);
}

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

// Мутирующий вариант моста для apply-фазы. effect_sink ещё не подключён (приедет с
// catalogue), поэтому на MVP эффект мутирует мир НАПРЯМУЮ. const_cast здесь определён:
// объект (actor_world_slice::world_) реально мутабелен, const на ctx.w — лишь контракт
// exec_context (мир «только чтение» для предикатов и dry-run-планирования).
static aesthetics::world& mutable_world_of(const act::exec_context& ctx) noexcept {
  return *const_cast<aesthetics::world*>(reinterpret_cast<const aesthetics::world*>(ctx.w));
}

// purpose-метка для counter-RNG (utils::mix) — разводит потоки случайности.
static constexpr uint64_t purpose_wander = 0x77616e646572ull; // "wander"
static constexpr uint64_t purpose_seek = 0x7365656bull;       // "seek"

// Биты GOAP-стейта (= индекс метрики; resolved ставят действия, метрики его не считают).
namespace flags {
enum : size_t { threat_present = 0,
                prey_present = 1,
                hungry = 2,
                bored = 3,
                prey_in_range = 4,
                resolved = 5 };
}

// ── тюнинг мотиваций (drives) ──────────────────────────────────────────────
// Скорости в единицах/секунду; пороги в 0..1. hunger копится всегда; boredom копится пока
// актор стоит (думает), быстро спадает в движении → колебание think⇄wander. Значения
// подобраны на глаз для main_fps≈20 (dt≈0.05). Все крутилки — кандидаты в config позже.
static constexpr float hunger_rate = 0.08f;  // голод/сек (≈12с до сытого→голодного)
static constexpr float bored_rate = 0.14f;   // скука/сек пока стоит (думает)
static constexpr float bored_relief = 0.30f; // спад скуки/сек в движении
static constexpr float hungry_threshold = 0.50f;
static constexpr float bored_threshold = 0.50f;
static constexpr float still_speed2 = 0.01f; // |vel|^2 ниже — считаем «стоит»
static constexpr float eat_radius = 0.9f;    // дистанция хвата добычи (≈ размер треуг. + запас)
static constexpr uint64_t eat_duration = 18; // тиков длится поедание (≈0.9с при 20fps) = окно коммита

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

static bool predicate_threat_present(const act::exec_context& ctx) noexcept {
  const auto* per = world_of(ctx).get<actor_perception>(aesthetics::entityid_t(ctx.primary().id));
  return per != nullptr && per->has_threat;
}

static bool predicate_prey_present(const act::exec_context& ctx) noexcept {
  const auto* per = world_of(ctx).get<actor_perception>(aesthetics::entityid_t(ctx.primary().id));
  return per != nullptr && per->has_prey;
}

static bool predicate_is_hungry(const act::exec_context& ctx) noexcept {
  const auto* dr = world_of(ctx).get<stats>(aesthetics::entityid_t(ctx.primary().id));
  return dr != nullptr && dr->hunger >= hungry_threshold;
}

static bool predicate_is_bored(const act::exec_context& ctx) noexcept {
  const auto* dr = world_of(ctx).get<stats>(aesthetics::entityid_t(ctx.primary().id));
  return dr != nullptr && dr->boredom >= bored_threshold;
}

// Добыча есть И в радиусе хвата — разделяет chase (далеко) и eat (вплотную).
static bool predicate_prey_in_range(const act::exec_context& ctx) noexcept {
  const auto self = aesthetics::entityid_t(ctx.primary().id);
  const auto& w = world_of(ctx);
  const auto* per = w.get<actor_perception>(self);
  const auto* pos = w.get<actor_position>(self);
  if (per == nullptr || pos == nullptr || !per->has_prey) {
    return false;
  }
  const glm::vec2 d = per->prey_pos - pos->value;
  return (d.x * d.x + d.y * d.y) <= eat_radius * eat_radius;
}

// Актор уже ест (есть actor_eating) — guard для перехода FSM в eating: транзит только если
// хват реально удался (effect_eat выставил компонент). Иначе остаёмся в прежнем состоянии.
static bool predicate_is_eating(const act::exec_context& ctx) noexcept {
  return world_of(ctx).get<actor_eating>(aesthetics::entityid_t(ctx.primary().id)) != nullptr;
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

static void effect_flee(const act::exec_context& ctx) noexcept {
  auto& world = mutable_world_of(ctx);
  const auto self = aesthetics::entityid_t(ctx.primary().id);
  const auto* pos = world.get<actor_position>(self);
  const auto* per = world.get<actor_perception>(self);
  if (pos == nullptr || per == nullptr) {
    return;
  }
  set_velocity(world, self, pos->value - per->threat_pos); // прочь от угрозы
}

static void effect_chase(const act::exec_context& ctx) noexcept {
  auto& world = mutable_world_of(ctx);
  const auto self = aesthetics::entityid_t(ctx.primary().id);
  const auto* pos = world.get<actor_position>(self);
  const auto* per = world.get<actor_perception>(self);
  if (pos == nullptr || per == nullptr) {
    return;
  }
  set_velocity(world, self, per->prey_pos - pos->value); // к добыче
}

// Поедание: схватить добычу. Бежит в apply (id-порядок) ⇒ при конкуренции за одну жертву
// хищник с МЕНЬШИМ id хватает первым, остальные видят actor_grabbed/actor_eating и пасуют
// (детерминированно). Успех помечается actor_eating на себе — guard actor.is_eating пускает
// FSM в состояние eating; провал ⇒ guard ложен ⇒ остаёмся в chase. Жертва замораживается и
// получает actor_grabbed (её собственный интент в этом же apply пропустится — см. apply()).
static void effect_eat(const act::exec_context& ctx) noexcept {
  auto& world = mutable_world_of(ctx);
  const auto self = aesthetics::entityid_t(ctx.primary().id);
  if (world.get<actor_eating>(self) != nullptr) {
    return; // уже ем (защитно)
  }
  const auto* per = world.get<actor_perception>(self);
  const auto* pos = world.get<actor_position>(self);
  if (per == nullptr || pos == nullptr || !per->has_prey) {
    return;
  }

  const auto prey = per->prey_id;
  if (aesthetics::is_invalid_entityid(prey) || !world.exists(prey)) {
    return; // жертва исчезла
  }
  if (world.get<actor_grabbed>(prey) != nullptr) {
    return; // уже схвачена (id-порядок)
  }
  if (world.get<actor_eating>(prey) != nullptr) {
    return; // сама ест — не трогаем
  }
  const auto* ppos = world.get<actor_position>(prey);
  if (ppos == nullptr) {
    return;
  }
  const glm::vec2 d = ppos->value - pos->value;
  if ((d.x * d.x + d.y * d.y) > eat_radius * eat_radius) {
    return; // вышла из радиуса
  }

  // хват: оба замирают, помечаем связь, жертва визуально «съедаемая».
  world.create<actor_eating>(self, prey, ctx.rng_tick + eat_duration);
  world.create<actor_grabbed>(prey, self);
  set_velocity(world, self, glm::vec2{0.0f, 0.0f});
  set_velocity(world, prey, glm::vec2{0.0f, 0.0f});
  if (auto* bs = world.get<actor_state>(prey); bs != nullptr) {
    bs->state = utils::string_hash("eaten");
  }
}

// Сидит на месте и «думает» — скорость в ноль. Накопление скуки — пассивно в apply
// (актор стоит ⇒ boredom растёт), так что эффект просто гасит движение.
static void effect_think(const act::exec_context& ctx) noexcept {
  set_velocity(mutable_world_of(ctx), aesthetics::entityid_t(ctx.primary().id), glm::vec2{0.0f, 0.0f});
}

// Ищет еду — то же случайное блуждание, что и wander, но со своим потоком RNG (provenance/
// звук позже могут отличаться). Голоден, но добычи в поле зрения нет ⇒ обшариваем местность.
static void effect_seek_food(const act::exec_context& ctx) noexcept {
  auto& world = mutable_world_of(ctx);
  const auto self = aesthetics::entityid_t(ctx.primary().id);
  const auto* brain = world.get<actor_brain>(self);
  const uint32_t phase = brain != nullptr ? brain->phase : 0u;
  const uint64_t slot = (ctx.rng_tick + phase) / 12u; // меняем курс чаще, чем при wander
  const uint64_t h = utils::mix(ctx.rng_seed, ctx.rng_entity, slot, purpose_seek);
  set_velocity(world, self, direction_from_hash(uint32_t(h)));
}

static void effect_wander(const act::exec_context& ctx) noexcept {
  auto& world = mutable_world_of(ctx);
  const auto self = aesthetics::entityid_t(ctx.primary().id);
  const auto* brain = world.get<actor_brain>(self);
  const uint32_t phase = brain != nullptr ? brain->phase : 0u;
  // медленная смена направления (~раз в 24 тика) с пер-акторной фазой.
  const uint64_t slot = (ctx.rng_tick + phase) / 24u;
  const uint64_t h = utils::mix(ctx.rng_seed, ctx.rng_entity, slot, purpose_wander);
  set_velocity(world, self, direction_from_hash(uint32_t(h)));
}

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
  // пересоздаём реестр с нуля: reg() ассертит на повторную регистрацию имени,
  // а init() может вызываться многократно.
  registry_ = act::registry{};
  goap_registry_ = acumen::registry{};
  fsm_registry_ = mood::registry{};
  goap_ = nullptr;
  fsm_ = nullptr;
  registry_.reg("actor.threat_present", std::make_unique<act::native_function<bool>>(
                                          &predicate_threat_present, "рядом есть актор крупнее"));
  registry_.reg("actor.prey_present", std::make_unique<act::native_function<bool>>(
                                        &predicate_prey_present, "рядом есть актор мельче"));
  // is_hungry: скрипт-предикат из tavl (hunger >= 0.5), если загружен; иначе нативный фолбэк
  // (тесты/резюме без ассетов). Скрипт исполняется на per-worker execution_scratch; засев root-скоупа —
  // seed_entity_scope. Поведение идентично нативному (тот же порог, та же семантика отсутствия drives).
  if (brains_.is_hungry_program != nullptr) {
    registry_.reg("actor.is_hungry", std::make_unique<act::script_function<bool>>(
                                       brains_.is_hungry_program, &seed_entity_scope));
  } else {
    registry_.reg("actor.is_hungry", std::make_unique<act::native_function<bool>>(
                                       &predicate_is_hungry, "голод выше порога"));
  }
  registry_.reg("actor.is_bored", std::make_unique<act::native_function<bool>>(
                                    &predicate_is_bored, "скука выше порога"));
  registry_.reg("actor.prey_in_range", std::make_unique<act::native_function<bool>>(
                                         &predicate_prey_in_range, "добыча в радиусе хвата"));
  // ВАЖНО: на это имя ссылается СТРОКА FSM (mood) как гвард — парсер mood не допускает точку
  // в идентификаторах, поэтому имя dot-free (в отличие от acumen-метрик "actor.*", которые в
  // парсер mood не попадают и резолвятся по полному хешу строки).
  registry_.reg("is_eating", std::make_unique<act::native_function<bool>>(
                               &predicate_is_eating, "актор уже ест (хват удался)"));
  registry_.reg("flee", std::make_unique<act::native_function<void>>(&effect_flee, "бежать от ближайшей угрозы"));
  registry_.reg("chase", std::make_unique<act::native_function<void>>(&effect_chase, "гнаться за добычей"));
  registry_.reg("eat", std::make_unique<act::native_function<void>>(&effect_eat, "схватить добычу и начать есть"));
  registry_.reg("seek_food", std::make_unique<act::native_function<void>>(&effect_seek_food, "искать еду — рыскать пока голоден"));
  registry_.reg("wander", std::make_unique<act::native_function<void>>(&effect_wander, "блуждать (от скуки)"));
  registry_.reg("think", std::make_unique<act::native_function<void>>(&effect_think, "стоять и думать (копит скуку)"));

  // GOAP-система: из tavl-конфига (goap/actor) если загружен, иначе хардкод-фолбэк (тесты/резюме).
  if (brains_.goap != nullptr) {
    build_goap_from_config(*brains_.goap);
  } else {
    // метрики (бит = индекс): 0 threat, 1 prey, 2 hungry, 3 bored, 4 prey_in_range. resolved(5) — действия.
    std::vector<acumen::state_metric> metrics = {
      acumen::state_metric("actor.threat_present"),
      acumen::state_metric("actor.prey_present"),
      acumen::state_metric("actor.is_hungry"),
      acumen::state_metric("actor.is_bored"),
      acumen::state_metric("actor.prey_in_range"),
    };

    // Приоритетная лестница (угроза доминирует; дальше чистое разбиение по hungry → prey →
    // in_range / bored ⇒ ровно одно действие на состояние, план в 1 шаг). Каждое ставит resolved.
    acumen::scoped_state flee_req;
    flee_req.set(flags::threat_present, true);
    acumen::scoped_state eat_req;
    eat_req.set(flags::threat_present, false);
    eat_req.set(flags::hungry, true);
    eat_req.set(flags::prey_present, true);
    eat_req.set(flags::prey_in_range, true);
    acumen::scoped_state chase_req;
    chase_req.set(flags::threat_present, false);
    chase_req.set(flags::hungry, true);
    chase_req.set(flags::prey_present, true);
    chase_req.set(flags::prey_in_range, false);
    acumen::scoped_state seek_req;
    seek_req.set(flags::threat_present, false);
    seek_req.set(flags::hungry, true);
    seek_req.set(flags::prey_present, false);
    acumen::scoped_state wander_req;
    wander_req.set(flags::threat_present, false);
    wander_req.set(flags::hungry, false);
    wander_req.set(flags::bored, true);
    acumen::scoped_state think_req;
    think_req.set(flags::threat_present, false);
    think_req.set(flags::hungry, false);
    think_req.set(flags::bored, false);
    acumen::scoped_state done;
    done.set(flags::resolved, true);

    std::vector<acumen::action> actions = {
      acumen::action("flee", flee_req, done, acumen::scoped_state{}),
      acumen::action("eat", eat_req, done, acumen::scoped_state{}),
      acumen::action("chase", chase_req, done, acumen::scoped_state{}),
      acumen::action("seek_food", seek_req, done, acumen::scoped_state{}),
      acumen::action("wander", wander_req, done, acumen::scoped_state{}),
      acumen::action("think", think_req, done, acumen::scoped_state{}),
    };

    acumen::scoped_state goal_state;
    goal_state.set(flags::resolved, true);
    std::vector<acumen::goal> goals = {acumen::goal{"resolved", acumen::scoped_state{}, goal_state}};

    // конструктор резолвит предикаты/эффекты по именам и кидает при промахе.
    goap_registry_.add("actor", acumen::system(&registry_, std::move(metrics), std::move(goals), std::move(actions)));
    goap_ = goap_registry_.get("actor");
  }

  // FSM-исполнитель (mood): событие = выбранное GOAP действие, ведёт в одноимённое состояние из
  // ЛЮБОГО (any_state — wildcard). Движение остаётся эффектом GOAP в apply; FSM держит состояние
  // (выбор анимации в рендере) и даёт точку для on_entry-эффектов: звук — фаза D, поедание (guard
  // «добыча в радиусе» → состояние eating с длительностью) — фаза C. Пока чистые рёбра без эффектов.
  // Переходы FSM из tavl-конфига (fsm/actor), если загружены; иначе хардкод-фолбэк (тесты/резюме без
  // ассетов). Гварды/действия в строках — имена функций из registry_ (нативки/скрипты), резолвит mood.
  if (brains_.fsm_transitions != nullptr) {
    fsm_registry_.add("actor", mood::system(&registry_, *brains_.fsm_transitions));
  } else {
    std::vector<std::string> fsm_lines = {
      "any_state + flee = flee",
      "any_state + eat [is_eating] = eating", // только если хват реально удался (guard; имя dot-free для парсера mood)
      "any_state + chase = chase",
      "any_state + seek_food = seek_food",
      "any_state + wander = wander",
      "any_state + think = think",
    };
    fsm_registry_.add("actor", mood::system(&registry_, std::move(fsm_lines)));
  }
  fsm_ = fsm_registry_.get("actor");

  // Префабы слайса (перестраиваются вместе с реестром — общая точка init/load). C++-специи компонентов
  // (какие типы бывают + per-prefab on_construct для DERIVED) регистрируются ЗДЕСЬ; тексты префабов
  // приходят из конфига (prefab/*.tavl через brain_config.prefabs), иначе хардкод-фолбэк (тесты/резюме).
  //   food:  data food_item из конфига; визуал (зелёный, food_size) + позиция — derived в construct.
  //   actor: data actor_tuning (разброс скорости/голода/силы) из конфига; brain/visual/stats/… — derived
  //          в construct из per-instance зерна (spawn_args). GOAP/FSM остаются slice-level (не per-entity).
  prefab_ = devils_engine::prefab::prefab_registry<spawn_args>{};
  prefab_.data<food_item>("food_item");
  prefab_.data<actor_tuning>("actor_tuning");
  prefab_.on_construct("food", [](aesthetics::entityid_t id, aesthetics::world& w, const spawn_args& a) {
    w.create<actor_position>(id, a.pos);
    w.create<actor_visual>(id, 0u, food_color(), food_size);
  });
  prefab_.on_construct("actor", [](aesthetics::entityid_t id, aesthetics::world& w, const spawn_args& a) {
    // DERIVED per-instance из зерна + тюнинга (формулы = историческому init-циклу, побайтовый паритет).
    const auto* tp = w.get<actor_tuning>(id);
    const actor_tuning t = tp != nullptr ? *tp : actor_tuning{};
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
  if (brains_.prefabs != nullptr && !brains_.prefabs->empty()) {
    for (const auto& d : *brains_.prefabs) {
      prefab_.add_prefab(d.name, d.text, prefab_lc);
    }
  } else {
    prefab_.add_prefab("food", "food_item = { nutrition = 1.0 }\n", prefab_lc);
    prefab_.add_prefab("actor",
                       "actor_tuning = { speed_base = 0.65, speed_var = 1.35, hunger_scale = 0.4, strength_mod = 11 }\n",
                       prefab_lc);
  }
}

void actor_world_slice::build_goap_from_config(const goap_config& cfg) {
  // Метрики ОПРЕДЕЛЯЮТ свои предикаты инлайн-скриптами: регистрируем каждый как script_function<bool>
  // под ключом метрики в registry_ (перед сборкой acumen — он резолвит метрики по имени). Скрипт
  // исполняется на per-worker execution_scratch; засев root-скоупа — seed_entity_scope. Контейнеры живут в
  // goap_config (в реестре ассетов), поэтому заимствование &m.program валидно на всё время слайса.
  for (const auto& m : cfg.metrics) {
    registry_.reg(m.key, std::make_unique<act::script_function<bool>>(&m.program, &seed_entity_scope));
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

void actor_world_slice::init(const uint32_t count, const glm::vec2 min_bound, const glm::vec2 max_bound, const uint32_t texture_count,
                             const brain_config& brains) {
  world_ = aesthetics::world{};
  brains_ = brains; // до setup_brain_registry: выбирает скрипт/конфиг vs натив/хардкод
  setup_brain_registry();
  intents_.reset(count); // предварительная ёмкость по числу актороов; растёт в cognition до index_capacity
  tick_ = 0;

  // СОЗДАЁМ аллокаторы пулов поедания заранее на ГЛАВНОМ потоке. Две причины: (1) view<>
  // (в resolve_eating) кидает, если аллокатора нет, а компоненты появляются лишь при первом
  // хвате; (2) component_type_id — sequential_type_id (присваивается при первом обращении),
  // фиксируем его до любого MT-доступа. block_size как в world::create (sizeof(T)*250).
  static_cast<void>(world_.get_or_create_allocator<actor_eating>(sizeof(actor_eating) * 250));
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

  // Спавн акторов через префаб «actor» (prefab/actor.tavl или фолбэк): data-компонент actor_tuning из
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

actor_metrics actor_world_slice::update(const float dt_seconds, actor_batch& batch, thread::atomic_pool& pool) {
  using build_sense_tree_perf = actor_perf_domain::fn_traits<&actor_world_slice::build_sense_tree, "sense.tree", "self">;
  using cognition_perf = actor_perf_domain::fn_traits<&actor_world_slice::cognition, "cognition", "self", "tick", "pool">;
  using apply_perf = actor_perf_domain::fn_traits<&actor_world_slice::apply, "apply", "self", "dt_seconds">;
  using resolve_eating_perf = actor_perf_domain::fn_traits<&actor_world_slice::resolve_eating, "eating", "self", "tick">;
  using maintain_food_perf = actor_perf_domain::fn_traits<&actor_world_slice::maintain_food, "food", "self">;

  using build_sense_tree_fn_t = build_sense_tree_perf::loc_fn_t;
  using cognition_fn_t = cognition_perf::loc_fn_t;
  using apply_fn_t = apply_perf::loc_fn_t;
  using resolve_eating_fn_t = resolve_eating_perf::loc_fn_t;
  using maintain_food_fn_t = maintain_food_perf::loc_fn_t;

  tick_ += 1;
  ensure_actor_perf_introspection();
  build_sense_tree_fn_t{}(*this);
  cognition_fn_t{}(*this, tick_, pool);
  apply_fn_t{}(*this, dt_seconds);
  resolve_eating_fn_t{}(*this, tick_);
  maintain_food_fn_t{}(*this);
  build_actor_batch_fn_t{}(batch, world_, tick_);

  if (tick_ % 100 == 0) {
    dump_actor_perf_stats(); // периодический дамп агрегатов (≈ раз в 5с при 20fps)
  }

  return actor_metrics{
    uint32_t(world_.count<actor_position>()),
    uint32_t(intents_.size()), // = сколько актороов реально думали в этот тик
    batch.count(),
    tick_};
}

// build_sense_tree — kD-дерево над ВСЕМИ акторами (даже не думающие — потенциальные цели).
// Дёшево (O(N), build ~0.5ms на 4096); запросы по нему делает только cognition для отобранных.
void actor_world_slice::build_sense_tree() {
  using kd = utils::kd_tree<perception_target>;
  sense_tree_.clear();
  for (auto [id, pos, vis] : world_.view<actor_position, actor_visual>()) {
    if (world_.get<actor_grabbed>(id) != nullptr) {
      continue; // схваченную добычу никто не таргетит
    }
    if (world_.get<obstacle>(id) != nullptr) {
      continue; // препятствие — не добыча/не угроза
    }
    sense_tree_.insert(kd::point{pos->value.x, pos->value.y},
                       perception_target{vis->size, id});
  }
  sense_tree_.build();
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

  // GOAP: dry-run ctx → decide (hit кеша ⇒ без A*) → первое действие плана → intent.
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
  cog->last_think = tick; // решение принято (даже если план пуст — переобдумаем по расписанию)
  if (n == 0 || plan[0] == nullptr) {
    return;
  }

  act::intent in;
  in.kind = act::intent_kind::call_function;
  in.actor = act::entity_id{uint32_t(id)};
  in.payload.call.fn = utils::string_hash(plan[0]->name);
  in.source_action = in.payload.call.fn;
  // Слот адресуется индексом ЭТОГО актора (id уникален среди отобранных ⇒ поток пишет свой слот).
  intents_.store(id, in);
}

// cognition — ПЛАНИРОВЩИК + MT-перебор отобранных.
//   1) отбор (ОДНОПОТОЧНО, дёшево): «созревшие» (ещё не думал ИЛИ истёк commit) → приоритет
//      по давности (дольше ждал — раньше; тай-брейк по id), обрезка бюджетом think_budget_.
//   2) MT: due_ раскидывается по потокам пула (distribute), каждый чанк в СВОЙ scratch/cache
//      (слот = pool.thread_index, эксклюзивен на поток) зовёт decide_actor.
//   3) decide_actor пишет intent в СВОЙ слот intents_ (по индексу актора) — записи непересекающиеся,
//      без локов; apply обходит буфер по индексу ⇒ детерминизм без merge и без sort.
// Стоимость восприятия+GOAP ∝ бюджету/числу_потоков. Отбор — O(N)-скан (на миллионах заменить
// на timing-wheel). sense_tree_ строится ДО (build_sense_tree) и только читается воркерами.
void actor_world_slice::cognition(const uint64_t tick, thread::atomic_pool& pool) {
  using request = decltype(scheduler_)::request;
  // Засайзить буфер интентов до ёмкости индексов мира НА ГЛАВНОМ потоке — покрывает всех живых
  // акторов, чтобы параллельный store не реаллоцировал (записи в непересекающиеся слоты безопасны).
  intents_.reset(world_.index_capacity());
  // generic-планировщик (libs/simul) держит budget-clamp, per-thread lanes, distribute/wait. Проект
  // поставляет знание сущности; выход планировщик не собирает — decide пишет intent в intents_:
  scheduler_.run(
    tick, pool,
    // enumerate: созревшие (skip едящих/схваченных — «коммит длительностью действия») + давность.
    [this, tick](std::vector<request>& due) {
      for (auto [id, cog] : world_.view<actor_cognition>()) {
        if (world_.get<actor_eating>(id) != nullptr || world_.get<actor_grabbed>(id) != nullptr) {
          continue;
        }
        if (scheduler_.matured(cog->last_think, tick)) {
          due.push_back(request{tick - cog->last_think, id});
        }
      }
    },
    // decide: восприятие (kD-запрос) + GOAP одной сущности в свой per-thread scratch; пишет intent
    // в свой слот intents_ (см. decide_actor).
    [this, tick](const aesthetics::entityid_t id, acumen::execution_scratch& scratch) {
      decide_actor(id, tick, scratch);
    },
    // index_of: детерминированный тай-брейк бюджета по индексу сущности.
    [](const aesthetics::entityid_t id) -> uint64_t { return aesthetics::get_entityid_index(id); });
  DE_TRACE(catalogue::log_domain::gameplay, "cognition tick={} intents={} budget={}", tick, intents_.size(), scheduler_.think_budget);
}

// apply — ДЕТЕРМИНИРОВАННЫЙ барьер в двух фазах.
//   1) исполнить эффекты выбранных действий в id-порядке (мутируют скорость). Все
//      эффекты читают позиции ДО интеграции → решения по согласованному снимку тика.
//   2) проинтегрировать позиции (движение НЕ ограничено — без отскоков/клампа).
// Эффект мутирует напрямую (effect_sink подключится вторым консьюмером с catalogue).
void actor_world_slice::apply(const float dt_seconds) {
  sound_emits_.clear(); // sim-звуки этого тика собираем заново
  // Обход буфера интентов в порядке возрастания индекса актора (= прежний sort by id) —
  // детерминированный apply без явной сортировки. Пустые слоты пропускаются самим for_each.
  intents_.for_each([this](const aesthetics::entityid_t /*key*/, const act::intent& in) {
    if (in.kind != act::intent_kind::call_function) {
      return;
    }
    const auto* effect = registry_.effect(in.payload.call.fn);
    if (effect == nullptr) {
      return;
    }

    const auto id = aesthetics::entityid_t(in.actor.id);
    // Актора схватили В ЭТОМ ЖЕ apply (хищник с меньшим id отработал раньше) → его собственный
    // интент гасим, иначе движение перебьёт заморозку. Детерминированно (id-порядок).
    if (world_.get<actor_grabbed>(id) != nullptr) {
      return;
    }
    const auto* brain = world_.get<actor_brain>(id);
    const uint64_t seed = brain != nullptr ? brain->seed : 0u;
    // cognition уже завершён: caller lane 0 планировщика можно безопасно переиспользовать для apply/FSM.
    const auto ctx = make_ctx(world_, id, seed, tick_, nullptr, &scheduler_.lanes().front().act);
    effect->invoke(ctx); // движение (скорость + плейсхолдер укуса) — арбитраж GOAP

    // FSM-исполнитель: событие = выбранное действие (его хеш уже лежит в intent.payload.call.fn,
    // как и event_hash в mood). Меняем состояние ТОЛЬКО при реальном переходе — самопереход
    // (то же действие) не пере-входит (не дёргает on_exit/on_entry зря; звук в фазе D — раз на смену).
    if (auto* st = world_.get<actor_state>(id); st != nullptr) {
      const auto outcome = mood::step(*fsm_, st->state, in.payload.call.fn, ctx);
      if (outcome.result == mood::step_result::transitioned &&
          outcome.next_state != utils::invalid_id && outcome.next_state != st->state) {
        st->state = mood::apply_transition(*fsm_, st->state, *outcome.taken, ctx);
        // sim-звук на ВХОД в состояние (eating→чавк, flee→тревога). Позиция актора — для
        // куллинга по слушателю в презентационном мосте. Эфемерно, не реплицируется.
        if (const uint64_t snd = sound_for_state(st->state); snd != 0) {
          if (const auto* p = world_.get<actor_position>(id); p != nullptr) {
            sound_emits_.push_back(sound_emit{snd, p->value});
          }
        }
      }
    }
  });

  for (auto [id, pos, vel] : world_.view<actor_position, actor_velocity>()) {
    static_cast<void>(id);
    pos->value += vel->value * dt_seconds;
    // Жёсткая коллизия с препятствиями: оказался внутри диска → вытолкнуть на границу. Простое
    // позиционное разрешение (без стиринга): актор «скользит» вдоль препятствия. O(M), M мало.
    for (const auto& o : obstacles_) {
      const glm::vec2 d = pos->value - o.pos;
      const float d2 = d.x * d.x + d.y * d.y;
      if (d2 < o.radius * o.radius) {
        const float len = std::sqrt(d2);
        const glm::vec2 n = len > 1e-6f ? d * (1.0f / len) : glm::vec2{1.0f, 0.0f};
        pos->value = o.pos + n * o.radius;
      }
    }
  }

  // Пассивная динамика мотиваций (ВСЕ акторы, каждый тик — дёшево, O(N), плавно независимо
  // от каденса обдумывания). hunger копится всегда; boredom растёт пока стоит (думает), быстро
  // спадает в движении. Решения по drives примутся на следующем созревании актора.
  for (auto [id, dr, vel] : world_.view<stats, actor_velocity>()) {
    static_cast<void>(id);
    dr->hunger = std::clamp(dr->hunger + hunger_rate * dt_seconds, 0.0f, 1.0f);
    const float speed2 = vel->value.x * vel->value.x + vel->value.y * vel->value.y;
    const float db = (speed2 <= still_speed2 ? bored_rate : -bored_relief) * dt_seconds;
    dr->boredom = std::clamp(dr->boredom + db, 0.0f, 1.0f);
  }
}

// resolve_eating — завершить поедание у хищников с истёкшим сроком: наелся (голод в ноль),
// снять actor_eating (хищник снова «созреет» и переобдумает), удалить съеденную жертву. Удаление
// компонентов/сущностей — ПОСЛЕ обхода view (нельзя мутировать пул, по которому идём). Поеданий
// за тик мало → локальные буферы дёшевы.
void actor_world_slice::resolve_eating(const uint64_t tick) {
  std::vector<aesthetics::entityid_t> finished; // хищники, закончившие есть
  std::vector<aesthetics::entityid_t> kill;     // съеденные жертвы на удаление
  for (auto [id, eat] : world_.view<actor_eating>()) {
    if (tick < eat->until_tick) {
      continue;
    }
    if (auto* dr = world_.get<stats>(id); dr != nullptr) {
      dr->hunger = 0.0f; // наелся
    }
    finished.push_back(id);
    kill.push_back(eat->target);
  }
  for (const auto pid : finished) {
    world_.remove<actor_eating>(pid);
  }
  for (const auto bid : kill) {
    if (world_.exists(bid)) {
      world_.remove_entity(bid);
    }
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
  uint64_t food_spawn_seq;
  glm::vec2 spawn_min;
  glm::vec2 spawn_max;
  uint32_t food_target;
  uint32_t texture_count;
  uint32_t commit_ticks;
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
  const sim_globals g{tick_, food_spawn_seq_, spawn_min_, spawn_max_,
                      food_target_, texture_count_,
                      uint32_t(scheduler_.commit_ticks), uint32_t(scheduler_.think_budget)};
  serial::serialize(wr, g);
  raw.resize(wr.pos());
  return serial::seal(raw, policy); // header + checksum + компрессия (+ скриншот — тут не задаём)
}

bool actor_world_slice::load(const std::span<const uint8_t> packet) {
  namespace serial = aesthetics::serial;
  // чистый слайс: пересобираем реестр функций + GOAP/FSM (как init), но БЕЗ спавна сущностей.
  world_ = aesthetics::world{};
  setup_brain_registry();
  intents_.clear();
  // как в init: аллокаторы пулов поедания заранее на главном потоке (view<> не кинет; type-id
  // фиксируется до MT). load_world и так тронет все зарегистрированные типы, но порядок важен.
  static_cast<void>(world_.get_or_create_allocator<actor_eating>(sizeof(actor_eating) * 250));
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
  food_spawn_seq_ = g.food_spawn_seq;
  spawn_min_ = g.spawn_min;
  spawn_max_ = g.spawn_max;
  food_target_ = g.food_target;
  texture_count_ = g.texture_count;
  scheduler_.commit_ticks = g.commit_ticks;
  scheduler_.think_budget = g.think_budget;

  rebuild_obstacle_cache(); // кэш выводим из мира, не хранится в снапшоте
  return true;
}

} // namespace core
} // namespace tile_frontier
