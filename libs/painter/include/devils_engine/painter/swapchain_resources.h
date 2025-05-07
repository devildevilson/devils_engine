#ifndef DEVILS_ENGINE_PAINTER_SWAPCHAIN_RESOURCES_H
#define DEVILS_ENGINE_PAINTER_SWAPCHAIN_RESOURCES_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include "primitives.h"

namespace devils_engine {
namespace painter {

struct container;

class surface_container : public arbitrary_data {
public:
  VkInstance instance;
  VkSurfaceKHR surface;

  inline surface_container(VkInstance instance, VkSurfaceKHR surface) noexcept : instance(instance), surface(surface) {}
  ~surface_container() noexcept;
};

// вообще у свопчейна может быть прилично настроек
// у свопчейна провайдер поди должен быть
class simple_swapchain : public swapchain_provider, public frame_acquisitor, public recreate_target {
public:
  simple_swapchain(VkDevice device, VkPhysicalDevice physical_device, VkSurfaceKHR surface, const uint32_t buffering_target);
  ~simple_swapchain() noexcept;

  uint32_t acquire_next_image(size_t timeout, VkSemaphore semaphore, VkFence fence) override;
  VkImage frame_storage(const uint32_t index) const override;
  uint32_t frame_format(const uint32_t index) const override;
  void recreate(const uint32_t width, const uint32_t height) override;
  void destroy_swapchain();
  VkSwapchainKHR get_swapchain() const;
protected:
  VkDevice device;
  VkPhysicalDevice physical_device;
  VkSurfaceKHR surface;
  uint32_t format;
  std::vector<VkImage> images;
};

class simple_swapchain_image_layout_changer : public recreate_target {
public:
  simple_swapchain_image_layout_changer(const container* gc, const frame_acquisitor* fa) noexcept;
  void recreate(const uint32_t width, const uint32_t height) override;
private:
  const container* gc;
  const frame_acquisitor* fa;
};

}
}

#endif