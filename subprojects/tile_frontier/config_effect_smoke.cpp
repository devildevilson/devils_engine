// Live acceptance for config-loaded GOAP actions and structured TAVL FSM transitions.

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include <devils_engine/aesthetics/serialization.h>
#include <devils_engine/thread/atomic_pool.h>
#include <spdlog/spdlog.h>

#include "core/actor_simulation.h"
#include "test_brain_fixture.h"

using namespace devils_engine;
namespace tf = tile_frontier::core;

namespace {

std::vector<std::byte> dump(const tf::actor_world_slice& slice) {
  return aesthetics::serial::dump_world(&slice.ecs());
}

} // namespace

int main() {
  spdlog::set_level(spdlog::level::err);

  test_brain_fixture fixture(TILE_FRONTIER_SOURCE_RESOURCE_ROOT);
  const auto& brains = fixture.config();
  const auto& goap = brains.goap;
  size_t scripted_actions = 0;
  for (const auto& action : goap->actions) {
    scripted_actions += action.has_effect_program ? 1u : 0u;
  }
  if (scripted_actions != 3) {
    std::cerr << "FAILED: expected 3 scripted actions in shipped goap/actor, got "
              << scripted_actions << '\n';
    return 1;
  }

  const auto& transitions = *brains.fsm_transitions;
  if (transitions.size() != 6 || transitions[1].guards != std::vector<std::string>{"is_eating"}) {
    std::cerr << "FAILED: shipped fsm/actor did not produce six structured TAVL transitions\n";
    return 1;
  }
  if (brains.is_hungry_program == nullptr || brains.prefabs.size() < 2) {
    std::cerr << "FAILED: required script/prefab resources were not loaded\n";
    return 1;
  }

  tf::actor_world_slice one_worker;
  tf::actor_world_slice four_workers;
  one_worker.init(512, {0.5f, 0.5f}, {64.0f, 64.0f}, 4, brains);
  four_workers.init(512, {0.5f, 0.5f}, {64.0f, 64.0f}, 4, brains);

  tf::actor_batch batch_one;
  tf::actor_batch batch_four;
  batch_one.bind("v2ui1c4v1");
  batch_four.bind("v2ui1c4v1");
  thread::atomic_pool pool_one(1);
  thread::atomic_pool pool_four(4);

  constexpr float dt = 1.0f / 60.0f;
  for (size_t tick = 0; tick < 30; ++tick) {
    one_worker.update(dt, batch_one, pool_one);
    four_workers.update(dt, batch_four, pool_four);
    if (dump(one_worker) != dump(four_workers)) {
      std::cerr << "FAILED: config-loaded effect diverged at tick " << (tick + 1) << '\n';
      return 1;
    }
  }

  std::cout << "OK: shipped goap/actor has " << scripted_actions
            << " tavl void actions and fsm/actor has " << transitions.size()
            << " structured TAVL transitions; "
               "1/4 workers bit-identical for 30 ticks\n";
  return 0;
}
