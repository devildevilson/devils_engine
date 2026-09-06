#ifndef TILE_FRONTIER_CORE_ACTOR_CHECKPOINT_H
#define TILE_FRONTIER_CORE_ACTOR_CHECKPOINT_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <devils_engine/utils/state_schema.h>
#include <devils_engine/utils/timeline.h>

#include "actor_simulation.h"

// Полный причинный checkpoint акторного слоя: timeline, ECS и собственные счётчики — три секции.
// Запись допустима на завершённой границе тика без pending intent и параллельного доступа.
// Tick/game time принадлежат timeline и не дублируются в actor-секции. Загрузка готовит отдельный
// мир и runtime, затем публикует всё целиком; обработчики snapshot_loaded_event не должны бросать.
// Буферы остаются у вызывающего. Документ несовместим со старым actor_world_slice::save/load.

namespace tile_frontier::core {

inline constexpr std::uint32_t checkpoint_timeline_section = UINT32_C(0x1000);
inline constexpr std::uint32_t checkpoint_world_section = UINT32_C(0x2000);
inline constexpr std::uint32_t checkpoint_actor_section = UINT32_C(0x3000);

struct actor_checkpoint_buffers {
  std::vector<std::byte> document;
  std::vector<std::byte> section_scratch;
};

struct actor_checkpoint_access {
  struct state {
    std::uint64_t food_spawn_seq = 0;
    glm::vec2 spawn_min{};
    glm::vec2 spawn_max{};
    std::uint32_t food_target = 0;
    std::uint32_t texture_count = 1;
    std::uint64_t commit_game_ticks = 150000;
    std::uint64_t think_budget = 2048;
  };

  struct staged_actor {
    state causal{};
    devils_engine::aesthetics::entityid_t player =
      devils_engine::aesthetics::invalid_entityid;
    std::vector<actor_world_slice::obstacle_disc> obstacles;
    std::unique_ptr<actor_brain_runtime> runtime;
  };

  static state capture(const actor_world_slice& actors) noexcept;
  static bool validate(const state& value) noexcept;
  static bool prepare_world(staged_actor& output, devils_engine::aesthetics::world&& world);
  static void prepare_runtime(staged_actor& output, const brain_config& brains);
  static void publish(actor_world_slice& actors, devils_engine::aesthetics::world&& world,
                      staged_actor&& staged,
                      const devils_engine::utils::timelines_causal_state& timeline) noexcept;
};

[[nodiscard]] bool write_actor_checkpoint(
  const actor_world_slice& actors,
  const devils_engine::utils::timelines& timelines,
  actor_checkpoint_buffers& buffers);

[[nodiscard]] devils_engine::utils::serial::state_load_result load_actor_checkpoint(
  actor_world_slice& actors,
  devils_engine::utils::timelines& timelines,
  std::span<const std::byte> document,
  const brain_config& brains);

[[nodiscard]] std::uint32_t actor_checkpoint_schema_fingerprint() noexcept;

} // namespace tile_frontier::core

#endif
