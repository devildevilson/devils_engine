#ifndef DEVILS_ENGINE_SIMUL_RENDER_RUNTIME_H
#define DEVILS_ENGINE_SIMUL_RENDER_RUNTIME_H

// Reusable renderer bootstrap and frame helpers for the standard render system.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <span>
#include <utility>

#include <devils_engine/catalogue/logging.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/input/core.h>
#include <devils_engine/painter/assets_base.h>
#include <devils_engine/painter/auxiliary.h>
#include <devils_engine/painter/glsl_source_file.h>
#include <devils_engine/painter/gpu_load_context.h>
#include <devils_engine/painter/gpu_texture_resource.h>
#include <devils_engine/painter/graphics_base.h>
#include <devils_engine/painter/makers.h>
#include <devils_engine/painter/shader_source_file.h>
#include <devils_engine/painter/structures.h>
#include <devils_engine/painter/system_info.h>
#include <devils_engine/painter/vulkan_header.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>
#include <devils_engine/utils/string_id.h>
#include <gtl/phmap.hpp>

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
    if (device != VK_NULL_HANDLE) {
      vk::Device(device).waitIdle();
    }
    ctx = painter::graphics_ctx{};
    assets.reset();
    base.reset();
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE) {
      vk::Instance(instance).destroy(surface);
    }
    if (device != VK_NULL_HANDLE) {
      vk::Device(device).destroy();
    }
    if (instance != VK_NULL_HANDLE && debug_messenger != VK_NULL_HANDLE) {
      painter::destroy_debug_messenger(instance, debug_messenger);
    }
    if (instance != VK_NULL_HANDLE) {
      vk::Instance(instance).destroy();
    }
  }
};

template <typename State>
void standard_render_drain(State& c) {
  if (c.device == VK_NULL_HANDLE) {
    return;
  }

  painter::load_dispatcher3(c.device);
  if (c.base) {
    c.base->wait_all_fences();
  }
  if (c.graphics_queue != VK_NULL_HANDLE) {
    vk::Queue(c.graphics_queue).waitIdle();
  }
}

template <typename State>
void standard_render_destroy_swapchain(State& c) {
  if (!c.base || c.base->swapchain == VK_NULL_HANDLE) {
    return;
  }

  standard_render_drain(c);

  vk::Device dev(c.device);
  if (c.base->swapchain_slot != painter::invalid_resource_slot) {
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

  if (c.base) {
    c.base->surface = VK_NULL_HANDLE;
  }
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
void standard_render_create_instance(State& c) {
  if (c.instance_ready) {
    return;
  }

  painter::load_dispatcher1(!c.config.headless);

  vk::ApplicationInfo ai{};
  ai.pApplicationName = c.config.app_name.c_str();
  ai.applicationVersion = c.config.app_version;
  ai.pEngineName = c.config.engine_name.c_str();
  ai.engineVersion = c.config.engine_version;
  ai.apiVersion = VK_API_VERSION_1_0;

  std::vector<const char*> exts = painter::get_required_extensions(!c.config.headless);
  vk::InstanceCreateInfo ici{};
  ici.pApplicationInfo = &ai;
  if (::enable_validation_layers) {
    if (!painter::check_validation_layer_support(painter::default_validation_layers)) {
      utils::error{}("Requested Vulkan validation layers are not available");
    }

    ici.enabledLayerCount = painter::default_validation_layers.size();
    ici.ppEnabledLayerNames = painter::default_validation_layers.data();
  }
  ici.enabledExtensionCount = exts.size();
  ici.ppEnabledExtensionNames = exts.data();

  DE_LOG(catalogue::log_domain::render, flow, "{}: creating Vulkan instance", c.config.app_name);
  c.instance = vk::createInstance(ici);
  DE_LOG(catalogue::log_domain::render, flow, "{}: Vulkan instance created", c.config.app_name);
  painter::load_dispatcher2(c.instance);
  c.debug_messenger = painter::create_debug_messenger(c.instance);
  c.instance_ready = true;
}

template <typename State>
void standard_render_create_device(State& c, const command_window_recreation* window = nullptr) {
  if (c.device_ready) {
    return;
  }

  bool cached = painter::system_info::try_load_cached_data(c.instance, &c.physical_device_data, nullptr);
  if (!cached) {
    if (c.config.headless) {
      painter::system_info si(c.instance);
      c.physical_device_data = si.choose_physical_device_headless();
    } else {
      if (window == nullptr || window->w == nullptr) {
        utils::warn("Render device cache is missing and no window surface is available; device creation is postponed");
        return;
      }

      painter::system_info si(c.instance);
      si.check_devices_surface_capability(c.surface);
      c.physical_device_data = si.choose_physical_device();
      si.dump_cache_to_disk(c.physical_device_data.handle, nullptr);
    }
  }

  painter::system_info::print_choosed_device(c.physical_device_data.handle);

  painter::device_maker dm(c.instance);
  dm.beginDevice(c.physical_device_data.handle);
  dm.createQueues(1);
  dm.features(vk::PhysicalDevice(c.physical_device_data.handle).getFeatures());
  if (c.config.headless) {
    dm.setExtensions({});
  } else {
    dm.setExtensions(painter::default_device_extensions);
  }

  c.device = dm.create({}, c.config.app_name + ".device");
  painter::load_dispatcher3(c.device);

  vk::Device dev(c.device);
  c.graphics_queue = dev.getQueue(c.physical_device_data.graphics_queue, 0);
  c.transfer_queue = dev.getQueue(c.physical_device_data.transfer_queue, 0);
  painter::set_name(dev, vk::Queue(c.graphics_queue), c.config.app_name + ".graphics_queue");

  c.device_ready = true;
}

template <typename State>
void standard_render_create_base_resources(State& c) {
  if (c.base_ready || !c.device_ready) {
    return;
  }

  c.base = std::make_unique<painter::graphics_base>(
    c.instance,
    c.device,
    c.physical_device_data.handle,
    c.config.headless ? painter::presentation_engine_type::no_present : painter::presentation_engine_type::main);

  c.base->create_allocator();
  c.base->create_command_pool(c.physical_device_data.graphics_queue, c.graphics_queue);
  c.base->create_descriptor_pool();
  c.base->get_or_create_pipeline_cache(c.config.cache_registry, c.config.pipeline_cache_id);

  if (c.config.engine_registry == nullptr) {
    utils::error{}("render: engine registry is null (render-graph source)");
  }
  auto render_cfg_data = painter::build_render_config(c.config.engine_registry, c.config.render_config_prefix);
  if (render_cfg_data.graphs.empty()) {
    utils::error{}("render: config prefix '{}' contains no render graphs", c.config.render_config_prefix);
  }
  const auto startup = std::find_if(
    render_cfg_data.graphs.begin(), render_cfg_data.graphs.end(),
    [](const auto& graph) { return graph.startup; });
  if (startup == render_cfg_data.graphs.end()) {
    utils::warn("render: no startup graph marked in '{}'; using first graph '{}'",
                c.config.render_config_prefix, render_cfg_data.graphs.front().name);
    c.config.graph_name = render_cfg_data.graphs.front().name;
  } else {
    const auto duplicate = std::find_if(
      std::next(startup), render_cfg_data.graphs.end(),
      [](const auto& graph) { return graph.startup; });
    if (duplicate != render_cfg_data.graphs.end()) {
      utils::error{}("render: multiple startup graphs '{}' and '{}'",
                     startup->name, duplicate->name);
    }
    c.config.graph_name = startup->name;
  }
  c.base->set_startup_graph(c.config.graph_name);
  for (const auto& graph : render_cfg_data.graphs) {
    if (graph.name != c.config.graph_name) {
      c.base->add_resident_graph(graph.name);
    }
  }
  const auto res = c.base->commit_parsed_resources(render_cfg_data);
  if (res != 0) {
    utils::error{}("Could not commit render config from engine registry prefix '{}'", c.config.render_config_prefix);
  }
  c.base->set_shader_source(c.config.engine_registry, c.config.shader_config_prefix);

  c.assets = std::make_unique<painter::assets_base>(c.device, c.physical_device_data.handle);
  c.assets->create_fence();
  c.assets->create_allocator(c.instance);
  c.assets->create_command_buffer(c.transfer_queue, c.physical_device_data.transfer_queue);
  c.assets->set_graphics_base(c.base.get());
  c.assets->create_default_texture();

  c.ctx.base = c.base.get();
  c.ctx.assets = c.assets.get();
  c.base_ready = true;
}

void standard_render_set_shader_sources_loaded(const demiurg::resource_system* reg, bool load);

template <typename State>
void standard_render_request_shader_prepare(State& c) {
  if (c.shader_prepare_requested || c.shaders_prepared || c.shader_prepare_failed) {
    return;
  }

  if (c.br == nullptr) {
    DE_LOG(catalogue::log_domain::render, flow, "render: shader prepare waits for broker");
    return;
  }

  command_prepare_shaders cmd;
  cmd.registry = c.config.engine_registry;
  cmd.prefix = c.config.shader_config_prefix;
  c.br->prepare_shaders.try_push(std::move(cmd));
  c.shader_prepare_requested = true;
  DE_LOG(catalogue::log_domain::render, flow, "render: requested shader prepare for prefix '{}'", c.config.shader_config_prefix);
}

template <typename State>
void standard_render_bind_texture_slot(State& c, const uint32_t slot) {
  const uint32_t di = c.base->find_descriptor(c.config.texture_descriptor_name);
  if (di == painter::invalid_resource_slot) {
    return;
  }

  auto& d = c.base->descriptors[di];
  if (d.texture_count == 0 || slot >= d.texture_count) {
    return;
  }
  if (slot >= c.assets->texture_slots.size()) {
    return;
  }

  vk::ImageView v(c.assets->texture_slots[slot].view);
  if (!v) {
    v = vk::ImageView(c.assets->default_texture_view());
  }
  if (!v) {
    return;
  }

  // Descriptor contents are consumed by submitted frames, but have no presentation-engine
  // lifetime. Frame fences are the narrow synchronization boundary here; queue waitIdle is
  // reserved for swapchain recreation/teardown where vkQueuePresentKHR is not covered by them.
  c.base->wait_all_fences();

  const uint32_t binding = uint32_t(d.layout.size());
  const vk::DescriptorImageInfo info(vk::Sampler{}, v, vk::ImageLayout::eShaderReadOnlyOptimal);

  std::vector<vk::WriteDescriptorSet> writes;
  for (const auto set : d.sets) {
    if (set == VK_NULL_HANDLE) {
      continue;
    }
    vk::WriteDescriptorSet w;
    w.dstSet = set;
    w.dstBinding = binding;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = vk::DescriptorType::eSampledImage;
    w.pImageInfo = &info;
    writes.push_back(w);
  }

  vk::Device(c.device).updateDescriptorSets(writes, nullptr);
}

template <typename State>
void standard_render_bind_textures(State& c) {
  const uint32_t di = c.base->find_descriptor(c.config.texture_descriptor_name);
  if (di == painter::invalid_resource_slot) {
    utils::warn("render: descriptor '{}' not found", c.config.texture_descriptor_name);
    return;
  }

  auto& d = c.base->descriptors[di];
  if (d.texture_count == 0) {
    return;
  }

  const vk::ImageView fallback(c.assets->default_texture_view());
  if (!fallback) {
    utils::warn("render: default texture not created — skip texture bind");
    return;
  }

  c.base->wait_all_fences();

  const uint32_t binding = uint32_t(d.layout.size());
  const uint32_t n = d.texture_count;
  std::vector<vk::DescriptorImageInfo> infos(n);
  for (uint32_t i = 0; i < n; ++i) {
    vk::ImageView v = (i < c.assets->texture_slots.size()) ? vk::ImageView(c.assets->texture_slots[i].view) : vk::ImageView{};
    if (!v) {
      v = fallback;
    }
    infos[i] = vk::DescriptorImageInfo(vk::Sampler{}, v, vk::ImageLayout::eShaderReadOnlyOptimal);
  }

  std::vector<vk::WriteDescriptorSet> writes;
  for (const auto set : d.sets) {
    if (set == VK_NULL_HANDLE) {
      continue;
    }
    vk::WriteDescriptorSet w;
    w.dstSet = set;
    w.dstBinding = binding;
    w.dstArrayElement = 0;
    w.descriptorCount = n;
    w.descriptorType = vk::DescriptorType::eSampledImage;
    w.pImageInfo = infos.data();
    writes.push_back(w);
  }

  vk::Device(c.device).updateDescriptorSets(writes, nullptr);
  DE_LOG(catalogue::log_domain::render, flow, "render: bound {} sampled-image slots into descriptor '{}' ({} sets)", n, c.config.texture_descriptor_name, writes.size());
}

template <typename State, typename OnGraphReady>
void standard_render_try_create_graph(State& c, OnGraphReady&& on_graph_ready) {
  if (c.graph_ready || !c.base_ready || c.shader_prepare_failed) {
    return;
  }
  if (!c.config.headless && !c.surface_ready) {
    return;
  }
  if (!c.shaders_prepared) {
    standard_render_request_shader_prepare(c);
    return;
  }

  if (c.pending_graph_width != 0 || c.pending_graph_height != 0) {
    c.base->resize_viewport(c.pending_graph_width, c.pending_graph_height);
  }

  const uint32_t graph_index = c.base->find_render_graph(c.config.graph_name);
  if (graph_index == painter::invalid_resource_slot) {
    utils::error{}("Could not find render graph '{}'", c.config.graph_name);
  }

  c.base->populate_constant_default_values();
  c.base->change_render_graph(graph_index);
  standard_render_set_shader_sources_loaded(c.config.engine_registry, false);
  c.base->dump_cache_on_disk(c.config.pipeline_cache_path);

  c.graph_ready = true;
  standard_render_build_wb_name_map(c);
  standard_render_bind_textures(c);
  on_graph_ready();
}

template <typename State, typename OnDetach, typename OnGraphReady>
void standard_render_attach_window(State& c, const command_window_recreation& cmd, OnDetach&& on_detach, OnGraphReady&& on_graph_ready) {
  if (c.config.headless) {
    return;
  }
  if (!c.instance_ready) {
    standard_render_create_instance(c);
  }

  if (c.surface != VK_NULL_HANDLE) {
    standard_render_detach_window(c);
    on_detach();
  }

  const auto res = input::create_window_surface(c.instance, cmd.w, nullptr, &c.surface);
  if (res != static_cast<uint32_t>(vk::Result::eSuccess)) {
    utils::error{}("Could not create window surface, got {}", vk::to_string(static_cast<vk::Result>(res)));
  }
  c.surface_ready = true;

  standard_render_create_device(c, &cmd);
  standard_render_create_base_resources(c);

  c.base->set_surface(c.surface, cmd.width, cmd.height);
  c.pending_graph_width = cmd.width;
  c.pending_graph_height = cmd.height;
  standard_render_try_create_graph(c, std::forward<OnGraphReady>(on_graph_ready));
}

template <typename State>
void standard_render_resize_swapchain(State& c, const uint32_t w, const uint32_t h) {
  if (c.config.headless || !c.base || !c.graph_ready || !c.surface_ready) {
    return;
  }
  if (w == 0 || h == 0) {
    return;
  }
  // Frame fences cover graphics submission, not the presentation-engine use begun by
  // vkQueuePresentKHR. The old swapchain is destroyed inside resize_viewport(), so this rare
  // WSI lifecycle boundary must drain the queue that also performs presentation.
  standard_render_drain(c);
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
  if (!c.graph_ready) {
    return;
  }
  br.write_buffer.drain([&c](const wb_msg& m, const std::span<const std::byte> payload) {
    standard_render_write_buffer_bytes(c, m.name_hash, payload);
  });
}

template <typename State, typename Broker>
void standard_render_drain_graph_switch(State& c, Broker& br) {
  if (!c.graph_ready || !c.base) {
    return;
  }

  if (const command_render_set_graph* cmd = br.set_active_graph.consume()) {
    const uint32_t idx = c.base->find_render_graph(cmd->name);
    if (idx == painter::invalid_resource_slot) {
      utils::warn("render: set_active_graph: graph '{}' was not found", cmd->name);
    } else if (idx != c.base->current_render_graph_index) {
      c.base->change_render_graph(idx);
      DE_LOG(catalogue::log_domain::render, flow, "active graph switched to '{}'", cmd->name);
    }
  }
}

template <typename State, typename Broker>
void standard_render_drain_update_constants(State& c, Broker& br) {
  if (!c.base_ready || !c.base) {
    return;
  }

  command_render_update_constant cmd;
  while (br.update_constant.try_pop(cmd)) {
    const uint32_t idx = c.base->find_constant(cmd.name);
    if (idx == painter::invalid_resource_slot) {
      utils::warn("render: update_constant: constant '{}' was not found", cmd.name);
      continue;
    }
    c.base->write_constant_data(idx, cmd.bytes.data(), cmd.bytes.size());
    c.base->update_constant_memory();
  }
}

template <typename State, typename Broker>
void standard_render_drain_gpu_transitions(State& c, Broker& br) {
  if (!c.base_ready) {
    return;
  }

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
        standard_render_bind_texture_slot(c, static_cast<painter::gpu_texture_resource*>(res)->gpu_index);
      }
    } else {
      res->unload(handle);
    }

    br.gpu_done.try_push(command_gpu_done{cmd.res});
  }
}

template <typename State, typename Broker, typename AttachWindow, typename TryCreateGraph>
void standard_render_drain_commands(
  State& c,
  Broker& br,
  AttachWindow&& attach_window,
  TryCreateGraph&& try_create_graph) {
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
  standard_render_drain_gpu_transitions(c, br);
  standard_render_drain_write_buffer(c, br);
}

} // namespace simul
} // namespace devils_engine

#endif
