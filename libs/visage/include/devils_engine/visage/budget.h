#ifndef DEVILS_ENGINE_VISAGE_BUDGET_H
#define DEVILS_ENGINE_VISAGE_BUDGET_H

#include <cstdint>

namespace devils_engine {
namespace visage {

// Runtime policy for one UI frame. Zero disables the corresponding limit/step.
// The host owns persistence; visage only enforces the already resolved values.
struct budget_config {
  uint64_t lua_instruction_limit = 10000000;
  uint64_t lua_wall_time_us = 50000;
  uint32_t lua_gc_step_kib = 64;
  uint64_t convert_wall_time_us = 50000;
  uint64_t max_vertex_bytes = 8ull * 1024ull * 1024ull;
  uint64_t max_index_bytes = 4ull * 1024ull * 1024ull;
  uint32_t max_draw_commands = 65536;
  uint32_t disable_after_failures = 3;
};

} // namespace visage
} // namespace devils_engine

#endif
