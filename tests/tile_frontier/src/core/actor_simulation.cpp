#include "actor_simulation.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <devils_engine/aesthetics/common.h>
#include <devils_engine/act/exec_context.h>
#include <devils_engine/act/function.h>
#include <devils_engine/utils/core.h>      // utils::error
#include <devils_engine/utils/prng.h>      // utils::mix
#include <devils_engine/utils/string_id.h> // utils::string_hash

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
  const uint64_t seed, const uint64_t tick, act::effect_sink* sink = nullptr) noexcept
{
  act::exec_context ctx;
  ctx.scope[0] = act::entity_id{ uint32_t(id) };
  ctx.scope_count = 1;
  ctx.w = reinterpret_cast<const act::world*>(&world);
  ctx.rng_seed = seed;
  ctx.rng_entity = aesthetics::get_entityid_index(id);
  ctx.rng_tick = tick;
  ctx.sink = sink;
  return ctx;
}

// purpose-метка для counter-RNG (utils::mix) — разводит потоки случайности.
static constexpr uint64_t purpose_wander = 0x77616e646572ull; // "wander"

// Нативная геймплейная функция-"мозг": решает, в какую из 8 сторон блуждать.
// Категория number (real_t) — возвращает индекс направления 0..7. Читает мир
// через мост (actor_brain даёт пер-акторную фазу), случайность — детерминированный
// counter-RNG из immutable-входов контекста. Это и есть acumen/mood-шов в зачатке:
// решение принимается ЧИСТО (dry-run), не мутируя мир; мутация — на apply-фазе.
static act::real_t wander_direction(const act::exec_context& ctx) noexcept {
  const auto& world = world_of(ctx);
  const auto id = aesthetics::entityid_t(ctx.primary().id);
  const auto* brain = world.get<actor_brain>(id);
  const uint32_t phase = brain != nullptr ? brain->phase : 0u;
  // медленная смена направления (~раз в 24 тика) с пер-акторной фазой.
  const uint64_t slot = (ctx.rng_tick + phase) / 24u;
  const uint64_t h = utils::mix(ctx.rng_seed, ctx.rng_entity, slot, purpose_wander);
  return act::real_t(h & 7u);
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
  static constexpr glm::vec2 dirs[] = {
    { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f },
    { diag, diag }, { -diag, diag }, { diag, -diag }, { -diag, -diag }
  };
  return dirs[h & 7u];
}

static float unit_from_hash(const uint32_t h) noexcept {
  return float(h & 0xffffu) / float(0xffffu);
}

static instance_layout::rgba8_color actor_color(const uint32_t index) noexcept {
  const auto pack = [] (const float v) {
    return uint32_t(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };

  static constexpr float colors[][4] = {
    { 0.05f, 0.30f, 1.00f, 1.0f }, // blue
    { 1.00f, 0.08f, 0.05f, 1.0f }, // red
    { 0.62f, 0.16f, 1.00f, 1.0f }, // purple
    { 0.05f, 0.85f, 0.25f, 1.0f }, // green
    { 1.00f, 0.85f, 0.05f, 1.0f }, // yellow
    { 1.00f, 0.35f, 0.03f, 1.0f }, // orange
    { 0.00f, 0.85f, 0.95f, 1.0f }, // cyan
    { 1.00f, 0.12f, 0.62f, 1.0f }, // magenta
  };
  const auto& c = colors[index % std::size(colors)];
  return instance_layout::rgba8_color{
    (pack(c[0]) << 0) | (pack(c[1]) << 8) | (pack(c[2]) << 16) | (pack(c[3]) << 24)
  };
}

static float actor_size(const uint32_t seed) noexcept {
  return 0.34f + unit_from_hash(mix32(seed ^ 0x9e37u)) * 0.42f;
}

void actor_batch::build(const aesthetics::world& world) {
  instances_.clear();
  instances_.reserve(world.count<actor_visual>());

  for (auto [id, pos, visual] : world.view<actor_position, actor_visual>()) {
    (void)id;
    instances_.push_back(actor_instance{pos->value, visual->texture, visual->color, visual->size});
  }
}

void actor_world_slice::setup_brain_registry() {
  // пересоздаём реестр с нуля: reg() ассертит на повторную регистрацию имени,
  // а init() может вызываться многократно.
  registry_ = act::registry{};
  registry_.reg("wander.direction", std::make_unique<act::native_function<act::real_t>>(
    &wander_direction, "пер-акторное направление блуждания, индекс 0..7"));
  wander_dir_fn_ = registry_.number(utils::string_hash("wander.direction"));
  if (wander_dir_fn_ == nullptr) utils::error{}("act: функция 'wander.direction' не зарегистрирована как number");
}

void actor_world_slice::init(const uint32_t count, const glm::vec2 min_bound, const glm::vec2 max_bound, const uint32_t texture_count) {
  world_ = aesthetics::world{};
  setup_brain_registry();
  intents_.clear();
  intents_.reserve(count);
  min_bound_ = min_bound;
  max_bound_ = max_bound;
  tick_ = 0;

  const uint32_t tex_count = std::max(texture_count, 1u);
  const glm::vec2 extent = max_bound_ - min_bound_;
  const uint32_t columns = std::max<uint32_t>(uint32_t(std::ceil(std::sqrt(float(std::max(count, 1u))))), 1u);
  const uint32_t rows = std::max<uint32_t>((count + columns - 1u) / columns, 1u);

  for (uint32_t i = 0; i < count; ++i) {
    const auto id = world_.gen_entityid();
    const uint32_t x = i % columns;
    const uint32_t y = i / columns;
    const float fx = (float(x) + 0.5f) / float(columns);
    const float fy = (float(y) + 0.5f) / float(rows);
    const uint32_t seed = mix32(i + 0x1234u);
    const float jitter_x = (unit_from_hash(mix32(seed ^ 0xa5a5u)) - 0.5f) * 0.35f;
    const float jitter_y = (unit_from_hash(mix32(seed ^ 0x5a5au)) - 0.5f) * 0.35f;

    world_.create<actor_position>(id, glm::vec2{
      min_bound_.x + extent.x * fx + jitter_x,
      min_bound_.y + extent.y * fy + jitter_y
    });
    world_.create<actor_velocity>(id, glm::vec2{0.0f, 0.0f});
    world_.create<actor_brain>(id, seed, seed % 31u, 0.65f + unit_from_hash(seed) * 1.35f);
    world_.create<actor_visual>(id, (i + 1u) % tex_count, actor_color(i), actor_size(seed));
  }
}

actor_metrics actor_world_slice::update(const float dt_seconds, actor_batch& batch) {
  tick_ += 1;
  think(tick_);
  apply(dt_seconds);
  batch.build(world_);

  return actor_metrics{
    uint32_t(world_.count<actor_position>()),
    uint32_t(intents_.size()),
    batch.count(),
    tick_
  };
}

// think — ЧИСТАЯ фаза: на каждую сущность строим dry-run контекст, гоняем мозг
// (act::number_function через реестр) и выдаём компактный act::intent. Мир НЕ
// мутируется здесь → фаза параллелизуема. Буфер сортируем по индексу сущности,
// чтобы apply шла в фиксированном детерминированном порядке.
void actor_world_slice::think(const uint64_t tick) {
  intents_.clear();
  for (auto [id, pos, brain] : world_.view<actor_position, actor_brain>()) {
    (void)pos;
    const auto ctx = make_ctx(world_, id, brain->seed, tick); // sink=nullptr ⇒ dry-run
    const uint32_t dir_index = uint32_t(wander_dir_fn_->invoke(ctx)) & 7u;
    const glm::vec2 dir = direction_from_hash(dir_index);

    act::intent in;
    in.kind = act::intent_kind::move_to;
    in.actor = act::entity_id{ uint32_t(id) };
    // payload.target несёт вектор скорости (направление * скорость) — apply его
    // интегрирует. (move_to как «точка назначения» придёт, когда появится навигация.)
    in.payload.target = act::vec3{ dir.x * brain->speed, dir.y * brain->speed, 0.0 };
    in.source_action = utils::string_hash("wander"); // provenance: кто породил интент
    intents_.push_back(in);
  }

  std::sort(intents_.begin(), intents_.end(), [] (const act::intent& a, const act::intent& b) {
    return aesthetics::get_entityid_index(a.actor.id) < aesthetics::get_entityid_index(b.actor.id);
  });
}

// apply — ДЕТЕРМИНИРОВАННЫЙ барьер: проходим отсортированный буфер интентов в
// id-порядке и мутируем компоненты. Единственный консьюмер на MVP; effect_sink
// (лог в catalogue) подключится вторым консьюмером позже.
void actor_world_slice::apply(const float dt_seconds) {
  for (const auto& in : intents_) {
    if (in.kind != act::intent_kind::move_to) continue;

    const auto id = aesthetics::entityid_t(in.actor.id);
    auto* pos = world_.get<actor_position>(id);
    auto* vel = world_.get<actor_velocity>(id);
    if (pos == nullptr || vel == nullptr) continue;

    vel->value = glm::vec2(float(in.payload.target.x), float(in.payload.target.y));
    pos->value += vel->value * dt_seconds;

    if (pos->value.x < min_bound_.x) { pos->value.x = min_bound_.x; vel->value.x = std::abs(vel->value.x); }
    if (pos->value.y < min_bound_.y) { pos->value.y = min_bound_.y; vel->value.y = std::abs(vel->value.y); }
    if (pos->value.x > max_bound_.x) { pos->value.x = max_bound_.x; vel->value.x = -std::abs(vel->value.x); }
    if (pos->value.y > max_bound_.y) { pos->value.y = max_bound_.y; vel->value.y = -std::abs(vel->value.y); }
  }
}

} // namespace core
} // namespace tile_frontier
