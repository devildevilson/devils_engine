#include "buffer_resources.h"

#include "vulkan_header.h"
#include "auxiliary.h"

namespace devils_engine {
namespace painter {

std::tuple<VkBuffer, VmaAllocation, void*> create_buffer(VmaAllocator allocator, const size_t size,const uint32_t usage, enum reside reside) {
  auto bflags = vk::BufferUsageFlags{};
  if ((usage & usage::storage) == usage::storage) bflags |= vk::BufferUsageFlagBits::eStorageBuffer;
  if ((usage & usage::uniform) == usage::uniform) bflags |= vk::BufferUsageFlagBits::eUniformBuffer;
  if ((usage & usage::indirect) == usage::indirect) bflags |= vk::BufferUsageFlagBits::eIndirectBuffer;
  if ((usage & usage::vertex) == usage::vertex) bflags |= vk::BufferUsageFlagBits::eVertexBuffer;
  if ((usage & usage::index) == usage::index) bflags |= vk::BufferUsageFlagBits::eIndexBuffer;
  if ((usage & usage::transfer_src) == usage::transfer_src) bflags |= vk::BufferUsageFlagBits::eTransferSrc;
  if ((usage & usage::transfer_dst) == usage::transfer_dst) bflags |= vk::BufferUsageFlagBits::eTransferDst;

  auto memusage = reside == reside::host ? vma::MemoryUsage::eCpuOnly : vma::MemoryUsage::eGpuOnly;
  assert(reside == reside::host || reside == reside::gpu);

  vma::AllocationInfo inf;
  const auto &[ binfo, ainfo ] = dedicated_buffer(size, bflags, memusage);
  auto [b, a] = vma::Allocator(allocator).createBuffer(binfo, ainfo, inf);
  return std::make_tuple(b, a, inf.pMappedData);
}

void destroy_buffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation) {
  vma::Allocator(allocator).destroyBuffer(buffer, allocation);
}

void copy(VkCommandBuffer cbuf, VkBuffer src, VkBuffer dst, size_t srcoffset, size_t dstoffset, size_t size) {
  vk::BufferCopy bc(srcoffset, dstoffset, size);
  vk::CommandBuffer(cbuf).copyBuffer(src, dst, bc);
}

static size_t get_proper_aligment(VmaAllocator allocator, const uint32_t usage) {
  size_t default_buffer_aligment = allocator_storage_aligment(allocator);
  if ((usage & usage::uniform) == usage::uniform) default_buffer_aligment = allocator_uniform_aligment(allocator);
  return default_buffer_aligment;
}

size_t align_to_device(const size_t size, VmaAllocator allocator, const uint32_t usage) {
  return utils::align_to(size, get_proper_aligment(allocator, usage));
}

common_buffer::common_buffer(VmaAllocator allocator, const size_t size, const uint32_t usage, enum reside reside) :
  allocator(allocator), _orig_size(align_to_device(size, allocator, usage)), allocation(VK_NULL_HANDLE), usage(usage), reside(reside)
{
  auto [b, a, m] = create_buffer(allocator, _orig_size, usage, reside);
  allocation = a;
  _mapped_data = m;
  buffer_provider::buffer = b; 
  buffer_provider::size = _orig_size;
}

common_buffer::~common_buffer() noexcept {
  destroy_buffer(allocator, buffer, allocation);
  buffer = VK_NULL_HANDLE;
}

size_t common_buffer::orig_size() const { return _orig_size; }
void* common_buffer::mapped_data() { return _mapped_data; }
void common_buffer::flush_memory() const {
  vma::Allocator(allocator).flushAllocation(allocation, 0, _orig_size);
}

void common_buffer::resize(const size_t new_size) {
  destroy_buffer(allocator, buffer, allocation);
  _orig_size = align_to_device(new_size, allocator, usage);
  auto [b, a, m] = create_buffer(allocator, _orig_size, usage, reside);
  allocation = a;
  _mapped_data = m;
  buffer_provider::buffer = b; 
  buffer_provider::size = _orig_size;
}

size_t reduce(const std::initializer_list<size_t> &sizes, const size_t aligment) {
  size_t all_size = 0;
  for (const auto &size : sizes) {
    all_size += utils::align_to(size, aligment);
  }
  return all_size;
}

packed_buffer::packed_buffer(VmaAllocator allocator, const std::initializer_list<size_t> &sizes, const uint32_t usage, enum reside reside) :
  common_buffer(allocator, reduce(sizes, get_proper_aligment(allocator, usage)), usage, reside)
{
  if (sizes.size() >= buffer_providers_count) utils::error("Too many sizes {} for packed_buffer", sizes.size());
  _count = sizes.size();
  size_t cur_offset = 0;
  for (size_t i = 0; i < sizes.size(); ++i) {
    providers[i].buffer = buffer;
    providers[i].offset = cur_offset;
    providers[i].size = utils::align_to(sizes.begin()[i], get_proper_aligment(allocator, usage));
    cur_offset += providers[i].size;
  }
}

packed_buffer::~packed_buffer() noexcept {
  for (size_t i = 0; i < _count; ++i) {
    providers[i].buffer = VK_NULL_HANDLE;
  }
}

const buffer_provider* packed_buffer::get(const size_t index) const {
  if (index >= _count) return nullptr;
  return &providers[index];
}

size_t packed_buffer::count() const { return _count; }

similar_buffer::similar_buffer(VmaAllocator allocator, const size_t individual_size, const size_t count, const uint32_t usage, enum reside reside) :
  packed_buffer(allocator, {align_to_device(individual_size, allocator, usage) * count}, usage, reside)
{
  if (count >= buffer_providers_count) utils::error("Too many sizes {} for similar_buffer", count);
  const size_t proper_size = utils::align_to(individual_size, standart_buffer_data_aligment);
  _count = count;
  size_t cur_offset = 0;
  for (size_t i = 0; i < _count; ++i) {
    providers[i].buffer = buffer;
    providers[i].offset = cur_offset;
    providers[i].size = align_to_device(individual_size, allocator, usage);
    cur_offset += providers[i].size;
  }
}



}
}