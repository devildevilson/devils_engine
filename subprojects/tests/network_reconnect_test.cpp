#include <cstdint>
#include <optional>

#include <devils_engine/network/network.h>
#include <doctest/doctest.h>

namespace net = devils_engine::network;

namespace {

// Microseconds, the same unit the credential uses for its validity window.
constexpr uint64_t ms = 1000;

net::reconnect_policy policy() {
  net::reconnect_policy value;
  value.suspect_after = 250 * ms;
  value.lost_after = 2000 * ms;
  value.first_backoff = 100 * ms;
  value.max_backoff = 800 * ms;
  value.max_attempts = 4;
  return value;
}

constexpr uint64_t start = 1'000'000;
// The deadline is the reconnect ticket's own expiry.
constexpr uint64_t deadline = start + 60'000'000;

using range = net::recovery_range<unsigned>;

struct one_unit {
  size_t operator()(const unsigned&) const noexcept {
    return 1;
  }
};

} // namespace

TEST_CASE("network reconnect policy is a declared schedule, not a guess") {
  const auto value = policy();
  REQUIRE(value.valid());
  CHECK(value.backoff_for(1) == 100 * ms);
  CHECK(value.backoff_for(2) == 200 * ms);
  CHECK(value.backoff_for(3) == 400 * ms);
  CHECK(value.backoff_for(4) == 800 * ms);
  // Capped, and the cap holds for any further attempt without overflowing.
  CHECK(value.backoff_for(5) == 800 * ms);
  CHECK(value.backoff_for(1000) == 800 * ms);
  CHECK(value.backoff_for(0) == 100 * ms);

  SUBCASE("an incoherent policy is refused rather than silently repaired") {
    auto broken = value;
    broken.lost_after = broken.suspect_after;
    CHECK_FALSE(broken.valid());
    broken = value;
    broken.suspect_after = 0;
    CHECK_FALSE(broken.valid());
    broken = value;
    broken.max_backoff = broken.first_backoff - 1;
    CHECK_FALSE(broken.valid());
    broken = value;
    broken.max_attempts = 0;
    CHECK_FALSE(broken.valid());
  }
}

TEST_CASE("network reconnect treats silence as suspicion before loss") {
  net::reconnect_coordinator coordinator(policy(), deadline, start);
  CHECK(coordinator.state() == net::reconnect_state::live);

  // Well inside the budget: nothing happens, and the caller is told when
  // polling becomes worthwhile.
  auto decision = coordinator.poll(start + 100 * ms);
  CHECK(decision.action == net::reconnect_action::none);
  CHECK(decision.next_instant == start + 250 * ms);
  CHECK(coordinator.state() == net::reconnect_state::live);

  SUBCASE("suspicion warns exactly once and does not reconnect") {
    decision = coordinator.poll(start + 300 * ms);
    CHECK(decision.action == net::reconnect_action::warn);
    CHECK(coordinator.state() == net::reconnect_state::suspect);
    CHECK(coordinator.attempts() == 0);
    // Repeated polling inside the same suspicion does not repeat the warning.
    decision = coordinator.poll(start + 400 * ms);
    CHECK(decision.action == net::reconnect_action::none);
    CHECK(coordinator.state() == net::reconnect_state::suspect);
  }

  SUBCASE("a silence spike which resolves itself costs no reconnect") {
    REQUIRE(coordinator.poll(start + 300 * ms).action == net::reconnect_action::warn);
    coordinator.observe_traffic(start + 400 * ms);
    CHECK(coordinator.state() == net::reconnect_state::live);
    CHECK(coordinator.poll(start + 500 * ms).action == net::reconnect_action::none);
    // And the warning is available again for the next spike.
    CHECK(coordinator.poll(start + 700 * ms).action == net::reconnect_action::warn);
  }

  SUBCASE("only the loss budget begins a reconnect") {
    decision = coordinator.poll(start + 2000 * ms);
    CHECK(decision.action == net::reconnect_action::connect);
    CHECK(coordinator.state() == net::reconnect_state::attempting);
    CHECK(coordinator.attempts() == 1);
  }

  SUBCASE("a transport which reported itself gone skips the silence budget") {
    coordinator.observe_transport_lost(start + 10 * ms);
    CHECK(coordinator.state() == net::reconnect_state::lost);
    const auto immediate = coordinator.poll(start + 10 * ms);
    CHECK(immediate.action == net::reconnect_action::connect);
    CHECK(coordinator.attempts() == 1);
  }
}

TEST_CASE("network reconnect runs one ordered flow to a published recovery") {
  net::reconnect_coordinator coordinator(policy(), deadline, start);
  coordinator.observe_transport_lost(start);

  REQUIRE(coordinator.poll(start).action == net::reconnect_action::connect);
  CHECK(coordinator.state() == net::reconnect_state::attempting);
  // While an attempt is in flight the caller is told to do nothing rather than
  // to open a second connection.
  CHECK(coordinator.poll(start + ms).action == net::reconnect_action::none);

  coordinator.observe_connected(start + 50 * ms);
  CHECK(coordinator.state() == net::reconnect_state::handshaking);
  CHECK(coordinator.poll(start + 60 * ms).action == net::reconnect_action::handshake);

  coordinator.observe_admitted(start + 100 * ms);
  CHECK(coordinator.state() == net::reconnect_state::recovering);
  CHECK(coordinator.poll(start + 110 * ms).action == net::reconnect_action::recover);

  coordinator.observe_recovered(start + 200 * ms);
  CHECK(coordinator.state() == net::reconnect_state::live);
  CHECK(coordinator.attempts() == 0);
  CHECK(coordinator.poll(start + 210 * ms).action == net::reconnect_action::none);

  SUBCASE("bytes from the abandoned handle do not resurrect an attempt") {
    net::reconnect_coordinator other(policy(), deadline, start);
    other.observe_transport_lost(start);
    REQUIRE(other.poll(start).action == net::reconnect_action::connect);
    other.observe_traffic(start + ms);
    CHECK(other.state() == net::reconnect_state::attempting);
    CHECK(other.attempts() == 1);
  }
}

TEST_CASE("network reconnect backs off and gives up for a named reason") {
  SUBCASE("failed attempts follow the declared schedule and then exhaust") {
    net::reconnect_coordinator coordinator(policy(), deadline, start);
    coordinator.observe_transport_lost(start);
    uint64_t now = start;
    const uint64_t expected[]{100 * ms, 200 * ms, 400 * ms, 800 * ms};
    for (uint32_t attempt = 1; attempt <= 4; ++attempt) {
      CAPTURE(attempt);
      REQUIRE(coordinator.poll(now).action == net::reconnect_action::connect);
      CHECK(coordinator.attempts() == attempt);
      coordinator.observe_connect_failed(now);
      const auto waiting = coordinator.poll(now);
      if (attempt < 4) {
        REQUIRE(waiting.action == net::reconnect_action::wait);
        CHECK(waiting.next_instant == now + expected[attempt - 1]);
        now = waiting.next_instant;
      } else {
        CHECK(waiting.action == net::reconnect_action::give_up);
      }
    }
    CHECK(coordinator.abandoned());
    CHECK(coordinator.abandoned_because() == net::abandon_reason::attempts_exhausted);
  }

  SUBCASE("an expired ticket gives up without spending an attempt") {
    net::reconnect_coordinator coordinator(policy(), start + 1000 * ms, start);
    coordinator.observe_transport_lost(start);
    const auto decision = coordinator.poll(start + 1000 * ms);
    CHECK(decision.action == net::reconnect_action::give_up);
    CHECK(coordinator.attempts() == 0);
    CHECK(coordinator.abandoned_because() == net::abandon_reason::deadline_passed);
  }

  SUBCASE("a terminal refusal is not retried on the schedule") {
    net::reconnect_coordinator coordinator(policy(), deadline, start);
    coordinator.observe_transport_lost(start);
    REQUIRE(coordinator.poll(start).action == net::reconnect_action::connect);
    coordinator.observe_connected(start + 10 * ms);
    coordinator.observe_refused(start + 20 * ms);
    CHECK(coordinator.state() == net::reconnect_state::abandoned);
    CHECK(coordinator.abandoned_because() == net::abandon_reason::refused);
    CHECK(coordinator.poll(start + 5000 * ms).action == net::reconnect_action::give_up);
  }

  SUBCASE("an unrecoverable history asks for a fresh join, not a failure") {
    net::reconnect_coordinator coordinator(policy(), deadline, start);
    coordinator.observe_transport_lost(start);
    REQUIRE(coordinator.poll(start).action == net::reconnect_action::connect);
    coordinator.observe_connected(start + 10 * ms);
    coordinator.observe_admitted(start + 20 * ms);
    coordinator.observe_unrecoverable(start + 30 * ms);
    CHECK(coordinator.abandoned_because() == net::abandon_reason::unrecoverable);
    // The distinction matters to the caller: give up on the session, keep the
    // player.
    CHECK(coordinator.poll(start + 40 * ms).action == net::reconnect_action::rejoin);
  }
}

TEST_CASE("network session holds are bounded, reaped and resolved only for their owner") {
  net::session_hold_table holds(2);
  CHECK(holds.capacity() == 2);
  REQUIRE(holds.hold(9001, 42, 3, start, start + 30'000'000) ==
          net::session_hold_table::hold_status::held);
  CHECK(holds.size() == 1);
  CHECK(holds.resolve(9001, 42, 3, start + ms) == net::session_hold_table::resolve_status::held);

  SUBCASE("a window which does not open is refused") {
    CHECK(holds.hold(9002, 7, 3, start, start) ==
          net::session_hold_table::hold_status::invalid_window);
    CHECK(holds.size() == 1);
  }

  SUBCASE("holding twice and holding past capacity are distinct refusals") {
    CHECK(holds.hold(9001, 42, 3, start, start + 1000) ==
          net::session_hold_table::hold_status::already_held);
    REQUIRE(holds.hold(9002, 7, 3, start, start + 1000) ==
            net::session_hold_table::hold_status::held);
    // A table which grows with disappearing peers is an allocation a peer
    // controls, so the declared capacity is a refusal, not a hint.
    CHECK(holds.hold(9003, 8, 3, start, start + 1000) ==
          net::session_hold_table::hold_status::at_capacity);
    CHECK(holds.size() == 2);
  }

  SUBCASE("a claim from the wrong principal or a migrated authority is refused") {
    CHECK(holds.resolve(9001, 43, 3, start + ms) ==
          net::session_hold_table::resolve_status::wrong_principal);
    // Same orientation as credential.h and classify_authority_message: the
    // epoch PRESENTED is compared against the one recorded, so older than the
    // record is stale and newer is unexpectedly ahead.
    CHECK(holds.resolve(9001, 42, 2, start + ms) ==
          net::session_hold_table::resolve_status::stale_epoch);
    CHECK(holds.resolve(9001, 42, 4, start + ms) ==
          net::session_hold_table::resolve_status::future_epoch);
    CHECK(holds.resolve(9999, 42, 3, start + ms) ==
          net::session_hold_table::resolve_status::unknown);
  }

  SUBCASE("expiry and absence are different answers") {
    // "Expired" tells a returning client its ticket is worthless; "unknown"
    // may mean it is talking to the wrong authority entirely.
    CHECK(holds.resolve(9001, 42, 3, start + 30'000'000) ==
          net::session_hold_table::resolve_status::expired);
    CHECK(holds.reap(start + 30'000'000) == 1);
    CHECK(holds.size() == 0);
    CHECK(holds.resolve(9001, 42, 3, start + 30'000'000) ==
          net::session_hold_table::resolve_status::unknown);
  }

  SUBCASE("reaping frees the capacity it declared") {
    REQUIRE(holds.hold(9002, 7, 3, start, start + 1000) ==
            net::session_hold_table::hold_status::held);
    CHECK(holds.hold(9003, 8, 3, start, start + 1000) ==
          net::session_hold_table::hold_status::at_capacity);
    CHECK(holds.reap(start + 2000) == 1);
    CHECK(holds.hold(9003, 8, 3, start, start + 1000) ==
          net::session_hold_table::hold_status::held);
  }

  SUBCASE("an explicit release makes room immediately") {
    holds.release(9001);
    CHECK(holds.size() == 0);
    holds.release(9001); // Releasing what is not held is not an error.
    CHECK(holds.size() == 0);
  }
}

TEST_CASE("network recovery feasibility measures the retention budget") {
  range plan;

  SUBCASE("a checkpoint plus the bundle that follows it is recoverable") {
    CHECK(net::assess_recovery(std::optional<unsigned>(100), std::optional<unsigned>(101), 140u,
                               plan) == net::recovery_feasibility::recoverable);
    CHECK(plan == range{100, 140});
    // Older history than required is fine; only a gap is not.
    CHECK(net::assess_recovery(std::optional<unsigned>(100), std::optional<unsigned>(50), 140u,
                               plan) == net::recovery_feasibility::recoverable);
  }

  SUBCASE("a target equal to the checkpoint needs no bundle at all") {
    CHECK(net::assess_recovery(std::optional<unsigned>(100), std::optional<unsigned>{}, 100u, plan) ==
          net::recovery_feasibility::recoverable);
    CHECK(plan == range{100, 100});
  }

  SUBCASE("the bundle after the checkpoint is exactly what must be retained") {
    // A checkpoint at K is the committed state AFTER K, so replay needs K+1.
    // Retaining only from K+2 leaves a hole no amount of replay fills.
    CHECK(net::assess_recovery(std::optional<unsigned>(100), std::optional<unsigned>(102), 140u,
                               plan) == net::recovery_feasibility::history_gap);
    CHECK(net::assess_recovery(std::optional<unsigned>(100), std::optional<unsigned>{}, 140u,
                               plan) == net::recovery_feasibility::history_gap);
  }

  SUBCASE("no retained checkpoint and an impossible target are named separately") {
    CHECK(net::assess_recovery(std::optional<unsigned>{}, std::optional<unsigned>(101), 140u,
                               plan) == net::recovery_feasibility::no_checkpoint);
    CHECK(net::assess_recovery(std::optional<unsigned>(150), std::optional<unsigned>(101), 140u,
                               plan) == net::recovery_feasibility::target_before_checkpoint);
  }

  SUBCASE("a refusal leaves the plan untouched") {
    plan = range{7, 9};
    CHECK(net::assess_recovery(std::optional<unsigned>(100), std::optional<unsigned>(102), 140u,
                               plan) == net::recovery_feasibility::history_gap);
    CHECK(plan == range{7, 9});
  }

  SUBCASE("a checkpoint at the last representable tick has no successor") {
    constexpr unsigned last = UINT_MAX;
    CHECK(net::assess_recovery(std::optional<unsigned>(last), std::optional<unsigned>(1), last,
                               plan) == net::recovery_feasibility::recoverable);
  }
}

TEST_CASE("network reconnect composes with the retained history it depends on") {
  // The authority's side of one reconnect: hold the session, resolve the
  // returning claim, then decide whether its history still permits recovery.
  net::session_hold_table holds(4);
  net::checkpoint_ring<unsigned, unsigned, one_unit> checkpoints(2, 64);
  net::bounded_history<unsigned, unsigned> bundles(3, 64);

  REQUIRE(holds.hold(9001, 42, 3, start, start + 30'000'000) ==
          net::session_hold_table::hold_status::held);
  REQUIRE(checkpoints.try_store(100u, 1u).stored());
  for (unsigned tick = 101; tick <= 103; ++tick) REQUIRE(bundles.try_store(tick, 1u, 1).stored());

  const uint64_t now = start + 1000 * ms;
  REQUIRE(holds.resolve(9001, 42, 3, now) == net::session_hold_table::resolve_status::held);

  range plan;
  const auto* checkpoint = checkpoints.latest_at_or_before(103u);
  REQUIRE(checkpoint != nullptr);
  CHECK(net::assess_recovery(std::optional<unsigned>(checkpoint->tick), bundles.oldest_tick(), 103u,
                             plan) == net::recovery_feasibility::recoverable);
  CHECK(plan == range{100, 103});

  SUBCASE("ticks kept flowing while the peer was away and evicted its history") {
    // Three more ticks push 101..103 out of a three-entry history, so the
    // bundle immediately after the retained checkpoint is gone.
    for (unsigned tick = 104; tick <= 106; ++tick)
      REQUIRE(bundles.try_store(tick, 1u, 1).stored());
    const auto* newer = checkpoints.latest_at_or_before(106u);
    REQUIRE(newer != nullptr);
    CHECK(newer->tick == 100);
    CHECK(net::assess_recovery(std::optional<unsigned>(newer->tick), bundles.oldest_tick(), 106u,
                               plan) == net::recovery_feasibility::history_gap);

    // The honest consequence: this returning client cannot be replayed, and the
    // authority answers with a fresh join rather than a partial replacement.
    net::reconnect_coordinator coordinator(policy(), start + 30'000'000, start);
    coordinator.observe_transport_lost(start);
    REQUIRE(coordinator.poll(start).action == net::reconnect_action::connect);
    coordinator.observe_connected(now);
    coordinator.observe_admitted(now);
    coordinator.observe_unrecoverable(now);
    CHECK(coordinator.poll(now).action == net::reconnect_action::rejoin);
  }

  SUBCASE("a newer checkpoint restores feasibility without a larger history") {
    for (unsigned tick = 104; tick <= 106; ++tick)
      REQUIRE(bundles.try_store(tick, 1u, 1).stored());
    REQUIRE(checkpoints.try_store(103u, 1u).stored());
    const auto* newer = checkpoints.latest_at_or_before(106u);
    REQUIRE(newer != nullptr);
    CHECK(newer->tick == 103);
    // Retention is two budgets working together: a checkpoint close enough to
    // the retained bundles is what makes recovery possible, not history alone.
    CHECK(net::assess_recovery(std::optional<unsigned>(newer->tick), bundles.oldest_tick(), 106u,
                               plan) == net::recovery_feasibility::recoverable);
    CHECK(plan == range{103, 106});
  }
}
