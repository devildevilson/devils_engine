#include "arbitrary_image_container.h"

#include "vulkan_header.h"
#include "auxiliary.h"
#include "makers.h"

namespace devils_engine {
namespace painter {

static const vk::ComponentMapping default_comp_map{ vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity };
static const vk::ImageSubresourceRange default_image_range{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };

const uint32_t rgb24_format = uint32_t(vk::Format::eR8G8B8Unorm);
const uint32_t rgba32_format = uint32_t(vk::Format::eR8G8B8A8Unorm);

arbitrary_image_container::arbitrary_image_container(std::string name, VkInstance instance, VkPhysicalDevice physics_device, VkDevice device, VmaAllocator allocator, const uint32_t initial_size) noexcept :
  image_container(std::move(name)), device(device), allocator(allocator), is_owning_allocator(allocator == VK_NULL_HANDLE), _size(0), images(initial_size)
{
  if (is_owning_allocator) {
    const auto f = make_functions();
    vma::AllocatorCreateInfo aci(
      {},
      physics_device,
      device,
      0,
      nullptr,
      nullptr,
      0,
      &f,
      instance,
      VK_API_VERSION_1_0,
      0
    );

    this->allocator = vma::createAllocator(aci);
  }
  
  // НУЛЛ КАРТИНКУ НУЖНО ЗАРАНЕЕ В ЭТИ КОНТЕЙНЕРЫ ПЕРЕДАВАТЬ
  vma::AllocationCreateInfo aci(vma::AllocationCreateFlagBits::eDedicatedMemory, vma::MemoryUsage::eGpuOnly);

  const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
  auto inf = texture2D({4, 4}, usage, vk::Format::eR8G8B8A8Unorm);
  inf.initialLayout = vk::ImageLayout::ePreinitialized;
  auto [img, al] = vma::Allocator(allocator).createImage(inf, aci);
  null_image.handle = img;
  set_name(device, vk::Image(null_image.handle), container_name + "_arbitrary_null_image");
  null_image.allocation = al;
  const auto ivci = make_view_info(null_image.handle);
  null_image.view = vk::Device(device).createImageView(ivci);
  set_name(device, vk::ImageView(null_image.view), container_name + "_arbitrary_null_image_view");
  sampler_maker sm(device);
  null_image.sampler = sm.create(container_name + "_arbitrary_null_sampler");
}

arbitrary_image_container::~arbitrary_image_container() noexcept {
  clear();

  vk::Device(device).destroy(null_image.sampler);
  vk::Device(device).destroy(null_image.view);
  vma::Allocator(allocator).destroyImage(null_image.handle, null_image.allocation);

  if (is_owning_allocator) {
    vma::Allocator(allocator).destroy();
    allocator = VK_NULL_HANDLE;
  }
}

void arbitrary_image_container::resize(const size_t new_size) {
  if (new_size == images.size()) return;
  if (new_size == 0) {
    clear();
    images.clear();
    _size = 0;
    return;
  }

  while (new_size < images.size()) {
    if (is_exists(images.size() - 1)) {
      vk::Device(device).destroy(images.back().view);
      vma::Allocator(allocator).destroyImage(images.back().handle, images.back().allocation);
      _size -= 1;
    }
    images.pop_back();
  }

  images.resize(new_size);
}

bool arbitrary_image_container::is_exists(const uint32_t index) const {
  if (index >= images.size()) return false;
  return images[index].allocation != VK_NULL_HANDLE;
}

uint32_t arbitrary_image_container::create(std::string name, const image_container::extent_t extent, const uint32_t format, VkSampler sampler) {
  uint32_t i = 0;
  for (; i < images.size() && is_exists(i); ++i) {}

  if (sampler == VK_NULL_HANDLE) utils::error{}("Trying to create image view '{}' without a sampler object", name);

  //const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
  const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
  const auto inf = texture2D({extent.width, extent.height}, usage, vk::Format(format));
  auto [img, al] = create_image(allocator, inf, vma::MemoryUsage::eGpuOnly, nullptr, name);
  const auto ivci = make_view_info(img, vk::Format(format));
  const auto view = vk::Device(device).createImageView(ivci);

  set_name(device, vk::Image(img), name);
  set_name(device, vk::ImageView(view), name + "_view");

  if (i >= images.size()) {
    images.push_back({});
    i = images.size()-1;
  }

  images[i].allocation = al;
  images[i].handle = img;
  images[i].view = view;
  images[i].width = extent.width;
  images[i].height = extent.height;
  images[i].format = format;
  images[i].name = std::move(name);
  images[i].sampler = sampler;
  _size += 1;

  return i;
}

uint32_t arbitrary_image_container::create_any(std::string name, const extent_t extent, const uint32_t format, VkSampler sampler) {
  return create(std::move(name), extent, format, sampler);
}

void arbitrary_image_container::destroy(const uint32_t index) {
  if (index >= images.size()) return;
  if (!is_exists(index)) return;

  auto& img = images[index];
  if (img.view != VK_NULL_HANDLE) vk::Device(device).destroy(img.view);
  vma::Allocator(allocator).destroyImage(img.handle, img.allocation);
  img.view = VK_NULL_HANDLE;
  img.handle = VK_NULL_HANDLE;
  img.allocation = VK_NULL_HANDLE;

  _size -= size_t(_size != 0);
}

VkImage arbitrary_image_container::storage(const uint32_t index) const {
  if (!is_exists(index)) return VK_NULL_HANDLE;
  return images[index].handle;
}

VkImageView arbitrary_image_container::view(const uint32_t index) const {
  if (!is_exists(index)) return VK_NULL_HANDLE;
  return images[index].view;
}

VkSampler arbitrary_image_container::sampler(const uint32_t index) const {
  if (!is_exists(index)) return VK_NULL_HANDLE;
  return images[index].sampler;
}

image_container::extent_t arbitrary_image_container::extent(const uint32_t index) const {
  if (!is_exists(index)) return {0,0};
  return { images[index].width, images[index].height };
}

uint32_t arbitrary_image_container::format(const uint32_t index) const {
  if (!is_exists(index)) return 0;
  return images[index].format;
}

std::string_view arbitrary_image_container::name(const uint32_t index) const {
  if (!is_exists(index)) return std::string_view();
  return std::string_view(images[index].name);
}

size_t arbitrary_image_container::capacity() const { return images.size(); }
size_t arbitrary_image_container::size() const { return _size; }

void arbitrary_image_container::clear() {
  for (size_t i = 0; i < images.size(); ++i) {
    destroy(i);
  }

  _size = 0;
}

void arbitrary_image_container::update_descriptor_set(VkDescriptorSet set, const uint32_t binding, const uint32_t first_element) const {
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

void arbitrary_image_container::change_layout(VkCommandBuffer buffer, const uint32_t index, const uint32_t old_layout, const uint32_t new_layout) const {
  if (index >= images.size()) utils::error{}("Trying to change layout on image index '{}', but capacity is {}", index, images.size());
  if (!is_exists(index)) utils::error{}("Trying to change layout on non existing image index '{}'", index);

  const auto img = images[index].handle;

  vk::CommandBuffer b(buffer);
  vk::ImageSubresourceRange isr(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
  const auto [ barrier, ss, ds ] = make_image_memory_barrier(img, vk::ImageLayout(old_layout), vk::ImageLayout(new_layout), isr);
  b.pipelineBarrier(ss, ds, vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);
}

void arbitrary_image_container::change_layout_all(VkCommandBuffer buffer, const uint32_t old_layout, const uint32_t new_layout) const {
  vk::CommandBuffer b(buffer);
  vk::ImageSubresourceRange isr(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
  vk::PipelineStageFlags ss, ds;
  std::vector<vk::ImageMemoryBarrier> array;
  array.reserve(images.size());

  for (const auto & img : images) {
    if (img.allocation == VK_NULL_HANDLE) continue;
    const auto [ barrier, ss_l, ds_l ] = make_image_memory_barrier(img.handle, vk::ImageLayout(old_layout), vk::ImageLayout(new_layout), isr);
    ss = ss_l;
    ds = ds_l;
    array.push_back(barrier);
  }

  b.pipelineBarrier(ss,ds, vk::DependencyFlagBits::eByRegion, nullptr, nullptr, array);
}

void arbitrary_image_container::copy_data(VkCommandBuffer buffer, VkImage image, const uint32_t index) const {
  if (index >= images.size()) utils::error{}("Trying to copy image to image index '{}', but capacity is {}", index, images.size());
  if (!is_exists(index)) utils::error{}("Trying to copy image to non existing image index '{}'", index);

  const auto &img = images[index];

  vk::CommandBuffer b(buffer);
  vk::ImageSubresourceLayers isl1(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  vk::ImageSubresourceLayers isl2(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  vk::ImageCopy ic(isl1, vk::Offset3D{0,0,0}, isl2, vk::Offset3D{0,0,0}, vk::Extent3D{img.width, img.height, 1});
  b.copyImage(image, vk::ImageLayout::eTransferSrcOptimal, img.handle, vk::ImageLayout::eTransferDstOptimal, ic);
}

#define MAKE_BLIT_OFFSETS(w_1,h_1) {VkOffset3D{0,0,0}, VkOffset3D{int32_t(w_1),int32_t(h_1),1}}

void arbitrary_image_container::blit_data(VkCommandBuffer buffer, const std::tuple<VkImage, uint32_t, uint32_t> &src_image, const uint32_t index, const uint32_t filter) const {
  if (index >= images.size()) utils::error{}("Trying to blit image to image index '{}', but capacity is {}", index, images.size());
  if (!is_exists(index)) utils::error{}("Trying to blit image to non existing image index '{}'", index);

  const auto &[ src, src_width, src_height ] = src_image;
  const auto &img = images[index];

  vk::CommandBuffer b(buffer);
  vk::ImageSubresourceLayers isl1(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  vk::ImageSubresourceLayers isl2(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  VkImageBlit blit{isl1, MAKE_BLIT_OFFSETS(src_width,src_height), isl2, MAKE_BLIT_OFFSETS(img.width,img.height)};
  b.blitImage(src, vk::ImageLayout::eTransferSrcOptimal, img.handle, vk::ImageLayout::eTransferDstOptimal, vk::ImageBlit(blit), vk::Filter(filter));
}

host_image_container::host_image_container(std::string name, VkDevice device, VmaAllocator allocator) :
  arbitrary_image_container(std::move(name), VK_NULL_HANDLE, VK_NULL_HANDLE, device, allocator)
{}

uint32_t host_image_container::create(std::string name, const extent_t extent, const uint32_t format, VkSampler sampler) {
  uint32_t i = 0;
  for (; i < images.size() && is_exists(i); ++i) {}
  
  if (i >= images.size()) {
    images.push_back({});
    i = images.size()-1;
  }

  //const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
  const auto usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
  const auto inf = texture2D_staging({extent.width, extent.height}, usage, vk::Format(format));
  auto [img, al] = create_image(allocator, inf, vma::MemoryUsage::eCpuOnly, &images[i].memory, name);

  set_name(device, vk::Image(img), name);

  images[i].allocation = al;
  images[i].handle = img;
  images[i].view = VK_NULL_HANDLE;
  images[i].width = extent.width;
  images[i].height = extent.height;
  images[i].format = format;
  images[i].name = std::move(name);
  images[i].sampler = sampler;
  _size += 1;

  return i;
}

void* host_image_container::mapped_memory(const uint32_t index) const {
  if (!is_exists(index)) return nullptr;
  return images[index].memory;
}

}
}