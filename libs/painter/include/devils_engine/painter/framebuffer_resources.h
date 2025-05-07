#ifndef DEVILS_ENGINE_PAINTER_FRAMEBUFFER_RESOURCES_H
#define DEVILS_ENGINE_PAINTER_FRAMEBUFFER_RESOURCES_H

#include <cstddef>
#include <cstdint>
#include "primitives.h"

namespace devils_engine {
namespace painter {

class simple_framebuffer : public framebuffer_provider {
public:
  simple_framebuffer(VkDevice device, const struct render_pass_provider* render_pass_provider, const class attachments_provider* attachments_provider, const class frame_acquisitor* frame_acquisitor);
  ~simple_framebuffer() noexcept;

  void recreate(const uint32_t width, const uint32_t height) override;
  VkFramebuffer current_framebuffer() const override;
  void clear();
protected:
  VkDevice device;
  const class frame_acquisitor* frame_acquisitor;
  std::vector<VkFramebuffer> framebuffers;
};

}
}

#endif