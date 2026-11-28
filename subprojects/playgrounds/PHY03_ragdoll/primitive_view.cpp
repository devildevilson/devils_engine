#include "primitive_view.h"

#include <cstring>

#include "devils_engine/input/core.h"
#include "devils_engine/painter/auxiliary.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/playground/free_camera.h"
#include "devils_engine/utils/error.h"

namespace devils_engine::phy03::primitive_view {
namespace {
void write_buffer(painter::graphics_base& render, const std::string_view name, const void* data, const size_t bytes) {
  const auto slot = render.find_resource(name);
  if (slot == painter::invalid_resource_slot) utils::error{}("Primitive viewer: missing buffer '{}'", name);
  const auto frame = render.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size)
    utils::error{}("Primitive viewer buffer '{}': requested {} bytes, capacity {}, mapped {}",
                   name, bytes, frame.sub.size, frame.mapped != nullptr);
  std::memcpy(static_cast<char*>(frame.mapped) + frame.sub.offset, data, bytes);
}

void write_draw(painter::graphics_base& render, const std::string_view name, const uint32_t first, const uint32_t count) {
  const auto command = render.find_constant(name);
  if (command == painter::invalid_resource_slot) utils::error{}("Primitive viewer: missing draw constant '{}'", name);
  render.write_constant_data(command, VkDrawIndirectCommand{count, 1, first, 0});
}

struct vulkan_handles {
  painter::created_instance instance;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  painter::created_device device;

  ~vulkan_handles() {
    if (device.device) vk::Device(device.device).destroy();
    if (surface) vk::Instance(instance.instance).destroy(surface);
    painter::destroy_debug_messenger(instance.instance, instance.messenger);
    if (instance.instance) vk::Instance(instance.instance).destroy();
  }
};

}

struct renderer::state {
  vulkan_handles gpu;
  std::unique_ptr<painter::graphics_base> render;
  painter::graphics_ctx context;
  uint32_t width = 0;
  uint32_t height = 0;

  ~state() {
    if (gpu.device.device) VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(gpu.device.device);
  }
};

renderer::renderer(GLFWwindow* window, const std::string& title, const bool validation) : data(std::make_unique<state>()) {
  auto& gpu = data->gpu;
  gpu.instance = painter::create_instance({.app_name = title, .validation = validation});
  const auto surface_result = input::create_window_surface(gpu.instance.instance, window, nullptr, &gpu.surface);
  if (surface_result != VK_SUCCESS) utils::error{}("{}: Vulkan surface creation failed with VkResult {}", title, surface_result);
  painter::system_info system(gpu.instance.instance);
  system.check_devices_surface_capability(gpu.surface);
  const auto physical = system.choose_physical_device();
  gpu.device = painter::create_device(gpu.instance.instance, physical, true, "physics playground viewer");

  const auto [width, height] = input::framebuffer_size(window);
  data->width = width;
  data->height = height;
  data->render = std::make_unique<painter::graphics_base>(
    gpu.instance.instance, gpu.device.device, physical.handle, painter::presentation_engine_type::main);
  auto& render = *data->render;
  render.create_allocator();
  render.create_command_pool(gpu.device.queues.graphics);
  render.create_descriptor_pool();
  const std::string resource_root = PHY03_RESOURCE_ROOT;
  render.set_shader_source_filesystem(resource_root + "/shaders/");
  render.set_startup_graph("phy03_room");
  auto config = painter::build_render_config(resource_root + "/render_config/");
  if (render.commit_parsed_resources(config) != 0) utils::error{}("{}: could not commit render graph '{}'", title, resource_root);
  render.set_present_mode(painter::physical_device_present_mode::values::mailbox,
                          painter::physical_device_present_mode::values::immediate);
  render.set_surface(gpu.surface, width, height);
  render.resize_viewport(width, height);
  render.populate_constant_default_values();
  const auto graph = render.find_render_graph("phy03_room");
  if (graph == painter::invalid_resource_slot) utils::error{}("{}: missing graph 'phy03_room'", title);
  render.change_render_graph(graph);
  data->context.base = &render;
}

renderer::~renderer() = default;

bool renderer::draw(const glm::mat4& view, const uint32_t width, const uint32_t height,
                    const std::span<const vertex> vertices, const uint32_t solid_count) {
  if (width == 0 || height == 0) return false;
  auto& render = *data->render;
  if (width != data->width || height != data->height || render.presentable_state_waiting_host_event()) {
    vk::Device(data->gpu.device.device).waitIdle();
    data->width = width;
    data->height = height;
    render.resize_viewport(width, height);
  }
  if (!render.can_draw()) return false;
  render.prepare_frame();
  if (!render.can_draw()) return false;

  const auto projection = playground::infinite_reverse_z_projection(1.25f, float(width) / float(height), .045f);
  const auto vp = projection * view;
  write_buffer(render, "camera_buffer", &vp, sizeof(vp));
  write_buffer(render, "scene_vertices", vertices.data(), vertices.size_bytes());
  write_draw(render, "room_draw", 0, solid_count);
  write_draw(render, "collider_draw", solid_count, uint32_t(vertices.size()) - solid_count);
  render.update_event();
  data->context.prepare();
  data->context.draw();
  render.submit_frame();
  return true;
}

}
