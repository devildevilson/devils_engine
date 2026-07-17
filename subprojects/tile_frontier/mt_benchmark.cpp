// Full tile_frontier actor-pipeline scaling benchmark. Every worker-count run starts from the same
// deterministic world, executes the same number of ticks, and must produce byte-identical world data.
// Timing excludes init, warmup and serialization/hash verification.
//
//   ./tile_frontier_mt_benchmark [entities] [measured_ticks] [warmup_ticks]

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <devils_engine/aesthetics/serialization.h>
#include <devils_engine/thread/atomic_pool.h>
#include <spdlog/spdlog.h>

#include "core/actor_simulation.h"

using namespace devils_engine;
namespace tf = tile_frontier::core;

namespace {

struct run_result {
  size_t workers = 0;
  double elapsed_ms = 0.0;
  std::vector<std::byte> world;
  uint64_t world_hash = 0;
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
               const size_t warmup_ticks, const size_t measured_ticks) {
  const glm::vec2 min_bound{0.5f, 0.5f};
  const glm::vec2 max_bound{64.0f, 64.0f};
  constexpr uint32_t texture_count = 4;
  constexpr float dt = 1.0f / 60.0f;

  thread::atomic_pool pool(workers);
  tf::actor_world_slice slice;
  tf::actor_batch batch;
  batch.bind("v2ui1c4v1");
  if (!batch.valid()) {
    throw std::runtime_error("tile_frontier benchmark actor batch layout is invalid");
  }
  slice.init(entity_count, min_bound, max_bound, texture_count);

  for (size_t i = 0; i < warmup_ticks; ++i) {
    slice.update(dt, batch, pool);
  }

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
  return result;
}

} // namespace

int main(int argc, char** argv) {
  spdlog::set_level(spdlog::level::warn);

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
    results.push_back(run(workers, entity_count, warmup_ticks, measured_ticks));
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

  if (!deterministic) {
    std::cerr << "FAILED: world hash/bytes diverged between worker counts\n";
    return 1;
  }
  std::cout << "OK: 1/2/4/8 workers produced identical world hash and bytes\n";
  return 0;
}
