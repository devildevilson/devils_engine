#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/scalar_constants.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec4.hpp>

#include "devils_engine/input/core.h"
#include "devils_engine/input/events.h"
#include "devils_engine/painter/assets_base.h"
#include "devils_engine/painter/auxiliary.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/makers.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/playground/free_camera.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"

using namespace devils_engine;

namespace {

constexpr uint32_t initial_width = 1280;
constexpr uint32_t initial_height = 720;
constexpr uint32_t light_count = 96;
constexpr float near_plane = 0.1f;

uint32_t pending_width = initial_width;
uint32_t pending_height = initial_height;
bool resize_pending = false;
int32_t escape_key = -1;

void error_callback(const int error, const char* message) noexcept {
  utils::warn("PF01 input error {}: {}", error, message);
}

void key_callback(GLFWwindow* window, const int key, const int scancode, const int action, const int) noexcept {
  input::events::update_key(scancode, action);
  if (key == escape_key && action == 1) {
    input::set_should_close(window, true);
  }
}

void framebuffer_callback(GLFWwindow*, const int width, const int height) noexcept {
  pending_width = width > 0 ? uint32_t(width) : 0u;
  pending_height = height > 0 ? uint32_t(height) : 0u;
  resize_pending = pending_width != 0 && pending_height != 0;
}

void bind_key(const std::string_view event, const std::string_view canonical) {
  const auto [key, scancode] = input::key_from_canonical(canonical);
  if (key < 0 || scancode < 0) {
    utils::error{}("PF01 could not resolve input key '{}'", canonical);
  }
  input::events::set_key(event, scancode, key);
}

struct vertex {
  float px, py, pz;
  float nx, ny, nz;
  float u, v;
};

void add_quad(
  std::vector<vertex>& vertices,
  const glm::vec3& a,
  const glm::vec3& b,
  const glm::vec3& c,
  const glm::vec3& d,
  const glm::vec3& normal) {
  const auto push = [&vertices, normal](const glm::vec3& p, const float u, const float v) {
    vertices.push_back(vertex{p.x, p.y, p.z, normal.x, normal.y, normal.z, u, v});
  };
  push(a, 0.0f, 0.0f);
  push(b, 1.0f, 0.0f);
  push(c, 1.0f, 1.0f);
  push(a, 0.0f, 0.0f);
  push(c, 1.0f, 1.0f);
  push(d, 0.0f, 1.0f);
}

std::vector<vertex> make_room() {
  constexpr float x = 6.0f;
  constexpr float y = 3.0f;
  constexpr float z = 6.0f;
  std::vector<vertex> out;
  out.reserve(36);

  add_quad(out, {-x, -y, -z}, {x, -y, -z}, {x, y, -z}, {-x, y, -z}, {0, 0, 1});
  add_quad(out, {x, -y, z}, {-x, -y, z}, {-x, y, z}, {x, y, z}, {0, 0, -1});
  add_quad(out, {-x, -y, z}, {-x, -y, -z}, {-x, y, -z}, {-x, y, z}, {1, 0, 0});
  add_quad(out, {x, -y, -z}, {x, -y, z}, {x, y, z}, {x, y, -z}, {-1, 0, 0});
  add_quad(out, {-x, -y, z}, {x, -y, z}, {x, -y, -z}, {-x, -y, -z}, {0, 1, 0});
  add_quad(out, {-x, y, -z}, {x, y, -z}, {x, y, z}, {-x, y, z}, {0, -1, 0});
  return out;
}

glm::vec3 hue_color(const float hue) {
  const glm::vec3 wave = glm::abs(glm::fract(glm::vec3(hue) + glm::vec3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f);
  return glm::clamp(wave - 1.0f, 0.0f, 1.0f);
}

struct alignas(16) camera_block {
  glm::mat4 view_projection;
  glm::mat4 view;
  glm::vec4 camera_position;
  glm::vec4 viewport_near;
};
static_assert(sizeof(camera_block) == 160);

std::array<glm::vec4, 1 + light_count * 2> make_lights(const float time) {
  std::array<glm::vec4, 1 + light_count * 2> words{};
  words[0].x = std::bit_cast<float>(light_count);
  for (uint32_t i = 0; i < light_count; ++i) {
    const uint32_t ix = i % 8;
    const uint32_t iy = (i / 8) % 4;
    const uint32_t iz = i / 32;
    const float phase = float(i) * 0.713f;
    const glm::vec3 base{
      -4.8f + float(ix) * (9.6f / 7.0f),
      -2.0f + float(iy) * (4.0f / 3.0f),
      -4.5f + float(iz) * 4.5f};
    const glm::vec3 motion{
      std::sin(time * 0.71f + phase) * 0.28f,
      std::sin(time * 0.93f + phase * 1.7f) * 0.24f,
      std::cos(time * 0.59f + phase) * 0.32f};
    words[1 + i * 2] = glm::vec4(base + motion, 2.75f);
    words[2 + i * 2] = glm::vec4(hue_color(float(i) / float(light_count)), 2.8f);
  }
  return words;
}

void write_current_buffer(painter::graphics_base& base, const std::string_view name, const void* data, const size_t bytes) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF01 buffer '{}' is absent from the configured graph", name);
  }
  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}("PF01 cannot write buffer '{}' (mapped {}, capacity {}, requested {})", name, frame.mapped != nullptr, frame.sub.size, bytes);
  }
  std::memcpy(static_cast<uint8_t*>(frame.mapped) + frame.sub.offset, data, bytes);
}

std::vector<const char*> instance_extensions(const bool validation) {
  uint32_t count = 0;
  const char** required = input::get_required_instance_extensions(&count);
  std::vector<const char*> extensions(required, required + count);
  if (validation) {
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }
  return extensions;
}

} // namespace

int main(int argc, char** argv) {
  const bool validation = argc > 1 && std::string_view(argv[1]) == "--validation";
  input::init input_runtime(&error_callback);
  input::events::init();
  painter::load_dispatcher1();

  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "PF01 Forward+";
  app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  app_info.pEngineName = "devils_engine";
  app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
  app_info.apiVersion = VK_API_VERSION_1_0;

  const auto extensions = instance_extensions(validation);
  vk::InstanceCreateInfo instance_info{};
  instance_info.pApplicationInfo = &app_info;
  instance_info.enabledExtensionCount = uint32_t(extensions.size());
  instance_info.ppEnabledExtensionNames = extensions.data();
  if (validation) {
    if (!painter::check_validation_layer_support(painter::default_validation_layers)) {
      utils::error{}("PF01 requested validation, but Vulkan validation layers are unavailable");
    }
    instance_info.enabledLayerCount = uint32_t(painter::default_validation_layers.size());
    instance_info.ppEnabledLayerNames = painter::default_validation_layers.data();
  }

  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);
  const VkDebugUtilsMessengerEXT messenger = validation ? painter::create_debug_messenger(instance) : VK_NULL_HANDLE;

  GLFWwindow* window = input::create_window(initial_width, initial_height, "PF01 — depth + compute + Forward+");
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  const auto surface_result = input::create_window_surface(instance, window, nullptr, &surface);
  if (surface_result != uint32_t(vk::Result::eSuccess)) {
    utils::error{}("PF01 could not create Vulkan surface: {}", vk::to_string(vk::Result(surface_result)));
  }

  painter::system_info system(instance);
  system.check_devices_surface_capability(surface);
  const auto physical = system.choose_physical_device();
  painter::system_info::print_choosed_device(physical.handle);
  const auto queue_plan = painter::make_device_queue_plan(physical);

  painter::device_maker device_maker(instance);
  device_maker.beginDevice(physical.handle);
  for (uint32_t i = 0; i < queue_plan.request_count; ++i) {
    device_maker.createQueue(queue_plan.requests[i].family, queue_plan.requests[i].count);
  }
  device_maker.features(vk::PhysicalDevice(physical.handle).getFeatures());
  device_maker.setExtensions(painter::default_device_extensions);
  const VkDevice device = device_maker.create({}, "pf01.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();
    const std::string resource_root = std::string(PF01_RESOURCE_ROOT) + "/";
    const std::string cache_path = utils::cache_folder() + "pf01_forward_plus.pipeline_cache";
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("pf01_forward_plus");

    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0) {
      utils::error{}("PF01 could not commit render graph from '{}'", resource_root);
    }
    base.set_surface(surface, initial_width, initial_height);
    base.resize_viewport(initial_width, initial_height);
    base.populate_constant_default_values();
    const uint32_t graph = base.find_render_graph("pf01_forward_plus");
    if (graph == painter::invalid_resource_slot) {
      utils::error{}("PF01 render graph was not found");
    }
    base.change_render_graph(graph);

    painter::assets_base assets(device, physical.handle);
    assets.create_fence();
    assets.create_allocator(instance);
    assets.create_command_buffer(queues.transfer, queues.graphics);
    assets.set_graphics_base(&base);

    const auto room = make_room();
    const auto mesh = assets.register_buffer_storage("pf01.room");
    assets.create_buffer_storage(mesh, painter::buffer_create_info{"room_geometry", uint32_t(room.size()), 0});
    assets.populate_buffer_storage(
      mesh,
      std::span(reinterpret_cast<const uint8_t*>(room.data()), room.size() * sizeof(room[0])),
      std::span<const uint8_t>{});
    assets.mark_ready_buffer_slot(mesh);
    const uint32_t draw_group = base.find_draw_group("room_draw_group");
    const uint32_t pair = base.register_pair(draw_group, mesh, 1);
    for (uint32_t offset = 0; offset < base.frames_in_flight(); ++offset) {
      const auto instance = base.get_current_instance_resource_frame(pair, offset);
      const auto indirect = base.get_current_indirect_resource_frame(pair, offset);
      const glm::vec4 identity_instance{0.0f};
      std::memcpy(static_cast<uint8_t*>(instance.mapped) + instance.sub.offset, &identity_instance, sizeof(identity_instance));
      VkDrawIndirectCommand command{};
      command.vertexCount = uint32_t(room.size());
      command.instanceCount = 1;
      std::memcpy(static_cast<uint8_t*>(indirect.mapped) + indirect.sub.offset, &command, sizeof(command));
    }

    input::events::clear_bindings();
    bind_key("camera_forward", "key_w");
    bind_key("camera_back", "key_s");
    bind_key("camera_left", "key_a");
    bind_key("camera_right", "key_d");
    bind_key("camera_down", "key_q");
    bind_key("camera_up", "key_e");
    bind_key("camera_fast", "left_shift");
    escape_key = input::glfw_key_from_canonical("escape");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_DISABLED);
    input::set_raw_mouse_motion(window);

    playground::free_camera camera;
    camera.position = {0.0f, 0.0f, 4.2f};
    auto [mouse_x, mouse_y] = input::cursor_pos(window);
    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;

    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;

    utils::info("PF01 controls: WASD move, Q/E down/up, Shift accelerate, mouse look, Esc exit");
    utils::info("PF01 graph: depth prepass -> compute light lists -> Forward+ -> present");

    while (!input::should_close(window)) {
      input::poll_events();
      const auto now = std::chrono::steady_clock::now();
      const float dt = std::clamp(std::chrono::duration<float>(now - previous_time).count(), 0.0f, 0.1f);
      previous_time = now;
      input::events::update(size_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<float>(dt)).count()));

      if (resize_pending) {
        vk::Device(device).waitIdle();
        base.resize_viewport(pending_width, pending_height);
        resize_pending = false;
      }

      const auto [next_mouse_x, next_mouse_y] = input::cursor_pos(window);
      playground::camera_motion motion;
      motion.forward = float(input::events::is_pressed("camera_forward")) - float(input::events::is_pressed("camera_back"));
      motion.right = float(input::events::is_pressed("camera_right")) - float(input::events::is_pressed("camera_left"));
      motion.up = float(input::events::is_pressed("camera_up")) - float(input::events::is_pressed("camera_down"));
      motion.fast = input::events::is_pressed("camera_fast");
      motion.look_delta = {float(next_mouse_x - mouse_x), float(next_mouse_y - mouse_y)};
      mouse_x = next_mouse_x;
      mouse_y = next_mouse_y;
      camera.update(motion, dt);

      if (!base.can_draw()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }

      base.prepare_frame();
      const float aspect = float(std::max(pending_width, 1u)) / float(std::max(pending_height, 1u));
      const auto view = camera.view();
      const auto projection = playground::infinite_reverse_z_projection(glm::radians(68.0f), aspect, near_plane);
      camera_block camera_data{};
      camera_data.view_projection = projection * view;
      camera_data.view = view;
      camera_data.camera_position = glm::vec4(camera.position, 1.0f);
      camera_data.viewport_near = glm::vec4(float(pending_width), float(pending_height), near_plane, 0.0f);
      write_current_buffer(base, "camera_buffer", &camera_data, sizeof(camera_data));

      const float seconds = std::chrono::duration<float>(now - start_time).count();
      const auto lights = make_lights(seconds);
      write_current_buffer(base, "light_buffer", lights.data(), sizeof(lights));

      context.prepare();
      context.draw();
      base.submit_frame();
    }

    vk::Device(device).waitIdle();
    base.dump_cache_on_disk(cache_path);
  }

  vk::Instance(instance).destroy(surface);
  input::destroy(window);
  vk::Device(device).destroy();
  painter::destroy_debug_messenger(instance, messenger);
  vk::Instance(instance).destroy();
  return 0;
}
