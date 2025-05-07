#include "framebuffer_resources.h"

#include "vulkan_header.h"

namespace devils_engine {
namespace painter {

simple_framebuffer::simple_framebuffer(VkDevice device, const struct render_pass_provider* render_pass_provider, const class attachments_provider* attachments_provider, const class frame_acquisitor* frame_acquisitor) :
  device(device), frame_acquisitor(frame_acquisitor)
{
  this->render_pass_provider = render_pass_provider;
  this->attachments_provider = attachments_provider;
  framebuffers.resize(frame_acquisitor->max_images, VK_NULL_HANDLE);
  //recreate(0, 0);
}

simple_framebuffer::~simple_framebuffer() noexcept {
  clear();
}

void simple_framebuffer::recreate(const uint32_t width, const uint32_t height) {
  clear();

  vk::Device d(device);
  VkImageView views[16]{};
  for (size_t i = 0; i < framebuffers.size(); ++i) {
    const size_t count = attachments_provider->attachment_handles(i, views, 16);

    vk::FramebufferCreateInfo fci({}, render_pass_provider->render_pass, count, (vk::ImageView*)views, attachments_provider->width, attachments_provider->height, 1);
    framebuffers[i] = d.createFramebuffer(fci);
  }
}

VkFramebuffer simple_framebuffer::current_framebuffer() const {
  return framebuffers[frame_acquisitor->current_image_index];
}

void simple_framebuffer::clear() {
  for (auto & buffer : framebuffers) {
    if (buffer == VK_NULL_HANDLE) continue;
    vk::Device(device).destroy(buffer);
    buffer = VK_NULL_HANDLE;
  }
}

}
}