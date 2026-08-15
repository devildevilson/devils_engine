#ifndef DEVILS_ENGINE_PAINTER_REGION_DRAW_H
#define DEVILS_ENGINE_PAINTER_REGION_DRAW_H

#include <cstddef>
#include <cstdint>
#include <limits>

namespace devils_engine::painter {

// CPU wire contract consumed by draw_regions. It deliberately contains only command-recording
// state and draw-group spans; scene/light/camera semantics live in the separately bound GPU data
// resource selected by data_index.
constexpr uint32_t region_draw_magic = 0x5245474eu; // "REGN"
constexpr uint32_t region_draw_version = 1u;

struct alignas(16) region_draw_header {
  uint32_t magic = region_draw_magic;
  uint32_t version = region_draw_version;
  uint32_t region_count = 0;
  uint32_t span_count = 0;
  uint32_t region_stride = 0;
  uint32_t span_stride = 0;
  uint32_t reserved[2]{};
};
static_assert(sizeof(region_draw_header) == 32);

struct alignas(16) region_draw_command {
  float viewport_x = 0.0f;
  float viewport_y = 0.0f;
  float viewport_width = 0.0f;
  float viewport_height = 0.0f;
  float min_depth = 0.0f;
  float max_depth = 1.0f;

  int32_t scissor_x = 0;
  int32_t scissor_y = 0;
  uint32_t scissor_width = 0;
  uint32_t scissor_height = 0;

  float depth_bias_constant = 0.0f;
  float depth_bias_clamp = 0.0f;
  float depth_bias_slope = 0.0f;

  uint32_t data_index = 0;
  uint32_t first_span = 0;
  uint32_t span_count = 0;
  uint32_t reserved[3]{};
};
static_assert(sizeof(region_draw_command) == 80);

struct alignas(16) region_draw_span {
  uint32_t pair_index = 0;
  uint32_t first_instance = 0;
  uint32_t instance_count = 0;
  uint32_t reserved = 0;
};
static_assert(sizeof(region_draw_span) == 16);

constexpr size_t region_draw_buffer_size(const uint32_t region_count, const uint32_t span_count) noexcept {
  constexpr size_t max = std::numeric_limits<size_t>::max();
  if (region_count > (max - sizeof(region_draw_header)) / sizeof(region_draw_command)) return max;
  const size_t regions_end = sizeof(region_draw_header) + size_t(region_count) * sizeof(region_draw_command);
  if (span_count > (max - regions_end) / sizeof(region_draw_span)) return max;
  return regions_end + size_t(span_count) * sizeof(region_draw_span);
}

} // namespace devils_engine::painter

#endif
