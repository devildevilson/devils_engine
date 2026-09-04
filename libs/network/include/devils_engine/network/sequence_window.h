#ifndef DEVILS_ENGINE_NETWORK_SEQUENCE_WINDOW_H
#define DEVILS_ENGINE_NETWORK_SEQUENCE_WINDOW_H

#include <bitset>
#include <concepts>
#include <cstddef>
#include <limits>
#include <optional>

namespace devils_engine::network {

enum class sequence_classification : unsigned char {
  new_value,
  duplicate,
  stale,
  too_far_ahead
};

// Bounded modulo sequence acceptance. Bit zero represents newest(), bit N an
// already observed value N steps behind it. WindowBits is both the retained
// duplicate window and the largest forward jump accepted implicitly. A larger
// authenticated jump needs an explicit reset/rebase by the session owner.
// Values exactly half a sequence space apart are ambiguous and are never
// accepted implicitly.
template <std::unsigned_integral Sequence, std::size_t WindowBits>
class sequence_window {
public:
  static_assert(WindowBits > 0, "network::sequence_window requires a non-empty window");
  static constexpr Sequence half_range =
    Sequence(std::numeric_limits<Sequence>::max() / Sequence{2} + Sequence{1});
  static_assert(WindowBits < half_range,
                "network::sequence_window must be smaller than half the sequence space");

  constexpr bool initialized() const noexcept {
    return newest_.has_value();
  }

  constexpr std::optional<Sequence> newest() const noexcept {
    return newest_;
  }

  constexpr void reset() noexcept {
    newest_.reset();
    observed_.reset();
  }

  constexpr void reset(const Sequence accepted) noexcept {
    newest_ = accepted;
    observed_.reset();
    observed_.set(0);
  }

  constexpr sequence_classification classify(const Sequence value) const noexcept {
    if (!newest_) return sequence_classification::new_value;

    const Sequence forward = Sequence(value - *newest_);
    if (forward == 0) return sequence_classification::duplicate;
    if (forward == half_range) return sequence_classification::too_far_ahead;
    if (forward < half_range) {
      return forward <= Sequence(WindowBits)
               ? sequence_classification::new_value
               : sequence_classification::too_far_ahead;
    }

    const Sequence age = Sequence(*newest_ - value);
    if (age >= Sequence(WindowBits)) return sequence_classification::stale;
    return observed_.test(static_cast<std::size_t>(age))
             ? sequence_classification::duplicate
             : sequence_classification::new_value;
  }

  // Mutates only for a value classified as new. A late unseen value inside
  // the retained window is accepted without changing newest().
  constexpr sequence_classification observe(const Sequence value) noexcept {
    const sequence_classification result = classify(value);
    if (result != sequence_classification::new_value) return result;

    if (!newest_) {
      reset(value);
      return result;
    }

    const Sequence forward = Sequence(value - *newest_);
    if (forward != 0 && forward < half_range) {
      observed_ <<= static_cast<std::size_t>(forward);
      newest_ = value;
      observed_.set(0);
      return result;
    }

    const Sequence age = Sequence(*newest_ - value);
    observed_.set(static_cast<std::size_t>(age));
    return result;
  }

private:
  std::optional<Sequence> newest_;
  std::bitset<WindowBits> observed_;
};

} // namespace devils_engine::network

#endif
