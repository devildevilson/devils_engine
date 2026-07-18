// Live acceptance for config-loaded GOAP actions and structured TAVL FSM transitions.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include <devils_engine/aesthetics/serialization.h>
#include <devils_engine/thread/atomic_pool.h>
#include <spdlog/spdlog.h>

#include "core/actor_simulation.h"
#include "core/entity_scope.h" // tf::entity_scope — root-скоуп парс-проверок building blocks
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
  const auto find_goap = [&brains](const std::string_view name) -> const acumen::goap_config* {
    for (const auto& g : brains.goaps) {
      if (g.name == name) {
        return g.config.get();
      }
    }
    return nullptr;
  };
  const auto* goap = find_goap("actor");
  if (goap == nullptr) {
    std::cerr << "FAILED: multi-brain loader did not produce the 'actor' GOAP brain\n";
    return 1;
  }
  size_t scripted_actions = 0;
  for (const auto& action : goap->actions) {
    scripted_actions += action.has_effect_program ? 1u : 0u;
  }
  // Все шесть действий config-only: flee/chase/think, target-aware eat (`eat = prey`) и
  // seek_food/wander на ds RNG (`set_course = chance`).
  if (scripted_actions != 6) {
    std::cerr << "FAILED: expected 6 scripted actions in shipped goap/actor, got "
              << scripted_actions << '\n';
    return 1;
  }

  // prey-мозг: наследник actor'а без хищных действий (chase/eat отключены, flee остаётся).
  const auto* prey = find_goap("prey");
  if (prey == nullptr) {
    std::cerr << "FAILED: multi-brain loader did not produce the 'prey' GOAP brain\n";
    return 1;
  }
  const auto has_action = [](const acumen::goap_config& cfg, const std::string_view name) {
    for (const auto& a : cfg.actions) {
      if (a.name == name) {
        return true;
      }
    }
    return false;
  };
  if (has_action(*prey, "chase") || has_action(*prey, "eat") || !has_action(*prey, "flee")) {
    std::cerr << "FAILED: prey brain must disable chase/eat and keep flee\n";
    return 1;
  }

  const tf::named_fsm* actor_fsm = nullptr;
  for (const auto& f : brains.fsms) {
    if (f.name == "actor") {
      actor_fsm = &f;
    }
  }
  if (actor_fsm == nullptr) {
    std::cerr << "FAILED: multi-brain loader did not produce the 'actor' FSM brain\n";
    return 1;
  }
  const auto& transitions = *actor_fsm->transitions;
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

  // 300 тиков: и bit-identity 1-vs-4, и живость config-eat (`eat = prey` реально хватает добычу).
  constexpr uint64_t dt = devils_engine::utils::timeline_ticks_per_second / 60; // µs game-времени за тик
  uint32_t eating_peak = 0;
  for (size_t tick = 0; tick < 300; ++tick) {
    const auto ma = one_worker.update(dt, batch_one, pool_one);
    four_workers.update(dt, batch_four, pool_four);
    eating_peak = std::max(eating_peak, ma.eating);
    if (dump(one_worker) != dump(four_workers)) {
      std::cerr << "FAILED: config-loaded effect diverged at tick " << (tick + 1) << '\n';
      return 1;
    }
  }
  if (eating_peak == 0) {
    std::cerr << "FAILED: config-loaded eat never grabbed prey in 300 ticks\n";
    return 1;
  }

  // Смена скорости и пауза мид-ран — та же последовательность game-дельт обоим слайсам ⇒ identity
  // обязана держаться: дельта = вход, флаги/сроки тикают только на game-дельту (пауза = 0).
  const uint64_t phases[][2] = {{dt * 3, 45}, {0, 15}, {dt / 2, 45}};
  for (const auto& [phase_dt, ticks] : phases) {
    for (uint64_t t = 0; t < ticks; ++t) {
      one_worker.update(phase_dt, batch_one, pool_one);
      four_workers.update(phase_dt, batch_four, pool_four);
      if (dump(one_worker) != dump(four_workers)) {
        std::cerr << "FAILED: diverged under game speed change (dt=" << phase_dt << ")\n";
        return 1;
      }
    }
  }

  // per-entity мозги (goap_ref/fsm_ref): prey-префаб живёт в общем мире с актёрами, ссылки
  // независимы (свой GOAP без chase/eat + ОБЩИЙ fsm/actor), популяция смешанная — identity 1-vs-4
  // обязана держаться и с двумя мозгами на одних per-thread solution_cache (system_salt в ключе).
  const auto prey_a = one_worker.spawn_prefab("prey", {32.0f, 32.0f});
  const auto prey_b = four_workers.spawn_prefab("prey", {32.0f, 32.0f});
  if (prey_a != prey_b) {
    std::cerr << "FAILED: prey spawn produced different entity ids across slices\n";
    return 1;
  }
  const auto* pref = one_worker.ecs().get<tf::goap_ref>(prey_a);
  const auto* fref = one_worker.ecs().get<tf::fsm_ref>(prey_a);
  if (pref == nullptr || pref->value != devils_engine::utils::string_hash("prey") ||
      fref == nullptr || fref->value != devils_engine::utils::string_hash("actor")) {
    std::cerr << "FAILED: prey prefab did not resolve goap=prey / fsm=actor refs\n";
    return 1;
  }
  for (size_t tick = 0; tick < 60; ++tick) {
    one_worker.update(dt, batch_one, pool_one);
    four_workers.update(dt, batch_four, pool_four);
    if (dump(one_worker) != dump(four_workers)) {
      std::cerr << "FAILED: mixed-brain population diverged at tick " << (tick + 1) << '\n';
      return 1;
    }
  }
  if (one_worker.ecs().get<tf::actor_eating>(prey_a) != nullptr) {
    std::cerr << "FAILED: prey (no eat action) somehow started eating\n";
    return 1;
  }

  // read-only UI-шов: pure act-вызовы по имени + describe-стрим узлов; effect через него недоступен.
  bool ui_seam_checked = false;
  for (uint32_t raw = 0; raw < 512 && !ui_seam_checked; ++raw) {
    const auto id = devils_engine::aesthetics::entityid_t(raw);
    const auto probe = one_worker.ui_predicate("is_eating", id);
    if (!probe.has_value()) {
      continue; // сущность съедена/невалидна — ищем живую
    }
    ui_seam_checked = true;
    if (one_worker.ui_predicate("eat", id).has_value()) {
      std::cerr << "FAILED: effect category leaked through the read-only UI seam\n";
      return 1;
    }
    if (one_worker.ui_predicate("no_such_fn", id).has_value()) {
      std::cerr << "FAILED: unknown act name returned a value\n";
      return 1;
    }
    size_t nodes = 0;
    const bool described = one_worker.ui_describe("actor.is_hungry", id, [&nodes](const std::string_view) {
      ++nodes;
    });
    if (!described || nodes == 0) {
      std::cerr << "FAILED: describe did not stream ds nodes (described=" << described
                << ", nodes=" << nodes << ")\n";
      return 1;
    }
  }
  if (!ui_seam_checked) {
    std::cerr << "FAILED: no live actor found for the UI seam probe\n";
    return 1;
  }

  // building blocks флагов видны парсеру под обычными сигнатурами (исполнение через record/commit
  // покрывает sated-путь выше: resolve_eating ставит флаг, drives читает, sweep тикает).
  auto& sys = fixture.scripts().sys;
  const auto set_script = sys.parse<void, tf::entity_scope>("flags_set", "set_flag = { stunned, 2 }");
  const auto clear_script = sys.parse<void, tf::entity_scope>("flags_clear", "clear_flag = stunned");
  const auto has_script = sys.parse<bool, tf::entity_scope>("flags_has", "has_flag(stunned)");
  if (set_script.cmds.empty() || clear_script.cmds.empty() || has_script.cmds.empty()) {
    std::cerr << "FAILED: flag building blocks did not compile\n";
    return 1;
  }

  std::cout << "OK: shipped goap/actor has " << scripted_actions
            << " tavl void actions and fsm/actor has " << transitions.size()
            << " structured TAVL transitions; "
               "1/4 workers bit-identical for 300 ticks, eating peak "
            << eating_peak << '\n';
  return 0;
}
