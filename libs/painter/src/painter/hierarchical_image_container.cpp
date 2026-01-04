#include "hierarchical_image_container.h"

#include "makers.h"
#include "auxiliary.h"
#include <algorithm>

namespace devils_engine {
namespace painter {
bool comp(const hierarchical_image_container2::size_info &a, const hierarchical_image_container2::size_info &b) {
  const size_t factor_a = size_t(std::max(a.width, a.height)) * 10000 + std::min(a.width, a.height);
  const size_t factor_b = size_t(std::max(b.width, b.height)) * 10000 + std::min(b.width, b.height);
  return factor_a < factor_b;
}

hierarchical_image_container2::hierarchical_image_container2(VkDevice device, VmaAllocator allocator, const uint32_t format, std::vector<size_info> info) : 
  device(device) 
{
  std::sort(info.begin(), info.end(), &comp);

  for (const auto & inf : info) {
    pools.push_back(std::make_unique<image_pool>("dependant_pool", device, allocator, format, image_container::extent_t{inf.width, inf.height}, inf.count));
  }
}

hierarchical_image_container2::~hierarchical_image_container2() noexcept {}

// надо прилично так постараться чтобы использовать аж 65к картинок, 
// поэтому положим в первые 16 бит индекс в иерархии
uint32_t hierarchical_image_container2::aquire_image(const uint32_t width, const uint32_t height) {
  for (size_t i = 0; i < pools.size(); ++i) {
    auto & pool = pools[i];
    const auto [ local_width, local_height ] = pool->extent();
    if (width < local_width || height < local_height) continue; 
    if (!pool->has_free_slots()) return UINT32_MAX;
    const auto idx = pool->aquire_slot();
    return (i << 16 | idx);
  }

  return UINT32_MAX;
}

uint32_t hierarchical_image_container2::aquire_image_any(const uint32_t width, const uint32_t height) {
  for (size_t i = 0; i < pools.size(); ++i) {
    auto & pool = pools[i];
    const auto [ local_width, local_height ] = pool->extent();
    if (width < local_width || height < local_height) continue; 
    if (!pool->has_free_slots()) continue;
    const auto idx = pool->aquire_slot();
    return (i << 16 | idx);
  }

  return UINT32_MAX;
}

void hierarchical_image_container2::free_image(const uint32_t slot) {
  const uint32_t index = slot >> 16;
  const uint32_t slot_idx = slot & 0xffff;
  if (index > pools.size()) return;
  pools[index]->free_slot(slot_idx);
}

VkImage hierarchical_image_container2::storage(const uint32_t slot) const {
  const uint32_t index = slot >> 16;
  if (index > pools.size()) return VK_NULL_HANDLE;
  return pools[index]->storage();
}

VkImageView hierarchical_image_container2::view(const uint32_t slot) const {
  const uint32_t index = slot >> 16;
  if (index > pools.size()) return VK_NULL_HANDLE;
  return pools[index]->view();
}

size_t hierarchical_image_container2::capaсity(const uint32_t slot) const {
  const uint32_t index = slot >> 16;
  if (index > pools.size()) return 0;
  return pools[index]->capacity();
}

size_t hierarchical_image_container2::size(const uint32_t slot) const {
  const uint32_t index = slot >> 16;
  if (index > pools.size()) return SIZE_MAX;
  return pools[index]->size();
}

std::tuple<uint32_t, uint32_t> hierarchical_image_container2::extent(const uint32_t slot) const {
  const uint32_t index = slot >> 16;
  if (index > pools.size()) return {0,0};
  return pools[index]->extent();
}

void hierarchical_image_container2::write_descriptor_set(VkDescriptorSet set, const uint32_t binding, const uint32_t start_element) const {
  descriptor_set_updater dsu(device);
  dsu.currentSet(set).begin(binding, start_element, vk::DescriptorType::eCombinedImageSampler);
  for (const auto & pool : pools) {
    dsu.image(pool->view(), vk::ImageLayout::eShaderReadOnlyOptimal);
  }
  dsu.update();
}

hierarchical_image_container::hierarchical_image_container(std::string name, VkDevice device, VkPhysicalDevice physical_device, extent_t memory_extents, extent_t block_extents) :
  image_container(std::move(name)), device(device), physical_device(physical_device), memory_extents(memory_extents), block_extents(block_extents)
{
  if (memory_extents.width < block_extents.width * 2) utils::error{}("Block extents ({}, {}) too big for this memory extent ({}, {})", block_extents.width, block_extents.height, memory_extents.width, memory_extents.height);
  if (memory_extents.height < block_extents.height * 2) utils::error{}("Block extents ({}, {}) too big for this memory extent ({}, {})", block_extents.width, block_extents.height, memory_extents.width, memory_extents.height);
  if (block_extents.width != block_extents.height) utils::error{}("Block extents ({}, {}) should be equal", block_extents.width, block_extents.height);
  if (memory_extents.width != utils::next_power_of_2(memory_extents.width) ||
      memory_extents.height != utils::next_power_of_2(memory_extents.height) ||
      block_extents.width != utils::next_power_of_2(block_extents.width) ||
      block_extents.height != utils::next_power_of_2(block_extents.height)) 
    utils::error{}("Sizes must be a power-of-2 number, for ex: ({}, {}), ({}, {})", utils::next_power_of_2(memory_extents.width), utils::next_power_of_2(memory_extents.height), utils::next_power_of_2(block_extents.width), utils::next_power_of_2(block_extents.height));

  const auto yes_flags = vk::MemoryPropertyFlagBits::eDeviceLocal;
  const auto no_flags = vk::MemoryPropertyFlagBits::eHostCached | vk::MemoryPropertyFlagBits::eHostCoherent | vk::MemoryPropertyFlagBits::eHostVisible;

  vk::Device d(device);
  vk::PhysicalDevice pd(physical_device);
  const auto props = pd.getMemoryProperties();
  size_t i = 0;
  for (; i < props.memoryTypeCount; ++i) {
    const auto &type = props.memoryTypes[i];
    
    if ((type.propertyFlags & yes_flags) == yes_flags && uint32_t(type.propertyFlags & no_flags) == 0) {
      break;
    }
  }

  if (i >= props.memoryTypeCount) utils::error{}("Could not find appropriate memory type");

  const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
  const auto inf1 = texture2D({block_extents.width, block_extents.height}, usage);
  const auto inf2 = texture2D({memory_extents.width, memory_extents.height}, usage);
  auto block_img = d.createImage(inf1);
  auto memory_img = d.createImage(inf2);
  const auto req1 = d.getImageMemoryRequirements(block_img);
  const auto req2 = d.getImageMemoryRequirements(memory_img);

  const size_t final_memory_size = utils::align_to(req2.size, req1.size);
  utils::println(req1.alignment, req2.alignment, req1.size, req2.size, final_memory_size);

  vk::MemoryAllocateInfo mai(final_memory_size, i);
  memory = d.allocateMemory(mai);
  set_name(device, vk::DeviceMemory(memory), container_name + "_hierarchical_device_memory");

  d.destroy(block_img);
  d.destroy(memory_img);

  const size_t overall_images_count = final_memory_size / req1.size;
  block_usage.resize(overall_images_count);
  images.resize(overall_images_count);

  block_size = req1.size;
  memory_size = final_memory_size;

  auto inf_null = texture2D({4, 4}, usage);
  inf_null.initialLayout = vk::ImageLayout::ePreinitialized;
  null_image.handle = d.createImage(inf1);
  set_name(device, vk::Image(null_image.handle), container_name + "_hierarchical_null_image");
  const auto ivci = make_view_info(null_image.handle);
  null_image.view = d.createImageView(ivci);
  set_name(device, vk::ImageView(null_image.view), container_name + "_hierarchical_null_image_view");
  const auto req_null = d.getImageMemoryRequirements(block_img);
  vk::MemoryAllocateInfo mai2(req_null.size, i);
  null_memory = d.allocateMemory(mai2);
  set_name(device, vk::DeviceMemory(null_memory), container_name + "_hierarchical_null_image_mamory");
  d.bindImageMemory(null_image.handle, null_memory, 0);
  sampler_maker sm(device);
  null_image.sampler = sm.create(container_name + "_null_sampler");
  null_image.width = 4; null_image.height = 4; null_image.format = uint32_t(vk::Format::eR8G8B8A8Unorm); null_image.size = req_null.size;
}

hierarchical_image_container::~hierarchical_image_container() noexcept {
  clear();
  
  vk::Device(device).destroy(null_image.view);
  vk::Device(device).destroy(null_image.handle);
  vk::Device(device).destroy(null_image.sampler);
  vk::Device(device).free(null_memory);

  vk::Device(device).free(memory);
}

bool hierarchical_image_container::is_exists(const uint32_t index) const {
  if (index >= images.size()) return false;
  return images[index].handle != VK_NULL_HANDLE;
}

uint32_t hierarchical_image_container::create(std::string name, const extent_t extent, const extent_t real_extent, const uint32_t format, VkSampler sampler) {
  if (const uint32_t el_size = format_element_size(format); el_size > sizeof(uint32_t)) {
    utils::error{}("Format '{}' has element size {} more than standart format '{}'. It is not currently supported", vk::to_string(vk::Format(format)), el_size, vk::to_string(vk::Format::eR8G8B8A8Unorm));
  }

  if (extent.width < block_extents.width || extent.height < block_extents.height) {
    utils::warn("Image size too small ({}, {}) for image '{}'", extent.width, extent.height, name);
  }

  if (sampler == VK_NULL_HANDLE) utils::error{}("Trying to create image view '{}' without a sampler object", name);

  // это совсем строгий алгоритм, но он позволит избежать сильной фрагментации
  // можем ли мы тут попробовать взять хотя бы какой то размер уменьшая запрашиваемый размер текстурки?
  // вообще можем
  const uint32_t width_pow2 = utils::next_power_of_2(extent.width);
  const uint32_t height_pow2 = utils::next_power_of_2(extent.height);

  if (width_pow2 == memory_extents.width || height_pow2 == memory_extents.height) utils::error{}("Image '{}' size ({}, {}) too big", name, extent.width, extent.height);

  // предположим что мы правильно указали block_extents
  const uint32_t bit_image_size = std::max(width_pow2, height_pow2) / block_extents.width;

  // bit_size позволит нам выделить регион памяти по которому мы бы хотели пробежаться
  const uint32_t bit_field_width = memory_extents.width / block_extents.width;
  const uint32_t bit_field_height = memory_extents.height / block_extents.height;

  sul::dynamic_bitset<size_t> image_bitset(block_usage.size(), 0);
  for (size_t i = 0; i < bit_image_size; ++i) {
    image_bitset.set(i, true);
  }

  size_t offset = SIZE_MAX;
  for (size_t i = 0; i < block_usage.size() / bit_image_size; ++i, image_bitset <<= bit_image_size) {
    const auto res = (block_usage & image_bitset) == image_bitset;
    if (res) {
      offset = i * bit_image_size * block_size;
      break;
    }
  }

  if (offset == SIZE_MAX) return UINT32_MAX;

  // занимаем слот
  block_usage |= image_bitset;
  const uint32_t image_size = width_pow2 * height_pow2 * sizeof(uint32_t);
  utils::println("offset", offset, "size", image_size);

  vk::Device d(device);
  //const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
  const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
  const auto inf1 = texture2D({real_extent.width, real_extent.height}, usage, vk::Format(format));
  auto img = d.createImage(inf1);
  d.bindImageMemory(img, memory, offset);
  vk::ImageViewCreateInfo ivci({}, img, vk::ImageViewType::e2D, vk::Format(format));
  auto view = d.createImageView(ivci);

  set_name(device, vk::Image(img), name);
  set_name(device, vk::ImageView(view), name + "_view");
  
  const uint32_t final_index = offset / block_size;

  utils_assertf(images[final_index].handle == VK_NULL_HANDLE, "Index {} is not empty", final_index);

  images[final_index].handle = img;
  images[final_index].view = view;
  images[final_index].sampler = sampler;
  images[final_index].width = real_extent.width;
  images[final_index].height = real_extent.height;
  images[final_index].format = format;
  images[final_index].offset = offset;
  images[final_index].size = image_size;
  images[final_index].name = std::move(name);

  return final_index;
}

uint32_t hierarchical_image_container::create(std::string name, const extent_t extent, const uint32_t format, VkSampler sampler) {
  return create(std::move(name), extent, extent, format, sampler);
}

uint32_t hierarchical_image_container::create_any(std::string name, const extent_t extent, const uint32_t format, VkSampler sampler) {
  uint32_t width_pow2 = std::max(utils::next_power_of_2(extent.width), block_extents.width);
  uint32_t height_pow2 = std::max(utils::next_power_of_2(extent.height), block_extents.height);
  uint32_t orig_width = extent.width;
  uint32_t orig_height = extent.height;
  // тут размер квадратным не надо сделать? было бы полезно наверное

  bool not_less_than_block = width_pow2 >= block_extents.width && height_pow2 >= block_extents.height;
  uint32_t final_index = UINT32_MAX;
  while (final_index == UINT32_MAX && not_less_than_block) {
    final_index = create(name, { width_pow2, height_pow2 }, { orig_width, orig_height }, format, sampler);
    width_pow2 /= 2;
    height_pow2 /= 2;
    orig_width /= 2;  orig_width  = utils::align_to( orig_width, 4);
    orig_height /= 2; orig_height = utils::align_to(orig_height, 4);
    not_less_than_block = width_pow2 >= block_extents.width && height_pow2 >= block_extents.height;
  }

  return final_index;
}

void hierarchical_image_container::destroy(const uint32_t index) {
  if (index >= images.size()) utils::error{}("Trying to destroy image index '{}', but container has only {} images", index, images.size());
  if (!is_exists(index)) utils::error{}("Trying to destroy non existant image index '{}'", index);

  vk::Device d(device);
  d.destroy(images[index].view);
  d.destroy(images[index].handle);
  images[index].view = VK_NULL_HANDLE;
  images[index].handle = VK_NULL_HANDLE;
  images[index].sampler = VK_NULL_HANDLE;

  const uint32_t width_pow2 = utils::next_power_of_2(images[index].width);
  const uint32_t height_pow2 = utils::next_power_of_2(images[index].height);
  const uint32_t bit_image_size = std::max(width_pow2, height_pow2) / block_extents.width;
  const size_t bit_index = images[index].offset / block_size;
  sul::dynamic_bitset<size_t> image_bitset(block_usage.size(), 0);
  for (size_t i = bit_index; i < bit_index+bit_image_size; ++i) {
    image_bitset.set(i, true);
  }

  block_usage &= (~image_bitset);
}

void hierarchical_image_container::adjust_extents(const uint32_t index, const extent_t extent) {
  if (index >= images.size()) utils::error{}("Trying to adjust image index '{}' sizes, but container has only {} images", index, images.size());
  if (!is_exists(index)) utils::error{}("Trying to adjust non existant image index '{}' sizes", index);
  images[index].width = extent.width;
  images[index].height = extent.height;
}

VkImage hierarchical_image_container::storage(const uint32_t index) const {
  if (!is_exists(index)) return VK_NULL_HANDLE;
  return images[index].handle;
}

VkImageView hierarchical_image_container::view(const uint32_t index) const {
  if (!is_exists(index)) return VK_NULL_HANDLE;
  return images[index].view;
}

VkSampler hierarchical_image_container::sampler(const uint32_t index) const {
  if (!is_exists(index)) return VK_NULL_HANDLE;
  return images[index].sampler;
}

hierarchical_image_container::extent_t hierarchical_image_container::extent(const uint32_t index) const {
  if (!is_exists(index)) return {0, 0};
  return { images[index].width, images[index].height };
}

uint32_t hierarchical_image_container::format(const uint32_t index) const {
  if (!is_exists(index)) return 0;
  return images[index].format;
}

std::string_view hierarchical_image_container::name(const uint32_t index) const {
  if (!is_exists(index)) return std::string_view();
  return images[index].name;
}

size_t hierarchical_image_container::capacity() const { return images.size(); }
size_t hierarchical_image_container::size() const { return 0; }

void hierarchical_image_container::clear() {
  for (auto & img : images) {
    if (img.handle == VK_NULL_HANDLE) continue;

    vk::Device d(device);
    d.destroy(img.view);
    d.destroy(img.handle);
    img.view = VK_NULL_HANDLE;
    img.handle = VK_NULL_HANDLE;
    img.sampler = VK_NULL_HANDLE;
  }

  const size_t size = block_usage.size();
  block_usage.clear();
  block_usage.resize(size, false);
}

// наверное имеет смысл убрать под отдельный виртуальный класс
void hierarchical_image_container::update_descriptor_set(VkDescriptorSet set, const uint32_t binding, const uint32_t first_element) const {
  descriptor_set_updater dsu(device);
  dsu.currentSet(set).begin(binding, first_element, vk::DescriptorType::eCombinedImageSampler);
  for (size_t i = 0; i < images.size(); ++i) {
    auto view = images[i].view;
    auto sampler = images[i].sampler;
    if (!is_exists(i)) view = null_image.view;
    if (!is_exists(i)) sampler = null_image.sampler;
    dsu.image(view, vk::ImageLayout::eShaderReadOnlyOptimal, sampler);
  }
  dsu.update();
}

void hierarchical_image_container::change_layout(VkCommandBuffer buffer, const uint32_t index, const uint32_t old_layout, const uint32_t new_layout) const {
  if (index >= images.size()) utils::error{}("Trying to change layout on image index '{}', but capacity is {}", index, images.size());
  if (is_exists(index)) utils::error{}("Trying to change layout on non existing image index '{}'", index);

  const auto img = images[index].handle;

  vk::CommandBuffer b(buffer);
  vk::ImageSubresourceRange isr(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
  const auto [ barrier, ss, ds ] = make_image_memory_barrier(img, vk::ImageLayout(old_layout), vk::ImageLayout(new_layout), isr);
  b.pipelineBarrier(ss, ds, vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);
}

void hierarchical_image_container::change_layout_all(VkCommandBuffer buffer, const uint32_t old_layout, const uint32_t new_layout) const {
  vk::CommandBuffer b(buffer);
  vk::ImageSubresourceRange isr(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
  vk::PipelineStageFlags ss, ds;
  std::vector<vk::ImageMemoryBarrier> array;
  array.reserve(images.size());

  for (const auto & img : images) {
    if (img.handle == VK_NULL_HANDLE) continue;
    const auto [ barrier, ss_l, ds_l ] = make_image_memory_barrier(img.handle, vk::ImageLayout(old_layout), vk::ImageLayout(new_layout), isr);
    ss = ss_l;
    ds = ds_l;
    array.push_back(barrier);
  }

  b.pipelineBarrier(ss,ds, vk::DependencyFlagBits::eByRegion, nullptr, nullptr, array);
}

void hierarchical_image_container::copy_data(VkCommandBuffer buffer, VkImage image, const uint32_t index) const {
  if (index >= images.size()) utils::error{}("Trying to copy image to image index '{}', but capacity is {}", index, images.size());
  if (is_exists(index)) utils::error{}("Trying to copy image to non existing image index '{}'", index);

  const auto &img = images[index];

  vk::CommandBuffer b(buffer);
  vk::ImageSubresourceLayers isl1(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  vk::ImageSubresourceLayers isl2(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  vk::ImageCopy ic(isl1, vk::Offset3D{0,0,0}, isl2, vk::Offset3D{0,0,0}, vk::Extent3D{img.width, img.height, 1});
  b.copyImage(image, vk::ImageLayout::eTransferSrcOptimal, img.handle, vk::ImageLayout::eTransferDstOptimal, ic);
}

#define MAKE_BLIT_OFFSETS(w_1,h_1) {VkOffset3D{0,0,0}, VkOffset3D{int32_t(w_1),int32_t(h_1),1}}

void hierarchical_image_container::blit_data(VkCommandBuffer buffer, const std::tuple<VkImage, uint32_t, uint32_t> &src_image, const uint32_t index, const uint32_t filter) const {
  if (index >= images.size()) utils::error{}("Trying to blit image to image index '{}', but capacity is {}", index, images.size());
  if (is_exists(index)) utils::error{}("Trying to blit image to non existing image index '{}'", index);

  const auto &[ src, src_width, src_height ] = src_image;
  const auto &img = images[index];

  vk::CommandBuffer b(buffer);
  vk::ImageSubresourceLayers isl1(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  vk::ImageSubresourceLayers isl2(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  VkImageBlit blit{isl1, MAKE_BLIT_OFFSETS(src_width,src_height), isl2, MAKE_BLIT_OFFSETS(img.width,img.height)};
  b.blitImage(src, vk::ImageLayout::eTransferSrcOptimal, img.handle, vk::ImageLayout::eTransferDstOptimal, vk::ImageBlit(blit), vk::Filter(filter));
}

}
}
