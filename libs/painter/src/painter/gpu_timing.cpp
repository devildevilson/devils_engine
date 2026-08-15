#include "gpu_timing.h"

#include <algorithm>

#include "devils_engine/utils/core.h"
#include "graphics_base.h"
#include "vulkan_header.h"

namespace devils_engine::painter {

gpu_timestamp_profiler::gpu_timestamp_profiler(const graphics_base& base)
  : device_(base.device),
    frames_in_flight_(base.frames_in_flight()) {
  if (device_ == VK_NULL_HANDLE || base.physical_device == VK_NULL_HANDLE ||
      base.current_render_graph_index == invalid_resource_slot ||
      base.current_render_graph_index >= base.graphs.size()) {
    return;
  }

  const auto properties = vk::PhysicalDevice(base.physical_device).getProperties();
  const auto queue_families = vk::PhysicalDevice(base.physical_device).getQueueFamilyProperties();
  const uint32_t graphics_family = base.graphics.family_index();
  if (graphics_family >= queue_families.size()) return;

  timestamp_valid_bits_ = queue_families[graphics_family].timestampValidBits;
  timestamp_period_ns_ = properties.limits.timestampPeriod;
  const auto& graph = base.graphs[base.current_render_graph_index];
  pass_count_ = uint32_t(graph.passes.size());
  if (timestamp_valid_bits_ == 0 || timestamp_period_ns_ <= 0.0 ||
      frames_in_flight_ == 0 || pass_count_ == 0) {
    return;
  }

  passes_.reserve(pass_count_);
  for (const uint32_t pass_index : graph.passes) {
    if (pass_index >= base.passes.size()) {
      utils::error{}("GPU timestamp profiler found an invalid pass index {}", pass_index);
    }
    passes_.push_back(gpu_pass_timing{base.passes[pass_index].name, 0.0});
  }

  const uint32_t query_count = frames_in_flight_ * pass_count_ * 2;
  vk::QueryPoolCreateInfo info{};
  info.queryType = vk::QueryType::eTimestamp;
  info.queryCount = query_count;
  pool_ = vk::Device(device_).createQueryPool(info);
  set_name(vk::Device(device_), vk::QueryPool(pool_), "graphics_ctx.gpu_timestamps");
  recorded_.resize(frames_in_flight_, false);
  raw_results_.resize(pass_count_ * 2);
}

gpu_timestamp_profiler::~gpu_timestamp_profiler() noexcept {
  if (device_ != VK_NULL_HANDLE && pool_ != VK_NULL_HANDLE) {
    vk::Device(device_).destroy(vk::QueryPool(pool_));
  }
}

bool gpu_timestamp_profiler::available() const noexcept {
  return pool_ != VK_NULL_HANDLE;
}

bool gpu_timestamp_profiler::has_results() const noexcept {
  return has_results_;
}

double gpu_timestamp_profiler::frame_milliseconds() const noexcept {
  return frame_milliseconds_;
}

std::span<const gpu_pass_timing> gpu_timestamp_profiler::passes() const noexcept {
  return passes_;
}

uint32_t gpu_timestamp_profiler::first_query(const uint32_t frame_slot, const uint32_t pass_index) const noexcept {
  return (frame_slot * pass_count_ + pass_index) * 2;
}

void gpu_timestamp_profiler::prepare_frame(const uint32_t frame_slot, const uint32_t pass_count) {
  if (!available()) return;
  if (frame_slot >= frames_in_flight_ || pass_count != pass_count_) {
    utils::error{}(
      "GPU timestamp profiler graph mismatch: frame {}/{}, passes {}/{}",
      frame_slot,
      frames_in_flight_,
      pass_count,
      pass_count_);
  }
  if (!recorded_[frame_slot]) return;

  const auto result = vk::Device(device_).getQueryPoolResults(
    vk::QueryPool(pool_),
    first_query(frame_slot, 0),
    pass_count_ * 2,
    raw_results_.size() * sizeof(raw_results_[0]),
    raw_results_.data(),
    sizeof(raw_results_[0]),
    vk::QueryResultFlagBits::e64);
  if (result == vk::Result::eNotReady) return;
  if (result != vk::Result::eSuccess) {
    utils::error{}("Could not read GPU timestamp queries: {}", vk::to_string(result));
  }

  constexpr double smoothing = 0.15;
  for (uint32_t i = 0; i < pass_count_; ++i) {
    const uint64_t ticks = gpu_timestamp_delta(
      raw_results_[i * 2],
      raw_results_[i * 2 + 1],
      timestamp_valid_bits_);
    const double milliseconds = double(ticks) * timestamp_period_ns_ / 1'000'000.0;
    passes_[i].milliseconds = has_results_
                                ? passes_[i].milliseconds * (1.0 - smoothing) + milliseconds * smoothing
                                : milliseconds;
  }

  const uint64_t frame_ticks = gpu_timestamp_delta(
    raw_results_.front(),
    raw_results_.back(),
    timestamp_valid_bits_);
  const double milliseconds = double(frame_ticks) * timestamp_period_ns_ / 1'000'000.0;
  frame_milliseconds_ = has_results_
                          ? frame_milliseconds_ * (1.0 - smoothing) + milliseconds * smoothing
                          : milliseconds;
  has_results_ = true;
}

void gpu_timestamp_profiler::record_pass_begin(
  VkCommandBuffer command_buffer,
  const uint32_t frame_slot,
  const uint32_t pass_index) {
  if (!available()) return;
  const uint32_t query = first_query(frame_slot, pass_index);
  vk::CommandBuffer task(command_buffer);
  task.resetQueryPool(vk::QueryPool(pool_), query, 2);
  task.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, vk::QueryPool(pool_), query);
}

void gpu_timestamp_profiler::record_pass_end(
  VkCommandBuffer command_buffer,
  const uint32_t frame_slot,
  const uint32_t pass_index) {
  if (!available()) return;
  const uint32_t query = first_query(frame_slot, pass_index);
  vk::CommandBuffer(command_buffer).writeTimestamp(
    vk::PipelineStageFlagBits::eBottomOfPipe,
    vk::QueryPool(pool_),
    query + 1);
  if (pass_index + 1 == pass_count_) recorded_[frame_slot] = true;
}

} // namespace devils_engine::painter
