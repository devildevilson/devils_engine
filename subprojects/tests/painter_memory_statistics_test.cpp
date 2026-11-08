#include <doctest/doctest.h>
#include "devils_engine/painter/memory_statistics.h"
#include "devils_engine/utils/error.h"

TEST_CASE("VMA owners are combined per heap without inventing process usage") {
  using namespace devils_engine::painter;
  std::array<gpu_memory_statistics, 2> owners{};
  for (size_t i = 0; i < owners.size(); ++i) {
    owners[i].available = true;
    owners[i].physical_device = reinterpret_cast<VkPhysicalDevice>(uintptr_t(1));
    owners[i].owner = reinterpret_cast<VmaAllocator>(uintptr_t(i + 1));
    owners[i].heap_count = 2;
  }
  owners[0].heaps[0] = {40, 100, 2, 1};
  owners[1].heaps[0] = {30, 60, 3, 1};
  owners[1].heaps[1] = {10, 20, 1, 1};
  const auto combined = combine_memory_statistics(owners);
  CHECK(combined.available);
  CHECK(combined.heaps[0].allocation_bytes == 70);
  CHECK(combined.heaps[0].block_bytes == 160);
  CHECK(combined.total().allocation_bytes == 80);
  CHECK(combined.total().block_count == 3);
  owners[1].owner = owners[0].owner;
  CHECK_THROWS_AS(combine_memory_statistics(owners), devils_engine::utils::critical_error);
  owners[1].owner = reinterpret_cast<VmaAllocator>(uintptr_t(2));
  owners[1].physical_device = reinterpret_cast<VkPhysicalDevice>(uintptr_t(3));
  CHECK_THROWS_AS(combine_memory_statistics(owners), devils_engine::utils::critical_error);
  CHECK_FALSE(read_memory_statistics(VK_NULL_HANDLE, VK_NULL_HANDLE).available);
}
