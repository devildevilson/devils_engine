#ifndef DEVILS_ENGINE_PAINTER_GPU_TIMING_H
#define DEVILS_ENGINE_PAINTER_GPU_TIMING_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "vulkan_minimal.h"

namespace devils_engine::painter {

struct graphics_base;

struct gpu_pass_timing {
  std::string name;
  double milliseconds = 0.0;
};

// Computes an elapsed timestamp in a counter with timestamp_valid_bits significant bits.
// Kept public so wraparound behavior can be covered without a Vulkan device.
constexpr uint64_t gpu_timestamp_delta(
  const uint64_t begin,
  const uint64_t end,
  const uint32_t timestamp_valid_bits) noexcept {
  if (timestamp_valid_bits == 0) return 0;
  if (timestamp_valid_bits >= 64) return end - begin;
  const uint64_t mask = (uint64_t{1} << timestamp_valid_bits) - 1;
  return (end - begin) & mask;
}

// Optional per-render-pass timestamp queries. The caller owns this object and attaches it to one
// graphics_ctx. Query results are collected only after that frame-in-flight slot's fence has been
// waited by graphics_base::prepare_frame; ordinary contexts pay no query-pool or GPU timestamp cost.
class gpu_timestamp_profiler {
public:
  explicit gpu_timestamp_profiler(const graphics_base& base);
  ~gpu_timestamp_profiler() noexcept;

  gpu_timestamp_profiler(const gpu_timestamp_profiler&) = delete;
  gpu_timestamp_profiler& operator=(const gpu_timestamp_profiler&) = delete;
  gpu_timestamp_profiler(gpu_timestamp_profiler&&) = delete;
  gpu_timestamp_profiler& operator=(gpu_timestamp_profiler&&) = delete;

  bool available() const noexcept;
  bool has_results() const noexcept;
  double frame_milliseconds() const noexcept;
  std::span<const gpu_pass_timing> passes() const noexcept;

  // Internal recording seam used by graphics_ctx/execution_group.
  void prepare_frame(uint32_t frame_slot, uint32_t pass_count);
  void record_pass_begin(VkCommandBuffer command_buffer, uint32_t frame_slot, uint32_t pass_index);
  void record_pass_end(VkCommandBuffer command_buffer, uint32_t frame_slot, uint32_t pass_index);

private:
  uint32_t first_query(uint32_t frame_slot, uint32_t pass_index) const noexcept;

  VkDevice device_ = VK_NULL_HANDLE;
  VkQueryPool pool_ = VK_NULL_HANDLE;
  uint32_t frames_in_flight_ = 0;
  uint32_t pass_count_ = 0;
  uint32_t timestamp_valid_bits_ = 0;
  double timestamp_period_ns_ = 0.0;
  bool has_results_ = false;
  std::vector<bool> recorded_;
  std::vector<uint64_t> raw_results_;
  std::vector<gpu_pass_timing> passes_;
  double frame_milliseconds_ = 0.0;
};

} // namespace devils_engine::painter

#endif
