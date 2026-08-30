#include "viewer.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include <glm/detail/type_half.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>

#include "devils_engine/input/core.h"
#include "devils_engine/input/events.h"
#include "devils_engine/painter/assets_base.h"
#include "devils_engine/painter/auxiliary.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/gpu_timing.h"
#include "devils_engine/painter/makers.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/playground/frame_pacer.h"
#include "devils_engine/playground/free_camera.h"
#include "devils_engine/playground/visage_overlay.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/visage/font.h"

#include "planet.h"

using namespace devils_engine;

namespace devils_engine::pf10 {
namespace {

uint32_t pending_width = 1280;
uint32_t pending_height = 720;
bool resize_pending = false;
int32_t escape_key = -1;
double scroll_accumulator = 0.0;
bool click_pending = false;

void error_callback(const int error, const char* message) noexcept {
  utils::warn("PF10 input error {}: {}", error, message);
}

void key_callback(GLFWwindow* window, const int key, const int scancode, const int action, const int) noexcept {
  input::events::update_key(scancode, action);
  if (key == escape_key && action == 1) input::set_should_close(window, true);
}

void framebuffer_callback(GLFWwindow*, const int width, const int height) noexcept {
  pending_width = width > 0 ? uint32_t(width) : 0u;
  pending_height = height > 0 ? uint32_t(height) : 0u;
  resize_pending = pending_width != 0 && pending_height != 0;
}

void scroll_callback(GLFWwindow*, const double, const double y) noexcept { scroll_accumulator += y; }

void mouse_callback(GLFWwindow*, const int button, const int action, const int) noexcept {
  if (button == 0 && action == 1) click_pending = true;
}

void bind_key(const std::string_view event, const std::string_view canonical) {
  const auto [key, scancode] = input::key_from_canonical(canonical);
  if (key < 0 || scancode < 0) utils::error{}("PF10 could not resolve input key '{}'", canonical);
  input::events::set_key(event, scancode, key);
}

std::vector<const char*> instance_extensions(const bool validation) {
  uint32_t count = 0;
  const char** required = input::get_required_instance_extensions(&count);
  std::vector<const char*> extensions(required, required + count);
  if (validation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  return extensions;
}

struct alignas(16) camera_block {
  glm::mat4 view_projection;
  glm::mat4 planet_to_world;
  glm::mat4 world_to_planet;
  glm::vec4 camera_position;
  glm::vec4 light_direction;
  glm::vec4 border_colour;
  glm::uvec4 params; // selected id, hovered id, cube-face side, flags
  glm::vec4 viewport_near;
  glm::mat4 inverse_view_projection;
};
static_assert(sizeof(camera_block) == 336);

struct alignas(16) decal_glyph {
  glm::mat4 decal_to_planet;
  glm::mat4 planet_to_decal;
  glm::vec4 uv_rect;
  glm::vec4 fill;
  glm::vec4 effect; // atlas slot, boldness, softness, label class
};
static_assert(sizeof(decal_glyph) == 176);

struct label_set {
  std::vector<decal_glyph> glyphs;
  uint32_t empire_glyphs = 0;
  uint32_t province_glyphs = 0;
};

void append_surface_label(std::vector<decal_glyph>& result, const visage::font_t& font,
                          const std::string_view text, glm::vec3 anchor, const float font_height,
                          const glm::vec4 fill, const uint32_t texture_slot, const uint32_t owner_region) {
  anchor = glm::normalize(anchor);
  const glm::vec3 reference = std::abs(anchor.y) > 0.92f ? glm::vec3(1.0f, 0.0f, 0.0f) :
                                                           glm::vec3(0.0f, 1.0f, 0.0f);
  const glm::vec3 right = glm::normalize(glm::cross(reference, anchor));
  const glm::vec3 up = glm::normalize(glm::cross(anchor, right));
  float cursor = -float(font.text_width(font_height, text)) * 0.5f;
  for (const unsigned char character : text) {
      const auto* glyph = font.find_glyph(uint32_t(character));
      if (glyph == nullptr) continue;
      const float advance = float(glyph->advance) * font_height;
      if (glyph->w > 0 && glyph->h > 0) {
        const float left = cursor + float(glyph->pl) * font_height;
        const float bottom = float(glyph->pb) * font_height - font_height * 0.32f;
        const float width = float(glyph->pr - glyph->pl) * font_height;
        const float height = float(glyph->pt - glyph->pb) * font_height;
        const glm::vec3 glyph_direction = glm::normalize(anchor + right * (left + width * 0.5f) +
                                                          up * (bottom + height * 0.5f));
        const glm::vec3 glyph_right = glm::normalize(right - glyph_direction * glm::dot(right, glyph_direction));
        const glm::vec3 glyph_up = glm::normalize(glm::cross(glyph_direction, glyph_right));
        const glm::vec3 centre = surface_position(glyph_direction);
        constexpr float projection_depth = 0.120f;
        const glm::mat4 decal_to_planet{
          glm::vec4(glyph_right * width, 0.0f), glm::vec4(glyph_up * height, 0.0f),
          glm::vec4(glyph_direction * projection_depth, 0.0f), glm::vec4(centre, 1.0f)};
        result.push_back(decal_glyph{
          decal_to_planet, glm::inverse(decal_to_planet),
          glm::vec4(float(glyph->al / double(font.width)), float(glyph->ab / double(font.height)),
                    float(glyph->ar / double(font.width)), float(glyph->at / double(font.height))),
          fill, glm::vec4(float(texture_slot), 0.0f, 0.05f, std::bit_cast<float>(owner_region))});
      }
      cursor += advance;
  }
}

label_set make_labels(const visage::font_t& font, const uint32_t texture_slot, const province_graph& graph) {
  label_set result;
  result.glyphs.reserve(graph.province_ids.size() * 6u + 64u);
  struct empire_source { const char* text; glm::vec3 direction; };
  constexpr empire_source empires[] = {
    {"NORTHREACH", {-0.85f, 0.35f, -0.38f}},
    {"MERIDIAN SEA", {0.05f, -0.249f, -0.967f}},
    {"EMBER COAST", {-0.73f, -0.50f, -0.46f}}};
  for (const auto& source : empires) {
    append_surface_label(result.glyphs, font, source.text, source.direction, 0.034f,
                         glm::vec4(0.94f, 0.91f, 0.80f, 1.0f), texture_slot, no_region);
  }
  result.empire_glyphs = uint32_t(result.glyphs.size());
  for (uint32_t province = 0; province < graph.province_ids.size(); ++province) {
    const std::string text = std::format("P{:04}", province + 1u);
    const float unit_width = std::max(float(font.text_width(1.0, text)), 0.001f);
    const float fitted_height = std::min(0.0080f, graph.label_clearance[province] * 1.45f / unit_width);
    append_surface_label(result.glyphs, font, text,
                         graph.label_directions[province], fitted_height,
                         glm::vec4(0.95f, 0.94f, 0.88f, 0.96f), texture_slot,
                         graph.province_ids[province]);
  }
  result.province_glyphs = uint32_t(result.glyphs.size()) - result.empire_glyphs;
  return result;
}

void write_buffer(painter::graphics_base& base, const std::string_view name, const void* data, const size_t bytes) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) utils::error{}("PF10 buffer '{}' is absent", name);
  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}("PF10 cannot write '{}': capacity {}, requested {}", name, frame.sub.size, bytes);
  }
  if (bytes != 0) std::memcpy(static_cast<uint8_t*>(frame.mapped) + frame.sub.offset, data, bytes);
}

void write_overlay(painter::graphics_base& base, const playground::visage_overlay& overlay) {
  write_buffer(base, "ui_vertices", overlay.vertices().data(), overlay.vertices().size());
  write_buffer(base, "ui_indices", overlay.indices().data(), overlay.indices().size());
  const auto commands = overlay.commands();
  const uint32_t slot = base.find_resource("ui_commands");
  const auto frame = base.get_current_buffer_resource_frame(slot);
  const uint32_t count = uint32_t(commands.size());
  auto* destination = static_cast<uint8_t*>(frame.mapped) + frame.sub.offset;
  std::memcpy(destination, &count, sizeof(count));
  if (!commands.empty()) std::memcpy(destination + sizeof(count), commands.data(), commands.size_bytes());
}

void bind_texture_descriptor(painter::graphics_base& base, const painter::assets_base& assets) {
  const uint32_t slot = base.find_descriptor("ui_textures");
  if (slot == painter::invalid_resource_slot) utils::error{}("PF10 descriptor 'ui_textures' is absent");
  auto& descriptor = base.descriptors[slot];
  const vk::ImageView fallback(assets.default_texture_view());
  std::vector<vk::DescriptorImageInfo> images(descriptor.texture_count);
  for (uint32_t i = 0; i < descriptor.texture_count; ++i) {
    vk::ImageView view;
    if (i < assets.texture_slots.size()) view = assets.texture_slots[i].view;
    images[i] = vk::DescriptorImageInfo(vk::Sampler{}, view ? view : fallback,
                                        vk::ImageLayout::eShaderReadOnlyOptimal);
  }
  std::vector<vk::WriteDescriptorSet> writes;
  for (const auto raw_set : descriptor.sets) {
    if (raw_set == VK_NULL_HANDLE) continue;
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

void dump_scene(painter::graphics_base& base, const std::string& path) {
  const uint32_t slot = base.find_resource("scene_color");
  const auto frame = base.get_current_image_resource_frame(slot);
  const auto [width, height] = base.swapchain_extent();
  const size_t bytes = size_t(width) * height * sizeof(uint16_t) * 4;

  vma::Allocator allocator(base.allocator);
  vk::BufferCreateInfo buffer_info{};
  buffer_info.usage = vk::BufferUsageFlagBits::eTransferDst;
  buffer_info.size = bytes;
  vma::AllocationCreateInfo allocation_info{};
  allocation_info.usage = vma::MemoryUsage::eGpuToCpu;
  allocation_info.flags = vma::AllocationCreateFlagBits::eMapped;
  auto [staging, allocation] = allocator.createBuffer(buffer_info, allocation_info);

  vk::Device device(base.device);
  vk::CommandBufferAllocateInfo command_info{};
  command_info.commandPool = base.command_pool;
  command_info.level = vk::CommandBufferLevel::ePrimary;
  command_info.commandBufferCount = 1;
  const auto buffers = device.allocateCommandBuffers(command_info);
  vk::CommandBuffer task(buffers[0]);
  task.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
  vk::BufferImageCopy region{};
  region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.imageSubresource.layerCount = 1;
  region.imageSubresource.baseArrayLayer = frame.sub.base_array_layer;
  region.imageExtent = vk::Extent3D{width, height, 1};
  task.copyImageToBuffer(frame.handle, vk::ImageLayout::eTransferSrcOptimal, staging, region);
  task.end();

  const vk::Fence fence = device.createFence({});
  vk::SubmitInfo submit{};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &task;
  {
    const auto lock = base.graphics.lock();
    if (vk::Queue(base.graphics.handle()).submit(1, &submit, fence) != vk::Result::eSuccess) {
      utils::error{}("PF10 could not submit the frame dump");
    }
  }
  if (device.waitForFences(fence, true, size_t(1000) * 1000 * 1000) != vk::Result::eSuccess) {
    utils::error{}("PF10 frame dump timed out");
  }

  const auto info = allocator.getAllocationInfo(allocation);
  const auto* halfs = static_cast<const uint16_t*>(info.pMappedData);
  std::string ppm = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
  ppm.reserve(ppm.size() + size_t(width) * height * 3);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    for (uint32_t channel = 0; channel < 3; ++channel) {
      const float value = glm::detail::toFloat32(glm::detail::hdata(halfs[i * 4 + channel]));
      ppm.push_back(char(uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f)));
    }
  }
  if (!file_io::write(std::span<const char>(ppm.data(), ppm.size()), path, file_io::type::binary)) {
    utils::error{}("PF10 could not write the dump '{}'", path);
  }
  device.destroy(fence);
  device.free(base.command_pool, buffers);
  allocator.destroyBuffer(staging, allocation);
}

glm::vec4 border_palette(const uint32_t index) {
  constexpr glm::vec4 colours[] = {{0.035f, 0.045f, 0.055f, 1.0f}, {0.96f, 0.76f, 0.20f, 1.0f},
                                    {0.88f, 0.91f, 0.96f, 1.0f}, {0.55f, 0.16f, 0.13f, 1.0f}};
  return colours[index % std::size(colours)];
}

} // namespace

int run_viewer(const viewer_options& options) {
  pending_width = options.width;
  pending_height = options.height;

  input::init input_runtime(&error_callback);
  input::events::init();
  painter::load_dispatcher1();

  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "PF10 functional planet";
  app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  app_info.pEngineName = "devils_engine";
  app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
  app_info.apiVersion = VK_API_VERSION_1_0;

  const auto extensions = instance_extensions(options.validation);
  vk::InstanceCreateInfo instance_info{};
  instance_info.pApplicationInfo = &app_info;
  instance_info.enabledExtensionCount = uint32_t(extensions.size());
  instance_info.ppEnabledExtensionNames = extensions.data();
  if (options.validation) {
    if (!painter::check_validation_layer_support(painter::default_validation_layers)) {
      utils::error{}("PF10 requested validation, but the layers are unavailable");
    }
    instance_info.enabledLayerCount = uint32_t(painter::default_validation_layers.size());
    instance_info.ppEnabledLayerNames = painter::default_validation_layers.data();
  }

  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);
  const VkDebugUtilsMessengerEXT messenger = options.validation ? painter::create_debug_messenger(instance) : VK_NULL_HANDLE;
  GLFWwindow* window = input::create_window(options.width, options.height, "PF10 — functional planet");
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (input::create_window_surface(instance, window, nullptr, &surface) != uint32_t(vk::Result::eSuccess)) {
    utils::error{}("PF10 could not create a Vulkan surface");
  }

  painter::system_info system(instance);
  system.check_devices_surface_capability(surface);
  const auto physical = system.choose_physical_device();
  const auto queue_plan = painter::make_device_queue_plan(physical);
  painter::device_maker device_maker(instance);
  device_maker.beginDevice(physical.handle);
  for (uint32_t i = 0; i < queue_plan.request_count; ++i) {
    device_maker.createQueue(queue_plan.requests[i].family, queue_plan.requests[i].count);
  }
  device_maker.features(vk::PhysicalDevice(physical.handle).getFeatures());
  device_maker.setExtensions(painter::default_device_extensions);
  const VkDevice device = device_maker.create({}, "pf10.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();

    const std::string resource_root = std::string(PF10_RESOURCE_ROOT) + "/";
    const std::string cache_path = utils::cache_folder() + "pf10_functional_planet.pipeline_cache";
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("pf10_planet");
    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0) {
      utils::error{}("PF10 could not commit the render graph from '{}'", resource_root);
    }
    base.set_surface(surface, options.width, options.height);
    base.resize_viewport(options.width, options.height);
    base.populate_constant_default_values();
    const uint32_t graph = base.find_render_graph("pf10_planet");
    if (graph == painter::invalid_resource_slot) utils::error{}("PF10 render graph was not found");
    base.change_render_graph(graph);

    painter::assets_base assets(device, physical.handle);
    assets.create_fence();
    assets.create_allocator(instance);
    assets.create_command_buffer(queues.transfer, queues.graphics);
    assets.set_graphics_base(&base);
    assets.create_default_texture();

    const std::string common_resources = std::string(PLAYGROUND_COMMON_RESOURCE_ROOT) + "/";
    playground::visage_overlay overlay(
      common_resources + "fonts/crimson.roman.ttf", common_resources + "ui/lab_overlay.lua",
      playground::overlay_description{
        "PF10 functional planet", "displaced sphere + 3-5k navigable provinces + stable planet-local ids",
        "WASD rotate | wheel zoom | LMB select | R auto-rotate | B border colour | P political | O objects | Esc quit"});
    const auto atlas = overlay.font_atlas();
    const auto font_texture = assets.register_texture_storage("playground.crimson_roman");
    assets.create_texture_storage(font_texture,
                                  painter::texture_create_info{{atlas.width, atlas.height, 1}, VK_FORMAT_R8G8B8A8_UNORM});
    assets.populate_texture_storage(font_texture, atlas.bytes);
    assets.mark_ready_texture_slot(font_texture);
    overlay.set_font_texture(font_texture);
    bind_texture_descriptor(base, assets);

    const auto landmarks = make_landmarks(24);
    constexpr uint32_t political_atlas_side = 512u;
    const auto surface_vertices = bake_surface_vertices(options.mesh_side);
    auto politics = bake_political_atlas(political_atlas_side);
    write_buffer(base, "surface_vertices", surface_vertices.data(), surface_vertices.size() * sizeof(glm::vec4));
    write_buffer(base, "political_atlas", politics.texels.data(), politics.texels.size() * sizeof(political_texel));
    politics.texels.clear();
    politics.texels.shrink_to_fit();
    const auto labels = make_labels(overlay.font_metrics(), font_texture, politics.graph);
    write_buffer(base, "label_glyphs", labels.glyphs.data(), labels.glyphs.size() * sizeof(decal_glyph));

    input::events::clear_bindings();
    bind_key("rotate_up", "key_w");
    bind_key("rotate_down", "key_s");
    bind_key("rotate_left", "key_a");
    bind_key("rotate_right", "key_d");
    bind_key("toggle_rotation", "key_r");
    bind_key("border_colour", "key_b");
    bind_key("toggle_political", "key_p");
    bind_key("toggle_objects", "key_o");
    escape_key = input::glfw_key_from_canonical("escape");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    input::set_window_callback(window, &scroll_callback);
    input::set_window_callback(window, &mouse_callback);

    float yaw = options.fixed_rotation ? 2.20f : 0.0f;
    float pitch = options.fixed_rotation ? -0.24f : -0.16f;
    float camera_distance = options.camera_distance;
    bool auto_rotate = !options.fixed_rotation;
    bool political = true;
    bool show_objects = true;
    uint32_t selected = no_region;
    uint32_t hovered = no_region;
    uint32_t palette = 0;
    bool rotation_latch = false;
    bool border_latch = false;
    bool political_latch = false;
    bool objects_latch = false;

    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;
    // PF10 is a throughput laboratory. Presentation already prefers MAILBOX and falls back to IMMEDIATE;
    // an additional producer deadline would hide the exact performance this slice is meant to measure.
    playground::frame_pacer pacer(0u);
    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;
    painter::gpu_timestamp_profiler gpu_profiler(base);
    context.set_gpu_profiler(&gpu_profiler);
    uint32_t drawn_frames = 0;
    std::vector<std::string> detail;

    while (!input::should_close(window)) {
      input::poll_events();
      const auto now = std::chrono::steady_clock::now();
      const float real_dt = std::clamp(std::chrono::duration<float>(now - previous_time).count(), 0.0f, 0.1f);
      const float dt = options.fixed_rotation ? 1.0f / 60.0f : real_dt;
      previous_time = now;
      input::events::update(size_t(real_dt * 1.0e6f));

      if (resize_pending) {
        vk::Device(device).waitIdle();
        base.resize_viewport(pending_width, pending_height);
        resize_pending = false;
      }
      if (scroll_accumulator != 0.0) {
        camera_distance = std::clamp(camera_distance * float(std::pow(0.88, scroll_accumulator)), 1.16f, 4.5f);
        scroll_accumulator = 0.0;
      }

      const auto toggle = [](const bool key, bool& latch, bool& value) {
        if (key && !latch) value = !value;
        latch = key;
      };
      toggle(input::events::is_pressed("toggle_rotation"), rotation_latch, auto_rotate);
      toggle(input::events::is_pressed("toggle_political"), political_latch, political);
      toggle(input::events::is_pressed("toggle_objects"), objects_latch, show_objects);
      const bool border_key = input::events::is_pressed("border_colour");
      if (border_key && !border_latch) ++palette;
      border_latch = border_key;

      if (!options.fixed_rotation) {
        if (auto_rotate) yaw += dt * 0.095f;
        yaw += dt * 0.85f * (float(input::events::is_pressed("rotate_right")) -
                             float(input::events::is_pressed("rotate_left")));
        pitch = std::clamp(pitch + dt * 0.75f * (float(input::events::is_pressed("rotate_up")) -
                                                 float(input::events::is_pressed("rotate_down"))),
                           -1.25f, 1.25f);
      }

      const glm::mat4 planet_to_world = glm::rotate(glm::mat4(1.0f), yaw, glm::normalize(glm::vec3(0.12f, 1.0f, 0.08f))) *
                                        glm::rotate(glm::mat4(1.0f), pitch, glm::vec3(1.0f, 0.0f, 0.0f));
      const glm::mat4 world_to_planet = glm::transpose(planet_to_world);
      const glm::vec3 eye{0.0f, 0.06f, camera_distance};
      const glm::vec3 target{0.0f};
      const glm::vec3 forward = glm::normalize(target - eye);
      const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
      const glm::vec3 up = glm::cross(right, forward);
      const float aspect = float(std::max(pending_width, 1u)) / float(std::max(pending_height, 1u));
      const float fov = glm::radians(49.0f);

      const auto [mouse_x, mouse_y] = input::cursor_pos(window);
      const float ndc_x = float(mouse_x / double(std::max(pending_width, 1u)) * 2.0 - 1.0);
      const float ndc_y = float(mouse_y / double(std::max(pending_height, 1u)) * 2.0 - 1.0);
      const float tangent = std::tan(fov * 0.5f);
      const glm::vec3 world_ray = glm::normalize(forward + right * (ndc_x * tangent * aspect) - up * (ndc_y * tangent));
      const glm::vec3 local_origin = glm::vec3(world_to_planet * glm::vec4(eye, 1.0f));
      const glm::vec3 local_ray = glm::normalize(glm::vec3(world_to_planet * glm::vec4(world_ray, 0.0f)));
      const auto hit = intersect_surface(local_origin, local_ray);
      hovered = hit.hit ? hit.region.id : no_region;
      if (click_pending) {
        selected = hovered;
        click_pending = false;
      }

      detail.clear();
      detail.push_back(std::format("materialized playable provinces: {}  water regions: {}  polar regions: {}",
                                   politics.graph.province_ids.size(), politics.water_regions, politics.polar_regions));
      detail.push_back(std::format("adjacency: {} nodes  {} edges  {} land components  mean degree {:.2f}",
                                   politics.graph.province_ids.size(), politics.graph.undirected_edges,
                                   politics.graph.connected_components,
                                   politics.graph.province_ids.empty() ? 0.0 :
                                     double(politics.graph.neighbours.size()) / double(politics.graph.province_ids.size())));
      detail.push_back(std::format("mesh: 6 x {} x {} cells = {} procedural triangles", options.mesh_side,
                                   options.mesh_side, uint64_t(options.mesh_side) * options.mesh_side * 12ull));
      if (hit.hit) {
        detail.push_back(std::format("hover: {} id 0x{:08x}  height {:+.4f} R", region_kind_name(hit.region.kind),
                                     hit.region.id, hit.height));
      } else detail.emplace_back("hover: space");
      detail.push_back(selected == no_region ? "selected: none" : std::format("selected id: 0x{:08x}", selected));
      detail.push_back(std::format("border palette {}  political {}  object anchors {}  distance {:.2f} R",
                                   palette % 4u, political ? "on" : "off", show_objects ? landmarks.size() : 0,
                                   camera_distance));
      detail.push_back(camera_distance < 1.72f ?
                         std::format("surface decals: province LOD ({} glyph volumes)", labels.province_glyphs) :
                         std::format("surface decals: empire LOD ({} glyph volumes)", labels.empire_glyphs));
      if (gpu_profiler.has_results()) {
        detail.push_back(std::format("GPU {:.3f} ms ({:.0f} fps equivalent)", gpu_profiler.frame_milliseconds(),
                                     1000.0 / std::max(gpu_profiler.frame_milliseconds(), 0.001)));
      }
      overlay.set_detail_lines(detail);

      if (!base.can_draw()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }
      base.prepare_frame();
      const auto view = glm::lookAtRH(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
      const auto projection = playground::infinite_reverse_z_projection(fov, aspect, 0.05f);
      camera_block camera{};
      camera.view_projection = projection * view;
      camera.planet_to_world = planet_to_world;
      camera.world_to_planet = world_to_planet;
      camera.camera_position = glm::vec4(eye, 1.0f);
      camera.light_direction = glm::vec4(glm::normalize(glm::vec3(-0.45f, -0.72f, -0.34f)), 0.0f);
      camera.border_colour = border_palette(palette);
      camera.params = glm::uvec4(selected, hovered, options.mesh_side,
                                 (political ? 1u : 0u) | (show_objects ? 2u : 0u));
      camera.viewport_near = glm::vec4(float(pending_width), float(pending_height), 0.05f,
                                       float(political_atlas_side));
      camera.inverse_view_projection = glm::inverse(camera.view_projection);
      write_buffer(base, "camera_buffer", &camera, sizeof(camera));
      write_buffer(base, "landmarks", landmarks.data(), landmarks.size() * sizeof(landmark));

      // One triangle strip per cube-face row. Instance boundaries restart the strip for free, retaining the
      // exact 2*side*side triangles while invoking ~3x fewer vertices than the old repeated triangle list.
      const VkDrawIndirectCommand planet_command{2u * (options.mesh_side + 1u), 6u * options.mesh_side, 0, 0};
      const VkDrawIndirectCommand marker_command{12, show_objects ? uint32_t(landmarks.size()) : 0u, 0, 0};
      const bool province_label_lod = camera_distance < 1.72f;
      const VkDrawIndirectCommand label_command{
        6u,
        political ? (province_label_lod ? labels.province_glyphs : labels.empire_glyphs) : 0u,
        0u,
        province_label_lod ? labels.empire_glyphs : 0u};
      base.write_constant_data(base.find_constant("planet_draw"), planet_command);
      base.write_constant_data(base.find_constant("marker_draw"), marker_command);
      base.write_constant_data(base.find_constant("label_draw"), label_command);
      base.update_event();

      const uint64_t delta_us = uint64_t(std::max(real_dt, 1.0e-6f) * 1.0e6f);
      const uint64_t stamp_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count());
      overlay.update(delta_us, stamp_us);
      write_overlay(base, overlay);
      context.prepare();
      context.draw();
      base.submit_frame();
      pacer.wait();

      ++drawn_frames;
      if (options.frames != 0 && drawn_frames >= options.frames) break;
    }

    vk::Device(device).waitIdle();
    if (!options.dump_path.empty()) {
      dump_scene(base, options.dump_path);
      utils::info("PF10 viewer: frame saved to '{}'", options.dump_path);
    }
    if (gpu_profiler.has_results()) {
      utils::info("PF10 GPU frame {:.3f} ms ({:.1f} fps equivalent)", gpu_profiler.frame_milliseconds(),
                  1000.0 / std::max(gpu_profiler.frame_milliseconds(), 0.001));
      for (const auto& pass : gpu_profiler.passes()) utils::info("  {}: {:.3f} ms", pass.name, pass.milliseconds);
    }
    base.dump_cache_on_disk(cache_path);
  }

  vk::Instance(instance).destroy(surface);
  input::destroy(window);
  vk::Device(device).destroy();
  painter::destroy_debug_messenger(instance, messenger);
  vk::Instance(instance).destroy();
  return 0;
}

} // namespace devils_engine::pf10
