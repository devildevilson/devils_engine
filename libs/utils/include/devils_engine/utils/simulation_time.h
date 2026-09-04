#ifndef DEVILS_ENGINE_UTILS_SIMULATION_TIME_H
#define DEVILS_ENGINE_UTILS_SIMULATION_TIME_H

#include <compare>
#include <cstdint>
#include <limits>

#include "core.h"

namespace devils_engine::utils {

inline constexpr uint64_t microseconds_per_second = UINT64_C(1000000);

struct simulation_tick {
  uint64_t value = 0;
  constexpr auto operator<=>(const simulation_tick&) const noexcept = default;
};

struct simulation_duration {
  uint64_t ticks = 0;
  constexpr auto operator<=>(const simulation_duration&) const noexcept = default;
};

struct authored_duration {
  uint64_t microseconds = 0;
  constexpr auto operator<=>(const authored_duration&) const noexcept = default;
};

constexpr simulation_tick operator+(const simulation_tick tick,
                                    const simulation_duration duration) {
  if (duration.ticks > std::numeric_limits<uint64_t>::max() - tick.value) {
    error{}("simulation tick deadline overflow");
  }
  return {tick.value + duration.ticks};
}

// Session-wide fixed simulation rate. Authored durations stay in microseconds,
// but causal state stores only integer simulation ticks. Integral Hz keeps the
// portable conversion below independent of compiler-specific 128-bit types.
class simulation_rate {
public:
  constexpr explicit simulation_rate(const uint32_t ticks_per_second)
    : ticks_per_second_(ticks_per_second) {
    if (ticks_per_second == 0) {
      error{}("simulation rate must be positive");
    }
  }

  constexpr uint32_t ticks_per_second() const noexcept {
    return ticks_per_second_;
  }

  // Deadlines use ceil: a positive authored duration never completes earlier
  // than requested. Zero remains the explicit immediate duration.
  constexpr simulation_duration to_ticks_ceil(const authored_duration duration) const {
    const uint64_t whole_seconds = duration.microseconds / microseconds_per_second;
    const uint64_t remainder = duration.microseconds % microseconds_per_second;
    if (whole_seconds > std::numeric_limits<uint64_t>::max() / ticks_per_second_) {
      error{}("authored duration does not fit simulation ticks");
    }

    const uint64_t whole_ticks = whole_seconds * ticks_per_second_;
    const uint64_t remainder_product = remainder * uint64_t(ticks_per_second_);
    const uint64_t partial_ticks =
      remainder_product / microseconds_per_second +
      uint64_t(remainder_product % microseconds_per_second != 0);
    if (partial_ticks > std::numeric_limits<uint64_t>::max() - whole_ticks) {
      error{}("authored duration does not fit simulation ticks");
    }
    return {whole_ticks + partial_ticks};
  }

  // Presentation/calendar code may derive nominal elapsed authored time from a
  // tick. Quotient/remainder avoids tick * 1'000'000 overflow in the common case.
  constexpr authored_duration to_microseconds_floor(const simulation_tick tick) const {
    const uint64_t whole_seconds = tick.value / ticks_per_second_;
    const uint64_t remainder = tick.value % ticks_per_second_;
    if (whole_seconds > std::numeric_limits<uint64_t>::max() / microseconds_per_second) {
      error{}("simulation tick does not fit authored microseconds");
    }
    const uint64_t whole_microseconds = whole_seconds * microseconds_per_second;
    const uint64_t partial_microseconds =
      (remainder * microseconds_per_second) / ticks_per_second_;
    if (partial_microseconds >
        std::numeric_limits<uint64_t>::max() - whole_microseconds) {
      error{}("simulation tick does not fit authored microseconds");
    }
    return {whole_microseconds + partial_microseconds};
  }

private:
  uint32_t ticks_per_second_;
};

// Non-causal pacing helper. Wall/presentation elapsed time may arrive in any
// partition; only complete fixed steps leave this accumulator. Unserved steps
// remain as explicit debt when max_steps limits catch-up.
class fixed_step_accumulator {
public:
  constexpr explicit fixed_step_accumulator(const simulation_rate rate)
    : rate_(rate) {}

  constexpr simulation_rate rate() const noexcept {
    return rate_;
  }

  constexpr uint64_t pending_steps() const noexcept {
    return pending_steps_;
  }

  constexpr uint64_t fractional_units() const noexcept {
    return fractional_units_;
  }

  constexpr void reset(const simulation_rate rate) noexcept {
    rate_ = rate;
    pending_steps_ = 0;
    fractional_units_ = 0;
  }

  constexpr void add_elapsed(const authored_duration elapsed) {
    const uint64_t whole_seconds = elapsed.microseconds / microseconds_per_second;
    const uint64_t remainder = elapsed.microseconds % microseconds_per_second;
    if (whole_seconds >
        std::numeric_limits<uint64_t>::max() / rate_.ticks_per_second()) {
      error{}("fixed-step pacing debt overflow");
    }

    uint64_t added_steps = whole_seconds * rate_.ticks_per_second();
    const uint64_t units = remainder * uint64_t(rate_.ticks_per_second());
    if (units / microseconds_per_second >
        std::numeric_limits<uint64_t>::max() - added_steps) {
      error{}("fixed-step pacing debt overflow");
    }
    added_steps += units / microseconds_per_second;

    const uint64_t added_fraction = units % microseconds_per_second;
    if (added_fraction >= microseconds_per_second - fractional_units_) {
      if (added_steps == std::numeric_limits<uint64_t>::max()) {
        error{}("fixed-step pacing debt overflow");
      }
      ++added_steps;
      fractional_units_ = added_fraction -
                          (microseconds_per_second - fractional_units_);
    } else {
      fractional_units_ += added_fraction;
    }

    if (added_steps > std::numeric_limits<uint64_t>::max() - pending_steps_) {
      error{}("fixed-step pacing debt overflow");
    }
    pending_steps_ += added_steps;
  }

  constexpr uint64_t take_steps(
    const uint64_t max_steps = std::numeric_limits<uint64_t>::max()) noexcept {
    const uint64_t result = pending_steps_ < max_steps ? pending_steps_ : max_steps;
    pending_steps_ -= result;
    return result;
  }

  constexpr uint64_t advance(
    const authored_duration elapsed,
    const uint64_t max_steps = std::numeric_limits<uint64_t>::max()) {
    add_elapsed(elapsed);
    return take_steps(max_steps);
  }

private:
  simulation_rate rate_;
  uint64_t pending_steps_ = 0;
  uint64_t fractional_units_ = 0;
};

constexpr simulation_tick deadline_after(const simulation_tick now,
                                         const authored_duration duration,
                                         const simulation_rate rate) {
  return now + rate.to_ticks_ceil(duration);
}

} // namespace devils_engine::utils

#endif
