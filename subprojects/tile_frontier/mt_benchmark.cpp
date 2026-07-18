// Full tile_frontier actor-pipeline scaling benchmark. Every worker-count run starts from the same
// deterministic world, executes the same number of ticks, and must produce byte-identical world data.
// Timing excludes init, warmup and serialization/hash verification.
//
//   ./tile_frontier_mt_benchmark [entities] [measured_ticks] [warmup_ticks]

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <devils_engine/aesthetics/serialization.h>
#include <devils_engine/catalogue/introspection.h>
#include <devils_engine/thread/atomic_pool.h>
#include <spdlog/spdlog.h>

#include "core/actor_simulation.h"
#include "test_brain_fixture.h"

using namespace devils_engine;
namespace tf = tile_frontier::core;

namespace {

struct run_result {
  struct phase_result {
    std::string name;
    double average_us = 0.0;
    uint64_t min_us = 0;
    uint64_t max_us = 0;
    uint64_t calls = 0;
  };

  size_t workers = 0;
  double elapsed_ms = 0.0;
  std::vector<std::byte> world;
  uint64_t world_hash = 0;
  std::vector<phase_result> phases;
};

uint64_t hash_bytes(const std::vector<std::byte>& bytes) noexcept {
  uint64_t value = 1469598103934665603ull;
  for (const std::byte byte : bytes) {
    value ^= std::to_integer<uint8_t>(byte);
    value *= 1099511628211ull;
  }
  return value;
}

run_result run(const size_t workers, const uint32_t entity_count,
               const size_t warmup_ticks, const size_t measured_ticks,
               const tf::brain_config& brains) {
  const glm::vec2 min_bound{0.5f, 0.5f};
  const glm::vec2 max_bound{64.0f, 64.0f};
  constexpr uint32_t texture_count = 4;
  constexpr uint64_t dt = devils_engine::utils::timeline_ticks_per_second / 60; // µs game-времени за тик

  thread::atomic_pool pool(workers);
  tf::actor_world_slice slice;
  tf::actor_batch batch;
  batch.bind("v2ui1c4v1");
  if (!batch.valid()) {
    throw std::runtime_error("tile_frontier benchmark actor batch layout is invalid");
  }
  slice.init(entity_count, min_bound, max_bound, texture_count, brains);

  for (size_t i = 0; i < warmup_ticks; ++i) {
    slice.update(dt, batch, pool);
  }
  tf::reset_actor_perf_statistics();

  const auto begin = std::chrono::steady_clock::now();
  for (size_t i = 0; i < measured_ticks; ++i) {
    slice.update(dt, batch, pool);
  }
  const auto end = std::chrono::steady_clock::now();

  run_result result;
  result.workers = workers;
  result.elapsed_ms = std::chrono::duration<double, std::milli>(end - begin).count();
  result.world = aesthetics::serial::dump_world(&slice.ecs());
  result.world_hash = hash_bytes(result.world);
  tf::actor_perf_statistics().for_each(
    [&result](const catalogue::statistics_store::function_record& rec) {
      result.phases.push_back(run_result::phase_result{
        std::string(rec.name), rec.average_mcs(), rec.min_mcs, rec.max_mcs, rec.call_count});
    });
  std::sort(result.phases.begin(), result.phases.end(), [](const auto& a, const auto& b) {
    return a.name < b.name;
  });
  return result;
}

const run_result::phase_result* find_phase(const run_result& result, const std::string_view name) {
  const auto it = std::find_if(result.phases.begin(), result.phases.end(), [name](const auto& phase) {
    return phase.name == name;
  });
  return it != result.phases.end() ? &*it : nullptr;
}

} // namespace

int main(int argc, char** argv) {
  spdlog::set_level(spdlog::level::warn);

  test_brain_fixture fixture(TILE_FRONTIER_SOURCE_RESOURCE_ROOT);

  const uint32_t entity_count = argc > 1 ? uint32_t(std::stoul(argv[1])) : 4096u;
  const size_t measured_ticks = argc > 2 ? size_t(std::stoull(argv[2])) : 120u;
  const size_t warmup_ticks = argc > 3 ? size_t(std::stoull(argv[3])) : 30u;
  if (entity_count == 0 || measured_ticks == 0) {
    std::cerr << "entities and measured_ticks must be non-zero\n";
    return 2;
  }

  constexpr std::array<size_t, 4> worker_counts{1, 2, 4, 8};
  std::vector<run_result> results;
  results.reserve(worker_counts.size());
  for (const size_t workers : worker_counts) {
    results.push_back(run(workers, entity_count, warmup_ticks, measured_ticks, fixture.config()));
  }

  const auto& baseline = results.front();
  const double baseline_ms = baseline.elapsed_ms;
  bool deterministic = true;

  std::cout << "tile_frontier actor pipeline: entities=" << entity_count
            << ", warmup_ticks=" << warmup_ticks
            << ", measured_ticks=" << measured_ticks << '\n';
  std::cout << "workers  total_ms  ms/tick  speedup  world_hash         identical\n";
  for (const auto& result : results) {
    const bool identical = result.world_hash == baseline.world_hash && result.world == baseline.world;
    deterministic &= identical;
    std::cout << std::setw(7) << result.workers << "  "
              << std::fixed << std::setprecision(3) << std::setw(8) << result.elapsed_ms << "  "
              << std::setw(7) << result.elapsed_ms / double(measured_ticks) << "  "
              << std::setw(7) << baseline_ms / result.elapsed_ms << "  "
              << "0x" << std::hex << std::setw(16) << std::setfill('0') << result.world_hash
              << std::dec << std::setfill(' ') << "  " << (identical ? "yes" : "NO") << '\n';
  }

  constexpr std::array<std::string_view, 8> phase_names{
    "sense.gather", "tail.sense+batch", "cognition", "apply",
    "integration+drives", "eating", "food", "build"};
  std::cout << "\nphase average (us/tick; min..max over measured ticks)\n";
  std::cout << std::left << std::setw(20) << "phase";
  for (const auto& result : results) {
    std::cout << std::right << std::setw(18) << (std::to_string(result.workers) + " workers");
  }
  std::cout << '\n';
  for (const std::string_view name : phase_names) {
    std::cout << std::left << std::setw(20) << name;
    for (const auto& result : results) {
      const auto* phase = find_phase(result, name);
      if (phase == nullptr) {
        std::cout << std::right << std::setw(18) << "n/a";
        continue;
      }
      const std::string cell = std::to_string(uint64_t(phase->average_us + 0.5)) + " (" +
                               std::to_string(phase->min_us) + ".." +
                               std::to_string(phase->max_us) + ")";
      std::cout << std::right << std::setw(18) << cell;
    }
    std::cout << '\n';
  }
  std::cout << "note: tail.sense+batch is the shared barrier elapsed time; actor batch runs inside "
               "that interval, so overlapping shares are not additive\n";

  std::cout << "\nphase share of wall-clock tick\n";
  std::cout << std::left << std::setw(20) << "phase";
  for (const auto& result : results) {
    std::cout << std::right << std::setw(12) << (std::to_string(result.workers) + "w");
  }
  std::cout << '\n';
  for (const std::string_view name : phase_names) {
    std::cout << std::left << std::setw(20) << name;
    for (const auto& result : results) {
      const auto* phase = find_phase(result, name);
      const double wall_us = result.elapsed_ms * 1000.0 / double(measured_ticks);
      const double share = phase != nullptr && wall_us != 0.0 ? 100.0 * phase->average_us / wall_us : 0.0;
      std::cout << std::right << std::fixed << std::setprecision(1) << std::setw(11) << share << '%';
    }
    std::cout << '\n';
  }

  if (!deterministic) {
    std::cerr << "FAILED: world hash/bytes diverged between worker counts\n";
    return 1;
  }
  std::cout << "OK: 1/2/4/8 workers produced identical world hash and bytes\n";
  return 0;
}
