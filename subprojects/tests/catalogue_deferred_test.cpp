#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

#include <devils_engine/catalogue/deferred.h>
#include <devils_engine/catalogue/introspection.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_script/container.h>
#include <devils_script/context.h>
#include <devils_script/system.h>
#include <doctest/doctest.h>

namespace catalogue = devils_engine::catalogue;
namespace mt = devils_engine::catalogue::mt;
namespace ds = devils_script;

namespace {

enum class event_kind : uint8_t {
  strength,
  prey
};

struct event {
  event_kind kind;
  uint64_t source;
  int64_t target;
  int64_t value;
};

struct test_state {
  std::vector<event> events;
};

struct gameplay_scope {
  uint64_t id = 0;
  test_state* state = nullptr;

  bool valid() const noexcept {
    return state != nullptr;
  }
};

void add_strength(gameplay_scope scope, const int64_t target, const int64_t value) {
  scope.state->events.push_back(event{event_kind::strength, scope.id, target, value});
}

void eat_prey(gameplay_scope scope, const int64_t target) {
  scope.state->events.push_back(event{event_kind::prey, scope.id, target, 0});
}

struct strength_strategy : mt::collect<
                             mt::key::entity_arg<1>,
                             mt::order::source_then_sequence,
                             mt::commit::parallel_groups> {};

struct prey_strategy : mt::elect<
                         mt::key::entity_arg<1>,
                         mt::order::source_then_sequence,
                         mt::commit::serial,
                         mt::conflict::target_not_source> {};

using strength_traits = mt::domain<strength_strategy>::fn_traits<
  &add_strength, "add_strength", "scope", "target", "value">;
using prey_traits = mt::domain<prey_strategy>::fn_traits<
  &eat_prey, "eat_prey", "scope", "target">;

enum class trace_domain : uint64_t {
  gameplay = 1
};

using traced_strength_traits = catalogue::domain<trace_domain::gameplay>::fn_traits<
  &add_strength, "add_strength", "scope", "target", "value">;
using deferred_traced_strength_traits = mt::domain<strength_strategy>::fn_traits<
  traced_strength_traits::fn_ptr, "add_strength", "scope", "target", "value">;

template <typename Strategy>
class executor_binding {
public:
  explicit executor_binding(mt::executor<Strategy>& executor) {
    mt::domain<Strategy>::set_executor(&executor);
  }

  ~executor_binding() {
    mt::domain<Strategy>::set_executor(nullptr);
  }

  executor_binding(const executor_binding&) = delete;
  executor_binding& operator=(const executor_binding&) = delete;
};

struct counter_scope {
  uint64_t id = 0;
  std::array<std::atomic<uint32_t>, 4>* counters = nullptr;
};

void increment_group(counter_scope scope, const uint32_t target) {
  (*scope.counters)[target].fetch_add(1, std::memory_order_relaxed);
}

struct parallel_strategy : mt::collect<
                             mt::key::entity_arg<1>,
                             mt::order::source_then_sequence,
                             mt::commit::parallel_groups> {};

using parallel_traits = mt::domain<parallel_strategy>::fn_traits<
  &increment_group, "increment_group", "scope", "target">;

} // namespace

TEST_CASE("catalogue deferred pointer mirrors an effect and records multiple calls per source") {
  static_assert(std::is_same_v<decltype(strength_traits::fn_deferred_ptr),
                               void (*const)(gameplay_scope, int64_t, int64_t)>);
  static_assert(mt::deferred_payload_size == 128);

  mt::executor<strength_strategy> executor;
  executor_binding binding(executor);
  executor.begin_record(1, 4, 2);
  CHECK(executor.call_capacity() == 2);

  test_state state;
  const gameplay_scope scope{42, &state};
  {
    mt::record_scope source(42, 0);
    strength_traits::fn_deferred_ptr(scope, 7, 5);
    strength_traits::fn_deferred_ptr(scope, 7, 3);
    CHECK(source.next_local_ordinal() == 2);
  }

  CHECK(state.events.empty()); // native body is deferred
  executor.seal();
  REQUIRE(executor.size() == 2);
  REQUIRE(executor.group_count() == 1);
  CHECK(executor.metadata(0).source == 42);
  CHECK(executor.metadata(0).local_sequence == 0);
  CHECK(executor.metadata(1).local_sequence == 1);

  executor.commit();
  REQUIRE(state.events.size() == 2);
  CHECK(state.events[0].value == 5);
  CHECK(state.events[1].value == 3);
}

TEST_CASE("catalogue collect restores deterministic source order after parallel recording") {
  constexpr size_t count = 256;
  mt::executor<strength_strategy> executor;
  executor_binding binding(executor);
  executor.begin_record(count, 2, count);

  test_state state;
  devils_engine::thread::atomic_pool pool(4);
  pool.distribute1(count, [&state](const size_t start, const size_t size) {
    for (size_t i = start; i < start + size; ++i) {
      const uint64_t source_id = uint64_t(count - i);
      mt::record_scope source(source_id, i);
      strength_traits::fn_deferred_ptr(gameplay_scope{source_id, &state}, 11, int64_t(source_id));
    }
  });
  pool.compute();
  pool.wait();

  executor.seal();
  executor.commit();
  REQUIRE(state.events.size() == count);
  for (size_t i = 0; i < count; ++i) {
    CHECK(state.events[i].source == i + 1);
  }
}

TEST_CASE("catalogue dense journal capacity is independent from sparse source indices") {
  mt::executor<strength_strategy> executor;
  executor_binding binding(executor);
  executor.begin_record(1'000'000, 16, 2);

  test_state state;
  {
    mt::record_scope source(20, 999'999);
    strength_traits::fn_deferred_ptr(gameplay_scope{20, &state}, 11, 20);
  }
  {
    mt::record_scope source(10, 123'456);
    strength_traits::fn_deferred_ptr(gameplay_scope{10, &state}, 11, 10);
  }

  CHECK(executor.source_capacity() == 1'000'000);
  CHECK(executor.call_capacity() == 2);
  executor.seal();
  CHECK(executor.metadata(0).local_sequence == 123'456ull * 16ull);
  CHECK(executor.metadata(1).local_sequence == 999'999ull * 16ull);
  executor.commit();
  REQUIRE(state.events.size() == 2);
  CHECK(state.events[0].source == 10);
  CHECK(state.events[1].source == 20);
}

TEST_CASE("catalogue elect commits the lowest deterministic source for each key") {
  mt::executor<prey_strategy> executor;
  executor_binding binding(executor);
  executor.begin_record(3, 3, 3);

  test_state state;
  {
    mt::record_scope source(90, 0);
    prey_traits::fn_deferred_ptr(gameplay_scope{90, &state}, 5);
  }
  {
    mt::record_scope source(10, 1);
    prey_traits::fn_deferred_ptr(gameplay_scope{10, &state}, 5);
  }
  {
    mt::record_scope source(40, 2);
    prey_traits::fn_deferred_ptr(gameplay_scope{40, &state}, 6);
  }

  executor.seal();
  REQUIRE(executor.group_count() == 2);
  executor.commit();

  REQUIRE(state.events.size() == 2);
  CHECK(state.events[0].target == 5);
  CHECK(state.events[0].source == 10);
  CHECK(state.events[1].target == 6);
  CHECK(state.events[1].source == 40);
}

TEST_CASE("catalogue elect can protect targets that are themselves interaction sources") {
  mt::executor<prey_strategy> executor;
  executor_binding binding(executor);
  executor.begin_record(2, 2, 2);

  test_state state;
  {
    mt::record_scope source(10, 0);
    prey_traits::fn_deferred_ptr(gameplay_scope{10, &state}, 20);
  }
  {
    mt::record_scope source(20, 1);
    prey_traits::fn_deferred_ptr(gameplay_scope{20, &state}, 30);
  }

  executor.seal();
  REQUIRE(executor.group_count() == 2);
  CHECK_FALSE(executor.group(0).eligible); // target 20 is also an eater/source
  CHECK(executor.group(1).eligible);
  executor.commit();

  REQUIRE(state.events.size() == 1);
  CHECK(state.events[0].source == 20);
  CHECK(state.events[0].target == 30);
}

TEST_CASE("catalogue parallel_groups exposes independent groups for worker commit") {
  mt::executor<parallel_strategy> executor;
  executor_binding binding(executor);
  executor.begin_record(64, 2, 64);

  std::array<std::atomic<uint32_t>, 4> counters{};
  for (size_t i = 0; i < 64; ++i) {
    mt::record_scope source(i + 1, i);
    parallel_traits::fn_deferred_ptr(counter_scope{i + 1, &counters}, uint32_t(i % counters.size()));
  }
  executor.seal();
  REQUIRE(executor.group_count() == counters.size());
  static_assert(mt::executor<parallel_strategy>::parallel_groups);

  devils_engine::thread::atomic_pool pool(4);
  pool.distribute1(executor.group_count(), [&executor](const size_t start, const size_t count) {
    for (size_t i = start; i < start + count; ++i)
      executor.dispatch_group(i);
  });
  pool.compute();
  pool.wait();
  executor.finish_commit();

  CHECK(executor.phase() == mt::executor_phase::committed);
  for (const auto& counter : counters)
    CHECK(counter.load(std::memory_order_relaxed) == 16);
}

TEST_CASE("catalogue trace domain stays independent and observes deferred commit") {
  mt::executor<strength_strategy> executor;
  executor_binding binding(executor);
  executor.begin_record(1, 2, 1);

  catalogue::statistics_store statistics(4);
  const catalogue::introspection trace{
    catalogue::introspection_mode::statistics,
    0,
    &statistics};
  catalogue::domain<trace_domain::gameplay>::set_introspection(&trace);

  test_state state;
  {
    mt::record_scope source(7, 0);
    deferred_traced_strength_traits::fn_deferred_ptr(gameplay_scope{7, &state}, 3, 9);
  }
  CHECK(statistics.count() == 0); // record itself is not the gameplay call

  executor.seal();
  executor.commit();
  CHECK(statistics.count() == 1); // trace wraps the native body at commit-time
  REQUIRE(state.events.size() == 1);

  catalogue::domain<trace_domain::gameplay>::set_introspection(nullptr);
}

TEST_CASE("devils_script records independent collect and elect effects through deferred pointers") {
  mt::executor<strength_strategy> strength_executor;
  mt::executor<prey_strategy> prey_executor;
  executor_binding strength_binding(strength_executor);
  executor_binding prey_binding(prey_executor);
  strength_executor.begin_record(1, 4, 1);
  prey_executor.begin_record(1, 4, 1);

  ds::system system;
  system.init_basic_functions();
  system.init_math();
  system.register_function<strength_traits::fn_deferred_ptr>("add_strength");
  system.register_function<prey_traits::fn_deferred_ptr>("eat_prey");

  const auto script = system.parse<void, gameplay_scope>(
    "deferred_effects",
    "{ { condition = true, add_strength = { 5, 7 } }, { condition = true, eat_prey = 5 } }");

  test_state state;
  ds::context vm;
  vm.set_arg(script.find_arg("root"), gameplay_scope{12, &state});
  {
    mt::record_scope source(12, 0);
    script.process(&vm);
  }

  CHECK(state.events.empty());
  strength_executor.seal();
  prey_executor.seal();
  REQUIRE(strength_executor.size() == 1);
  REQUIRE(prey_executor.size() == 1);
  CHECK(strength_executor.metadata(0).local_sequence == 0);
  CHECK(prey_executor.metadata(0).local_sequence == 1);

  strength_executor.commit();
  prey_executor.commit();
  REQUIRE(state.events.size() == 2);
  CHECK(state.events[0].kind == event_kind::strength);
  CHECK(state.events[0].value == 7);
  CHECK(state.events[1].kind == event_kind::prey);
}

TEST_CASE("catalogue deferred calls fail loudly without executor, scope, or capacity") {
  mt::domain<strength_strategy>::set_executor(nullptr);
  CHECK_THROWS(strength_traits::fn_deferred_ptr(gameplay_scope{}, 0, 0));

  mt::executor<strength_strategy> executor;
  executor_binding binding(executor);
  executor.begin_record(1, 2, 1);
  CHECK_THROWS(strength_traits::fn_deferred_ptr(gameplay_scope{}, 0, 0));
  test_state state;
  {
    mt::record_scope source(1, 0);
    strength_traits::fn_deferred_ptr(gameplay_scope{1, &state}, 0, 0);
    CHECK_THROWS(strength_traits::fn_deferred_ptr(gameplay_scope{1, &state}, 0, 0));
  }
  executor.seal();
  executor.commit();
}
