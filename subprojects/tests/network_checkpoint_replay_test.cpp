#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <devils_engine/network/replay.h>
#include <devils_engine/network/checkpoint_ring.h>
#include <doctest/doctest.h>

namespace network = devils_engine::network;

namespace {

struct tick_id {
  std::uint32_t value = 0;
  auto operator<=>(const tick_id&) const = default;
};

struct next_tick {
  std::optional<tick_id> operator()(const tick_id tick) const noexcept {
    if (tick.value == UINT32_MAX) return std::nullopt;
    return tick_id{tick.value + 1};
  }
};

struct checkpoint_blob {
  std::int64_t causal_value = 0;
  std::size_t retained_bytes = 0;
  bool operator==(const checkpoint_blob&) const = default;
};

struct checkpoint_size {
  std::size_t operator()(const checkpoint_blob& checkpoint) const noexcept {
    return checkpoint.retained_bytes;
  }
};

struct intent_bundle {
  std::vector<std::int64_t> intents;
  bool operator==(const intent_bundle&) const = default;
};

struct bundle_entry {
  tick_id tick;
  intent_bundle bundle;
};

struct fake_world {
  std::int64_t causal_value = 0;
  std::int64_t pending_intents = 0;
  std::size_t presentation_events = 0;
};

using ring = network::checkpoint_ring<tick_id, checkpoint_blob, checkpoint_size>;
using bundle_history = network::bounded_history<tick_id, intent_bundle>;

static_assert(std::same_as<decltype(std::declval<ring&>().find(tick_id{})),
                           const checkpoint_blob*>);
static_assert(std::same_as<decltype(std::declval<ring&>().entries()),
                           const std::deque<ring::entry>&>);

void apply_causal(fake_world& world, const intent_bundle& bundle) {
  for (const std::int64_t value : bundle.intents) {
    world.pending_intents += value;
  }
}

void step_causal(fake_world& world, const tick_id tick) {
  world.causal_value = world.causal_value * 3 + world.pending_intents + tick.value;
  world.pending_intents = 0;
}

struct generated_run {
  static constexpr std::uint32_t last_tick = 6;

  ring checkpoints{last_tick + 1, 1024};
  bundle_history bundles{last_tick, 1024};
  std::vector<std::int64_t> roots = std::vector<std::int64_t>(last_tick + 1);

  generated_run() {
    const std::vector<intent_bundle> input{
      {{2}}, {{-1, 4}}, {}, {{8}}, {{-3, 1}}, {{5}}};

    fake_world world{7};
    roots[0] = world.causal_value;
    REQUIRE(checkpoints.try_store(tick_id{0}, {world.causal_value, 8}).stored());
    for (std::uint32_t value = 1; value <= last_tick; ++value) {
      const intent_bundle& bundle = input[value - 1];
      REQUIRE(
        bundles.try_store(tick_id{value}, bundle, bundle.intents.size() * 8).stored());
      apply_causal(world, bundle);
      step_causal(world, tick_id{value});
      roots[value] = world.causal_value;
      REQUIRE(
        checkpoints.try_store(tick_id{value}, {world.causal_value, 8}).stored());
    }
  }
};

auto restore_operation(std::size_t& calls) {
  return [&calls](fake_world& world, const checkpoint_blob& checkpoint) {
    ++calls;
    world.causal_value = checkpoint.causal_value;
    world.pending_intents = 0;
    return true;
  };
}

auto causal_apply_operation(std::size_t& calls) {
  return [&calls](
           fake_world& world,
           const intent_bundle& bundle,
           const network::replay_context context) {
    CHECK(context.presentation_suppressed());
    if (!context.presentation_suppressed()) ++world.presentation_events;
    ++calls;
    apply_causal(world, bundle);
    return true;
  };
}

auto causal_step_operation(std::size_t& calls) {
  return [&calls](
           fake_world& world,
           const tick_id tick,
           const network::replay_context context) {
    CHECK(context.presentation_suppressed());
    if (!context.presentation_suppressed()) ++world.presentation_events;
    ++calls;
    step_causal(world, tick);
    return true;
  };
}

} // namespace

TEST_CASE("network checkpoint ring retains immutable blobs within both budgets") {
  SUBCASE("byte budget") {
    ring checkpoints(3, 9);
    REQUIRE(checkpoints.try_store(tick_id{2}, {20, 3}).stored());
    REQUIRE(checkpoints.try_store(tick_id{4}, {40, 4}).stored());

    const auto result = checkpoints.try_store(tick_id{8}, {80, 5});
    CHECK(result.stored());
    CHECK(result.evicted_count == 1);
    CHECK(result.evicted_bytes == 3);
    CHECK(checkpoints.retained_count() == 2);
    CHECK(checkpoints.retained_bytes() == 9);
    CHECK(checkpoints.find(tick_id{2}) == nullptr);
    REQUIRE(checkpoints.find(tick_id{4}) != nullptr);
    CHECK(checkpoints.find(tick_id{4})->causal_value == 40);

    CHECK(checkpoints.latest_at_or_before(tick_id{3}) == nullptr);
    REQUIRE(checkpoints.latest_at_or_before(tick_id{4}) != nullptr);
    CHECK(checkpoints.latest_at_or_before(tick_id{7})->tick == tick_id{4});
    CHECK(checkpoints.latest_at_or_before(tick_id{99})->tick == tick_id{8});

    CHECK(
      checkpoints.try_store(tick_id{8}, {81, 1}).status ==
      network::history_store_status::duplicate_tick);
    CHECK(
      checkpoints.try_store(tick_id{6}, {60, 1}).status ==
      network::history_store_status::out_of_order);
    CHECK(
      checkpoints.try_store(tick_id{9}, {90, 10}).status ==
      network::history_store_status::budget_exceeded);
    CHECK(checkpoints.retained_count() == 2);
    CHECK(checkpoints.retained_bytes() == 9);
  }

  SUBCASE("count budget") {
    ring checkpoints(2, 100);
    REQUIRE(checkpoints.try_store(tick_id{1}, {10, 3}).stored());
    REQUIRE(checkpoints.try_store(tick_id{2}, {20, 4}).stored());
    const auto result = checkpoints.try_store(tick_id{3}, {30, 5});
    CHECK(result.stored());
    CHECK(result.evicted_count == 1);
    CHECK(result.evicted_bytes == 3);
    CHECK(checkpoints.oldest_tick() == tick_id{2});
    CHECK(checkpoints.newest_tick() == tick_id{3});
    CHECK(checkpoints.retained_bytes() == 9);
  }
}

TEST_CASE("network replay from every checkpoint reproduces every uninterrupted root") {
  generated_run run;

  for (std::uint32_t checkpoint_value = 0;
       checkpoint_value <= generated_run::last_tick;
       ++checkpoint_value) {
    CAPTURE(checkpoint_value);
    const auto* checkpoint = run.checkpoints.find(tick_id{checkpoint_value});
    REQUIRE(checkpoint != nullptr);

    fake_world replayed{999, 123, 17};
    std::size_t restore_calls = 0;
    std::size_t apply_calls = 0;
    std::size_t step_calls = 0;
    const auto verify = [&run](const fake_world& world, const tick_id tick) {
      return world.causal_value == run.roots[tick.value];
    };

    const auto result = network::replay_to(
      replayed,
      tick_id{checkpoint_value},
      *checkpoint,
      tick_id{generated_run::last_tick},
      run.bundles.entries(),
      restore_operation(restore_calls),
      causal_apply_operation(apply_calls),
      causal_step_operation(step_calls),
      verify,
      next_tick{});

    REQUIRE(result.completed());
    CHECK(result.replayed_ticks == generated_run::last_tick - checkpoint_value);
    CHECK(replayed.causal_value == run.roots.back());
    CHECK(replayed.pending_intents == 0);
    CHECK(replayed.presentation_events == 17);
    CHECK(restore_calls == 1);
    CHECK(apply_calls == result.replayed_ticks);
    CHECK(step_calls == result.replayed_ticks);
  }
}

TEST_CASE("network replay requires an explicit bundle for every tick") {
  const checkpoint_blob checkpoint{7, 8};
  std::size_t restore_calls = 0;
  const auto restore = restore_operation(restore_calls);
  const auto apply = [](
                       fake_world&,
                       const intent_bundle&,
                       network::replay_context) {
    return true;
  };
  const auto step = [](fake_world&, tick_id, network::replay_context) {
    return true;
  };
  const auto verify = [](const fake_world&, tick_id) {
    return true;
  };

  SUBCASE("evicted beginning") {
    const std::vector<bundle_entry> entries{{tick_id{2}, {}}, {tick_id{3}, {}}};
    fake_world world{};
    const auto result = network::replay_to(
      world,
      tick_id{0},
      checkpoint,
      tick_id{3},
      entries,
      restore,
      apply,
      step,
      verify,
      next_tick{});
    CHECK(result.status == network::replay_status::target_before_history);
    CHECK(result.tick == tick_id{1});
  }

  SUBCASE("gap after retained bundle") {
    const std::vector<bundle_entry> entries{{tick_id{1}, {}}, {tick_id{3}, {}}};
    fake_world world{};
    const auto result = network::replay_to(
      world,
      tick_id{0},
      checkpoint,
      tick_id{3},
      entries,
      restore,
      apply,
      step,
      verify,
      next_tick{});
    CHECK(result.status == network::replay_status::missing_bundle);
    CHECK(result.tick == tick_id{2});
  }

  SUBCASE("target beyond retained end") {
    const std::vector<bundle_entry> entries{{tick_id{1}, {}}, {tick_id{2}, {}}};
    fake_world world{};
    const auto result = network::replay_to(
      world,
      tick_id{0},
      checkpoint,
      tick_id{3},
      entries,
      restore,
      apply,
      step,
      verify,
      next_tick{});
    CHECK(result.status == network::replay_status::target_after_history);
    CHECK(result.tick == tick_id{3});
  }

  CHECK(restore_calls == 0);
}

TEST_CASE("network replay rejects range order faults before restoring") {
  const checkpoint_blob checkpoint{7, 8};
  std::size_t restore_calls = 0;
  const auto restore = restore_operation(restore_calls);
  const auto apply = [](
                       fake_world&,
                       const intent_bundle&,
                       network::replay_context) {
    return true;
  };
  const auto step = [](fake_world&, tick_id, network::replay_context) {
    return true;
  };
  const auto verify = [](const fake_world&, tick_id) {
    return true;
  };

  SUBCASE("duplicate") {
    const std::vector<bundle_entry> entries{
      {tick_id{1}, {}}, {tick_id{1}, {}}, {tick_id{2}, {}}};
    fake_world world{};
    const auto result = network::replay_to(
      world,
      tick_id{0},
      checkpoint,
      tick_id{2},
      entries,
      restore,
      apply,
      step,
      verify,
      next_tick{});
    CHECK(result.status == network::replay_status::duplicate_bundle);
    CHECK(result.tick == tick_id{1});
  }

  SUBCASE("out of order") {
    const std::vector<bundle_entry> entries{{tick_id{2}, {}}, {tick_id{1}, {}}};
    fake_world world{};
    const auto result = network::replay_to(
      world,
      tick_id{0},
      checkpoint,
      tick_id{2},
      entries,
      restore,
      apply,
      step,
      verify,
      next_tick{});
    CHECK(result.status == network::replay_status::out_of_order_bundle);
    CHECK(result.tick == tick_id{1});
  }

  SUBCASE("target before checkpoint") {
    const std::vector<bundle_entry> entries;
    fake_world world{};
    const auto result = network::replay_to(
      world,
      tick_id{2},
      checkpoint,
      tick_id{1},
      entries,
      restore,
      apply,
      step,
      verify,
      next_tick{});
    CHECK(result.status == network::replay_status::target_before_checkpoint);
    CHECK(result.tick == tick_id{1});
  }

  SUBCASE("tick successor unavailable") {
    const std::vector<bundle_entry> entries{{tick_id{1}, {}}};
    fake_world world{};
    const auto unavailable = [](tick_id) -> std::optional<tick_id> {
      return std::nullopt;
    };
    const auto result = network::replay_to(
      world,
      tick_id{0},
      checkpoint,
      tick_id{1},
      entries,
      restore,
      apply,
      step,
      verify,
      unavailable);
    CHECK(result.status == network::replay_status::tick_successor_unavailable);
    CHECK(result.tick == tick_id{0});
  }

  CHECK(restore_calls == 0);
}

TEST_CASE("network replay reports restore apply step and first state mismatch") {
  const checkpoint_blob checkpoint{7, 8};
  const std::vector<bundle_entry> entries{
    {tick_id{1}, {{2}}}, {tick_id{2}, {{3}}}, {tick_id{3}, {{4}}}};

  SUBCASE("restore refusal") {
    fake_world world{99};
    const auto result = network::replay_to(
      world,
      tick_id{0},
      checkpoint,
      tick_id{3},
      entries,
      [](fake_world&, const checkpoint_blob&) {
        return false;
      },
      [](fake_world&, const intent_bundle&, network::replay_context) {
        return true;
      },
      [](fake_world&, tick_id, network::replay_context) {
        return true;
      },
      [](const fake_world&, tick_id) {
        return true;
      },
      next_tick{});
    CHECK(result.status == network::replay_status::restore_failed);
    CHECK(result.tick == tick_id{0});
    CHECK(world.causal_value == 99);
  }

  SUBCASE("apply refusal") {
    fake_world world{};
    std::size_t restore_calls = 0;
    std::size_t apply_calls = 0;
    const auto result = network::replay_to(
      world,
      tick_id{0},
      checkpoint,
      tick_id{3},
      entries,
      restore_operation(restore_calls),
      [&apply_calls](fake_world&, const intent_bundle&, network::replay_context context) {
        CHECK(context.presentation_suppressed());
        return ++apply_calls != 2;
      },
      [](fake_world&, tick_id, network::replay_context) {
        return true;
      },
      [](const fake_world&, tick_id) {
        return true;
      },
      next_tick{});
    CHECK(result.status == network::replay_status::apply_failed);
    CHECK(result.tick == tick_id{2});
    CHECK(result.replayed_ticks == 1);
  }

  SUBCASE("step refusal") {
    fake_world world{};
    std::size_t restore_calls = 0;
    const auto result = network::replay_to(
      world,
      tick_id{0},
      checkpoint,
      tick_id{3},
      entries,
      restore_operation(restore_calls),
      [](fake_world&, const intent_bundle&, network::replay_context) {
        return true;
      },
      [](fake_world&, const tick_id tick, network::replay_context context) {
        CHECK(context.presentation_suppressed());
        return tick != tick_id{2};
      },
      [](const fake_world&, tick_id) {
        return true;
      },
      next_tick{});
    CHECK(result.status == network::replay_status::step_failed);
    CHECK(result.tick == tick_id{2});
    CHECK(result.replayed_ticks == 1);
  }

  SUBCASE("first mismatching root") {
    fake_world world{};
    std::size_t restore_calls = 0;
    const auto result = network::replay_to(
      world,
      tick_id{0},
      checkpoint,
      tick_id{3},
      entries,
      restore_operation(restore_calls),
      [](fake_world& state, const intent_bundle& bundle, network::replay_context) {
        apply_causal(state, bundle);
        return true;
      },
      [](fake_world& state, const tick_id tick, network::replay_context) {
        step_causal(state, tick);
        return true;
      },
      [](const fake_world&, const tick_id tick) {
        return tick != tick_id{2};
      },
      next_tick{});
    CHECK(result.status == network::replay_status::state_mismatch);
    CHECK(result.tick == tick_id{2});
    CHECK(result.replayed_ticks == 2);
  }
}

TEST_CASE("network checked integral tick successor refuses overflow") {
  constexpr network::checked_tick_successor<std::uint32_t> next;
  static_assert(next(41) == std::optional<std::uint32_t>{42});
  static_assert(!next(UINT32_MAX).has_value());
  CHECK(next(0) == std::optional<std::uint32_t>{1});
}
