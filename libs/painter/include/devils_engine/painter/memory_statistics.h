#pragma once

#include <array>
#include <cstdint>
#include <span>
#include "vulkan_minimal.h"

namespace devils_engine::painter {
inline constexpr uint32_t max_gpu_memory_heaps = 16;
struct heap_memory_statistics {
  uint64_t allocation_bytes = 0;
  uint64_t block_bytes = 0;
  uint64_t allocation_count = 0;
  uint64_t block_count = 0;
};
// Owned VMA memory only. Retiring allocations remain counted until real free.
// Driver/swapchain/external allocations and process-wide heap budget are not added here.
struct gpu_memory_statistics {
  bool available = false;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VmaAllocator owner = VK_NULL_HANDLE;
  uint32_t heap_count = 0;
  std::array<heap_memory_statistics, max_gpu_memory_heaps> heaps{};
  heap_memory_statistics total() const;
};
gpu_memory_statistics read_memory_statistics(VmaAllocator allocator, VkPhysicalDevice physical_device);
gpu_memory_statistics combine_memory_statistics(std::span<const gpu_memory_statistics> owners);
} // namespace devils_engine::painter
