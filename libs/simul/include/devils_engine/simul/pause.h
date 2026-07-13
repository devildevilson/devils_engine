#ifndef DEVILS_ENGINE_SIMUL_PAUSE_H
#define DEVILS_ENGINE_SIMUL_PAUSE_H

#include <cstdint>

namespace devils_engine {
namespace simul {

enum class pause_domain : uint8_t {
  gameplay = 1u << 0,
  presentation = 1u << 1,
};

// Engine-owned pause policy. Components remain passive data; schedulers query domains before
// mutating gameplay or advancing world presentation. Engine/UI time deliberately never pauses.
class pause_state {
public:
  constexpr void set(const pause_domain domain, const bool value) noexcept {
    const uint8_t bit = static_cast<uint8_t>(domain);
    if (value) mask_ |= bit;
    else mask_ &= uint8_t(~bit);
  }
  constexpr bool paused(const pause_domain domain) const noexcept {
    return (mask_ & static_cast<uint8_t>(domain)) != 0;
  }
  constexpr void set_world(const bool value) noexcept {
    set(pause_domain::gameplay, value);
    set(pause_domain::presentation, value);
  }
  constexpr bool world_paused() const noexcept {
    return paused(pause_domain::gameplay) && paused(pause_domain::presentation);
  }

private:
  uint8_t mask_ = 0;
};

}
}

#endif
