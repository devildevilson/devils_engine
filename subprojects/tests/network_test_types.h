#ifndef DEVILS_ENGINE_TESTS_NETWORK_TEST_TYPES_H
#define DEVILS_ENGINE_TESTS_NETWORK_TEST_TYPES_H

#include <cstdint>

namespace devils_engine::network::test {

// Deliberately project-shaped fixtures: neither derives from an engine base nor
// includes an ECS, transport or gameplay library.
struct move_intent {
  uint16_t tick = 0;
  uint16_t principal = 0;
  uint16_t sequence = 0;
  int8_t x = 0;
  int8_t y = 0;

  constexpr bool operator==(const move_intent&) const noexcept = default;
};

struct transform_state_frame {
  uint32_t tick = 0;
  uint32_t entity = 0;
  float x = 0.0f;
  float y = 0.0f;
  float angle = 0.0f;

  constexpr bool operator==(const transform_state_frame&) const noexcept = default;
};

} // namespace devils_engine::network::test

#endif
