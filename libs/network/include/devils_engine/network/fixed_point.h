#ifndef DEVILS_ENGINE_NETWORK_FIXED_POINT_H
#define DEVILS_ENGINE_NETWORK_FIXED_POINT_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

// Key plus fixed point: the replication counterpart of the world-generation
// lesson that accumulation is cured by writing the accumulator, not by moving
// the origin. A world coordinate is split into an integer cell key and a code
// inside that cell, so resolution does not degrade with distance from zero.
//
// The quantum is declared as a NEGATIVE POWER OF TWO, and that is not a
// stylistic choice. Multiplying an IEEE-754 double by a power of two is exact,
// so the code is a deterministic function of its input on every conforming
// platform, and decoding then re-encoding returns the same code by
// construction instead of by floating-point luck. A decimal quantum such as
// 0.001 would make both of those true only approximately.
//
// This header owns the only floating-point arithmetic in the hot wire path.
// The batch codecs move integers, so they cannot contribute divergence at all.

namespace devils_engine::network {

// The largest magnitude at which `scaled - double(truncated)` is still exact
// and `double(total)` still round-trips. Beyond it the split reports a clamp
// rather than returning a silently rounded code.
inline constexpr double fixed_point_exact_limit = 4503599627370496.0; // 2^52

template <class Code>
struct fixed_axis {
  static_assert(std::numeric_limits<Code>::is_integer && !std::numeric_limits<Code>::is_signed,
                "network fixed axis code must be an unsigned integer");
  static_assert(std::numeric_limits<Code>::digits <= 32,
                "network fixed axis code must fit the signed intermediate arithmetic");
  static constexpr unsigned code_bits = unsigned(std::numeric_limits<Code>::digits);

  // A cell spans 2^cell_shift world units; the quantum is 2^-fraction_bits.
  // The two must exactly fill the code, so every code inside a cell is
  // reachable and no code is unused: a partially used code space would make
  // two peers disagree about which codes are legal.
  uint8_t cell_shift = 0;
  uint8_t fraction_bits = 0;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return unsigned(cell_shift) + unsigned(fraction_bits) == code_bits;
  }

  [[nodiscard]] constexpr double quantum() const noexcept {
    return 1.0 / double(uint64_t(1) << fraction_bits);
  }

  [[nodiscard]] constexpr double cell_size() const noexcept {
    return double(uint64_t(1) << cell_shift);
  }
};

struct axis_split {
  int32_t key = 0;
  uint32_t code = 0;
  // The input was outside the representable range and the result was clamped.
  // Reported rather than hidden: a clamped position is a project fault, and
  // silently folding it would place an entity somewhere plausible but wrong.
  bool clamped = false;

  bool operator==(const axis_split&) const = default;
};

// Round half away from zero, computed without any <cmath> call. Every step is
// exact for |scaled| < 2^52: the product is exact because the scale is a power
// of two, the truncation is exact, and the remainder of two nearby
// representable values is exact. The comparison against one half is therefore
// a comparison of exact quantities, which is what makes the result identical
// on any conforming platform.
template <class Code>
[[nodiscard]] constexpr axis_split split_axis(const fixed_axis<Code> axis,
                                              const double world) noexcept {
  const double scale = double(uint64_t(1) << axis.fraction_bits);
  const double scaled = world * scale;
  const bool over = !(scaled > -fixed_point_exact_limit && scaled < fixed_point_exact_limit);
  const double bounded = over ? (scaled < 0.0 ? -fixed_point_exact_limit : fixed_point_exact_limit)
                              : scaled;

  const int64_t truncated = int64_t(bounded);
  const double remainder = bounded - double(truncated);
  int64_t total = truncated;
  if (remainder >= 0.5) ++total;
  else if (remainder <= -0.5) --total;

  // Signed right shift is arithmetic, so this is floor division by the power of
  // two number of codes per cell, and the masked remainder is never negative.
  int64_t key = total >> fixed_axis<Code>::code_bits;
  const uint32_t code =
    uint32_t(total & ((int64_t(1) << fixed_axis<Code>::code_bits) - 1));

  bool clamped = over;
  if (key > std::numeric_limits<int32_t>::max()) {
    key = std::numeric_limits<int32_t>::max();
    clamped = true;
  } else if (key < std::numeric_limits<int32_t>::min()) {
    key = std::numeric_limits<int32_t>::min();
    clamped = true;
  }
  return axis_split{int32_t(key), code, clamped};
}

template <class Code>
[[nodiscard]] constexpr double join_axis(const fixed_axis<Code> axis, const int32_t key,
                                         const uint32_t code) noexcept {
  const int64_t total =
    (int64_t(key) << fixed_axis<Code>::code_bits) | int64_t(code);
  return double(total) / double(uint64_t(1) << axis.fraction_bits);
}

// A hot message carries a cell RELATIVE to one the receiver already knows, so
// the conversion between an absolute key and that delta belongs here rather
// than in each caller. Narrowing an absolute key to the delta width silently
// would place a distant or forged target somewhere plausible but wrong, which
// is precisely the class of bug a wire format must not permit.
struct cell_delta {
  int8_t value = 0;
  bool out_of_range = false;

  bool operator==(const cell_delta&) const = default;
};

[[nodiscard]] constexpr cell_delta relative_cell(const int32_t base_key,
                                                 const int32_t key) noexcept {
  const int64_t difference = int64_t(key) - int64_t(base_key);
  if (difference < std::numeric_limits<int8_t>::min() ||
      difference > std::numeric_limits<int8_t>::max())
    return cell_delta{0, true};
  return cell_delta{int8_t(difference), false};
}

[[nodiscard]] constexpr int32_t absolute_cell(const int32_t base_key,
                                                   const int8_t delta) noexcept {
  const int64_t total = int64_t(base_key) + int64_t(delta);
  if (total > std::numeric_limits<int32_t>::max())
    return std::numeric_limits<int32_t>::max();
  if (total < std::numeric_limits<int32_t>::min())
    return std::numeric_limits<int32_t>::min();
  return int32_t(total);
}

struct turn_split {
  uint32_t code = 0;
  bool clamped = false;

  bool operator==(const turn_split&) const = default;
};

// A direction is quantized on its own scale: a full turn maps onto the whole
// code range, so wrap is the natural modular wrap of the code and there is no
// boundary at which two peers can disagree about the representable set. The
// library does not compute an angle from a vector; the project passes turns
// (1.0 == one full turn) so that no trigonometric libm call enters this path.
// A non-finite input is reported, never folded into a plausible direction.
template <class Code>
[[nodiscard]] constexpr turn_split split_turn(const double turns) noexcept {
  static_assert(std::numeric_limits<Code>::digits <= 32,
                "network turn code must fit the signed intermediate arithmetic");
  constexpr unsigned bits = unsigned(std::numeric_limits<Code>::digits);
  const double scale = double(uint64_t(1) << bits);
  const double scaled = turns * scale;
  const bool over = !(scaled > -fixed_point_exact_limit && scaled < fixed_point_exact_limit);
  if (over) return turn_split{0, true};

  const int64_t truncated = int64_t(scaled);
  const double remainder = scaled - double(truncated);
  int64_t total = truncated;
  if (remainder >= 0.5) ++total;
  else if (remainder <= -0.5) --total;
  return turn_split{uint32_t(uint64_t(total) & ((uint64_t(1) << bits) - 1)), false};
}

template <class Code>
[[nodiscard]] constexpr double join_turn(const Code code) noexcept {
  constexpr unsigned bits = unsigned(std::numeric_limits<Code>::digits);
  return double(code) / double(uint64_t(1) << bits);
}

} // namespace devils_engine::network

#endif
