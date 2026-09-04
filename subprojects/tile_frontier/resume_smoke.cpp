// Resume-детерминизм РЕАЛЬНОГО геймплея tile_frontier: прогоняем симуляцию, save() → load() в
// чистый слайс, затем гоняем оба слайса синхронно и сверяем ПОЛНОЕ состояние побайтово. Оракул
// равенства — сам сериализатор: dump_world детерминирован (позиционно + сортировка мап), поэтому
// dump_world(A) == dump_world(B) ⇔ миры идентичны во всех компонентах. Это доказывает, что снапшот
// (а) round-trip'ит всё состояние и (б) захватывает ДОСТАТОЧНО, чтобы продолжить один-в-один.
// Плоский main (без doctest): печатает результат, код возврата 0/1.
#include <cstdint>
#include <cstdio>
#include <vector>

#include <devils_engine/aesthetics/serialization.h> // dump_world — оракул равенства
#include <devils_engine/thread/atomic_pool.h>
#include <spdlog/spdlog.h>

#include "core/actor_simulation.h"
#include "test_brain_fixture.h"

using namespace devils_engine;
namespace tf = tile_frontier::core;

static int failures = 0;
#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
      ++failures;                                             \
    }                                                         \
  } while (0)

static std::vector<std::byte> dump(const tf::actor_world_slice& s) {
  return aesthetics::serial::dump_world(&s.ecs());
}

int main() {
  spdlog::set_level(spdlog::level::warn); // приглушить per-tick info из update()

  // CAT-03 metadata is opt-in: this headless inspector/test is the first caller. Normal update()
  // never registers or looks up these descriptors.
  const auto effect_phases = tf::actor_effect_phase_metadata();
  CHECK(effect_phases.size() == 3);
  for (const auto& phase : effect_phases) {
    catalogue::validate_phase_descriptor(phase);
  }

  test_brain_fixture fixture(TILE_FRONTIER_SOURCE_RESOURCE_ROOT);
  const auto& brains = fixture.config();

  const uint32_t count = 2000;
  const glm::vec2 mn{0.5f, 0.5f}, mx{64.0f, 64.0f};
  const uint32_t tex = 4;
  thread::atomic_pool single_pool(1);
  thread::atomic_pool pool(4); // тем же пулом гоняем оба слайса ⇒ MT-детерминизм тоже под тестом

  tf::actor_batch batch_a, batch_b;
  batch_a.bind("v2ui1c4v1");
  batch_b.bind("v2ui1c4v1");
  CHECK(batch_a.valid());
  CHECK(batch_b.valid());

  // Один и тот же deferred-effect pipeline не должен зависеть от числа worker-ов: record append-ится
  // физически, seal сортирует по semantic key, elect выбирает по стабильному source id.
  {
    tf::actor_world_slice one_worker, four_workers;
    one_worker.init(512, mn, mx, tex, brains);
    four_workers.init(512, mn, mx, tex, brains);
    utils::timelines clocks(utils::simulation_rate(60));
    int worker_count_diverged_at = -1;
    for (int i = 1; i <= 45 && worker_count_diverged_at < 0; ++i) {
      const auto tick = clocks.simulation_now() + utils::simulation_duration{1};
      const auto game_delta = clocks.advance_simulation(tick);
      one_worker.update(tick, game_delta, batch_a, single_pool);
      four_workers.update(tick, game_delta, batch_b, pool);
      if (dump(one_worker) != dump(four_workers)) {
        worker_count_diverged_at = i;
      }
    }
    CHECK(worker_count_diverged_at < 0);
    std::printf("1-worker vs 4-worker deferred pipeline: %s\n",
                worker_count_diverged_at < 0 ? "45 ticks bit-identical" : "DIVERGED");
  }

  // --- warmup: настоящий геймплей до момента снапшота (поедание/респавн/FSM успели наработать) ---
  tf::actor_world_slice a;
  a.init(count, mn, mx, tex, brains);
  utils::timelines clocks_a(utils::simulation_rate(60));
  for (int i = 0; i < 60; ++i) {
    const auto tick = clocks_a.simulation_now() + utils::simulation_duration{1};
    a.update(tick, clocks_a.advance_simulation(tick), batch_a, pool);
  }

  // --- save → load в чистый слайс ---
  const auto packet = a.save();
  std::printf("saved packet: %zu bytes (world + sim scalars, sealed)\n", packet.size());

  tf::actor_world_slice b;
  if (!b.load(packet, brains)) {
    std::printf("RESUME FAILED: load returned false\n");
    return 1;
  }
  utils::timelines clocks_b(utils::simulation_rate(1));
  CHECK(clocks_b.restore_causal_state(clocks_a.causal_state()));

  // --- сразу после load миры должны быть побайтово равны ---
  const bool equal_after_load = dump(a) == dump(b);
  CHECK(equal_after_load);
  std::printf("post-load world bytes equal: %s\n", equal_after_load ? "yes" : "NO");

  // --- гоняем оба дальше синхронно: должны оставаться идентичны тик-в-тик ---
  int diverged_at = -1;
  for (int i = 1; i <= 120 && diverged_at < 0; ++i) {
    const auto tick_a = clocks_a.simulation_now() + utils::simulation_duration{1};
    const auto tick_b = clocks_b.simulation_now() + utils::simulation_duration{1};
    const auto dt_a = clocks_a.advance_simulation(tick_a);
    const auto dt_b = clocks_b.advance_simulation(tick_b);
    CHECK(tick_a == tick_b);
    CHECK(dt_a == dt_b);
    const auto ma = a.update(tick_a, dt_a, batch_a, pool);
    const auto mb = b.update(tick_b, dt_b, batch_b, pool);
    if (ma.actors != mb.actors || ma.ticks != mb.ticks || dump(a) != dump(b)) {
      diverged_at = i;
    }
  }
  CHECK(diverged_at < 0);
  if (diverged_at >= 0) {
    std::printf("DIVERGED at resumed tick %d\n", diverged_at);
  } else {
    std::printf("120 resumed ticks: A and B bit-identical\n");
  }

  if (failures) {
    std::printf("RESUME FAILED: %d check(s)\n", failures);
    return 1;
  }
  std::printf("RESUME OK: gameplay save/load round-trips + deterministically resumes\n");
  return 0;
}
