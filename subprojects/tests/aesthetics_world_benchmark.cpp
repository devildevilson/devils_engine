#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "devils_engine/aesthetics/world.h"

using namespace devils_engine;

namespace {

struct position {
  int x = 0;
  int y = 0;
};

struct velocity {
  int x = 0;
  int y = 0;
};

struct renderable {
  int layer = 0;
};

volatile std::uint64_t sink = 0;

template <typename Range_T>
std::uint64_t consume_range(const Range_T& range) {
  std::uint64_t sum = 0;
  for (const auto& tuple : range) {
    sum += aesthetics::get_entityid_index(std::get<0>(tuple));
    std::apply([&sum](const auto, const auto*... ptrs) {
      ((sum += ptrs != nullptr ? static_cast<std::uint64_t>(ptrs->x + 1) : 0), ...);
    },
               tuple);
  }
  return sum;
}

template <typename F>
void bench(const std::string_view name, const size_t iterations, F&& f) {
  std::uint64_t local = 0;
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < iterations; ++i) {
    local += f();
  }
  const auto end = std::chrono::steady_clock::now();
  sink = local;

  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  const double per_iter_us = static_cast<double>(ns) / 1000.0 / static_cast<double>(iterations);
  std::cout << name << ": " << per_iter_us << " us/iteration, checksum=" << local << '\n';
}

void print_group(const std::string_view name) {
  std::cout << "\n[" << name << "]\n";
}

std::vector<aesthetics::entityid_t> make_ids(const size_t entity_count) {
  std::vector<aesthetics::entityid_t> ids;
  ids.reserve(entity_count);
  for (size_t i = 0; i < entity_count; ++i) {
    ids.push_back(aesthetics::make_entityid(i, 0));
  }
  return ids;
}

void populate(aesthetics::world& world, const std::vector<aesthetics::entityid_t>& ids) {
  for (size_t i = 0; i < ids.size(); ++i) {
    world.create<position>(ids[i], static_cast<int>(i), static_cast<int>(i * 2));
    if ((i % 2) == 0) {
      world.create<velocity>(ids[i], static_cast<int>(i + 1), static_cast<int>(i + 2));
    }
    if ((i % 3) == 0) {
      world.create<renderable>(ids[i], static_cast<int>(i % 7));
    }
  }
}

void create_positions(aesthetics::world& world, const std::vector<aesthetics::entityid_t>& ids) {
  for (size_t i = 0; i < ids.size(); ++i) {
    world.create<position>(ids[i], static_cast<int>(i), static_cast<int>(i + 1));
  }
}

std::uint64_t remove_positions_forward(aesthetics::world& world, const std::vector<aesthetics::entityid_t>& ids) {
  std::uint64_t removed = 0;
  for (const auto id : ids) {
    removed += world.remove<position>(id) ? 1 : 0;
  }
  return removed;
}

std::uint64_t remove_positions_reverse(aesthetics::world& world, const std::vector<aesthetics::entityid_t>& ids) {
  std::uint64_t removed = 0;
  for (size_t i = ids.size(); i > 0; --i) {
    removed += world.remove<position>(ids[i - 1]) ? 1 : 0;
  }
  return removed;
}

void warm_position_storage(aesthetics::world& world, const std::vector<aesthetics::entityid_t>& ids) {
  create_positions(world, ids);
  remove_positions_reverse(world, ids);
}

template <typename Setup_T, typename Run_T, typename Cleanup_T>
void bench_measured_phase(const std::string_view name, const size_t iterations, Setup_T&& setup, Run_T&& run, Cleanup_T&& cleanup) {
  std::uint64_t local = 0;
  std::chrono::nanoseconds total{0};
  for (size_t i = 0; i < iterations; ++i) {
    setup();
    const auto start = std::chrono::steady_clock::now();
    local += run();
    const auto end = std::chrono::steady_clock::now();
    cleanup();
    total += end - start;
  }
  sink = local;

  const double per_iter_us = static_cast<double>(total.count()) / 1000.0 / static_cast<double>(iterations);
  std::cout << name << ": " << per_iter_us << " us/iteration, checksum=" << local << '\n';
}

} // namespace

int main(int argc, char** argv) {
  const size_t entity_count = argc > 1 ? static_cast<size_t>(std::stoull(argv[1])) : 100000;
  const size_t iterations = argc > 2 ? static_cast<size_t>(std::stoull(argv[2])) : 100;
  const size_t mutation_count = argc > 3 ? static_cast<size_t>(std::stoull(argv[3])) : std::min<size_t>(entity_count, 10000);

  aesthetics::world world;
  const auto ids = make_ids(entity_count);
  populate(world, ids);

  std::cout << "entities=" << entity_count
            << ", iterations=" << iterations
            << ", mutation_count=" << mutation_count << '\n';
  std::cout << "position=" << world.count<position>()
            << ", velocity=" << world.count<velocity>()
            << ", renderable=" << world.count<renderable>() << '\n';

  auto query_1 = world.query<position>();
  auto lazy_query_1 = world.lazy_query<position>();
  auto query_2 = world.query<position, velocity>();
  auto lazy_query_2 = world.lazy_query<position, velocity>();
  const auto lazy_view_1 = world.lazy_view<position>();
  const auto lazy_view_2 = world.lazy_view<position, velocity>();

  print_group("all-component traversal");
  bench("view<position> construct+iterate", iterations, [&world]() {
    return consume_range(world.view<position>());
  });
  bench("query<position> prebuilt iterate", iterations, [&query_1]() {
    return consume_range(query_1);
  });
  bench("view<position, velocity> construct+iterate", iterations, [&world]() {
    return consume_range(world.view<position, velocity>());
  });
  bench("query<position, velocity> prebuilt iterate", iterations, [&query_2]() {
    return consume_range(query_2);
  });

  print_group("lazy/any-component traversal");
  bench("lazy_view<position> prebuilt iterate", iterations, [&lazy_view_1]() {
    return consume_range(lazy_view_1);
  });
  bench("lazy_query<position> prebuilt iterate", iterations, [&lazy_query_1]() {
    return consume_range(lazy_query_1);
  });
  bench("lazy_view<position, velocity> prebuilt iterate", iterations, [&lazy_view_2]() {
    return consume_range(lazy_view_2);
  });
  bench("lazy_query<position, velocity> prebuilt iterate", iterations, [&lazy_query_2]() {
    return consume_range(lazy_query_2);
  });
  bench("lazy_view<position, velocity> construct+iterate", std::max<size_t>(iterations / 10, 1), [&world]() {
    return consume_range(world.lazy_view<position, velocity>());
  });

  const auto mutation_ids = make_ids(mutation_count);

  print_group("component mutation");
  {
    std::unique_ptr<aesthetics::world> local_world;
    bench_measured_phase(
      "create<position> raw storage",
      iterations,
      [&local_world]() {
        local_world = std::make_unique<aesthetics::world>();
      },
      [&local_world, &mutation_ids]() {
        create_positions(*local_world, mutation_ids);
        return local_world->count<position>();
      },
      [&local_world]() {
        local_world.reset();
      });
  }

  {
    aesthetics::world warm_world;
    warm_position_storage(warm_world, mutation_ids);
    bench_measured_phase(
      "create<position> warm storage",
      iterations,
      []() {},
      [&warm_world, &mutation_ids]() {
        create_positions(warm_world, mutation_ids);
        return warm_world.count<position>();
      },
      [&warm_world, &mutation_ids]() {
        remove_positions_reverse(warm_world, mutation_ids);
      });
  }

  {
    std::unique_ptr<aesthetics::world> local_world;
    bench_measured_phase(
      "remove<position> raw storage forward",
      std::max<size_t>(iterations / 10, 1),
      [&local_world, &mutation_ids]() {
        local_world = std::make_unique<aesthetics::world>();
        create_positions(*local_world, mutation_ids);
      },
      [&local_world, &mutation_ids]() {
        return remove_positions_forward(*local_world, mutation_ids);
      },
      [&local_world]() {
        local_world.reset();
      });
  }

  {
    std::unique_ptr<aesthetics::world> local_world;
    bench_measured_phase(
      "remove<position> raw storage reverse",
      iterations,
      [&local_world, &mutation_ids]() {
        local_world = std::make_unique<aesthetics::world>();
        create_positions(*local_world, mutation_ids);
      },
      [&local_world, &mutation_ids]() {
        return remove_positions_reverse(*local_world, mutation_ids);
      },
      [&local_world]() {
        local_world.reset();
      });
  }

  {
    aesthetics::world warm_world;
    warm_position_storage(warm_world, mutation_ids);
    bench_measured_phase(
      "remove<position> warm storage forward",
      std::max<size_t>(iterations / 10, 1),
      [&warm_world, &mutation_ids]() {
        create_positions(warm_world, mutation_ids);
      },
      [&warm_world, &mutation_ids]() {
        return remove_positions_forward(warm_world, mutation_ids);
      },
      []() {
      });
  }

  {
    aesthetics::world warm_world;
    warm_position_storage(warm_world, mutation_ids);
    bench_measured_phase(
      "remove<position> warm storage reverse",
      iterations,
      [&warm_world, &mutation_ids]() {
        create_positions(warm_world, mutation_ids);
      },
      [&warm_world, &mutation_ids]() {
        return remove_positions_reverse(warm_world, mutation_ids);
      },
      []() {
      });
  }

  return sink == 0 ? 1 : 0;
}
