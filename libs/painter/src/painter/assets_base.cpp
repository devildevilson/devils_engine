#include "assets_base.h"

#include "devils_engine/utils/core.h"
#include "vulkan_header.h"
#include "auxiliary.h"
#include "graphics_base.h"

constexpr auto ATOMIC_ONLY = std::memory_order_relaxed;
constexpr auto PUBLISH = std::memory_order_release;
constexpr auto CONSUME = std::memory_order_acquire;
constexpr auto PUB_CONSUME = std::memory_order_acq_rel;

namespace devils_engine {
namespace painter  {
buffer_slot::buffer_slot() noexcept :
  state(asset_state::empty),
  forbid_after_frame(0),
  geometry(INVALID_RESOURCE_SLOT),
  vertex_count(0),
  first_vertex(0),
  vertex_offset(0),
  index_count(0),
  first_index(0),
  vertex_alc(VK_NULL_HANDLE),
  vertex_storage(VK_NULL_HANDLE),
  index_alc(VK_NULL_HANDLE),
  index_storage(VK_NULL_HANDLE),
  vertex_size(0),
  index_size(0)
{}

buffer_slot::buffer_slot(const buffer_slot& copy) noexcept :
  name(copy.name),
  geometry_name(copy.geometry_name),
  state(copy.state.load(CONSUME)),
  forbid_after_frame(copy.forbid_after_frame),
  geometry(copy.geometry),
  vertex_count(copy.vertex_count),
  first_vertex(copy.first_vertex),
  vertex_offset(copy.vertex_offset),
  index_count(copy.index_count),
  first_index(copy.first_index),
  vertex_alc(copy.vertex_alc),
  vertex_storage(copy.vertex_storage),
  index_alc(copy.index_alc),
  index_storage(copy.index_storage),
  vertex_size(copy.vertex_size),
  index_size(copy.index_size)
{}

buffer_slot& buffer_slot::operator=(const buffer_slot& copy) noexcept {
  name = copy.name;
  geometry_name = copy.geometry_name;
  state.store(copy.state.load(CONSUME), PUBLISH);
  forbid_after_frame = copy.forbid_after_frame;
  geometry = copy.geometry;
  vertex_count = copy.vertex_count;
  first_vertex = copy.first_vertex;
  vertex_offset = copy.vertex_offset;
  index_count = copy.index_count;
  first_index = copy.first_index;
  vertex_alc = copy.vertex_alc;
  vertex_storage = copy.vertex_storage;
  index_alc = copy.index_alc;
  index_storage = copy.index_storage;
  vertex_size = copy.vertex_size;
  index_size = copy.index_size;
  return *this;
}

buffer_slot::buffer_slot(buffer_slot&& move) noexcept :
  name(std::move(move.name)),
  geometry_name(std::move(move.geometry_name)),
  state(move.state.load(CONSUME)),
  forbid_after_frame(move.forbid_after_frame),
  geometry(move.geometry),
  vertex_count(move.vertex_count),
  first_vertex(move.first_vertex),
  vertex_offset(move.vertex_offset),
  index_count(move.index_count),
  first_index(move.first_index),
  vertex_alc(move.vertex_alc),
  vertex_storage(move.vertex_storage),
  index_alc(move.index_alc),
  index_storage(move.index_storage),
  vertex_size(move.vertex_size),
  index_size(move.index_size)
{}

buffer_slot& buffer_slot::operator=(buffer_slot&& move) noexcept {
  name = std::move(move.name);
  geometry_name = std::move(move.geometry_name);
  state.store(move.state.load(CONSUME), PUBLISH);
  forbid_after_frame = move.forbid_after_frame;
  geometry = move.geometry;
  vertex_count = move.vertex_count;
  first_vertex = move.first_vertex;
  vertex_offset = move.vertex_offset;
  index_count = move.index_count;
  first_index = move.first_index;
  vertex_alc = move.vertex_alc;
  vertex_storage = move.vertex_storage;
  index_alc = move.index_alc;
  index_storage = move.index_storage;
  vertex_size = move.vertex_size;
  index_size = move.index_size;
  return *this;
}

texture_slot::texture_slot() noexcept :
  state(asset_state::empty),
  forbid_after_frame(0),
  format(0),
  extents{0,0,0},
  alc(VK_NULL_HANDLE),
  storage(VK_NULL_HANDLE),
  view(VK_NULL_HANDLE)
{}

texture_slot::texture_slot(const texture_slot& copy) noexcept :
  name(copy.name),
  state(copy.state.load(CONSUME)),
  forbid_after_frame(copy.forbid_after_frame),
  format(copy.format),
  extents(copy.extents),
  alc(copy.alc),
  storage(copy.storage),
  view(copy.view)
{}

texture_slot& texture_slot::operator=(const texture_slot& copy) noexcept {
  name = copy.name;
  state.store(copy.state.load(CONSUME), PUBLISH);
  forbid_after_frame = copy.forbid_after_frame;
  format = copy.format;
  extents = copy.extents;
  alc = copy.alc;
  storage = copy.storage;
  view = copy.view;
  return *this;
}

texture_slot::texture_slot(texture_slot&& move) noexcept :
  name(std::move(move.name)),
  state(move.state.load(CONSUME)),
  forbid_after_frame(move.forbid_after_frame),
  format(move.format),
  extents(move.extents),
  alc(move.alc),
  storage(move.storage),
  view(move.view)
{}

texture_slot& texture_slot::operator=(texture_slot&& move) noexcept {
  name = std::move(move.name);
  state.store(move.state.load(CONSUME), PUBLISH);
  forbid_after_frame = move.forbid_after_frame;
  format = move.format;
  extents = move.extents;
  alc = move.alc;
  storage = move.storage;
  view = move.view;
  return *this;
}

assets_base::assets_base(VkDevice device, VkPhysicalDevice physical_device) noexcept :
  device(device),
  physical_device(physical_device),
  transfer(VK_NULL_HANDLE),
  command_pool(VK_NULL_HANDLE),
  command_buffer(VK_NULL_HANDLE),
  allocator(VK_NULL_HANDLE),
  base(nullptr)
{
  buffer_slots.resize(MAX_BUFFER_SLOTS);
  texture_slots.resize(MAX_TEXTURE_SLOTS);
}

assets_base::~assets_base() noexcept {
  if (device == VK_NULL_HANDLE) return;
  if (allocator == VK_NULL_HANDLE) return;

  if (base != nullptr) base->wait_all_fences();

  vk::Device dev(device);
  vma::Allocator a(allocator);

  for (auto& buf : buffer_slots) {
    a.destroyBuffer(buf.index_storage, buf.index_alc);
    a.destroyBuffer(buf.vertex_storage, buf.vertex_alc);
  }

  for (auto& tex : texture_slots) {
    dev.destroy(tex.view);
    a.destroyImage(tex.storage, tex.alc);
  }

  a.destroy();
  dev.destroy(command_pool);
  dev.destroy(fence);
}

void assets_base::create_fence() {
  fence = vk::Device(device).createFence(vk::FenceCreateInfo{});
}

void assets_base::create_command_buffer(VkQueue transfer, const uint32_t queue_family_index) {
  vk::CommandPoolCreateInfo cpci{};
  cpci.queueFamilyIndex = queue_family_index;
  cpci.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient;

  command_pool = vk::Device(device).createCommandPool(cpci);
  command_buffer = vk::Device(device).allocateCommandBuffers(vk::CommandBufferAllocateInfo(command_pool, vk::CommandBufferLevel::ePrimary, 1))[0];
  this->transfer = transfer;
}

void assets_base::create_allocator(VkInstance inst, const size_t preferred_heap_block) {
  vma::VulkanFunctions fns = make_functions();

  // аллокатор
  vma::AllocatorCreateInfo aci{};
  aci.instance = inst;
  aci.physicalDevice = physical_device;
  aci.device = device;
  aci.vulkanApiVersion = VK_API_VERSION_1_0;
  aci.pVulkanFunctions = &fns;
  aci.preferredLargeHeapBlockSize = preferred_heap_block;

  allocator = vma::createAllocator(aci);
}

void assets_base::set_graphics_base(const graphics_base* base) {
  this->base = base;
}

buffer_asset_handle assets_base::register_buffer_storage(std::string name) {
  uint32_t i = 0;
  for (; i < buffer_slots.size(); ++i) {
    auto& slot = buffer_slots[i];
    auto empty_state = asset_state::empty;
    const bool success = slot.state.compare_exchange_strong(
      empty_state, 
      asset_state::reserved, 
      CONSUME, // нужно ли нам что то записывать в empty?
      ATOMIC_ONLY
    );

    if (!success) continue;

    slot.name = std::move(name);
    slot.forbid_after_frame = 0;
    return i;
  }

  // пусть даже не догадывается то того что лежит в первом слоте
  return UINT32_MAX;
}

texture_asset_handle assets_base::register_texture_storage(std::string name) {
  uint32_t i = 0;
  for (; i < texture_slots.size(); ++i) {
    auto& slot = texture_slots[i];
    auto empty_state = asset_state::empty;
    const bool success = slot.state.compare_exchange_strong(
      empty_state,
      asset_state::reserved,
      CONSUME, // нужно ли нам что то записывать в empty?
      ATOMIC_ONLY
    );

    if (!success) continue;

    slot.name = std::move(name);
    slot.forbid_after_frame = 0;
    return i;
  }

  // пусть даже не догадывается то того что лежит в первом слоте
  return UINT32_MAX;
}

void assets_base::clear_buffer_storage(const buffer_asset_handle& h) {
  if (h >= buffer_slots.size()) utils::error{}("Assets buffer_slots must not change. Got buffer_asset_handle::slot {}", h);

  const auto s = buffer_slots[h].state.load(CONSUME);
  if (s != asset_state::pending_remove) return;

  // проверим текущий кадр

  auto& buf = buffer_slots[h];

  vma::Allocator a(allocator);
  a.destroyBuffer(buf.index_storage, buf.index_alc);
  a.destroyBuffer(buf.vertex_storage, buf.vertex_alc);

  buf.index_storage = VK_NULL_HANDLE;
  buf.index_alc = VK_NULL_HANDLE;
  buf.vertex_storage = VK_NULL_HANDLE;
  buf.vertex_alc = VK_NULL_HANDLE;

  buf.geometry = INVALID_RESOURCE_SLOT;

  buf.vertex_size = 0;
  buf.index_size = 0;

  buf.vertex_count = 0;
  buf.index_count = 0;

  buf.state.store(asset_state::empty, PUBLISH);
}

void assets_base::clear_texture_storage(const texture_asset_handle& h) {
  if (h >= texture_slots.size()) utils::error{}("Assets texture_slots must not change. Got buffer_asset_handle::slot {}", h);

  const auto s = texture_slots[h].state.load(CONSUME);
  if (s != asset_state::pending_remove) return;

  // проверим текущий кадр

  auto& tex = texture_slots[h];

  vk::Device dev(device);
  vma::Allocator a(allocator);
  dev.destroy(tex.view);
  a.destroyImage(tex.storage, tex.alc);

  tex.storage = VK_NULL_HANDLE;
  tex.alc = VK_NULL_HANDLE;
  tex.view = VK_NULL_HANDLE;

  tex.format = 0;
  memset(&tex.extents, 0, sizeof(tex.extents));

  tex.state.store(asset_state::empty, PUBLISH);
}

buffer_asset_handle assets_base::find_buffer_storage(const std::string_view& name) const {
  // адекватный поиск только когда state == ready?
  // похоже на то, но часто ли нам нужно будет поиск такой делать?

  for (uint32_t i = 0; i < buffer_slots.size(); ++i) {
    const auto s = buffer_slots[i].state.load(CONSUME);
    if (s == asset_state::ready && buffer_slots[i].name == name) return i;
  }

  return UINT32_MAX;
}

texture_asset_handle assets_base::find_texture_storage(const std::string_view& name) const {
  for (uint32_t i = 0; i < texture_slots.size(); ++i) {
    const auto s = texture_slots[i].state.load(CONSUME);
    if (s == asset_state::ready && texture_slots[i].name == name) return i;
  }

  return UINT32_MAX;
}

void assets_base::create_buffer_storage(const buffer_asset_handle& h, const buffer_create_info& info) {
  if (h >= buffer_slots.size()) utils::error{}("Assets buffer_slots must not change. Got buffer_asset_handle::slot {}", h);

  // тут мы просто проверим если состояние reserved то создадим ГПУ ресурсы
  const auto s = buffer_slots[h].state.load(CONSUME);
  if (s != asset_state::reserved) return;

  // тут нужно найти геометрию 
  const uint32_t index = base->find_geometry(info.geometry_name);
  if (index == INVALID_RESOURCE_SLOT) utils::error{}("Could not find geometry '{}' for buffer '{}'", info.geometry_name, buffer_slots[h].name);

  const auto& geo = DS_ASSERT_ARRAY_GET(base->geometries, index);

  const size_t vertex_size = geo.stride * info.vertex_count;
  const size_t index_size = geo.index_size() * info.index_count;

  vk::Device dev(device);
  vma::Allocator a(allocator);

  vk::BufferCreateInfo bci{};
  bci.size = vertex_size;
  bci.usage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer;

  vma::AllocationCreateInfo aci{};
  aci.usage = vma::MemoryUsage::eGpuOnly;

  vma::AllocationInfo v_ai{};
  vma::AllocationInfo i_ai{};
  const auto& [v_buf, v_alc] = a.createBuffer(bci, aci, &v_ai);
  bci.size = index_size;
  bci.usage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer;
  const auto& [i_buf, i_alc] = index_size != 0 ? a.createBuffer(bci, aci, &i_ai) : std::make_pair(vk::Buffer{}, vma::Allocation{});

  set_name(dev, v_buf, buffer_slots[h].name + "_vertex");
  if (index_size != 0) set_name(dev, i_buf, buffer_slots[h].name + "_index");

  //buffer_slots[h].name = info.name;
  buffer_slots[h].geometry_name = info.geometry_name;
  buffer_slots[h].index_alc = i_alc;
  buffer_slots[h].index_storage = i_buf;
  buffer_slots[h].vertex_alc = v_alc;
  buffer_slots[h].vertex_storage = v_buf;
  buffer_slots[h].index_count = info.index_count;
  buffer_slots[h].vertex_count = info.vertex_count;
  buffer_slots[h].index_size = index_size;
  buffer_slots[h].vertex_size = vertex_size;
}

void assets_base::create_texture_storage(const texture_asset_handle& h, const texture_create_info& info) {
  if (h >= texture_slots.size()) utils::error{}("Assets texture_slots must not change. Got buffer_asset_handle::slot {}", h);

  // тут мы просто проверим если состояние reserved то создадим ГПУ ресурсы
  const auto s = texture_slots[h].state.load(CONSUME);
  if (s != asset_state::reserved) return;

  vk::Device dev(device);
  vma::Allocator a(allocator);

  vk::ImageCreateInfo ici{};
  ici.format = static_cast<vk::Format>(info.format);
  ici.imageType = vk::ImageType::e2D; // пока работаем только с 2д картинками
  ici.extent = vk::Extent3D{ info.extents.x, info.extents.y, 1 };
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = vk::SampleCountFlagBits::e1;
  ici.tiling = vk::ImageTiling::eOptimal;
  ici.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;

  vma::AllocationCreateInfo aci{};
  aci.usage = vma::MemoryUsage::eGpuOnly;

  const auto& [image, allocation] = a.createImage(ici, aci);

  vk::ImageViewCreateInfo ivci{};
  ivci.image = image;
  ivci.viewType = vk::ImageViewType::e2D;
  ivci.format = ici.format;
  ivci.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };

  auto view = dev.createImageView(ivci);

  set_name(dev, image, texture_slots[h].name + "_storage");
  set_name(dev, view, texture_slots[h].name + "_view");

  //texture_slots[h].name = info.name;
  texture_slots[h].alc = allocation;
  texture_slots[h].storage = image;
  texture_slots[h].view = view;
  texture_slots[h].format = info.format;
  texture_slots[h].extents = { info.extents.x, info.extents.y, 1 };
}

void assets_base::populate_buffer_storage(const buffer_asset_handle& h, const std::span<const uint8_t>& vertex_data, const std::span<const uint8_t>& index_data) {
  if (h >= buffer_slots.size()) utils::error{}("Assets buffer_slots must not change. Got buffer_asset_handle::slot {}", h);

  // тут мы просто проверим если состояние reserved то создадим ГПУ ресурсы
  const auto s = buffer_slots[h].state.load(CONSUME);
  if (s != asset_state::reserved) return;

  // вот тут нужны стаджинг буферы
  // мы их тут создаем? вообще может быть и нет
  // на самом деле нужно оставить и такой вариант и другой

  const auto& slot = buffer_slots[h];

  assert(vertex_data.size() == slot.vertex_size);
  assert(index_data.size() == slot.index_size);

  vk::Device dev(device);
  vma::Allocator a(allocator);

  vk::BufferCreateInfo bci{};
  bci.size = vertex_data.size();
  bci.usage = vk::BufferUsageFlagBits::eTransferSrc;

  vma::AllocationCreateInfo aci{};
  aci.usage = vma::MemoryUsage::eCpuOnly;
  aci.flags = vma::AllocationCreateFlagBits::eMapped;

  vma::AllocationInfo v_ai{};
  vma::AllocationInfo i_ai{};
  const auto& [v_buf, v_alc] = a.createBuffer(bci, aci, &v_ai);
  bci.size = index_data.size();
  const auto& [i_buf, i_alc] = !index_data.empty() ? a.createBuffer(bci, aci, &i_ai) : std::make_pair(vk::Buffer{}, vma::Allocation{});

  memcpy(v_ai.pMappedData, vertex_data.data(), vertex_data.size());
  if (!index_data.empty()) memcpy(i_ai.pMappedData, index_data.data(), index_data.size());

  a.flushAllocation(v_alc, 0, vertex_data.size());
  if (!index_data.empty()) a.flushAllocation(i_alc, 0, index_data.size());

  const vk::BufferCopy v_c(0, 0, vertex_data.size());
  const vk::BufferCopy i_c(0, 0, index_data.size());
  do_command(device, transfer, fence, command_buffer, [&](VkCommandBuffer cb) {
    vk::CommandBuffer task(cb);
    task.copyBuffer(v_buf, slot.vertex_storage, v_c);
    if (!index_data.empty()) task.copyBuffer(i_buf, slot.index_storage, i_c);
  });

  a.destroyBuffer(v_buf, v_alc);
  if (!index_data.empty()) a.destroyBuffer(i_buf, i_alc);
}

void assets_base::populate_texture_storage(const texture_asset_handle& h, const std::span<const uint8_t>& data) {
  if (h >= texture_slots.size()) utils::error{}("Assets texture_slots must not change. Got buffer_asset_handle::slot {}", h);

  const auto s = texture_slots[h].state.load(CONSUME);
  if (s != asset_state::reserved) return;

  const auto& slot = texture_slots[h];

  const size_t computed_size = size_t(slot.extents.x) * size_t(slot.extents.y) * size_t(format_element_size(slot.format));
  assert(data.size() == computed_size);

  vk::Device dev(device);
  vma::Allocator a(allocator);

  vk::BufferCreateInfo bci{};
  bci.size = data.size();
  bci.usage = vk::BufferUsageFlagBits::eTransferSrc;

  vma::AllocationCreateInfo aci{};
  aci.usage = vma::MemoryUsage::eCpuOnly;
  aci.flags = vma::AllocationCreateFlagBits::eMapped;

  vma::AllocationInfo ai{};
  const auto& [buf, allocation] = a.createBuffer(bci, aci, &ai);
  
  memcpy(ai.pMappedData, data.data(), data.size());

  a.flushAllocation(allocation, 0, data.size());

  const auto range1 = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
  const auto range2 = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };

  vk::ImageMemoryBarrier bar1(vk::AccessFlags{}, vk::AccessFlagBits::eTransferWrite, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, slot.storage, range1);
  vk::ImageMemoryBarrier bar2(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, slot.storage, range1);
  vk::BufferImageCopy bic(0, slot.extents.x, slot.extents.y, range2, { 0,0,0 }, { slot.extents.x, slot.extents.y, 1 });

  do_command(device, transfer, fence, command_buffer, [&](VkCommandBuffer cb) {
    vk::CommandBuffer task(cb);
    task.pipelineBarrier(
      vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
      nullptr, nullptr, bar1
    );

    task.copyBufferToImage(buf, slot.storage, vk::ImageLayout::eTransferDstOptimal, bic);

    task.pipelineBarrier(
      vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
      nullptr, nullptr, bar2
    );
  });

  a.destroyBuffer(buf, allocation);

  // ну и вроде все
}

void assets_base::mark_ready_buffer_slot(const buffer_asset_handle& h) {
  if (h >= buffer_slots.size()) utils::error{}("Assets buffer_slots must not change. Got buffer_asset_handle::slot {}", h);

  buffer_slots[h].state.store(asset_state::ready, PUBLISH);
}

void assets_base::mark_ready_texture_slot(const texture_asset_handle& h) {
  if (h >= texture_slots.size()) utils::error{}("Assets texture_slots must not change. Got buffer_asset_handle::slot {}", h);

  texture_slots[h].state.store(asset_state::ready, PUBLISH);
}

void assets_base::mark_remove_buffer_slot(const buffer_asset_handle& h) {
  if (h >= buffer_slots.size()) utils::error{}("Assets buffer_slots must not change. Got buffer_asset_handle::slot {}", h);

  // тут пока ничего удалять не будем а просто пометим слот к удалению
  buffer_slots[h].forbid_after_frame = base->current_frame_index() + base->frames_in_flight() + 1;
  buffer_slots[h].state.store(asset_state::pending_remove, PUBLISH);
}

void assets_base::mark_remove_texture_slot(const texture_asset_handle& h) {
  if (h >= texture_slots.size()) utils::error{}("Assets texture_slots must not change. Got buffer_asset_handle::slot {}", h);

  // тут пока ничего удалять не будем а просто пометим слот к удалению
  texture_slots[h].forbid_after_frame = base->current_frame_index() + base->frames_in_flight() + 1;
  texture_slots[h].state.store(asset_state::pending_remove, PUBLISH);
}

}
}