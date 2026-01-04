#include "image_pool.h"

#include "vulkan_header.h"
#include "makers.h"
#include "auxiliary.h"

namespace devils_engine {
namespace painter {

static vk::ImageAspectFlags choose_aspect(const uint32_t format) {
  auto aspect = vk::ImageAspectFlagBits::eColor;
  if (format_is_stencil_only(format)) aspect = vk::ImageAspectFlagBits::eStencil;
  if (format_is_depth_only(format) || format_is_depth_and_stencil(format)) aspect = vk::ImageAspectFlagBits::eDepth;
  return aspect;
}

// блен тут так то сложно, у меня указан тексель компатибл бит
static bool is_compatible_format(const uint32_t storage_format, const uint32_t view_format) {
  if (format_is_compressed(storage_format) && format_is_color(view_format)) return true;
  if (format_compatibility_class(storage_format) == format_compatibility_class(view_format)) return true;
  return false;
}

// прикол пуллов в том что они не фрагментируются как другие штуки
// вообще в будущем у меня большая часть текстур будет одного размера (причем высокого)
// будет иметь смысл создавать имдж пуллы для более больших текстурок
image_pool::image_pool(std::string name, VkDevice device, VmaAllocator allocator, const uint32_t format, const extent_t extent, const uint32_t count) :
  image_container(std::move(name)), _device(device), _allocator(allocator), _allocation(VK_NULL_HANDLE), _storage(VK_NULL_HANDLE), _view(VK_NULL_HANDLE), _width(extent.width), _height(extent.height), _format(format), _size(0), _images(count)
{
  vk::Device d(_device);
  vma::Allocator a(_allocator);

  const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;

  const vk::ImageCreateInfo ici(
    vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eBlockTexelViewCompatible,
    vk::ImageType::e2D,
    vk::Format(_format),
    vk::Extent3D{ _width, _height, 1 },
    1, _images.size(),
    vk::SampleCountFlagBits::e1,
    vk::ImageTiling::eOptimal,
    usage,
    vk::SharingMode::eExclusive, nullptr
  );

  const vma::AllocationCreateInfo aci(vma::AllocationCreateFlagBits::eDedicatedMemory, vma::MemoryUsage::eGpuOnly);
  auto [ img, alloc ] = a.createImage(ici, aci);
  _storage = img;
  _allocation = alloc;
  set_name(d, vk::Image(_storage), container_name + "_image_pull_image");

  // я же могу создать отдельный вью для каждой картинки в массиве...
  const auto ivci = view_info(_storage, vk::Format(_format), vk::ImageViewType::e2DArray, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, _images.size()));
  _view = d.createImageView(ivci);
  set_name(d, vk::ImageView(_view), container_name + "_image_pull_image_view");

  auto null_inf = texture2D({4, 4}, usage);
  const vma::AllocationCreateInfo aci2(vma::AllocationCreateFlagBits::eDedicatedMemory, vma::MemoryUsage::eGpuOnly);
  auto [ img2, alloc2 ] = a.createImage(null_inf, aci2);
  _null_storage = img2;
  _null_allocation = alloc2;
  set_name(d, vk::Image(_null_storage), container_name + "_image_pull_null_image");

  const auto ivci2 = make_view_info(_null_storage);
  _null_image.view = d.createImageView(ivci2);
  set_name(d, vk::ImageView(_null_image.view), container_name + "_image_pull_null_image_view");
  _null_image.sampler = sampler_maker(device).create(container_name + "_image_pool_null_sampler");
}

image_pool::~image_pool() noexcept {
  clear();

  vk::Device d(_device);
  vma::Allocator a(_allocator);

  d.destroy(_null_image.view);
  d.destroy(_null_image.sampler);
  a.destroyImage(_null_storage, _null_allocation);

  d.destroy(_view);
  a.destroyImage(_storage, _allocation);
}

//bool image_pool::is_free(const uint32_t index) const {
//  const uint32_t index_within_block = index % (sizeof(size_t) * CHAR_BIT);
//  const uint32_t block_index = index / (sizeof(size_t) * CHAR_BIT);
//  const size_t mask = (size_t(0x1) << index_within_block);
//  return (_availability_mask[block_index] & mask) == 0;
//}

bool image_pool::is_exists(const uint32_t index) const {
  if (index >= _images.size()) return false;
  return _images[index].view != VK_NULL_HANDLE;
}

uint32_t image_pool::create(std::string name, const extent_t, const uint32_t format, VkSampler sampler) {
  const uint32_t index = aquire_slot();
  if (index == UINT32_MAX) return UINT32_MAX;

  // отказаться от проверок компатибилити ? или может быть позже добавить если мне придет в голову что то совсем дикое
  // проверить сэмплер на наличие? вообще имеет смысл
  if (sampler == VK_NULL_HANDLE) utils::error{}("Trying to create image view '{}' without a sampler object", name);

  const vk::ImageSubresourceRange isr(choose_aspect(format), 0, 1, index, 1);
  const auto ivci = view_info(_storage, vk::Format(format), vk::ImageViewType::e2D, isr);
  _images[index].view = vk::Device(_device).createImageView(ivci);
  _images[index].sampler = sampler;
  _images[index].name = std::move(name);
  _images[index].layer = index;
  _images[index].format = format;
  set_name(_device, vk::ImageView(_images[index].view), _images[index].name);
  return index;
}

uint32_t image_pool::create_any(std::string name, const extent_t extent, const uint32_t format, VkSampler sampler) {
  return create(std::move(name), extent, format, sampler);
}

void image_pool::destroy(const uint32_t index) {
  free_slot(index);
}

uint32_t image_pool::aquire_slot() {
  /*for (size_t i = 0; i < _availability_mask.size(); ++i) {
    for (size_t j = 0; j < sizeof(size_t) * CHAR_BIT; ++j) {
      const size_t mask = (size_t(0x1) << j);
      const bool available = (_availability_mask[i] & mask) == mask;
      if (available) {
        _availability_mask[i] = _availability_mask[i] | mask;
        const uint32_t index = i * (sizeof(size_t) * CHAR_BIT) + j;
        _size += 1;
        return index;
      }
    }
  }*/

  size_t i = 0;
  for (; i < _images.size() && is_exists(i); ++i) {}
  _size += size_t(i < _images.size());
  return i < _images.size() ? i : UINT32_MAX;
}

void image_pool::free_slot(const uint32_t index) {
  if (!is_exists(index)) return;

  /*const uint32_t index_within_block = index % (sizeof(size_t) * CHAR_BIT);
  const uint32_t block_index = index / (sizeof(size_t) * CHAR_BIT);
  const size_t mask = (size_t(0x1) << index_within_block);
  _availability_mask[block_index] = (_availability_mask[block_index] & (~mask));
  _size -= size_t(_size != 0);*/

  vk::Device(_device).destroy(_images[index].view);
  _images[index].view = VK_NULL_HANDLE;
  _images[index].sampler = VK_NULL_HANDLE;
  _size -= size_t(_size != 0);
}

image_container::extent_t image_pool::extent(const uint32_t) const { return { _width, _height }; }

uint32_t image_pool::format(const uint32_t index) const {
  if (!is_exists(index)) return 0;
  return _images[index].format;
}

VkImage image_pool::storage(const uint32_t) const {
  return _storage;
}

VkImageView image_pool::view(const uint32_t index) const {
  if (!is_exists(index)) return VK_NULL_HANDLE;
  return _images[index].view;
}

VkSampler image_pool::sampler(const uint32_t index) const {
  if (!is_exists(index)) return VK_NULL_HANDLE;
  return _images[index].sampler;
}

std::string_view image_pool::name(const uint32_t index) const {
  if (!is_exists(index)) return std::string_view();
  return std::string_view(_images[index].name);
}

VkImage image_pool::storage() const { return _storage; }
VkImageView image_pool::view() const { return _view; }
size_t image_pool::capacity() const { return _images.size(); }
size_t image_pool::size() const { return _size; }
std::tuple<uint32_t, uint32_t> image_pool::extent() const { return std::make_tuple(_width, _height); }
bool image_pool::has_free_slots() const { return size() < capacity(); }

void image_pool::clear() {
  for (size_t i = 0; i < _images.size(); ++i) {
    free_slot(i);
  }
}

void image_pool::update_descriptor_set(VkDescriptorSet set, const uint32_t binding, const uint32_t element) const {
  /*descriptor_set_updater dsu(_device);
  dsu.currentSet(set)
     .begin(binding, element, vk::DescriptorType::eCombinedImageSampler)
     .image(_view, vk::ImageLayout::eShaderReadOnlyOptimal)
     .update();*/

  descriptor_set_updater dsu(_device);
  dsu.currentSet(set).begin(binding, element, vk::DescriptorType::eCombinedImageSampler);
  for (size_t i = 0; i < _images.size(); ++i) {
    auto view = _images[i].view;
    auto sampler = _images[i].sampler;
    if (!is_exists(i)) view = _null_image.view;
    if (!is_exists(i)) sampler = _null_image.sampler;
    dsu.image(view, vk::ImageLayout::eShaderReadOnlyOptimal, sampler);
  }
  dsu.update();
}

void image_pool::change_layout(VkCommandBuffer buffer, const uint32_t index, const uint32_t old_layout, const uint32_t new_layout) const {
  if (index >= capacity()) utils::error{}("Trying to change layout on image index '{}', but capacity is {}", index, capacity());

  vk::CommandBuffer b(buffer);
  vk::ImageSubresourceRange isr(vk::ImageAspectFlagBits::eColor, 0, 1, index, 1);
  const auto [ barrier, ss, ds ] = make_image_memory_barrier(_storage, vk::ImageLayout(old_layout), vk::ImageLayout(new_layout), isr);
  b.pipelineBarrier(ss, ds, vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);
}

void image_pool::change_layout_all(VkCommandBuffer buffer, const uint32_t old_layout, const uint32_t new_layout) const {
  vk::CommandBuffer b(buffer);
  vk::ImageSubresourceRange isr(vk::ImageAspectFlagBits::eColor, 0, 1, 0, capacity());
  const auto [ barrier, ss, ds ] = make_image_memory_barrier(_storage, vk::ImageLayout(old_layout), vk::ImageLayout(new_layout), isr);
  b.pipelineBarrier(ss, ds, vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);
}

void image_pool::copy_data(VkCommandBuffer buffer, VkImage image, const uint32_t index) const {
  if (index >= capacity()) utils::error{}("Trying to copy image to image index '{}', but capacity is {}", index, capacity());

  vk::CommandBuffer b(buffer);
  vk::ImageSubresourceLayers isl1(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  vk::ImageSubresourceLayers isl2(vk::ImageAspectFlagBits::eColor, 0, index, 1);
  vk::ImageCopy ic(isl1, vk::Offset3D{0,0,0}, isl2, vk::Offset3D{0,0,0}, vk::Extent3D{_width, _height, 1});
  b.copyImage(image, vk::ImageLayout::eTransferSrcOptimal, _storage, vk::ImageLayout::eTransferDstOptimal, ic);
}

#define MAKE_BLIT_OFFSETS(w_1,h_1) {VkOffset3D{0,0,0}, VkOffset3D{int32_t(w_1),int32_t(h_1),1}}

void image_pool::blit_data(VkCommandBuffer buffer, const std::tuple<VkImage,uint32_t,uint32_t> &src_image, const uint32_t index, const uint32_t filter) const {
  if (index >= capacity()) utils::error{}("Trying to blit image to image index '{}', but capacity is {}", index, capacity());

  const auto &[ src, src_width, src_height ] = src_image;
  vk::CommandBuffer b(buffer);
  vk::ImageSubresourceLayers isl1(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  vk::ImageSubresourceLayers isl2(vk::ImageAspectFlagBits::eColor, 0, index, 1);
  VkImageBlit blit{isl1, MAKE_BLIT_OFFSETS(src_width, src_height), isl2, MAKE_BLIT_OFFSETS(_width, _height)};
  b.blitImage(src, vk::ImageLayout::eTransferSrcOptimal, _storage, vk::ImageLayout::eTransferDstOptimal, vk::ImageBlit(blit), vk::Filter(filter));
}

}
}