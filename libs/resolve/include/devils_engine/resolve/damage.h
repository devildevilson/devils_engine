#ifndef DEVILS_ENGINE_RESOLVE_DAMAGE_H
#define DEVILS_ENGINE_RESOLVE_DAMAGE_H

#include <algorithm>
#include <cstdint>
#include <type_traits>

namespace devils_engine {
namespace resolve {

enum class damage_channel : uint8_t {
  primary,
  reaction,
  retaliation,
  periodic,
  cost,
  custom
};

template <typename Scalar, typename Kind>
struct damage_payload {
  static_assert(std::is_arithmetic_v<Scalar>);
  static_assert(std::is_trivially_copyable_v<Kind>);
  Kind kind{};
  Scalar amount{};
  damage_channel channel = damage_channel::primary;
  uint64_t tags = 0;
  constexpr bool operator==(const damage_payload&) const noexcept = default;
};

// Result of magnitude modifiers and shield/armor routing, immediately before the authoritative HP
// write. Commit guards such as immortality modify `committed_hp_after`, never resistance magnitude.
template <typename Scalar>
struct damage_route {
  static_assert(std::is_arithmetic_v<Scalar>);
  Scalar requested{};
  Scalar modified{};
  Scalar shield_absorbed{};
  Scalar hp_before{};
  Scalar proposed_hp_after{};
  Scalar committed_hp_after{};
  Scalar lethal_prevented{};
  bool survived_by_guard = false;
  constexpr bool operator==(const damage_route&) const noexcept = default;
};

template <typename Scalar>
constexpr void apply_minimum_hp_guard(damage_route<Scalar>& route,
                                      const Scalar minimum_hp) noexcept {
  // A guard may prevent this damage from crossing a floor, but it must never heal or resurrect a
  // target that was already below that floor before this instance was applied.
  const Scalar effective_minimum = std::min(minimum_hp, route.hp_before);
  if (route.committed_hp_after >= effective_minimum) return;
  route.lethal_prevented += effective_minimum - route.committed_hp_after;
  route.committed_hp_after = effective_minimum;
  route.survived_by_guard = effective_minimum > Scalar{0};
}

template <typename Scalar, typename Kind>
struct damage_outcome {
  damage_payload<Scalar, Kind> damage{};
  damage_route<Scalar> route{};
  bool target_valid = false;
  bool committed = false;
  constexpr bool operator==(const damage_outcome&) const noexcept = default;
};

} // namespace resolve
} // namespace devils_engine

#endif
