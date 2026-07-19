#include "queue.h"

#include <algorithm>
#include <vector>

#include "devils_engine/utils/core.h"
#include "vulkan_header.h"

namespace devils_engine::painter {

const queue_location& device_queue_plan::location(const queue_role role) const noexcept {
  switch (role) {
    case queue_role::graphics: return graphics;
    case queue_role::transfer: return transfer;
    case queue_role::compute: return compute;
    case queue_role::count: break;
  }

  return graphics;
}

device_queue_plan make_device_queue_plan(
  const std::span<const uint32_t> family_queue_counts,
  const uint32_t graphics_family,
  const uint32_t transfer_family,
  const uint32_t compute_family) {
  device_queue_plan plan;
  const std::array families = {graphics_family, transfer_family, compute_family};
  std::array<queue_location*, 3> locations = {&plan.graphics, &plan.transfer, &plan.compute};

  for (uint32_t role_index = 0; role_index < families.size(); ++role_index) {
    const uint32_t family = families[role_index];
    if (family >= family_queue_counts.size() || family_queue_counts[family] == 0) {
      utils::error{}("Queue role {} refers to unavailable family {}", role_index, family);
    }

    uint32_t previous_roles = 0;
    for (uint32_t i = 0; i < role_index; ++i) {
      previous_roles += uint32_t(families[i] == family);
    }

    const uint32_t capacity = family_queue_counts[family];
    // Queue 0 is the stable fallback. In a two-queue universal family this gives graphics=0,
    // transfer=1, compute=0; with three queues every role is independent.
    const uint32_t index = previous_roles < capacity ? previous_roles : 0;
    *locations[role_index] = queue_location{family, index};

    bool found = false;
    for (uint32_t i = 0; i < plan.request_count; ++i) {
      if (plan.requests[i].family != family) {
        continue;
      }
      plan.requests[i].count = std::max(plan.requests[i].count, index + 1);
      found = true;
      break;
    }
    if (!found) {
      plan.requests[plan.request_count++] = queue_family_request{family, index + 1};
    }
  }

  std::sort(plan.requests.begin(), plan.requests.begin() + plan.request_count, [](const auto& a, const auto& b) {
    return a.family < b.family;
  });
  return plan;
}

device_queues device_queues::get(const VkDevice device, const device_queue_plan& plan) {
  if (device == VK_NULL_HANDLE) {
    utils::error{}("Could not get queues from a null VkDevice");
  }

  vk::Device dev(device);
  std::array<std::shared_ptr<detail::queue_state>, 3> states;
  const std::array locations = {plan.graphics, plan.transfer, plan.compute};

  for (uint32_t i = 0; i < locations.size(); ++i) {
    if (!locations[i].valid()) {
      utils::error{}("Could not get queue role {}: invalid queue location", i);
    }

    for (uint32_t j = 0; j < i; ++j) {
      if (locations[j] == locations[i]) {
        states[i] = states[j];
        break;
      }
    }

    if (states[i] == nullptr) {
      states[i] = std::make_shared<detail::queue_state>();
      states[i]->handle = dev.getQueue(locations[i].family, locations[i].index);
      states[i]->location = locations[i];
    }
  }

  device_queues queues;
  queues.graphics = graphics_queue(states[0]);
  queues.transfer = transfer_queue(states[1]);
  queues.compute = compute_queue(states[2]);
  return queues;
}

} // namespace devils_engine::painter
