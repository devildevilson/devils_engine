#include "memory_statistics.h"
#include <algorithm>
#include <limits>
#include <vector>
#include "vulkan_header.h"
#include "devils_engine/utils/error.h"

namespace devils_engine::painter {
static_assert(max_gpu_memory_heaps == VK_MAX_MEMORY_HEAPS);
namespace {
uint64_t add(uint64_t a, uint64_t b) {
  if (b > std::numeric_limits<uint64_t>::max() - a) utils::error{}("painter memory statistics: counter overflow ({} + {})", a, b);
  return a + b;
}
void accumulate(heap_memory_statistics& into, const heap_memory_statistics& from) {
  into.allocation_bytes = add(into.allocation_bytes, from.allocation_bytes);
  into.block_bytes = add(into.block_bytes, from.block_bytes);
  into.allocation_count = add(into.allocation_count, from.allocation_count);
  into.block_count = add(into.block_count, from.block_count);
}
}
heap_memory_statistics gpu_memory_statistics::total() const {
  if (heap_count > heaps.size()) utils::error{}("painter memory statistics: heap count {} exceeds {}", heap_count, heaps.size());
  heap_memory_statistics result;
  for (uint32_t i = 0; i < heap_count; ++i) accumulate(result, heaps[i]);
  return result;
}
gpu_memory_statistics read_memory_statistics(VmaAllocator allocator, VkPhysicalDevice physical_device) {
  gpu_memory_statistics result;
  if (!allocator) return result;
  result.available = true;
  result.owner = allocator;
  result.physical_device = physical_device;
  const VkPhysicalDeviceMemoryProperties* properties = nullptr;
  vmaGetMemoryProperties(allocator, &properties);
  result.heap_count = properties->memoryHeapCount;
  std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
  vmaGetHeapBudgets(allocator, budgets.data());
  for (uint32_t i = 0; i < result.heap_count; ++i) {
    const auto& stats = budgets[i].statistics;
    result.heaps[i] = {stats.allocationBytes, stats.blockBytes, stats.allocationCount, stats.blockCount};
  }
  return result;
}
gpu_memory_statistics combine_memory_statistics(std::span<const gpu_memory_statistics> owners) {
  gpu_memory_statistics result;
  std::vector<VmaAllocator> seen;
  for (const auto& snapshot : owners) {
    if (!snapshot.available) continue;
    if (snapshot.heap_count > snapshot.heaps.size()) utils::error{}("painter memory statistics: invalid heap count {}", snapshot.heap_count);
    if (result.available && (snapshot.physical_device != result.physical_device || snapshot.heap_count != result.heap_count)) utils::error{}("painter memory statistics: owners refer to different physical heaps");
    if (snapshot.owner && std::find(seen.begin(), seen.end(), snapshot.owner) != seen.end()) utils::error{}("painter memory statistics: allocator counted twice");
    seen.push_back(snapshot.owner);
    result.available = true;
    result.physical_device = snapshot.physical_device;
    result.heap_count = snapshot.heap_count;
    for (uint32_t i = 0; i < result.heap_count; ++i) accumulate(result.heaps[i], snapshot.heaps[i]);
  }
  return result;
}
} // namespace devils_engine::painter
