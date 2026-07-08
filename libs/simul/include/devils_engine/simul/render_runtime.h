#ifndef DEVILS_ENGINE_SIMUL_RENDER_RUNTIME_H
#define DEVILS_ENGINE_SIMUL_RENDER_RUNTIME_H

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <memory>
#include <utility>

#include <gtl/phmap.hpp>

#include <devils_engine/catalogue/logging.h>
#include <devils_engine/painter/assets_base.h>
#include <devils_engine/painter/auxiliary.h>
#include <devils_engine/painter/gpu_load_context.h>
#include <devils_engine/painter/gpu_texture_resource.h>
#include <devils_engine/painter/graphics_base.h>
#include <devils_engine/painter/system_info.h>
#include <devils_engine/painter/vulkan_header.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>
#include <devils_engine/utils/string_id.h>

#include "messages.h"
#include "render_config.h"
#include "write_buffer_channel.h"

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

template <typename State>
void standard_render_resize_swapchain(State& c, const uint32_t w, const uint32_t h) {
  if (c.config.headless || !c.base || !c.graph_ready || !c.surface_ready) return;
  if (w == 0 || h == 0) return;
  c.base->wait_all_fences();
  c.pending_graph_width = w;
  c.pending_graph_height = h;
  c.base->resize_viewport(w, h);
  DE_LOG(catalogue::log_domain::render, flow, "resize swapchain {}x{}", w, h);
}

template <typename State>
void standard_render_build_wb_name_map(State& c) {
  c.wb_name_to_res.clear();
  for (uint32_t i = 0; i < c.base->resources.size(); ++i) {
    c.wb_name_to_res.emplace(utils::string_hash(c.base->resources[i].name), i);
  }
}

template <typename State>
void standard_render_write_buffer_bytes(State& c, const uint64_t name_hash, const std::span<const std::byte> payload) {
  const auto it = c.wb_name_to_res.find(name_hash);
  if (it == c.wb_name_to_res.end()) {
    utils::warn("write_buffer: resource hash {} not found", name_hash);
    return;
  }

  const uint32_t ri = it->second;
  const auto& res = c.base->resources[ri];
  const auto [frame_size, ext] = res.compute_frame_size(c.base.get());
  const uint32_t buffering = res.compute_buffering(c.base.get());
  const size_t n = std::min(payload.size(), size_t(frame_size));

  for (uint32_t off = 0; off < buffering; ++off) {
    const auto bf = c.base->get_current_buffer_resource_frame(ri, off);
    if (bf.mapped == nullptr) {
      utils::warn("write_buffer: resource {} is not host-visible", ri);
      return;
    }
    std::memcpy(static_cast<uint8_t*>(bf.mapped) + bf.sub.offset, payload.data(), n);
  }
}

template <typename State, typename Broker>
void standard_render_drain_write_buffer(State& c, Broker& br) {
  if (!c.graph_ready) return;
  br.write_buffer.drain([&c] (const wb_msg& m, const std::span<const std::byte> payload) {
    standard_render_write_buffer_bytes(c, m.name_hash, payload);
  });
}

template <typename State, typename Broker>
void standard_render_drain_graph_switch(State& c, Broker& br) {
  if (!c.graph_ready || !c.base) return;

  if (const command_render_set_graph* cmd = br.set_active_graph.consume()) {
    const uint32_t idx = c.base->find_render_graph(cmd->name);
    if (idx == painter::INVALID_RESOURCE_SLOT) {
      utils::warn("render: set_active_graph — граф '{}' не найден", cmd->name);
    } else if (idx != c.base->current_render_graph_index) {
      c.base->change_render_graph(idx);
      DE_LOG(catalogue::log_domain::render, flow, "active graph switched to '{}'", cmd->name);
    }
  }
}

template <typename State, typename Broker>
void standard_render_drain_update_constants(State& c, Broker& br) {
  if (!c.base_ready || !c.base) return;

  command_render_update_constant cmd;
  while (br.update_constant.try_pop(cmd)) {
    const uint32_t idx = c.base->find_constant(cmd.name);
    if (idx == painter::INVALID_RESOURCE_SLOT) {
      utils::warn("render: update_constant — константа '{}' не найдена", cmd.name);
      continue;
    }
    c.base->write_constant_data(idx, cmd.bytes.data(), cmd.bytes.size());
    c.base->update_constant_memory();
  }
}

template <typename State, typename Broker, typename OnTextureLoaded>
void standard_render_drain_gpu_transitions(State& c, Broker& br, OnTextureLoaded&& on_texture_loaded) {
  if (!c.base_ready) return;

  command_gpu_transition cmd{};
  while (br.gpu_transition.try_pop(cmd)) {
    auto* res = cmd.res.get();
    if (res == nullptr) {
      utils::warn("render: gpu_transition with unresolved resource handle");
      continue;
    }

    painter::gpu_load_context ctx{c.assets.get(), c.base.get()};
    const utils::safe_handle_t handle(&ctx);
    if (cmd.load) {
      res->load(handle);
      if (res->loading_type_id == utils::type_id<painter::gpu_texture_resource>()) {
        on_texture_loaded(static_cast<painter::gpu_texture_resource*>(res)->gpu_index);
      }
    } else {
      res->unload(handle);
    }

    br.gpu_done.try_push(command_gpu_done{cmd.res});
  }
}

template <typename State, typename Broker, typename AttachWindow, typename TryCreateGraph, typename OnTextureLoaded>
void standard_render_drain_commands(
  State& c,
  Broker& br,
  AttachWindow&& attach_window,
  TryCreateGraph&& try_create_graph,
  OnTextureLoaded&& on_texture_loaded
) {
  if (const command_window_recreation* cmd = br.window_recreation.consume()) {
    attach_window(*cmd);
  }

  if (const command_window_resize* cmd = br.window_resize.consume()) {
    standard_render_resize_swapchain(c, cmd->width, cmd->height);
  }

  if (const command_render_set_active* cmd = br.render_set_active.consume()) {
    c.draw_active = cmd->draw;
  }

  if (const command_shaders_prepared* cmd = br.shaders_prepared.consume()) {
    c.shader_prepare_requested = false;
    c.shader_prepare_failed = cmd->failed != 0;
    c.shaders_prepared = cmd->failed == 0;
    DE_LOG(catalogue::log_domain::render, flow, "render: shader prepare result compiled={} failed={}", cmd->compiled, cmd->failed);
    try_create_graph();
  }

  standard_render_drain_graph_switch(c, br);
  standard_render_drain_update_constants(c, br);
  standard_render_drain_gpu_transitions(c, br, std::forward<OnTextureLoaded>(on_texture_loaded));
  standard_render_drain_write_buffer(c, br);
}

}
}

#endif
