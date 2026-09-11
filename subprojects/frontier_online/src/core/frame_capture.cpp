#include "frame_capture.h"

#include <algorithm>
#include <string>
#include <vector>

// Именно ЭТОТ заголовок, а не <vulkan/vulkan.hpp> напрямую: движок работает с динамической
// загрузкой точек входа (VK_NO_PROTOTYPES + VULKAN_HPP_DISPATCH_LOADER_DYNAMIC), и включение
// vulkan.hpp мимо него дало бы статические прототипы, которых в линковке нет.
#include <devils_engine/painter/vulkan_header.h>

#include <devils_engine/painter/graphics_base.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/fileio.h>

namespace frontier_online {
namespace core {

using namespace devils_engine;

frame_capture_request& capture_request() noexcept {
  static frame_capture_request request;
  return request;
}

void capture_frame(painter::graphics_base& base, const std::string& resource_name,
                   const std::string& path) {
  const uint32_t slot = base.find_resource(resource_name);
  if (slot == UINT32_MAX) {
    utils::error{}("frame capture: render graph has no resource '{}'", resource_name);
  }
  const auto frame = base.get_current_image_resource_frame(slot);
  const auto [width, height] = base.swapchain_extent();
  // Цель объявлена форматом `c4` — четыре байта на пиксель. Если формат когда-нибудь сменится,
  // молча прочитать не то было бы хуже отказа.
  if (frame.vk_format != uint32_t(VK_FORMAT_R8G8B8A8_UNORM) &&
      frame.vk_format != uint32_t(VK_FORMAT_B8G8R8A8_UNORM)) {
    utils::error{}("frame capture: resource '{}' has format {}, expected 8-bit RGBA/BGRA",
                   resource_name, frame.vk_format);
  }
  const bool swap_rb = frame.vk_format == uint32_t(VK_FORMAT_B8G8R8A8_UNORM);
  const size_t bytes = size_t(width) * height * 4;

  vma::Allocator allocator(base.allocator);
  vk::BufferCreateInfo buffer_info{};
  buffer_info.usage = vk::BufferUsageFlagBits::eTransferDst;
  buffer_info.size = bytes;
  vma::AllocationCreateInfo allocation_info{};
  allocation_info.usage = vma::MemoryUsage::eGpuToCpu;
  allocation_info.flags = vma::AllocationCreateFlagBits::eMapped;
  auto [allocation, staging] = allocator.createBuffer(buffer_info, allocation_info);

  vk::Device device(base.device);
  vk::CommandBufferAllocateInfo command_info{};
  command_info.commandPool = base.command_pool;
  command_info.level = vk::CommandBufferLevel::ePrimary;
  command_info.commandBufferCount = 1;
  const auto buffers = device.allocateCommandBuffers(command_info);
  vk::CommandBuffer task(buffers[0]);
  task.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

  // Барьеры ЯВНЫЕ, а не «граф наверняка оставил нужный слой». Последним использованием
  // `albedo_res` в графе стоит color_attachment, поэтому переход отсюда и обратно — часть работы
  // съёмщика, а не предположение о чужом конфиге.
  const auto barrier = [&](const vk::ImageLayout from, const vk::ImageLayout to,
                           const vk::AccessFlags src_access, const vk::AccessFlags dst_access,
                           const vk::PipelineStageFlags src_stage,
                           const vk::PipelineStageFlags dst_stage) {
    vk::ImageMemoryBarrier b{};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = frame.handle;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.subresourceRange.baseArrayLayer = frame.sub.base_array_layer;
    task.pipelineBarrier(src_stage, dst_stage, {}, nullptr, nullptr, b);
  };

  barrier(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eTransferSrcOptimal,
          vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eTransferRead,
          vk::PipelineStageFlagBits::eColorAttachmentOutput,
          vk::PipelineStageFlagBits::eTransfer);

  vk::BufferImageCopy region{};
  region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.imageSubresource.layerCount = 1;
  region.imageSubresource.baseArrayLayer = frame.sub.base_array_layer;
  region.imageExtent = vk::Extent3D{width, height, 1};
  task.copyImageToBuffer(frame.handle, vk::ImageLayout::eTransferSrcOptimal, staging, region);

  barrier(vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eColorAttachmentOptimal,
          vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eColorAttachmentWrite,
          vk::PipelineStageFlagBits::eTransfer,
          vk::PipelineStageFlagBits::eColorAttachmentOutput);
  task.end();

  const vk::Fence fence = device.createFence({});
  vk::SubmitInfo submit{};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &task;
  {
    const auto lock = base.graphics.lock();
    if (vk::Queue(base.graphics.handle()).submit(1, &submit, fence) != vk::Result::eSuccess) {
      utils::error{}("frame capture: could not submit the copy");
    }
  }
  if (device.waitForFences(fence, true, size_t(1000) * 1000 * 1000) != vk::Result::eSuccess) {
    utils::error{}("frame capture: copy timed out");
  }

  const auto info = allocator.getAllocationInfo(allocation);
  const auto* pixels = static_cast<const uint8_t*>(info.pMappedData);
  std::string ppm = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
  ppm.reserve(ppm.size() + size_t(width) * height * 3);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    const uint8_t r = pixels[i * 4 + (swap_rb ? 2 : 0)];
    const uint8_t g = pixels[i * 4 + 1];
    const uint8_t b = pixels[i * 4 + (swap_rb ? 0 : 2)];
    ppm.push_back(char(r));
    ppm.push_back(char(g));
    ppm.push_back(char(b));
  }
  if (!file_io::write(std::span<const char>(ppm.data(), ppm.size()), path, file_io::type::binary)) {
    utils::error{}("frame capture: could not write '{}'", path);
  }
  utils::info("frame capture: wrote {}x{} to '{}'", width, height, path);

  device.destroy(fence);
  device.free(base.command_pool, buffers);
  allocator.destroyBuffer(staging, allocation);
}

} // namespace core
} // namespace frontier_online
