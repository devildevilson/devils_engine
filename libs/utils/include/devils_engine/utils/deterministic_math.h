#ifndef DEVILS_ENGINE_UTILS_DETERMINISTIC_MATH_H
#define DEVILS_ENGINE_UTILS_DETERMINISTIC_MATH_H

// Scalar adaptation of Jolt Physics 5.6.0, Jolt/Math/Vec4.inl::SinCos.
// The original is based on the Cephes sinf/cosf implementation and uses
// Cody-Waite argument reduction.
// SPDX-FileCopyrightText: 2021 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#include <bit>
#include <cstdint>

namespace devils_engine::utils::deterministic {

struct sin_cos_result {
  float sine;
  float cosine;
};

// Cody-Waite reduction below deliberately keeps the quadrant below 2^16.
// Besides retaining the precision described by Jolt, this avoids the differing
// out-of-range float-to-int behavior of the SSE and NEON implementations.
inline constexpr float max_abs_angle = 100000.0f;

inline bool is_supported_angle(const float angle) noexcept {
  constexpr std::uint32_t magnitude_mask = UINT32_C(0x7fffffff);
  return (std::bit_cast<std::uint32_t>(angle) & magnitude_mask)
      <= std::bit_cast<std::uint32_t>(max_abs_angle);
}

// The bit operations deliberately preserve the sign of zero. Application code
// using this function must be compiled without FP contraction; the project-wide
// devils_engine::options target supplies that policy. Inputs outside the declared
// reduction range (including infinities and NaNs) return one canonical NaN pair.
inline sin_cos_result sin_cos(const float angle) noexcept {
  constexpr std::uint32_t sign_bit = UINT32_C(0x80000000);
  constexpr float canonical_nan = std::bit_cast<float>(UINT32_C(0x7fc00000));
  if (!is_supported_angle(angle)) return {canonical_nan, canonical_nan};

  const std::uint32_t input_bits = std::bit_cast<std::uint32_t>(angle);
  std::uint32_t sine_sign = input_bits & sign_bit;
  float x = std::bit_cast<float>(input_bits & ~sign_bit);

  const std::uint32_t quadrant = static_cast<std::uint32_t>(0.6366197723675814f * x + 0.5f);
  const float float_quadrant = static_cast<float>(quadrant);
  x = ((x - float_quadrant * 1.5703125f)
       - float_quadrant * 0.0004837512969970703125f)
      - float_quadrant * 7.549789948768648e-8f;

  const float x2 = x * x;
  const float taylor_cosine =
      ((2.443315711809948e-5f * x2 - 1.388731625493765e-3f) * x2
       + 4.166664568298827e-2f)
          * x2 * x2
      - 0.5f * x2 + 1.0f;
  const float taylor_sine =
      ((-1.9515295891e-4f * x2 + 8.3321608736e-3f) * x2 - 1.6666654611e-1f)
          * x2 * x
      + x;

  const bool use_cosine_for_sine = (quadrant & 1u) != 0;
  float sine = use_cosine_for_sine ? taylor_cosine : taylor_sine;
  float cosine = use_cosine_for_sine ? taylor_sine : taylor_cosine;
  const std::uint32_t quadrant_bit_1 = (quadrant & 1u) << 31;
  const std::uint32_t quadrant_bit_2 = (quadrant & 2u) << 30;
  sine_sign ^= quadrant_bit_2;
  const std::uint32_t cosine_sign = quadrant_bit_1 ^ quadrant_bit_2;
  sine = std::bit_cast<float>(std::bit_cast<std::uint32_t>(sine) ^ sine_sign);
  cosine = std::bit_cast<float>(std::bit_cast<std::uint32_t>(cosine) ^ cosine_sign);
  return {sine, cosine};
}

inline float sin(const float angle) noexcept { return sin_cos(angle).sine; }
inline float cos(const float angle) noexcept { return sin_cos(angle).cosine; }

} // namespace devils_engine::utils::deterministic

#endif
