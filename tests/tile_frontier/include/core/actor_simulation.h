#ifndef TILE_FRONTIER_CORE_ACTOR_SIMULATION_H
#define TILE_FRONTIER_CORE_ACTOR_SIMULATION_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <devils_engine/aesthetics/world.h>
#include <devils_engine/act/registry.h> // act::registry + function<RetT>
#include <devils_engine/act/intent.h>   // act::intent — обобщённый буфер интентов

#include "draw_intent.h"
#include "tile_map.h"

namespace tile_frontier {
namespace core {

// First lightweight actor slice: tiny deterministic brains write move intents,
// then a stable apply phase mutates aesthetics components.
struct actor_position {
  glm::vec2 value{0.0f, 0.0f};
};

struct actor_velocity {
  glm::vec2 value{0.0f, 0.0f};
};

struct actor_brain {
  uint32_t seed = 0;
  uint32_t phase = 0;
  float speed = 1.0f;
};

struct actor_visual {
  uint32_t texture = 0;
  instance_layout::rgba8_color color{};
  float size = 1.0f;
};

// GPU instance for actor draw group. Layout: "v2ui1c4v1".
struct actor_instance {
  glm::vec2 pos;
  uint32_t texture;
  instance_layout::rgba8_color color;
  float size;
};

struct actor_metrics {
  uint32_t actors = 0;
  uint32_t intents = 0;
  uint32_t instances = 0;
  uint64_t ticks = 0;
};

class actor_batch {
public:
  instance_layout::match_result bind(const std::string_view& layout = "v2ui1c4v1") {
    return intent_.bind(layout);
  }
  bool valid() const noexcept { return intent_.valid(); }

  void build(const devils_engine::aesthetics::world& world);

  std::span<const actor_instance> instances() const noexcept { return instances_; }
  uint32_t count() const noexcept { return uint32_t(instances_.size()); }
  static constexpr uint32_t stride() noexcept { return draw_intent<actor_instance>::stride(); }

  std::size_t blit(const std::span<uint8_t>& dst) const {
    return intent_.blit(std::span<const actor_instance>(instances_), dst);
  }

private:
  draw_intent<actor_instance> intent_;
  std::vector<actor_instance> instances_;
};

class actor_world_slice {
public:
  void init(uint32_t count, glm::vec2 min_bound, glm::vec2 max_bound, uint32_t texture_count);
  actor_metrics update(float dt_seconds, actor_batch& batch);

  devils_engine::aesthetics::world& ecs() noexcept { return world_; }
  const devils_engine::aesthetics::world& ecs() const noexcept { return world_; }

private:
  // Регистрирует нативные геймплейные функции "мозга" в act::registry и кэширует
  // типизированные указатели (резолв по имени уходит из горячего пути think).
  void setup_brain_registry();
  void think(uint64_t tick);
  void apply(float dt_seconds);

  devils_engine::aesthetics::world world_;
  devils_engine::act::registry registry_; // общий реестр геймплейных функций (см. libs/act)
  const devils_engine::act::number_function* wander_dir_fn_ = nullptr; // кэш "wander.direction"
  std::vector<devils_engine::act::intent> intents_; // обобщённый буфер интентов (sort by actor id)
  glm::vec2 min_bound_{0.0f, 0.0f};
  glm::vec2 max_bound_{0.0f, 0.0f};
  uint64_t tick_ = 0;
};

} // namespace core
} // namespace tile_frontier

#endif
