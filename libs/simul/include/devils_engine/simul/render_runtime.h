#ifndef DEVILS_ENGINE_SIMUL_RENDER_RUNTIME_H
#define DEVILS_ENGINE_SIMUL_RENDER_RUNTIME_H

#include <cstdint>
#include <memory>

#include <gtl/phmap.hpp>

#include <devils_engine/painter/assets_base.h>
#include <devils_engine/painter/auxiliary.h>
#include <devils_engine/painter/graphics_base.h>
#include <devils_engine/painter/system_info.h>
#include <devils_engine/painter/vulkan_header.h>

#include "render_config.h"

namespace devils_engine {
namespace simul {

template <typename Broker>
struct standard_render_state {
  Broker* br = nullptr;
  gtl::flat_hash_map<uint64_t, uint32_t> wb_name_to_res;

  render_system_config config;

  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  painter::physical_device_data physical_device_data;

  VkQueue graphics_queue = VK_NULL_HANDLE;
  VkQueue transfer_queue = VK_NULL_HANDLE;

  std::unique_ptr<painter::graphics_base> base;
  std::unique_ptr<painter::assets_base> assets;
  painter::graphics_ctx ctx;

  bool instance_ready = false;
  bool device_ready = false;
  bool base_ready = false;
  bool surface_ready = false;
  bool graph_ready = false;
  bool draw_active = true;
  bool shader_prepare_requested = false;
  bool shaders_prepared = false;
  bool shader_prepare_failed = false;
  uint32_t pending_graph_width = 0;
  uint32_t pending_graph_height = 0;

  ~standard_render_state() noexcept {
    if (device != VK_NULL_HANDLE) vk::Device(device).waitIdle();
    ctx = painter::graphics_ctx{};
    assets.reset();
    base.reset();
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE) vk::Instance(instance).destroy(surface);
    if (device != VK_NULL_HANDLE) vk::Device(device).destroy();
    if (instance != VK_NULL_HANDLE && debug_messenger != VK_NULL_HANDLE) painter::destroy_debug_messenger(instance, debug_messenger);
    if (instance != VK_NULL_HANDLE) vk::Instance(instance).destroy();
  }
};

template <typename State>
void standard_render_drain(State& c) {
  if (c.device == VK_NULL_HANDLE) return;

  painter::load_dispatcher3(c.device);
  if (c.base) c.base->wait_all_fences();
  if (c.graphics_queue != VK_NULL_HANDLE) vk::Queue(c.graphics_queue).waitIdle();
}

template <typename State>
void standard_render_destroy_swapchain(State& c) {
  if (!c.base || c.base->swapchain == VK_NULL_HANDLE) return;

  standard_render_drain(c);

  vk::Device dev(c.device);
  if (c.base->swapchain_slot != painter::INVALID_RESOURCE_SLOT) {
    auto& res = DS_ASSERT_ARRAY_GET(c.base->resources, c.base->swapchain_slot);
    for (size_t i = 0; i < c.base->swapchain_images.size(); ++i) {
      if (res.handles[i].view != VK_NULL_HANDLE) {
        dev.destroy(res.handles[i].view);
        res.handles[i].view = VK_NULL_HANDLE;
      }
    }
  }

  c.base->swapchain_images.clear();
  dev.destroy(c.base->swapchain);
  c.base->swapchain = VK_NULL_HANDLE;
}

template <typename State>
void standard_render_detach_window(State& c) {
  standard_render_destroy_swapchain(c);

  if (c.instance != VK_NULL_HANDLE && c.surface != VK_NULL_HANDLE) {
    vk::Instance(c.instance).destroy(c.surface);
    c.surface = VK_NULL_HANDLE;
  }

  if (c.base) c.base->surface = VK_NULL_HANDLE;
  c.surface_ready = false;
  c.graph_ready = false;
}

template <typename State>
void standard_render_shutdown(State& c) {
  standard_render_drain(c);

  c.ctx = painter::graphics_ctx{};
  c.assets.reset();
  c.base.reset();

  if (c.instance != VK_NULL_HANDLE && c.surface != VK_NULL_HANDLE) {
    vk::Instance(c.instance).destroy(c.surface);
    c.surface = VK_NULL_HANDLE;
  }

  if (c.device != VK_NULL_HANDLE) {
    vk::Device(c.device).destroy();
    c.device = VK_NULL_HANDLE;
  }

  if (c.instance != VK_NULL_HANDLE) {
    if (c.debug_messenger != VK_NULL_HANDLE) {
      painter::destroy_debug_messenger(c.instance, c.debug_messenger);
      c.debug_messenger = VK_NULL_HANDLE;
    }

    vk::Instance(c.instance).destroy();
    c.instance = VK_NULL_HANDLE;
  }

  c.graphics_queue = VK_NULL_HANDLE;
  c.transfer_queue = VK_NULL_HANDLE;
  c.physical_device_data = painter::physical_device_data{};
  c.instance_ready = false;
  c.device_ready = false;
  c.base_ready = false;
  c.surface_ready = false;
  c.graph_ready = false;
}

}
}

#endif
