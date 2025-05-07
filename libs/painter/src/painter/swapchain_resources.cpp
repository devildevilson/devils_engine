#include "swapchain_resources.h"

#include "vulkan_header.h"
#include "container.h"
#include "auxiliary.h"
#include <cmath>

namespace devils_engine {
namespace painter {
surface_container::~surface_container() noexcept {
  vk::Instance(instance).destroySurfaceKHR(surface);
  instance = VK_NULL_HANDLE;
  surface = VK_NULL_HANDLE;
}

simple_swapchain::simple_swapchain(VkDevice device, VkPhysicalDevice physical_device, VkSurfaceKHR surface, const uint32_t buffering_target) :
  device(device), physical_device(physical_device), surface(surface), format(0)
{
  max_images = buffering_target;
  vk::PhysicalDevice pd(physical_device);
  const auto surface_formats = pd.getSurfaceFormatsKHR(surface);
  const auto surface_format = choose_swapchain_surface_format(surface_formats);
  format = uint32_t(surface_format.format);
}

simple_swapchain::~simple_swapchain() noexcept {
  destroy_swapchain();
}

uint32_t simple_swapchain::acquire_next_image(size_t timeout, VkSemaphore semaphore, VkFence fence) {
  const auto [ res, index ] = vk::Device(device).acquireNextImageKHR(swapchain_provider::swapchain, timeout, semaphore, fence);
  frame_acquisitor::current_image_index = index;
  return uint32_t(res);
}

VkImage simple_swapchain::frame_storage(const uint32_t index) const {
  return index < images.size() ? images[index] : VK_NULL_HANDLE;
}

uint32_t simple_swapchain::frame_format(const uint32_t) const {
  return format;
}

void simple_swapchain::recreate(const uint32_t width, const uint32_t height) {
  vk::Device dev(device);
  vk::PhysicalDevice pd(physical_device);

  const auto caps = pd.getSurfaceCapabilitiesKHR(surface);
  const auto surface_formats = pd.getSurfaceFormatsKHR(surface);
  const auto modes = pd.getSurfacePresentModesKHR(surface);

  const auto mode = choose_swapchain_present_mode(modes);
  const auto surface_format = choose_swapchain_surface_format(surface_formats);
  const auto ext = choose_swapchain_extent(width, height, caps);
  format = uint32_t(surface_format.format);
  if (ext.width != width || ext.height != height) {
    utils::error("Wut? {} == {} == {} && {} == {} == {}", width, caps.currentExtent.width, ext.width, height, caps.currentExtent.height, ext.height);
  }

  const uint32_t max_count = caps.maxImageCount > 0 ? caps.maxImageCount : 3;
  const auto image_count = std::clamp(max_images, caps.minImageCount, max_count);
  if (image_count == 1) utils::error("This vulkan device does not supports double buffering");
  if (image_count != max_images) utils::error("Gets new buffering size?");

  //const auto f = vk::Format::eR8G8B8A8Unorm;
  //vk::ImageFormatListCreateInfo flist(1, &f);
  
  // капец нужно еще полтонны расширений... блин но это супер удобно эх
  // vk::SwapchainCreateFlagBitsKHR::eMutableFormat
  const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
  const vk::SwapchainCreateInfoKHR inf(
    {}, vk::SurfaceKHR(surface), 
    image_count, surface_format.format, surface_format.colorSpace, 
    caps.currentExtent, 1, usage, vk::SharingMode::eExclusive, 0, nullptr, 
    caps.currentTransform, vk::CompositeAlphaFlagBitsKHR::eOpaque, 
    mode, VK_TRUE, vk::SwapchainKHR(swapchain_provider::swapchain)//, &flist
  );

  auto new_swapchain = dev.createSwapchainKHR(inf);

  destroy_swapchain();

  swapchain_provider::swapchain = new_swapchain;
  set_name(device, vk::SwapchainKHR(swapchain), "window_swapchain");

  const auto vk_images = dev.getSwapchainImagesKHR(swapchain_provider::swapchain);
  if (vk_images.size() != image_count) {
    utils::error("This vulkan device does not supports double buffering");
  }

  images.resize(vk_images.size(), VK_NULL_HANDLE);
  for (size_t i=0; i<vk_images.size();++i) { images[i] = vk_images[i]; }

  // layout сменить
}

void simple_swapchain::destroy_swapchain() {
  vk::Device(device).destroy(swapchain_provider::swapchain);
  swapchain_provider::swapchain = VK_NULL_HANDLE;
}

VkSwapchainKHR simple_swapchain::get_swapchain() const {
  return swapchain_provider::swapchain;
}

simple_swapchain_image_layout_changer::simple_swapchain_image_layout_changer(const container* gc, const frame_acquisitor* fa) noexcept :
  gc(gc), fa(fa)
{}

void simple_swapchain_image_layout_changer::recreate(const uint32_t, const uint32_t) {
  painter::do_command(gc->device, gc->transfer_command_pool, gc->graphics_queue, gc->transfer_fence, [&](VkCommandBuffer t) {
    for (size_t i = 0; i < fa->max_images; ++i) {
      const auto img = fa->frame_storage(i);
      auto task = vk::CommandBuffer(t);
      const vk::ImageSubresourceRange isr(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
      const auto [b_info, srcStage, dstStage] = make_image_memory_barrier(img, vk::ImageLayout::eUndefined, vk::ImageLayout::ePresentSrcKHR, isr);
      task.pipelineBarrier(srcStage, dstStage, vk::DependencyFlagBits::eByRegion, nullptr, nullptr, b_info);
    }
  });
}
}
}