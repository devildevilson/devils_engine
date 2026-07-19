#ifndef DEVILS_ENGINE_PAINTER_QUEUE_H
#define DEVILS_ENGINE_PAINTER_QUEUE_H

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#include "vulkan_minimal.h"

namespace devils_engine::painter {

enum class queue_role : uint32_t {
  graphics,
  transfer,
  compute,
  count
};

struct queue_location {
  uint32_t family = UINT32_MAX;
  uint32_t index = UINT32_MAX;

  constexpr bool valid() const noexcept {
    return family != UINT32_MAX && index != UINT32_MAX;
  }

  friend constexpr bool operator==(const queue_location&, const queue_location&) noexcept = default;
};

struct queue_family_request {
  uint32_t family = UINT32_MAX;
  uint32_t count = 0;
};

struct device_queue_plan {
  queue_location graphics;
  queue_location transfer;
  queue_location compute;
  std::array<queue_family_request, 3> requests{};
  uint32_t request_count = 0;

  const queue_location& location(queue_role role) const noexcept;
};

// Assigns distinct queue indices inside a shared family while capacity permits. If a family is
// exhausted, the remaining roles alias queue 0 and therefore also share its external-sync mutex.
// Requests are sorted by family index so VkDevice creation is deterministic.
device_queue_plan make_device_queue_plan(
  std::span<const uint32_t> family_queue_counts,
  uint32_t graphics_family,
  uint32_t transfer_family,
  uint32_t compute_family);

namespace detail {
struct queue_state {
  VkQueue handle = VK_NULL_HANDLE;
  queue_location location;
  mutable std::mutex external_sync;
};
} // namespace detail

struct graphics_queue_tag {};
struct transfer_queue_tag {};
struct compute_queue_tag {};

template <typename Tag>
class typed_queue {
public:
  typed_queue() noexcept = default;

  VkQueue handle() const noexcept {
    return state_ != nullptr ? state_->handle : VK_NULL_HANDLE;
  }

  uint32_t family_index() const noexcept {
    return state_ != nullptr ? state_->location.family : UINT32_MAX;
  }

  uint32_t queue_index() const noexcept {
    return state_ != nullptr ? state_->location.index : UINT32_MAX;
  }

  explicit operator bool() const noexcept {
    return handle() != VK_NULL_HANDLE;
  }

  std::unique_lock<std::mutex> lock() const {
    return state_ != nullptr ? std::unique_lock(state_->external_sync) : std::unique_lock<std::mutex>{};
  }

  template <typename OtherTag>
  bool aliases(const typed_queue<OtherTag>& other) const noexcept {
    return state_ != nullptr && state_ == other.state_;
  }

private:
  explicit typed_queue(std::shared_ptr<detail::queue_state> state) noexcept : state_(std::move(state)) {}

  std::shared_ptr<detail::queue_state> state_;

  template <typename>
  friend class typed_queue;
  friend struct device_queues;
};

using graphics_queue = typed_queue<graphics_queue_tag>;
using transfer_queue = typed_queue<transfer_queue_tag>;
using compute_queue = typed_queue<compute_queue_tag>;

struct device_queues {
  graphics_queue graphics;
  transfer_queue transfer;
  compute_queue compute;

  static device_queues get(VkDevice device, const device_queue_plan& plan);
};

} // namespace devils_engine::painter

#endif
