#include "viewer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <glm/detail/type_half.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
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

struct surface_label_curve {
  glm::vec3 start{0.0f, 0.0f, 1.0f};
  glm::vec3 control{0.0f, 0.0f, 1.0f};
  glm::vec3 end{0.0f, 0.0f, 1.0f};
};

glm::vec3 evaluate_curve(const surface_label_curve& curve, const float t) {
  const float u = 1.0f - t;
  return glm::normalize(curve.start * (u * u) + curve.control * (2.0f * u * t) + curve.end * (t * t));
}

float curve_length(const surface_label_curve& curve) {
  float result = 0.0f;
  glm::vec3 previous = evaluate_curve(curve, 0.0f);
  for (uint32_t i = 1u; i <= 16u; ++i) {
    const glm::vec3 current = evaluate_curve(curve, float(i) / 16.0f);
    result += std::acos(std::clamp(glm::dot(previous, current), -1.0f, 1.0f));
    previous = current;
  }
  return result;
}

surface_label_curve fit_group_curve(const std::vector<glm::vec3>& points) {
  if (points.empty()) return {};
  glm::vec3 sum{0.0f};
  glm::mat3 moment{0.0f};
  for (const glm::vec3 raw : points) {
    const glm::vec3 direction = glm::normalize(raw);
    sum += direction;
    moment += glm::mat3(direction * direction.x, direction * direction.y, direction * direction.z);
  }
  const glm::vec3 centre = glm::normalize(sum);
  const glm::mat3 projection = glm::mat3(1.0f) - glm::mat3(centre * centre.x, centre * centre.y, centre * centre.z);
  const glm::mat3 covariance = projection * (moment / float(points.size())) * projection;
  glm::vec3 east = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), centre);
  if (glm::dot(east, east) < 1.0e-6f) east = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), centre);
  east = glm::normalize(east);
  glm::vec3 axis = east;
  for (uint32_t iteration = 0u; iteration < 8u; ++iteration) {
    const glm::vec3 next = projection * (covariance * axis);
    if (glm::dot(next, next) < 1.0e-12f) break;
    axis = glm::normalize(next);
  }
  if (glm::dot(axis, east) < 0.0f) axis = -axis;

  surface_label_curve result{points.front(), points.front(), points.front()};
  float centre_score = -2.0f;
  float minimum = std::numeric_limits<float>::max();
  float maximum = -std::numeric_limits<float>::max();
  for (const glm::vec3 raw : points) {
    const glm::vec3 direction = glm::normalize(raw);
    const float score = glm::dot(direction, centre);
    if (score > centre_score) {
      centre_score = score;
      result.control = direction;
    }
    const float along = std::atan2(glm::dot(direction, axis), glm::dot(direction, centre));
    if (along < minimum) { minimum = along; result.start = direction; }
    if (along > maximum) { maximum = along; result.end = direction; }
  }
  constexpr float endpoint_inset = 0.58f;
  result.start = glm::normalize(glm::mix(result.control, result.start, endpoint_inset));
  result.end = glm::normalize(glm::mix(result.control, result.end, endpoint_inset));
  return result;
}

void append_surface_label(std::vector<decal_glyph>& result, const visage::font_t& font,
                          const std::string_view text, const surface_label_curve& curve,
                          const float maximum_font_height, const float transverse_clearance,
                          const glm::vec4 fill, const uint32_t texture_slot, const uint32_t owner_region) {
  const float unit_width = std::max(float(font.text_width(1.0, text)), 0.001f);
  const float available_length = std::max(curve_length(curve) * 0.78f, 0.001f);
  const float font_height = std::min({maximum_font_height, available_length / unit_width,
                                      transverse_clearance * 1.25f});
  float unit_cursor = 0.0f;
  for (const unsigned char character : text) {
      const auto* glyph = font.find_glyph(uint32_t(character));
      if (glyph == nullptr) continue;
      if (glyph->w > 0 && glyph->h > 0) {
        const float bottom = float(glyph->pb) * font_height - font_height * 0.32f;
        const float width = float(glyph->pr - glyph->pl) * font_height;
        const float height = float(glyph->pt - glyph->pb) * font_height;
        const float glyph_centre_unit = unit_cursor + (float(glyph->pl) +
                                         float(glyph->pr - glyph->pl) * 0.5f);
        const float t = std::clamp(0.11f + 0.78f * glyph_centre_unit / unit_width, 0.0f, 1.0f);
        const glm::vec3 curve_direction = evaluate_curve(curve, t);
        const glm::vec3 before = evaluate_curve(curve, std::max(t - 0.004f, 0.0f));
        const glm::vec3 after = evaluate_curve(curve, std::min(t + 0.004f, 1.0f));
        const glm::vec3 curve_right = glm::normalize((after - before) - curve_direction *
                                                     glm::dot(after - before, curve_direction));
        const glm::vec3 curve_up = glm::normalize(glm::cross(curve_direction, curve_right));
        const glm::vec3 glyph_direction = glm::normalize(curve_direction +
                                                          curve_up * (bottom + height * 0.5f));
        const glm::vec3 glyph_right = glm::normalize(curve_right - glyph_direction *
                                                                   glm::dot(curve_right, glyph_direction));
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
      unit_cursor += float(glyph->advance);
  }
}

label_set make_labels(const visage::font_t& font, const uint32_t texture_slot, const province_graph& graph,
                      const glm::vec3 presentation_direction) {
  label_set result;
  result.glyphs.reserve(graph.province_ids.size() * 6u + 64u);
  struct empire_source { const char* text; glm::vec3 seed; };
  const glm::vec3 front = glm::normalize(presentation_direction);
  const auto state_seed = [&](const uint32_t state) {
    return state < graph.state_centres.size() ? graph.state_centres[state] : front;
  };
  const std::array<empire_source, 3> empires{{
    {"NORTHREACH", state_seed(0u)}, {"MERIDIAN SEA", state_seed(1u)}, {"EMBER COAST", state_seed(2u)}}};
  std::array<std::vector<std::pair<float, glm::vec3>>, 3> state_candidates;
  for (uint32_t province = 0u; province < graph.centre_directions.size(); ++province) {
    const uint32_t owner = province < graph.state_ids.size() ? graph.state_ids[province] : no_region;
    if (owner >= state_candidates.size()) continue;
    const float owner_score = glm::dot(graph.centre_directions[province], empires[owner].seed);
    state_candidates[owner].emplace_back(owner_score, graph.centre_directions[province]);
  }
  for (uint32_t empire = 0u; empire < std::size(empires); ++empire) {
    auto& nearest = state_candidates[empire];
    std::ranges::sort(nearest, [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<glm::vec3> member_provinces;
    const size_t member_count = std::min(nearest.size(), size_t(96));
    member_provinces.reserve(member_count);
    for (size_t member = 0; member < member_count; ++member) member_provinces.push_back(nearest[member].second);
    append_surface_label(result.glyphs, font, empires[empire].text, fit_group_curve(member_provinces),
                         0.034f, 0.034f, glm::vec4(0.94f, 0.91f, 0.80f, 1.0f), texture_slot, no_region);
  }
  result.empire_glyphs = uint32_t(result.glyphs.size());
  for (uint32_t province = 0; province < graph.province_ids.size(); ++province) {
    const std::string text = std::format("P{:04}", province + 1u);
    append_surface_label(result.glyphs, font, text,
                         {graph.label_curve_starts[province], graph.label_directions[province],
                          graph.label_curve_ends[province]},
                         0.0080f, graph.label_clearance[province],
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
        "WASD orbit camera | wheel zoom | LMB select | R auto-rotate | B border colour | P political | O objects | Esc quit"});
    const auto atlas = overlay.font_atlas();
    const auto font_texture = assets.register_texture_storage("playground.crimson_roman");
    assets.create_texture_storage(font_texture,
                                  painter::texture_create_info{{atlas.width, atlas.height, 1}, VK_FORMAT_R8G8B8A8_UNORM});
    assets.populate_texture_storage(font_texture, atlas.bytes);
    assets.mark_ready_texture_slot(font_texture);
    overlay.set_font_texture(font_texture);
    bind_texture_descriptor(base, assets);

    const auto landmarks = make_landmarks(24);
    constexpr uint32_t political_atlas_side = 1024u;
    const auto surface_vertices = bake_surface_vertices(options.mesh_side);
    auto politics = bake_political_atlas(political_atlas_side);
    const auto hydrology = make_hydrology_features();
    const glm::vec3 presentation_source = hydrology.empty() ? glm::vec3(0.0f, 0.0f, 1.0f) :
                                                              glm::normalize(glm::vec3(hydrology.front().a_direction_height));
    const glm::vec3 presentation_target{0.0f, 0.0f, 1.0f};
    const float presentation_angle = std::acos(std::clamp(glm::dot(presentation_source, presentation_target), -1.0f, 1.0f));
    const glm::vec3 presentation_cross = glm::cross(presentation_source, presentation_target);
    const glm::mat4 presentation_rotation = glm::dot(presentation_cross, presentation_cross) > 1.0e-8f ?
      glm::rotate(glm::mat4(1.0f), presentation_angle, glm::normalize(presentation_cross)) : glm::mat4(1.0f);
    assign_fixture_states(politics.graph, presentation_source, 3u);
    const auto packed_politics = pack_political_atlas(politics);
    const auto state_borders = make_state_borders(politics);
    if (packed_politics.texels.empty() || packed_politics.cells.empty()) {
      utils::error{}("PF10 could not compact the political atlas into R16 indices");
    }
    write_buffer(base, "surface_vertices", surface_vertices.data(), surface_vertices.size() * sizeof(glm::vec4));
    write_buffer(base, "political_atlas", packed_politics.texels.data(), packed_politics.texels.size() * sizeof(uint32_t));
    write_buffer(base, "political_region_table", packed_politics.cells.data(),
                 packed_politics.cells.size() * sizeof(political_cell_record));
    write_buffer(base, "hydrology_features", hydrology.data(),
                 hydrology.size() * sizeof(hydrology_feature));
    write_buffer(base, "state_borders", state_borders.data(),
                 state_borders.size() * sizeof(state_border_segment));
    politics.texels.clear();
    politics.texels.shrink_to_fit();
    const auto labels = make_labels(overlay.font_metrics(), font_texture, politics.graph, presentation_source);
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

    float planet_yaw = 0.0f;
    glm::vec3 camera_direction = glm::normalize(glm::vec3(0.0f, 0.06f, 1.0f));
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
        if (auto_rotate) planet_yaw += dt * 0.095f;
        const float horizontal = float(input::events::is_pressed("rotate_right")) -
                                 float(input::events::is_pressed("rotate_left"));
        const float vertical = float(input::events::is_pressed("rotate_up")) -
                               float(input::events::is_pressed("rotate_down"));
        if (horizontal != 0.0f || vertical != 0.0f) {
          // The helper derives east/north from the radial normal and clamps latitude before +Y lookAt can
          // become singular at either pole.
          camera_direction = orbit_camera_direction(camera_direction, horizontal, vertical, dt * 0.82f);
        }
      }

      const glm::mat4 planet_to_world = glm::rotate(glm::mat4(1.0f), planet_yaw,
                                                     glm::normalize(glm::vec3(0.12f, 1.0f, 0.08f))) *
                                        presentation_rotation;
      const glm::mat4 world_to_planet = glm::transpose(planet_to_world);
      const glm::vec3 eye = camera_direction * camera_distance;
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
      constexpr uint32_t surface_patch_side = 16u;
      auto visible_patches = visible_surface_patches(options.mesh_side, surface_patch_side, local_origin);
      const auto refined_patches = refined_surface_patches(options.mesh_side, surface_patch_side, local_origin);
      std::erase_if(visible_patches, [&](const surface_patch& patch) {
        return std::ranges::any_of(refined_patches, [&](const surface_patch& refined) {
          return patch.face == refined.face && patch.x == refined.x && patch.y == refined.y;
        });
      });
      hovered = hit.hit ? hit.region.id : no_region;
      if (click_pending) {
        selected = hovered;
        click_pending = false;
      }

      detail.clear();
      detail.push_back(std::format("playable provinces: {}  water: {}  mountain chains: {}  polar: {}",
                                   politics.graph.province_ids.size(), politics.water_regions,
                                   politics.mountain_regions, politics.polar_regions));
      detail.push_back(std::format("adjacency: {} nodes  {} edges  {} land components  mean degree {:.2f}",
                                   politics.graph.province_ids.size(), politics.graph.undirected_edges,
                                   politics.graph.connected_components,
                                   politics.graph.province_ids.empty() ? 0.0 :
                                     double(politics.graph.neighbours.size()) / double(politics.graph.province_ids.size())));
      detail.push_back(std::format("politics: {} exact cells, atlas only gates near-border refinement",
                                   packed_politics.cells.size()));
      detail.push_back(std::format("states: {} connected fixtures, {} exact patterned border segments",
                                   politics.graph.state_count, state_borders.size()));
      detail.push_back(std::format("feature layer: {} tapered river/lake primitives", hydrology.size()));
      detail.push_back(std::format("mesh: 6 x {} x {} cells = {} procedural triangles", options.mesh_side,
                                   options.mesh_side, uint64_t(options.mesh_side) * options.mesh_side * 12ull));
      detail.push_back(std::format("surface residency: {} / {} visible {}x{} patches", visible_patches.size(),
                                   6u * (options.mesh_side / surface_patch_side) *
                                     (options.mesh_side / surface_patch_side),
                                   surface_patch_side, surface_patch_side));
      detail.push_back(std::format("inspection LOD: {} central patches at effective {} cells/face",
                                   refined_patches.size(), options.mesh_side * 4u));
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
                                 (political ? 1u : 0u) | (show_objects ? 2u : 0u) |
                                 (options.border_debug << 8u));
      camera.viewport_near = glm::vec4(float(pending_width), float(pending_height), 0.05f,
                                       float(political_atlas_side));
      camera.inverse_view_projection = glm::inverse(camera.view_projection);
      write_buffer(base, "camera_buffer", &camera, sizeof(camera));
      write_buffer(base, "landmarks", landmarks.data(), landmarks.size() * sizeof(landmark));
      write_buffer(base, "surface_patches", visible_patches.data(),
                   visible_patches.size() * sizeof(surface_patch));
      write_buffer(base, "refined_surface_patches", refined_patches.data(),
                   refined_patches.size() * sizeof(surface_patch));

      // One strip per visible patch row.  A conservative CPU horizon test rejects the hidden sphere before
      // vertex shading; this is the uniform-resolution precursor to the same patch list carrying LOD levels.
      const VkDrawIndirectCommand planet_command{2u * (surface_patch_side + 1u),
                                                  uint32_t(visible_patches.size()) * surface_patch_side, 0, 0};
      constexpr uint32_t refined_patch_side = surface_patch_side * 4u;
      const VkDrawIndirectCommand refined_planet_command{2u * (refined_patch_side + 1u),
                                                          uint32_t(refined_patches.size()) * refined_patch_side,
                                                          0u, 0u};
      const VkDrawIndirectCommand marker_command{12, show_objects ? uint32_t(landmarks.size()) : 0u, 0, 0};
      const bool province_label_lod = camera_distance < 1.72f;
      const VkDrawIndirectCommand label_command{
        6u,
        political ? (province_label_lod ? labels.province_glyphs : labels.empire_glyphs) : 0u,
        0u,
        province_label_lod ? labels.empire_glyphs : 0u};
      const VkDrawIndirectCommand hydrology_command{6u, options.show_hydrology ? uint32_t(hydrology.size()) : 0u, 0u, 0u};
      const VkDrawIndirectCommand state_border_command{
        6u, political && options.show_state_borders ? uint32_t(state_borders.size()) : 0u, 0u, 0u};
      base.write_constant_data(base.find_constant("planet_draw"), planet_command);
      base.write_constant_data(base.find_constant("refined_planet_draw"), refined_planet_command);
      base.write_constant_data(base.find_constant("marker_draw"), marker_command);
      base.write_constant_data(base.find_constant("label_draw"), label_command);
      base.write_constant_data(base.find_constant("hydrology_draw"), hydrology_command);
      base.write_constant_data(base.find_constant("state_border_draw"), state_border_command);
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
