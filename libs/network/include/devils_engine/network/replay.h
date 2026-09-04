#ifndef DEVILS_ENGINE_NETWORK_REPLAY_H
#define DEVILS_ENGINE_NETWORK_REPLAY_H

#include <concepts>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>

namespace devils_engine::network {

enum class replay_effect_policy : unsigned char {
  suppress_presentation
};

struct replay_context {
  replay_effect_policy effects = replay_effect_policy::suppress_presentation;

  constexpr bool presentation_suppressed() const noexcept {
    return effects == replay_effect_policy::suppress_presentation;
  }
};

enum class replay_status : unsigned char {
  completed,
  target_before_checkpoint,
  target_before_history,
  target_after_history,
  duplicate_bundle,
  out_of_order_bundle,
  missing_bundle,
  tick_successor_unavailable,
  restore_failed,
  apply_failed,
  step_failed,
  state_mismatch
};

template <class Tick>
struct replay_result {
  replay_status status = replay_status::completed;
  std::optional<Tick> tick;
  std::size_t replayed_ticks = 0;

  constexpr bool completed() const noexcept {
    return status == replay_status::completed;
  }
};

template <std::integral Tick>
struct checked_tick_successor {
  constexpr std::optional<Tick> operator()(const Tick tick) const noexcept {
    if (tick == std::numeric_limits<Tick>::max()) return std::nullopt;
    return Tick(tick + Tick{1});
  }
};

struct replay_entry_tick {
  template <class Entry>
  constexpr decltype(auto) operator()(const Entry& entry) const noexcept {
    return (entry.tick);
  }
};

struct replay_entry_bundle {
  template <class Entry>
  constexpr decltype(auto) operator()(const Entry& entry) const noexcept {
    return (entry.bundle);
  }
};

// A checkpoint represents the committed state at checkpoint_tick. Bundle T is
// applied before stepping T, so reaching target_tick replays K+1..N. The range
// is preflighted before Restore runs: malformed or incomplete history cannot
// partially mutate Host. Restore itself must provide transactional checkpoint
// replacement. ApplyBundle and Step receive an explicit suppression context;
// honoring it is part of their project contract because generic code cannot
// observe side effects hidden inside a callback. Replay after a successful
// restore advances Host in place; when replay failure must preserve a running
// instance, the caller passes a detached staging Host and publishes it only
// after replay_result::completed().
template <
  class Host,
  std::totally_ordered Tick,
  class Checkpoint,
  std::ranges::forward_range BundleRange,
  class Restore,
  class ApplyBundle,
  class Step,
  class VerifyState,
  class NextTick,
  class TickOf = replay_entry_tick,
  class BundleOf = replay_entry_bundle>
  requires requires(
    Host& host,
    const Host& const_host,
    const Checkpoint& checkpoint,
    std::ranges::range_reference_t<BundleRange> entry,
    const Tick tick,
    const replay_context context,
    Restore& restore,
    ApplyBundle& apply_bundle,
    Step& step,
    VerifyState& verify_state,
    NextTick& next_tick,
    TickOf& tick_of,
    BundleOf& bundle_of) {
    { std::invoke(tick_of, entry) } -> std::convertible_to<Tick>;
    { std::invoke(next_tick, tick) } -> std::same_as<std::optional<Tick>>;
    { std::invoke(restore, host, checkpoint) } -> std::convertible_to<bool>;
    { std::invoke(apply_bundle, host, std::invoke(bundle_of, entry), context) } -> std::convertible_to<bool>;
    { std::invoke(step, host, tick, context) } -> std::convertible_to<bool>;
    { std::invoke(verify_state, const_host, tick) } -> std::convertible_to<bool>;
  }
[[nodiscard]] replay_result<Tick> replay_to(
  Host& host,
  const Tick checkpoint_tick,
  const Checkpoint& checkpoint,
  const Tick target_tick,
  BundleRange&& bundles,
  Restore restore,
  ApplyBundle apply_bundle,
  Step step,
  VerifyState verify_state,
  NextTick next_tick,
  TickOf tick_of = {},
  BundleOf bundle_of = {}) {
  if (target_tick < checkpoint_tick) {
    return {replay_status::target_before_checkpoint, target_tick, 0};
  }

  if (checkpoint_tick < target_tick) {
    std::optional<Tick> previous;
    for (const auto& entry : bundles) {
      const Tick current = std::invoke(tick_of, entry);
      if (previous.has_value()) {
        if (current == *previous) {
          return {replay_status::duplicate_bundle, current, 0};
        }
        if (current < *previous) {
          return {replay_status::out_of_order_bundle, current, 0};
        }
      }
      previous = current;
    }

    auto expected = std::invoke(next_tick, checkpoint_tick);
    if (!expected.has_value()) {
      return {replay_status::tick_successor_unavailable, checkpoint_tick, 0};
    }

    bool first_relevant = true;
    bool target_covered = false;
    for (const auto& entry : bundles) {
      const Tick current = std::invoke(tick_of, entry);
      if (!(checkpoint_tick < current)) continue;

      if (current != *expected) {
        return {
          first_relevant ? replay_status::target_before_history
                         : replay_status::missing_bundle,
          *expected,
          0,
        };
      }
      first_relevant = false;

      if (current == target_tick) {
        target_covered = true;
        break;
      }
      if (target_tick < current) {
        return {replay_status::target_before_history, target_tick, 0};
      }

      expected = std::invoke(next_tick, current);
      if (!expected.has_value()) {
        return {replay_status::tick_successor_unavailable, current, 0};
      }
    }

    if (!target_covered) {
      return {replay_status::target_after_history, target_tick, 0};
    }
  }

  if (!bool(std::invoke(restore, host, checkpoint))) {
    return {replay_status::restore_failed, checkpoint_tick, 0};
  }
  if (!bool(std::invoke(verify_state, std::as_const(host), checkpoint_tick))) {
    return {replay_status::state_mismatch, checkpoint_tick, 0};
  }
  if (target_tick == checkpoint_tick) return {};

  constexpr replay_context context{};
  std::size_t replayed_ticks = 0;
  for (const auto& entry : bundles) {
    const Tick current = std::invoke(tick_of, entry);
    if (!(checkpoint_tick < current)) continue;

    if (!bool(std::invoke(
          apply_bundle,
          host,
          std::invoke(bundle_of, entry),
          context))) {
      return {replay_status::apply_failed, current, replayed_ticks};
    }
    if (!bool(std::invoke(step, host, current, context))) {
      return {replay_status::step_failed, current, replayed_ticks};
    }
    ++replayed_ticks;
    if (!bool(std::invoke(verify_state, std::as_const(host), current))) {
      return {replay_status::state_mismatch, current, replayed_ticks};
    }
    if (current == target_tick) {
      return {replay_status::completed, std::nullopt, replayed_ticks};
    }
  }

  // Preflight proved coverage; reaching this return means a callback mutated
  // externally-owned range state, which the replay contract forbids.
  return {replay_status::target_after_history, target_tick, replayed_ticks};
}

} // namespace devils_engine::network

#endif
