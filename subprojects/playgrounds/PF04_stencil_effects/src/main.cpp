#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
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
bool local_effect = true;
bool spatial_window = true;
bool face_fixture = true;
bool wallhack_effect = true;
bool outline_overlay = false;
constexpr std::array<uint32_t, 3> selection_bits{0x01u, 0x08u, 0x80u};
uint32_t selection_channel = 0;
bool selection_compare_enabled = true;
bool selection_write_enabled = true;
bool selection_state_dirty = true;
int32_t escape_key = -1;
int32_t debug_key = -1;
int32_t local_effect_key = -1;
int32_t window_key = -1;
int32_t selection_channel_key = -1;
int32_t selection_compare_key = -1;
int32_t selection_write_key = -1;
int32_t face_fixture_key = -1;
int32_t wallhack_key = -1;
int32_t outline_overlay_key = -1;

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
  if (key == local_effect_key && action == 1) {
    local_effect = !local_effect;
  }
  if (key == window_key && action == 1) {
    spatial_window = !spatial_window;
  }
  if (key == selection_channel_key && action == 1) {
    selection_channel = (selection_channel + 1) % uint32_t(selection_bits.size());
    selection_state_dirty = true;
  }
  if (key == selection_compare_key && action == 1) {
    selection_compare_enabled = !selection_compare_enabled;
    selection_state_dirty = true;
  }
  if (key == selection_write_key && action == 1) {
    selection_write_enabled = !selection_write_enabled;
    selection_state_dirty = true;
  }
  if (key == face_fixture_key && action == 1) {
    face_fixture = !face_fixture;
  }
  if (key == wallhack_key && action == 1) {
    wallhack_effect = !wallhack_effect;
  }
  if (key == outline_overlay_key && action == 1) {
    outline_overlay = !outline_overlay;
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

struct mask_vertex {
  float px, py, pz;
};

struct face_vertex {
  float x, y;
};

struct spatial_window_link {
  glm::mat4 source_world;
  glm::mat4 destination_world;
  glm::vec2 half_extent;
  float clip_offset;
};

glm::mat4 make_plane_frame(
  const glm::vec3& center,
  const glm::vec3& right,
  const glm::vec3& up,
  const glm::vec3& normal) noexcept {
  return glm::mat4{
    glm::vec4(glm::normalize(right), 0.0f),
    glm::vec4(glm::normalize(up), 0.0f),
    glm::vec4(glm::normalize(normal), 0.0f),
    glm::vec4(center, 1.0f)};
}

glm::vec4 forward_clip_plane(const glm::mat4& plane_world, const float offset) noexcept {
  const glm::vec3 normal = glm::normalize(glm::vec3(plane_world[2]));
  const glm::vec3 center = glm::vec3(plane_world[3]);
  // gl_ClipDistance keeps the non-negative half-space. Destination +Z points away from virtual camera.
  return glm::vec4(normal, -glm::dot(normal, center) + std::max(offset, 0.0f));
}

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
  glm::vec4 effect_params;
  glm::vec4 clip_plane;
};
static_assert(sizeof(camera_block) == 208);

struct dynamic_stencil_state {
  uint32_t reference;
  uint32_t compare_mask;
  uint32_t write_mask;
};
static_assert(sizeof(dynamic_stencil_state) == sizeof(uint32_t) * 3);

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

template <typename Instance, size_t Count>
void write_pair(
  painter::graphics_base& base,
  const uint32_t pair,
  const std::array<Instance, Count>& instances,
  const uint32_t vertex_count) {
  for (uint32_t offset = 0; offset < base.frames_in_flight(); ++offset) {
    const auto instance_frame = base.get_current_instance_resource_frame(pair, offset);
    const auto indirect_frame = base.get_current_indirect_resource_frame(pair, offset);
    const size_t instance_bytes = sizeof(Instance) * instances.size();
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

float half_to_float(const uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1fu;
  const uint32_t mantissa = h & 0x3ffu;
  if (exponent == 0) {
    if (mantissa == 0) {
      return std::bit_cast<float>(sign);
    }
    uint32_t shift = 0;
    uint32_t normalized = mantissa;
    while ((normalized & 0x400u) == 0) {
      normalized <<= 1;
      shift += 1;
    }
    normalized &= 0x3ffu;
    return std::bit_cast<float>(sign | ((127 - 15 - shift + 1) << 23) | (normalized << 13));
  }
  if (exponent == 0x1fu) {
    return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));
  }
  return std::bit_cast<float>(sign | ((exponent + 127 - 15) << 23) | (mantissa << 13));
}

// Frame-exact PPM dump copied from PF03's proven path. scene_color is left in transfer_src by the present
// pass, so the copy observes the rendered image itself rather than an asynchronously timed desktop capture.
void dump_scene_image(painter::graphics_base& base, const std::string& path) {
  const uint32_t slot = base.find_resource("scene_color");
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF04 resource 'scene_color' is absent from the configured graph");
  }
  const auto frame = base.get_current_image_resource_frame(slot);
  const auto [width, height] = base.swapchain_extent();
  const size_t bytes = size_t(width) * size_t(height) * sizeof(uint16_t) * 4;

  vma::Allocator allocator(base.allocator);
  vk::BufferCreateInfo buffer_info{};
  buffer_info.usage = vk::BufferUsageFlagBits::eTransferDst;
  buffer_info.size = bytes;
  vma::AllocationCreateInfo allocation_info{};
  allocation_info.usage = vma::MemoryUsage::eGpuToCpu;
  allocation_info.flags = vma::AllocationCreateFlagBits::eMapped;
  auto [staging, allocation] = allocator.createBuffer(buffer_info, allocation_info);

  vk::Device dev(base.device);
  vk::CommandBufferAllocateInfo command_info{};
  command_info.commandPool = base.command_pool;
  command_info.level = vk::CommandBufferLevel::ePrimary;
  command_info.commandBufferCount = 1;
  const auto buffers = dev.allocateCommandBuffers(command_info);
  vk::CommandBuffer task(buffers[0]);
  task.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

  vk::ImageMemoryBarrier before{};
  before.srcAccessMask = vk::AccessFlagBits::eTransferRead;
  before.dstAccessMask = vk::AccessFlagBits::eTransferRead;
  before.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
  before.newLayout = vk::ImageLayout::eTransferSrcOptimal;
  before.image = frame.handle;
  before.subresourceRange = std::bit_cast<vk::ImageSubresourceRange>(frame.sub);
  task.pipelineBarrier(
    vk::PipelineStageFlagBits::eTransfer,
    vk::PipelineStageFlagBits::eTransfer,
    vk::DependencyFlags{}, nullptr, nullptr, before);

  vk::BufferImageCopy region{};
  region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = frame.sub.base_array_layer;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = vk::Extent3D{width, height, 1};
  task.copyImageToBuffer(frame.handle, vk::ImageLayout::eTransferSrcOptimal, staging, region);
  task.end();

  const auto fence = dev.createFence(vk::FenceCreateInfo{});
  {
    vk::SubmitInfo submit{};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &task;
    const auto queue_lock = base.graphics.lock();
    if (vk::Queue(base.graphics.handle()).submit(1, &submit, fence) != vk::Result::eSuccess) {
      utils::error{}("PF04 could not submit the image dump copy");
    }
  }
  if (dev.waitForFences(fence, true, size_t(1000) * 1000 * 1000) != vk::Result::eSuccess) {
    utils::error{}("PF04 image dump did not finish in time");
  }

  const auto info = allocator.getAllocationInfo(allocation);
  const auto* halfs = static_cast<const uint16_t*>(info.pMappedData);
  std::string ppm = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
  ppm.reserve(ppm.size() + size_t(width) * height * 3);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    for (uint32_t c = 0; c < 3; ++c) {
      const float value = half_to_float(halfs[i * 4 + c]);
      ppm.push_back(char(uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f)));
    }
  }
  if (!file_io::write(std::span<const char>(ppm.data(), ppm.size()), path, file_io::type::binary)) {
    utils::error{}("PF04 could not write the dump to '{}'", path);
  }

  dev.destroy(fence);
  dev.freeCommandBuffers(base.command_pool, task);
  allocator.destroyBuffer(staging, allocation);
  utils::info("PF04 dumped {}x{} scene frame to '{}'", width, height, path);
}

} // namespace

int main(int argc, char** argv) {
  bool validation = false;
  bool uncapped = false;
  bool fixed_camera = false;
  uint32_t frame_limit = 0;
  std::string dump_path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option(argv[i]);
    validation = validation || option == "--validation";
    uncapped = uncapped || option == "--uncapped";
    fixed_camera = fixed_camera || option == "--fixed-camera";
    stencil_debug = stencil_debug || option == "--stencil-debug";
    local_effect = local_effect && option != "--no-local-effect";
    spatial_window = spatial_window && option != "--no-window";
    face_fixture = face_fixture && option != "--no-face-fixture";
    wallhack_effect = wallhack_effect && option != "--no-wallhack";
    outline_overlay = outline_overlay || option == "--outline-overlay";
    constexpr std::string_view frames_prefix = "--frames=";
    if (option.starts_with(frames_prefix)) {
      frame_limit = uint32_t(std::stoul(std::string(option.substr(frames_prefix.size()))));
    }
    constexpr std::string_view dump_prefix = "--dump=";
    if (option.starts_with(dump_prefix)) {
      dump_path = std::string(option.substr(dump_prefix.size()));
    }
    constexpr std::string_view channel_prefix = "--selection-channel=";
    if (option.starts_with(channel_prefix)) {
      selection_channel = uint32_t(std::stoul(std::string(option.substr(channel_prefix.size()))));
    }
    constexpr std::string_view compare_prefix = "--selection-compare=";
    if (option.starts_with(compare_prefix)) {
      selection_compare_enabled = std::stoul(std::string(option.substr(compare_prefix.size()))) != 0;
    }
    constexpr std::string_view write_prefix = "--selection-write=";
    if (option.starts_with(write_prefix)) {
      selection_write_enabled = std::stoul(std::string(option.substr(write_prefix.size()))) != 0;
    }
  }
  if (selection_channel >= selection_bits.size()) {
    utils::error{}("PF04 --selection-channel={} is outside 0..{}", selection_channel, selection_bits.size() - 1);
  }
  if (!dump_path.empty() && frame_limit == 0) {
    frame_limit = 1;
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
    const uint32_t selection_stencil_slot = base.find_constant("selection_stencil_state");
    if (selection_stencil_slot == painter::invalid_resource_slot) {
      utils::error{}("PF04 constant 'selection_stencil_state' is absent");
    }
    const auto publish_selection_stencil = [&base, selection_stencil_slot]() {
      const uint32_t bit = selection_bits[selection_channel];
      const dynamic_stencil_state state{
        bit,
        selection_compare_enabled ? bit : 0u,
        selection_write_enabled ? bit : 0u};
      base.write_constant_data(selection_stencil_slot, state);
      base.update_event();
      selection_state_dirty = false;
    };
    publish_selection_stencil();

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
    // Two triangles form a world-space proxy. It never changes scene color; it writes stencil bit 0x02.
    const std::array<mask_vertex, 6> local_mask_vertices{
      mask_vertex{-1.4f, -1.1f, 0.0f}, mask_vertex{1.4f, -1.1f, 0.0f}, mask_vertex{1.4f, 1.4f, 0.0f},
      mask_vertex{-1.4f, -1.1f, 0.0f}, mask_vertex{1.4f, 1.4f, 0.0f}, mask_vertex{-1.4f, 1.4f, 0.0f}};
    const auto mask_mesh = assets.register_buffer_storage("pf04.local_mask");
    assets.create_buffer_storage(
      mask_mesh,
      painter::buffer_create_info{"local_mask_geometry", uint32_t(local_mask_vertices.size()), 0});
    assets.populate_buffer_storage(
      mask_mesh,
      std::span(
        reinterpret_cast<const uint8_t*>(local_mask_vertices.data()),
        local_mask_vertices.size() * sizeof(local_mask_vertices[0])),
      std::span<const uint8_t>{});
    assets.mark_ready_buffer_slot(mask_mesh);
    // The two authored plane frames are the portal contract. The aperture mesh below reads its extent and
    // source transform from this same object, so rendering cannot silently drift from camera mapping.
    const spatial_window_link window_link{
      make_plane_frame(
        glm::vec3{1.65f, 0.35f, -0.25f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        glm::vec3{0.0f, 1.0f, 0.0f},
        glm::vec3{0.0f, 0.0f, 1.0f}),
      make_plane_frame(
        glm::vec3{1.997212f, -0.959121f, -0.600328f},
        glm::vec3{-0.681639f, 0.0f, -0.731689f},
        glm::vec3{0.195164f, 0.963771f, -0.181814f},
        glm::vec3{0.705180f, -0.266731f, -0.656944f}),
      glm::vec2{0.75f, 0.85f},
      0.01f};
    const std::array<mask_vertex, 6> window_mask_vertices{
      mask_vertex{-window_link.half_extent.x, -window_link.half_extent.y, 0.0f},
      mask_vertex{ window_link.half_extent.x, -window_link.half_extent.y, 0.0f},
      mask_vertex{ window_link.half_extent.x,  window_link.half_extent.y, 0.0f},
      mask_vertex{-window_link.half_extent.x, -window_link.half_extent.y, 0.0f},
      mask_vertex{ window_link.half_extent.x,  window_link.half_extent.y, 0.0f},
      mask_vertex{-window_link.half_extent.x,  window_link.half_extent.y, 0.0f}};
    const auto window_mesh = assets.register_buffer_storage("pf04.window_mask");
    assets.create_buffer_storage(
      window_mesh,
      painter::buffer_create_info{"window_mask_geometry", uint32_t(window_mask_vertices.size()), 0});
    assets.populate_buffer_storage(
      window_mesh,
      std::span(
        reinterpret_cast<const uint8_t*>(window_mask_vertices.data()),
        window_mask_vertices.size() * sizeof(window_mask_vertices[0])),
      std::span<const uint8_t>{});
    assets.mark_ready_buffer_slot(window_mesh);
    // Positive Vulkan viewport height reverses clip-XY winding in framebuffer coordinates.
    // Therefore the left quad is CW in clip XY -> CCW/front in the pipeline; right is the opposite.
    const std::array<face_vertex, 12> face_fixture_vertices{
      face_vertex{-0.92f, 0.55f}, face_vertex{-0.62f, 0.88f}, face_vertex{-0.62f, 0.55f},
      face_vertex{-0.92f, 0.55f}, face_vertex{-0.92f, 0.88f}, face_vertex{-0.62f, 0.88f},
      face_vertex{0.62f, 0.55f}, face_vertex{0.92f, 0.55f}, face_vertex{0.92f, 0.88f},
      face_vertex{0.62f, 0.55f}, face_vertex{0.92f, 0.88f}, face_vertex{0.62f, 0.88f}};
    const auto face_fixture_mesh = assets.register_buffer_storage("pf04.face_fixture");
    assets.create_buffer_storage(
      face_fixture_mesh,
      painter::buffer_create_info{"face_fixture_geometry", uint32_t(face_fixture_vertices.size()), 0});
    assets.populate_buffer_storage(
      face_fixture_mesh,
      std::span(
        reinterpret_cast<const uint8_t*>(face_fixture_vertices.data()),
        face_fixture_vertices.size() * sizeof(face_fixture_vertices[0])),
      std::span<const uint8_t>{});
    assets.mark_ready_buffer_slot(face_fixture_mesh);

    const uint32_t scene_group = base.find_draw_group("scene_draw_group");
    const uint32_t selected_group = base.find_draw_group("selected_draw_group");
    const uint32_t wallhack_target_group = base.find_draw_group("wallhack_target_draw_group");
    const uint32_t mask_group = base.find_draw_group("local_mask_draw_group");
    const uint32_t window_group = base.find_draw_group("window_mask_draw_group");
    const uint32_t face_fixture_group = base.find_draw_group("face_fixture_draw_group");
    if (scene_group == painter::invalid_resource_slot ||
        selected_group == painter::invalid_resource_slot ||
        wallhack_target_group == painter::invalid_resource_slot ||
        mask_group == painter::invalid_resource_slot ||
        window_group == painter::invalid_resource_slot ||
        face_fixture_group == painter::invalid_resource_slot) {
      utils::error{}("PF04 scene draw groups are absent");
    }
    const uint32_t room_pair = base.register_pair(scene_group, room_mesh, 1);
    const uint32_t props_pair = base.register_pair(scene_group, cube_mesh, 3);
    const uint32_t selected_pair = base.register_pair(selected_group, cube_mesh, 1);
    const uint32_t wallhack_target_pair = base.register_pair(wallhack_target_group, cube_mesh, 1);
    const uint32_t mask_pair = base.register_pair(mask_group, mask_mesh, 1);
    const uint32_t window_pair = base.register_pair(window_group, window_mesh, 1);
    const uint32_t face_fixture_pair = base.register_pair(face_fixture_group, face_fixture_mesh, 1);

    // Destination basis retains the useful cold-room bookmark of the earlier proof, but it is now a plane
    // rather than a camera pose.
    glm::mat4 portal_facing_flip{1.0f};
    portal_facing_flip[0][0] = -1.0f;
    portal_facing_flip[2][2] = -1.0f;
    const glm::mat4 main_to_window =
      window_link.destination_world * portal_facing_flip * glm::inverse(window_link.source_world);
    const glm::mat4 window_to_main =
      window_link.source_world * portal_facing_flip * glm::inverse(window_link.destination_world);
    const glm::vec4 window_clip_plane =
      forward_clip_plane(window_link.destination_world, window_link.clip_offset);

    const std::array<glm::vec4, 1> room_instances{glm::vec4{0.0f, 0.0f, 0.0f, 0.0f}};
    const std::array<glm::vec4, 3> prop_instances{
      // Передний cube перекрывает левую часть selection: local/overlay outline отличаются уже на fixed view.
      glm::vec4{-0.72f, -0.78f, -0.75f, 1.0f},
      glm::vec4{2.1f, -0.78f, -2.1f, 1.0f},
      glm::vec4{0.1f, 0.75f, -3.6f, 1.0f}};
    const std::array<glm::vec4, 1> selected_instances{glm::vec4{0.0f, -0.72f, -1.55f, 2.0f}};
    // Green target is partly hidden by the selected blue cube from the fixed-camera bookmark.
    const std::array<glm::vec4, 1> wallhack_target_instances{glm::vec4{0.55f, 0.05f, -3.15f, 3.0f}};
    const std::array<glm::vec4, 1> mask_instances{glm::vec4{-0.65f, 0.1f, -0.45f, 0.0f}};
    const std::array<glm::mat4, 1> window_instances{window_link.source_world};
    const std::array<glm::vec4, 1> face_fixture_instances{glm::vec4{0.0f}};
    write_pair(base, room_pair, room_instances, uint32_t(room.size()));
    write_pair(base, props_pair, prop_instances, uint32_t(cube.size()));
    write_pair(base, selected_pair, selected_instances, uint32_t(cube.size()));
    write_pair(base, wallhack_target_pair, wallhack_target_instances, uint32_t(cube.size()));
    write_pair(base, mask_pair, mask_instances, uint32_t(local_mask_vertices.size()));
    write_pair(base, window_pair, window_instances, uint32_t(window_mask_vertices.size()));
    write_pair(base, face_fixture_pair, face_fixture_instances, uint32_t(face_fixture_vertices.size()));

    const std::string common_resources = std::string(PLAYGROUND_COMMON_RESOURCE_ROOT) + "/";
    playground::visage_overlay overlay(
      common_resources + "fonts/crimson.roman.ttf",
      common_resources + "ui/lab_overlay.lua",
      playground::overlay_description{
        "PF04 — Stencil effects",
        "bits 0/1/2: base effects · bit 6: hidden target · bits 4/5: face fixture",
        "O outline mode · P window · L tint · H hidden target · V debug · R/C/X dynamic · F faces · Esc exit"});
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
    local_effect_key = input::glfw_key_from_canonical("key_l");
    window_key = input::glfw_key_from_canonical("key_p");
    selection_channel_key = input::glfw_key_from_canonical("key_r");
    selection_compare_key = input::glfw_key_from_canonical("key_c");
    selection_write_key = input::glfw_key_from_canonical("key_x");
    face_fixture_key = input::glfw_key_from_canonical("key_f");
    wallhack_key = input::glfw_key_from_canonical("key_h");
    outline_overlay_key = input::glfw_key_from_canonical("key_o");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    if (fixed_camera) {
      input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_NORMAL);
    } else {
      input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_DISABLED);
      input::set_raw_mouse_motion(window);
    }

    playground::free_camera camera;
    camera.position = {0.0f, 0.2f, 5.2f};
    auto [mouse_x, mouse_y] = input::cursor_pos(window);
    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;
    playground::frame_pacer frame_pacer(uncapped ? 0u : 60u);

    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;

    utils::info("PF04 controls: move/look, O through-wall outline, P window, L tint, H hidden target, V stencil debug, R selection channel, C compare mask, X write mask, F face fixture, Esc exit");
    utils::info("PF04 graph: selection coverage+depth -> local outline -> spatial window -> optional overlay selector + occluded target -> UI -> present");

    uint32_t frames_total = 0;
    while (!input::should_close(window)) {
      input::poll_events();
      if (selection_state_dirty) {
        publish_selection_stencil();
      }
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
      camera_data.debug_params = glm::vec4(
        stencil_debug ? 1.0f : 0.0f,
        local_effect ? 1.0f : 0.0f,
        spatial_window ? 1.0f : 0.0f,
        face_fixture ? 1.0f : 0.0f);
      camera_data.effect_params = glm::vec4(
        wallhack_effect ? 1.0f : 0.0f,
        outline_overlay ? 1.0f : 0.0f,
        0.0f,
        0.0f);
      camera_data.clip_plane = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
      write_current_buffer(base, "camera_buffer", &camera_data, sizeof(camera_data));
      camera_block window_camera_data{};
      // If X_remote = M * X_main and C_remote = M * C_main, then
      // V_remote = V_main * inverse(M). Consequently P*V_remote*X_remote equals
      // P*V_main*X_main for every point, so camera rotation cannot make the image slip under the aperture.
      window_camera_data.view = view * window_to_main;
      window_camera_data.view_projection = projection * window_camera_data.view;
      const glm::mat4 window_camera_world = main_to_window * glm::inverse(view);
      window_camera_data.camera_position = window_camera_world[3];
      window_camera_data.viewport_near = camera_data.viewport_near;
      window_camera_data.debug_params = camera_data.debug_params;
      window_camera_data.effect_params = camera_data.effect_params;
      window_camera_data.clip_plane = window_clip_plane;
      write_current_buffer(base, "window_camera_buffer", &window_camera_data, sizeof(window_camera_data));

      const uint64_t frame_delta_us = uint64_t(std::max(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<float>(dt)).count(),
        int64_t{1}));
      const uint64_t timestamp_us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count());
      const uint32_t selection_bit = selection_bits[selection_channel];
      const std::array<std::string, 7> details{
        std::format(
          "Dynamic selection: ref 0x{:02X}, read 0x{:02X}, write 0x{:02X}",
          selection_bit,
          selection_compare_enabled ? selection_bit : 0u,
          selection_write_enabled ? selection_bit : 0u),
        local_effect ? "Local tint: ON; invisible proxy writes mask 0x02" : "Local tint: OFF (press L); mask remains independent",
        wallhack_effect ? "Hidden target: ON; depth-fail writes 0x40, red=occluded only" : "Hidden target: OFF (press H)",
        outline_overlay ? "Outline: LOCAL + through-wall overlay (press O)" : "Outline: LOCAL depth-tested (press O for overlay)",
        spatial_window ? "Window: ON; explicit portal pair + destination clip" : "Window: OFF (press P)",
        face_fixture ? "Faces: ON; green=front replace 0x10, yellow=back invert 0x30" : "Faces: OFF (press F)",
        stencil_debug ? "Debug tint: ON (dynamic equal)" : "Debug tint: OFF (press V)"};
      overlay.set_detail_lines(details);
      overlay.update(frame_delta_us, timestamp_us);
      write_overlay_buffers(base, overlay);

      context.prepare();
      context.draw();
      base.submit_frame();
      frame_pacer.wait();
      frames_total += 1;
      if (frame_limit != 0 && frames_total >= frame_limit) {
        if (!dump_path.empty()) {
          vk::Device(device).waitIdle();
          dump_scene_image(base, dump_path);
        }
        utils::info("PF04 reached the requested {} frames", frame_limit);
        break;
      }
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
