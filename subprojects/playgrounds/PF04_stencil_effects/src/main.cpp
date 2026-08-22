#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "devils_engine/input/core.h"
#include "devils_engine/input/events.h"
#include "devils_engine/painter/assets_base.h"
#include "devils_engine/painter/auxiliary.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/makers.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/playground/frame_pacer.h"
#include "devils_engine/playground/free_camera.h"
#include "devils_engine/playground/visage_overlay.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"

using namespace devils_engine;

namespace {

constexpr uint32_t initial_width = 1280;
constexpr uint32_t initial_height = 720;
constexpr float near_plane = 0.1f;

uint32_t pending_width = initial_width;
uint32_t pending_height = initial_height;
bool resize_pending = false;
bool stencil_debug = false;
int32_t escape_key = -1;
int32_t debug_key = -1;

void error_callback(const int error, const char* message) noexcept {
  utils::warn("PF04 input error {}: {}", error, message);
}

void key_callback(GLFWwindow* window, const int key, const int scancode, const int action, const int) noexcept {
  input::events::update_key(scancode, action);
  if (key == escape_key && action == 1) {
    input::set_should_close(window, true);
  }
  if (key == debug_key && action == 1) {
    stencil_debug = !stencil_debug;
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
    utils::error{}("PF04 could not resolve input key '{}'", canonical);
  }
  input::events::set_key(event, scancode, key);
}

struct vertex {
  float px, py, pz;
  float nx, ny, nz;
};

void add_quad(
  std::vector<vertex>& out,
  const glm::vec3& a,
  const glm::vec3& b,
  const glm::vec3& c,
  const glm::vec3& d,
  const glm::vec3& normal) {
  const auto push = [&out, normal](const glm::vec3& p) {
    out.push_back(vertex{p.x, p.y, p.z, normal.x, normal.y, normal.z});
  };
  push(a); push(b); push(c);
  push(a); push(c); push(d);
}

void add_box(std::vector<vertex>& out, const glm::vec3& center, const glm::vec3& half) {
  const glm::vec3 mn = center - half;
  const glm::vec3 mx = center + half;
  add_quad(out, {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}, {0, 0, 1});
  add_quad(out, {mx.x, mn.y, mn.z}, {mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}, {0, 0, -1});
  add_quad(out, {mx.x, mn.y, mx.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}, {1, 0, 0});
  add_quad(out, {mn.x, mn.y, mn.z}, {mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z}, {-1, 0, 0});
  add_quad(out, {mn.x, mx.y, mx.z}, {mx.x, mx.y, mx.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z}, {0, 1, 0});
  add_quad(out, {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z}, {0, -1, 0});
}

std::vector<vertex> make_room() {
  std::vector<vertex> out;
  constexpr float x = 5.0f;
  constexpr float y0 = -1.5f;
  constexpr float y1 = 3.5f;
  constexpr float z0 = -5.0f;
  constexpr float z1 = 3.0f;
  add_quad(out, {-x, y0, z1}, {x, y0, z1}, {x, y0, z0}, {-x, y0, z0}, {0, 1, 0});
  add_quad(out, {-x, y0, z0}, {x, y0, z0}, {x, y1, z0}, {-x, y1, z0}, {0, 0, 1});
  add_quad(out, {-x, y0, z1}, {-x, y0, z0}, {-x, y1, z0}, {-x, y1, z1}, {1, 0, 0});
  add_quad(out, {x, y0, z0}, {x, y0, z1}, {x, y1, z1}, {x, y1, z0}, {-1, 0, 0});
  return out;
}

std::vector<vertex> make_cube() {
  std::vector<vertex> out;
  out.reserve(36);
  add_box(out, {}, {0.72f, 0.72f, 0.72f});
  return out;
}

struct alignas(16) camera_block {
  glm::mat4 view_projection;
  glm::mat4 view;
  glm::vec4 camera_position;
  glm::vec4 viewport_near;
  glm::vec4 debug_params;
};
static_assert(sizeof(camera_block) == 176);

void write_current_buffer(
  painter::graphics_base& base,
  const std::string_view name,
  const void* data,
  const size_t bytes) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF04 buffer '{}' is absent from the configured graph", name);
  }
  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}(
      "PF04 cannot write buffer '{}' (mapped {}, capacity {}, requested {})",
      name, frame.mapped != nullptr, frame.sub.size, bytes);
  }
  if (bytes != 0) {
    std::memcpy(static_cast<uint8_t*>(frame.mapped) + frame.sub.offset, data, bytes);
  }
}

void write_overlay_buffers(painter::graphics_base& base, const playground::visage_overlay& overlay) {
  const auto vertices = overlay.vertices();
  const auto indices = overlay.indices();
  write_current_buffer(base, "ui_vertices", vertices.data(), vertices.size());
  write_current_buffer(base, "ui_indices", indices.data(), indices.size());

  const auto commands = overlay.commands();
  const uint32_t slot = base.find_resource("ui_commands");
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF04 buffer 'ui_commands' is absent from the configured graph");
  }
  const auto frame = base.get_current_buffer_resource_frame(slot);
  const uint32_t count = uint32_t(commands.size());
  const size_t bytes = sizeof(count) + commands.size_bytes();
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}(
      "PF04 cannot write UI commands (mapped {}, capacity {}, requested {})",
      frame.mapped != nullptr, frame.sub.size, bytes);
  }
  auto* destination = static_cast<uint8_t*>(frame.mapped) + frame.sub.offset;
  std::memcpy(destination, &count, sizeof(count));
  if (!commands.empty()) {
    std::memcpy(destination + sizeof(count), commands.data(), commands.size_bytes());
  }
}

void bind_texture_descriptor(
  painter::graphics_base& base,
  const painter::assets_base& assets,
  const std::string_view descriptor_name) {
  const uint32_t slot = base.find_descriptor(descriptor_name);
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF04 texture descriptor '{}' is absent", descriptor_name);
  }
  auto& descriptor = base.descriptors[slot];
  const vk::ImageView fallback(assets.default_texture_view());
  if (!fallback || descriptor.texture_count == 0) {
    utils::error{}("PF04 texture descriptor '{}' has no fallback or slots", descriptor_name);
  }

  std::vector<vk::DescriptorImageInfo> images(descriptor.texture_count);
  for (uint32_t i = 0; i < descriptor.texture_count; ++i) {
    vk::ImageView view;
    if (i < assets.texture_slots.size()) {
      view = assets.texture_slots[i].view;
    }
    images[i] = vk::DescriptorImageInfo(
      vk::Sampler{},
      view ? view : fallback,
      vk::ImageLayout::eShaderReadOnlyOptimal);
  }

  std::vector<vk::WriteDescriptorSet> writes;
  for (const auto raw_set : descriptor.sets) {
    if (raw_set == VK_NULL_HANDLE) {
      continue;
    }
    vk::WriteDescriptorSet write;
    write.dstSet = raw_set;
    write.dstBinding = uint32_t(descriptor.layout.size());
    write.descriptorCount = descriptor.texture_count;
    write.descriptorType = vk::DescriptorType::eSampledImage;
    write.pImageInfo = images.data();
    writes.push_back(write);
  }
  vk::Device(base.device).updateDescriptorSets(writes, nullptr);
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

void write_pair(
  painter::graphics_base& base,
  const uint32_t pair,
  const std::span<const glm::vec4> instances,
  const uint32_t vertex_count) {
  for (uint32_t offset = 0; offset < base.frames_in_flight(); ++offset) {
    const auto instance_frame = base.get_current_instance_resource_frame(pair, offset);
    const auto indirect_frame = base.get_current_indirect_resource_frame(pair, offset);
    const size_t instance_bytes = instances.size_bytes();
    if (instance_bytes > instance_frame.sub.size) {
      utils::error{}("PF04 pair {} instance data exceeds its configured budget", pair);
    }
    std::memcpy(
      static_cast<uint8_t*>(instance_frame.mapped) + instance_frame.sub.offset,
      instances.data(),
      instance_bytes);
    VkDrawIndirectCommand command{};
    command.vertexCount = vertex_count;
    command.instanceCount = uint32_t(instances.size());
    std::memcpy(
      static_cast<uint8_t*>(indirect_frame.mapped) + indirect_frame.sub.offset,
      &command,
      sizeof(command));
  }
}

} // namespace

int main(int argc, char** argv) {
  bool validation = false;
  bool uncapped = false;
  bool fixed_camera = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option(argv[i]);
    validation = validation || option == "--validation";
    uncapped = uncapped || option == "--uncapped";
    fixed_camera = fixed_camera || option == "--fixed-camera";
    stencil_debug = stencil_debug || option == "--stencil-debug";
  }

  input::init input_runtime(&error_callback);
  input::events::init();
  painter::load_dispatcher1();

  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "PF04 Stencil Effects";
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
      utils::error{}("PF04 requested validation, but Vulkan validation layers are unavailable");
    }
    instance_info.enabledLayerCount = uint32_t(painter::default_validation_layers.size());
    instance_info.ppEnabledLayerNames = painter::default_validation_layers.data();
  }

  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);
  const VkDebugUtilsMessengerEXT messenger = validation
    ? painter::create_debug_messenger(instance)
    : VK_NULL_HANDLE;

  GLFWwindow* window = input::create_window(initial_width, initial_height, "PF04 — stencil outline");
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  const auto surface_result = input::create_window_surface(instance, window, nullptr, &surface);
  if (surface_result != uint32_t(vk::Result::eSuccess)) {
    utils::error{}("PF04 could not create Vulkan surface: {}", vk::to_string(vk::Result(surface_result)));
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
  const VkDevice device = device_maker.create({}, "pf04.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();
    const std::string resource_root = std::string(PF04_RESOURCE_ROOT) + "/";
    const std::string cache_path = utils::cache_folder() + "pf04_stencil_effects.pipeline_cache";
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("pf04_stencil");

    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0) {
      utils::error{}("PF04 could not commit render graph from '{}'", resource_root);
    }
    base.set_surface(surface, initial_width, initial_height);
    base.resize_viewport(initial_width, initial_height);
    base.populate_constant_default_values();
    const uint32_t graph = base.find_render_graph("pf04_stencil");
    if (graph == painter::invalid_resource_slot) {
      utils::error{}("PF04 render graph was not found");
    }
    base.change_render_graph(graph);

    painter::assets_base assets(device, physical.handle);
    assets.create_fence();
    assets.create_allocator(instance);
    assets.create_command_buffer(queues.transfer, queues.graphics);
    assets.set_graphics_base(&base);
    assets.create_default_texture();

    const auto upload_mesh = [&assets](const std::string& id, const std::vector<vertex>& data) {
      const auto slot = assets.register_buffer_storage(id);
      assets.create_buffer_storage(slot, painter::buffer_create_info{"scene_geometry", uint32_t(data.size()), 0});
      assets.populate_buffer_storage(
        slot,
        std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(data[0])),
        std::span<const uint8_t>{});
      assets.mark_ready_buffer_slot(slot);
      return slot;
    };

    const auto room = make_room();
    const auto cube = make_cube();
    const auto room_mesh = upload_mesh("pf04.room", room);
    const auto cube_mesh = upload_mesh("pf04.cube", cube);

    const uint32_t scene_group = base.find_draw_group("scene_draw_group");
    const uint32_t selected_group = base.find_draw_group("selected_draw_group");
    if (scene_group == painter::invalid_resource_slot || selected_group == painter::invalid_resource_slot) {
      utils::error{}("PF04 scene draw groups are absent");
    }
    const uint32_t room_pair = base.register_pair(scene_group, room_mesh, 1);
    const uint32_t props_pair = base.register_pair(scene_group, cube_mesh, 3);
    const uint32_t selected_pair = base.register_pair(selected_group, cube_mesh, 1);

    const std::array<glm::vec4, 1> room_instances{glm::vec4{0.0f, 0.0f, 0.0f, 0.0f}};
    const std::array<glm::vec4, 3> prop_instances{
      glm::vec4{-2.0f, -0.78f, -1.0f, 1.0f},
      glm::vec4{2.1f, -0.78f, -2.1f, 1.0f},
      glm::vec4{0.1f, 0.75f, -3.6f, 1.0f}};
    const std::array<glm::vec4, 1> selected_instances{glm::vec4{0.0f, -0.72f, -1.55f, 2.0f}};
    write_pair(base, room_pair, room_instances, uint32_t(room.size()));
    write_pair(base, props_pair, prop_instances, uint32_t(cube.size()));
    write_pair(base, selected_pair, selected_instances, uint32_t(cube.size()));

    const std::string common_resources = std::string(PLAYGROUND_COMMON_RESOURCE_ROOT) + "/";
    playground::visage_overlay overlay(
      common_resources + "fonts/crimson.roman.ttf",
      common_resources + "ui/lab_overlay.lua",
      playground::overlay_description{
        "PF04 — Stencil effects",
        "selected object writes stencil=1 → expanded shell tests !=1",
        "WASD/QE move · mouse look · V stencil tint · Esc exit"});
    const auto atlas = overlay.font_atlas();
    const auto font_texture = assets.register_texture_storage("playground.crimson_roman");
    assets.create_texture_storage(
      font_texture,
      painter::texture_create_info{{atlas.width, atlas.height, 1}, VK_FORMAT_R8G8B8A8_UNORM});
    assets.populate_texture_storage(font_texture, atlas.bytes);
    assets.mark_ready_texture_slot(font_texture);
    overlay.set_font_texture(font_texture);
    bind_texture_descriptor(base, assets, "ui_textures");

    input::events::clear_bindings();
    bind_key("camera_forward", "key_w");
    bind_key("camera_back", "key_s");
    bind_key("camera_left", "key_a");
    bind_key("camera_right", "key_d");
    bind_key("camera_down", "key_q");
    bind_key("camera_up", "key_e");
    bind_key("camera_fast", "left_shift");
    escape_key = input::glfw_key_from_canonical("escape");
    debug_key = input::glfw_key_from_canonical("key_v");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_DISABLED);
    input::set_raw_mouse_motion(window);

    playground::free_camera camera;
    camera.position = {0.0f, 0.2f, 5.2f};
    auto [mouse_x, mouse_y] = input::cursor_pos(window);
    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;
    playground::frame_pacer frame_pacer(uncapped ? 0u : 60u);

    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;

    utils::info("PF04 controls: WASD/QE move, Shift accelerate, mouse look, V stencil tint, Esc exit");
    utils::info("PF04 graph: scene -> stencil writer -> outline -> stencil debug consumer -> UI -> present");

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
      if (!fixed_camera) {
        camera.update(motion, dt);
      }

      if (!base.can_draw()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }

      base.prepare_frame();
      const float aspect = float(std::max(pending_width, 1u)) / float(std::max(pending_height, 1u));
      const auto view = camera.view();
      const auto projection = playground::infinite_reverse_z_projection(glm::radians(65.0f), aspect, near_plane);
      camera_block camera_data{};
      camera_data.view_projection = projection * view;
      camera_data.view = view;
      camera_data.camera_position = glm::vec4(camera.position, 1.0f);
      camera_data.viewport_near = glm::vec4(float(pending_width), float(pending_height), near_plane, 0.0f);
      camera_data.debug_params = glm::vec4(stencil_debug ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
      write_current_buffer(base, "camera_buffer", &camera_data, sizeof(camera_data));

      const uint64_t frame_delta_us = uint64_t(std::max(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<float>(dt)).count(),
        int64_t{1}));
      const uint64_t timestamp_us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count());
      const std::array<std::string, 2> details{
        "Stencil writer: reference 1, compare always, pass replace",
        stencil_debug ? "Debug tint: ON (fullscreen compare equal 1)" : "Debug tint: OFF (press V)"};
      overlay.set_detail_lines(details);
      overlay.update(frame_delta_us, timestamp_us);
      write_overlay_buffers(base, overlay);

      context.prepare();
      context.draw();
      base.submit_frame();
      frame_pacer.wait();
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
