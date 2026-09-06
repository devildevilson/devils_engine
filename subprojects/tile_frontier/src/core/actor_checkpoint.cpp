#include "core/actor_checkpoint.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include <devils_engine/aesthetics/serialization.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/serialization.h>

#include "core/actor_snapshot.h"

// Состав checkpoint задаёт проект, а не ECS. Все секции читаются в отдельное состояние;
// проверка timeline и actor-полей предшествует публикации. Реестры заранее собраны в стабильном
// runtime-владельце; привязанные к адресу мира системы сбрасываются и создаются на следующем tick.

namespace tile_frontier::core {

namespace serial = devils_engine::utils::serial;
namespace aesthetics = devils_engine::aesthetics;
namespace utils = devils_engine::utils;

actor_checkpoint_access::state actor_checkpoint_access::capture(
  const actor_world_slice& actors) noexcept {
  return {
    .food_spawn_seq = actors.food_spawn_seq_,
    .spawn_min = actors.spawn_min_,
    .spawn_max = actors.spawn_max_,
    .food_target = actors.food_target_,
    .texture_count = actors.texture_count_,
    .commit_game_ticks = actors.commit_game_ticks_,
    .think_budget = actors.think_budget_,
  };
}

bool actor_checkpoint_access::validate(const state& value) noexcept {
  return std::isfinite(value.spawn_min.x) && std::isfinite(value.spawn_min.y) &&
         std::isfinite(value.spawn_max.x) && std::isfinite(value.spawn_max.y) &&
         value.spawn_min.x <= value.spawn_max.x &&
         value.spawn_min.y <= value.spawn_max.y &&
         value.texture_count != 0 && value.commit_game_ticks != 0 &&
         value.think_budget != 0;
}

bool actor_checkpoint_access::prepare_world(staged_actor& output,
                                            aesthetics::world&& world) {
  aesthetics::entityid_t player = aesthetics::invalid_entityid;
  for (auto [id, controller] : world.view<player_controller>()) {
    static_cast<void>(controller);
    if (player != aesthetics::invalid_entityid) return false;
    player = id;
  }
  if (player == aesthetics::invalid_entityid) return false;
  world.create<player_intent_queue>(player);

  std::vector<actor_world_slice::obstacle_disc> obstacles;
  obstacles.reserve(world.count<obstacle>());
  for (auto [id, value, position] : world.view<obstacle, actor_position>()) {
    static_cast<void>(id);
    obstacles.push_back({position->value, value->radius});
  }

  output.player = player;
  output.obstacles = std::move(obstacles);
  return true;
}

namespace {

struct checkpoint_staging {
  utils::timelines_causal_state timeline{};
  std::optional<aesthetics::world> world;
  actor_checkpoint_access::staged_actor actor;
};

struct checkpoint_host {
  using staging_type = checkpoint_staging;

  const actor_world_slice* actors = nullptr;
  const utils::timelines* timeline = nullptr;
};

struct timeline_section {
  static constexpr std::uint32_t id = checkpoint_timeline_section;
  static constexpr std::uint32_t version = 1;

  static void write(const checkpoint_host& host, serial::state_writer& output) {
    serial::serialize(output, host.timeline->causal_state());
  }

  static bool read(checkpoint_staging& staging, serial::state_reader& input) {
    serial::deserialize(input, staging.timeline);
    return input.good();
  }

  static bool validate(const checkpoint_staging& staging) {
    utils::timelines candidate;
    return candidate.restore_causal_state(staging.timeline);
  }
};

struct world_section {
  static constexpr std::uint32_t id = checkpoint_world_section;
  static constexpr std::uint32_t version = 1;

  static void write(const checkpoint_host& host, serial::state_writer& output) {
    aesthetics::serial::dump_world(&host.actors->ecs(), output);
  }

  static bool read(checkpoint_staging& staging, serial::state_reader& input) {
    auto world = aesthetics::serial::stage_world(input);
    if (!world.has_value() ||
        !actor_checkpoint_access::prepare_world(staging.actor, std::move(*world))) {
      return false;
    }
    staging.world.emplace(std::move(*world));
    return true;
  }

  static bool validate(const checkpoint_staging& staging) {
    return staging.world.has_value() &&
           staging.actor.player != aesthetics::invalid_entityid;
  }
};

struct actor_section {
  static constexpr std::uint32_t id = checkpoint_actor_section;
  static constexpr std::uint32_t version = 1;

  static void write(const checkpoint_host& host, serial::state_writer& output) {
    serial::serialize(output, actor_checkpoint_access::capture(*host.actors));
  }

  static bool read(checkpoint_staging& staging, serial::state_reader& input) {
    serial::deserialize(input, staging.actor.causal);
    return input.good();
  }

  static bool validate(const checkpoint_staging& staging) {
    return actor_checkpoint_access::validate(staging.actor.causal);
  }
};

using checkpoint_schema = serial::state_schema<
  checkpoint_host, serial::state_writer, serial::state_reader,
  timeline_section, world_section, actor_section>;

struct validate_checkpoint {
  bool operator()(const checkpoint_staging& staging) const noexcept {
    return staging.timeline.simulation.value != std::numeric_limits<std::uint64_t>::max() &&
           staging.actor.causal.think_budget <=
             std::uint64_t(std::numeric_limits<std::size_t>::max());
  }
};

struct publish_checkpoint {
  actor_world_slice& actors;
  utils::timelines& timeline;

  void operator()(checkpoint_host&, checkpoint_staging&& staging) const noexcept {
    const bool restored = timeline.restore_causal_state(staging.timeline);
    if (!restored || !staging.world.has_value()) std::terminate();
    actor_checkpoint_access::publish(actors, std::move(*staging.world),
                                     std::move(staging.actor), staging.timeline);
  }
};

} // namespace

bool write_actor_checkpoint(const actor_world_slice& actors,
                            const utils::timelines& timelines,
                            actor_checkpoint_buffers& buffers) {
  if (actors.simulation_now() > timelines.simulation_now() ||
      actors.game_now() != timelines.game_now() ||
      !actor_checkpoint_access::validate(actor_checkpoint_access::capture(actors))) return false;
  const auto* pending = actors.ecs().get<player_intent_queue>(actors.player_entity());
  if (pending == nullptr || !pending->pending.empty()) return false;
  checkpoint_host host{&actors, &timelines};
  return checkpoint_schema::write(host, buffers.document, buffers.section_scratch);
}

serial::state_load_result load_actor_checkpoint(
  actor_world_slice& actors, utils::timelines& timelines,
  const std::span<const std::byte> document, const brain_config& brains) {
  checkpoint_host host{&actors, &timelines};
  checkpoint_staging staging;
  actor_checkpoint_access::prepare_runtime(staging.actor, brains);
  serial::state_reader input{document};
  return checkpoint_schema::load(host, input, std::move(staging),
                                 validate_checkpoint{}, publish_checkpoint{actors, timelines});
}

std::uint32_t actor_checkpoint_schema_fingerprint() noexcept {
  return checkpoint_schema::schema_fingerprint();
}

} // namespace tile_frontier::core
