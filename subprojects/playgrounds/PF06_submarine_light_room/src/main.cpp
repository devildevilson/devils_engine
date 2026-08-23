#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glm/common.hpp>
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
constexpr uint32_t blackout_mode = 0;
constexpr uint32_t exploration_mode = 1;
constexpr uint32_t safe_mode = 2;

uint32_t pending_width = initial_width;
uint32_t pending_height = initial_height;
bool resize_pending = false;
uint32_t lighting_mode = exploration_mode;
bool flashlight_enabled = false;
int32_t escape_key = -1;
int32_t lighting_key = -1;
int32_t flashlight_key = -1;

void error_callback(const int error, const char* message) noexcept {
  utils::warn("PF06 input error {}: {}", error, message);
}

void key_callback(GLFWwindow* window, const int key, const int scancode, const int action, const int) noexcept {
  input::events::update_key(scancode, action);
  if (key == escape_key && action == 1) input::set_should_close(window, true);
  if (key == lighting_key && action == 1) lighting_mode = (lighting_mode + 1u) % 3u;
  if (key == flashlight_key && action == 1) flashlight_enabled = !flashlight_enabled;
}

void framebuffer_callback(GLFWwindow*, const int width, const int height) noexcept {
  pending_width = width > 0 ? uint32_t(width) : 0u;
  pending_height = height > 0 ? uint32_t(height) : 0u;
  resize_pending = pending_width != 0 && pending_height != 0;
}

void bind_key(const std::string_view event, const std::string_view canonical) {
  const auto [key, scancode] = input::key_from_canonical(canonical);
  if (key < 0 || scancode < 0) utils::error{}("PF06 could not resolve input key '{}'", canonical);
  input::events::set_key(event, scancode, key);
}

std::string_view mode_name(const uint32_t mode) noexcept {
  if (mode == blackout_mode) return "blackout";
  if (mode == safe_mode) return "safe";
  return "exploration";
}

struct scene_vertex {
  float px, py, pz;
  float nx, ny, nz;
};

struct scene_instance {
  glm::vec4 position_material;
  glm::vec4 half_roughness;
  glm::vec4 albedo_metallic;
};
static_assert(sizeof(scene_instance) == 48);

struct alignas(16) camera_block {
  glm::mat4 view_projection;
  glm::mat4 view;
  glm::vec4 camera_position;
  glm::vec4 viewport_near;
};
static_assert(sizeof(camera_block) == 160);

struct alignas(16) lighting_block {
  glm::vec4 state;
  glm::vec4 presentation;
  glm::vec4 weak_position_radius;
  glm::vec4 weak_color_energy;
  glm::vec4 safe_position_radius;
  glm::vec4 safe_color_energy;
  glm::vec4 flashlight_direction_cos;
  glm::vec4 flashlight_color_energy;
  glm::vec4 room_irradiance;
  glm::vec4 source_reach;
  glm::vec4 medium_params;
  glm::vec4 medium_absorption;
  glm::vec4 medium_scattering;
};
static_assert(sizeof(lighting_block) == 208);

float advance_source(
  float value,
  const bool enabled,
  const float dt,
  const float enable_seconds,
  const float disable_seconds) noexcept {
  const float direction = enabled ? 1.0f : -1.0f;
  const float seconds = enabled ? enable_seconds : disable_seconds;
  return std::clamp(value + direction * dt / std::max(seconds, 0.001f), 0.0f, 1.0f);
}

float propagation_curve(const float envelope) noexcept {
  const float remaining = 1.0f - std::clamp(envelope, 0.0f, 1.0f);
  return 1.0f - remaining * remaining; // quadratic ease-out
}

void add_quad(
  std::vector<scene_vertex>& out,
  const glm::vec3& a,
  const glm::vec3& b,
  const glm::vec3& c,
  const glm::vec3& d,
  const glm::vec3& normal) {
  const auto push = [&out, normal](const glm::vec3& p) {
    out.push_back(scene_vertex{p.x, p.y, p.z, normal.x, normal.y, normal.z});
  };
  push(a); push(b); push(c);
  push(a); push(c); push(d);
}

std::vector<scene_vertex> make_unit_cube() {
  std::vector<scene_vertex> out;
  constexpr float n = -0.5f;
  constexpr float p = 0.5f;
  add_quad(out, {n,n,p}, {p,n,p}, {p,p,p}, {n,p,p}, {0,0,1});
  add_quad(out, {p,n,n}, {n,n,n}, {n,p,n}, {p,p,n}, {0,0,-1});
  add_quad(out, {p,n,p}, {p,n,n}, {p,p,n}, {p,p,p}, {1,0,0});
  add_quad(out, {n,n,n}, {n,n,p}, {n,p,p}, {n,p,n}, {-1,0,0});
  add_quad(out, {n,p,p}, {p,p,p}, {p,p,n}, {n,p,n}, {0,1,0});
  add_quad(out, {n,n,n}, {p,n,n}, {p,n,p}, {n,n,p}, {0,-1,0});
  return out;
}

scene_instance object(
  const glm::vec3 position,
  const glm::vec3 half,
  const glm::vec3 albedo,
  const float roughness,
  const float material = 0.0f,
  const float metallic = 0.0f) noexcept {
  return scene_instance{
    glm::vec4(position, material),
    glm::vec4(half, roughness),
    glm::vec4(albedo, metallic)};
}

std::vector<scene_instance> make_room_instances() {
  std::vector<scene_instance> out;
  const glm::vec3 hull{0.145f, 0.125f, 0.105f};
  const glm::vec3 corridor{0.090f, 0.085f, 0.075f};
  const glm::vec3 metal{0.185f, 0.165f, 0.140f};

  out.push_back(object({0,-1.55f,-0.5f}, {5.1f,0.10f,4.6f}, hull, 0.72f));
  out.push_back(object({0, 3.05f,-0.5f}, {5.1f,0.10f,4.6f}, hull, 0.78f));
  out.push_back(object({-5.05f,0.75f,-0.5f}, {0.10f,2.3f,4.6f}, hull, 0.64f));
  out.push_back(object({ 5.05f,0.75f,-0.5f}, {0.10f,2.3f,4.6f}, hull, 0.58f));
  out.push_back(object({0,0.75f,4.05f}, {5.1f,2.3f,0.10f}, hull, 0.68f));

  // Back wall is split around a narrow opening, so the corridor can read as a literal portal into darkness.
  out.push_back(object({-2.95f,0.75f,-5.05f}, {2.15f,2.3f,0.10f}, hull, 0.70f));
  out.push_back(object({ 2.95f,0.75f,-5.05f}, {2.15f,2.3f,0.10f}, hull, 0.70f));
  out.push_back(object({0,2.35f,-5.05f}, {0.80f,0.70f,0.10f}, hull, 0.70f));

  out.push_back(object({0,-1.50f,-8.0f}, {1.45f,0.10f,3.0f}, corridor, 0.88f));
  out.push_back(object({0, 2.35f,-8.0f}, {1.45f,0.10f,3.0f}, corridor, 0.88f));
  out.push_back(object({-1.45f,0.42f,-8.0f}, {0.10f,1.92f,3.0f}, corridor, 0.86f));
  out.push_back(object({ 1.45f,0.42f,-8.0f}, {0.10f,1.92f,3.0f}, corridor, 0.86f));
  out.push_back(object({0,0.42f,-11.0f}, {1.45f,1.92f,0.10f}, corridor, 0.92f));

  out.push_back(object({-2.0f,-0.70f,-1.4f}, {0.72f,0.75f,0.72f}, metal, 0.38f, 1.0f, 0.35f));
  out.push_back(object({ 1.7f,-0.95f,-2.6f}, {1.10f,0.50f,0.60f}, {0.11f,0.14f,0.15f}, 0.48f, 1.0f, 0.25f));
  out.push_back(object({ 0.3f,-1.10f, 0.2f}, {0.45f,0.45f,0.45f}, {0.15f,0.12f,0.10f}, 0.62f, 1.0f));
  out.push_back(object({-3.9f,0.35f,-2.0f}, {0.10f,0.42f,0.85f}, {0.02f,0.14f,0.22f}, 0.18f, 2.0f));
  out.push_back(object({ 0.0f,2.83f,-1.4f}, {1.05f,0.08f,0.26f}, {0.35f,0.24f,0.12f}, 0.12f, 3.0f));
  return out;
}

void write_current_buffer(
  painter::graphics_base& base,
  const std::string_view name,
  const void* data,
  const size_t bytes) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) utils::error{}("PF06 buffer '{}' is absent", name);
  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}("PF06 cannot write '{}' (capacity {}, requested {})", name, frame.sub.size, bytes);
  }
  if (bytes != 0) std::memcpy(static_cast<uint8_t*>(frame.mapped) + frame.sub.offset, data, bytes);
}

void write_overlay_buffers(painter::graphics_base& base, const playground::visage_overlay& overlay) {
  const auto vertices = overlay.vertices();
  const auto indices = overlay.indices();
  write_current_buffer(base, "ui_vertices", vertices.data(), vertices.size());
  write_current_buffer(base, "ui_indices", indices.data(), indices.size());
  const auto commands = overlay.commands();
  const uint32_t slot = base.find_resource("ui_commands");
  const auto frame = base.get_current_buffer_resource_frame(slot);
  const uint32_t count = uint32_t(commands.size());
  const size_t bytes = sizeof(count) + commands.size_bytes();
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}("PF06 cannot write UI commands (capacity {}, requested {})", frame.sub.size, bytes);
  }
  auto* destination = static_cast<uint8_t*>(frame.mapped) + frame.sub.offset;
  std::memcpy(destination, &count, sizeof(count));
  if (!commands.empty()) std::memcpy(destination + sizeof(count), commands.data(), commands.size_bytes());
}

void bind_texture_descriptor(
  painter::graphics_base& base,
  const painter::assets_base& assets,
  const std::string_view descriptor_name) {
  const uint32_t slot = base.find_descriptor(descriptor_name);
  if (slot == painter::invalid_resource_slot) utils::error{}("PF06 descriptor '{}' is absent", descriptor_name);
  auto& descriptor = base.descriptors[slot];
  const vk::ImageView fallback(assets.default_texture_view());
  std::vector<vk::DescriptorImageInfo> images(descriptor.texture_count);
  for (uint32_t i = 0; i < descriptor.texture_count; ++i) {
    vk::ImageView view;
    if (i < assets.texture_slots.size()) view = assets.texture_slots[i].view;
    images[i] = vk::DescriptorImageInfo(
      vk::Sampler{}, view ? view : fallback, vk::ImageLayout::eShaderReadOnlyOptimal);
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

void write_pair(
  painter::graphics_base& base,
  const uint32_t pair,
  const std::span<const scene_instance> instances,
  const uint32_t vertex_count) {
  for (uint32_t offset = 0; offset < base.frames_in_flight(); ++offset) {
    const auto instance_frame = base.get_current_instance_resource_frame(pair, offset);
    const auto indirect_frame = base.get_current_indirect_resource_frame(pair, offset);
    if (instances.size_bytes() > instance_frame.sub.size) utils::error{}("PF06 scene instance budget exceeded");
    std::memcpy(
      static_cast<uint8_t*>(instance_frame.mapped) + instance_frame.sub.offset,
      instances.data(), instances.size_bytes());
    const VkDrawIndirectCommand command{vertex_count, uint32_t(instances.size()), 0, 0};
    std::memcpy(
      static_cast<uint8_t*>(indirect_frame.mapped) + indirect_frame.sub.offset,
      &command, sizeof(command));
  }
}

std::vector<const char*> instance_extensions(const bool validation) {
  uint32_t count = 0;
  const char** required = input::get_required_instance_extensions(&count);
  std::vector<const char*> extensions(required, required + count);
  if (validation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  return extensions;
}

float half_to_float(const uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1fu;
  const uint32_t mantissa = h & 0x3ffu;
  if (exponent == 0) {
    if (mantissa == 0) return std::bit_cast<float>(sign);
    uint32_t shift = 0;
    uint32_t normalized = mantissa;
    while ((normalized & 0x400u) == 0) { normalized <<= 1; shift += 1; }
    normalized &= 0x3ffu;
    return std::bit_cast<float>(sign | ((127 - 15 - shift + 1) << 23) | (normalized << 13));
  }
  if (exponent == 0x1fu) return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));
  return std::bit_cast<float>(sign | ((exponent + 127 - 15) << 23) | (mantissa << 13));
}

void dump_final_image(painter::graphics_base& base, const std::string& path) {
  const uint32_t slot = base.find_resource("final_color");
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
  vk::Device dev(base.device);
  vk::CommandBufferAllocateInfo command_info{};
  command_info.commandPool = base.command_pool;
  command_info.level = vk::CommandBufferLevel::ePrimary;
  command_info.commandBufferCount = 1;
  const auto buffers = dev.allocateCommandBuffers(command_info);
  vk::CommandBuffer task(buffers[0]);
  task.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
  vk::BufferImageCopy region{};
  region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.imageSubresource.layerCount = 1;
  region.imageSubresource.baseArrayLayer = frame.sub.base_array_layer;
  region.imageExtent = vk::Extent3D{width, height, 1};
  task.copyImageToBuffer(frame.handle, vk::ImageLayout::eTransferSrcOptimal, staging, region);
  task.end();
  const vk::Fence fence = dev.createFence({});
  vk::SubmitInfo submit{};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &task;
  {
    const auto lock = base.graphics.lock();
    if (vk::Queue(base.graphics.handle()).submit(1, &submit, fence) != vk::Result::eSuccess) {
      utils::error{}("PF06 could not submit image dump");
    }
  }
  if (dev.waitForFences(fence, true, size_t(1000) * 1000 * 1000) != vk::Result::eSuccess) {
    utils::error{}("PF06 image dump timed out");
  }
  const auto info = allocator.getAllocationInfo(allocation);
  const auto* halfs = static_cast<const uint16_t*>(info.pMappedData);
  std::string ppm = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
  ppm.reserve(ppm.size() + size_t(width) * height * 3);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    for (uint32_t channel = 0; channel < 3; ++channel) {
      const float value = half_to_float(halfs[i * 4 + channel]);
      ppm.push_back(char(uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f)));
    }
  }
  if (!file_io::write(std::span<const char>(ppm.data(), ppm.size()), path, file_io::type::binary)) {
    utils::error{}("PF06 could not write dump '{}'", path);
  }
  dev.destroy(fence);
  dev.freeCommandBuffers(base.command_pool, task);
  allocator.destroyBuffer(staging, allocation);
  utils::info("PF06 dumped {}x{} final frame to '{}'", width, height, path);
}

} // namespace

int main(int argc, char** argv) {
  bool validation = false;
  bool uncapped = false;
  bool fixed_camera = false;
  bool fixed_step = false;
  uint32_t frame_limit = 0;
  float exposure = 1.55f;
  float pattern_strength = 1.55f;
  float pattern_speed = 0.075f;
  float bounce_override = -1.0f;
  float medium_density = 0.14f;
  float medium_anisotropy = 0.42f;
  float god_ray_strength = 0.90f;
  float mote_strength = 0.75f;
  bool medium_enabled = true;
  uint32_t flashlight_on_frame = UINT32_MAX;
  uint32_t flashlight_off_frame = UINT32_MAX;
  uint32_t exploration_on_frame = UINT32_MAX;
  std::string dump_path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option(argv[i]);
    validation = validation || option == "--validation";
    uncapped = uncapped || option == "--uncapped";
    fixed_camera = fixed_camera || option == "--fixed-camera";
    fixed_step = fixed_step || option == "--fixed-step";
    if (option == "--no-medium") medium_enabled = false;
    flashlight_enabled = flashlight_enabled || option == "--flashlight";
    constexpr std::string_view lighting_prefix = "--lighting=";
    constexpr std::string_view exposure_prefix = "--exposure=";
    constexpr std::string_view pattern_prefix = "--pattern=";
    constexpr std::string_view pattern_speed_prefix = "--pattern-speed=";
    constexpr std::string_view bounce_prefix = "--bounce=";
    constexpr std::string_view medium_prefix = "--medium-density=";
    constexpr std::string_view anisotropy_prefix = "--medium-anisotropy=";
    constexpr std::string_view god_rays_prefix = "--god-rays=";
    constexpr std::string_view motes_prefix = "--motes=";
    constexpr std::string_view flashlight_on_prefix = "--flashlight-on-frame=";
    constexpr std::string_view flashlight_off_prefix = "--flashlight-off-frame=";
    constexpr std::string_view exploration_on_prefix = "--exploration-on-frame=";
    constexpr std::string_view frames_prefix = "--frames=";
    constexpr std::string_view dump_prefix = "--dump=";
    if (option.starts_with(lighting_prefix)) {
      const auto value = option.substr(lighting_prefix.size());
      if (value == "blackout") lighting_mode = blackout_mode;
      else if (value == "exploration") lighting_mode = exploration_mode;
      else if (value == "safe") lighting_mode = safe_mode;
      else utils::error{}("PF06 unknown lighting mode '{}'; expected blackout, exploration or safe", value);
    }
    if (option.starts_with(exposure_prefix)) exposure = std::max(std::stof(std::string(option.substr(exposure_prefix.size()))), 0.0f);
    if (option.starts_with(pattern_prefix)) pattern_strength = std::clamp(std::stof(std::string(option.substr(pattern_prefix.size()))), 0.0f, 2.0f);
    if (option.starts_with(pattern_speed_prefix)) pattern_speed = std::max(std::stof(std::string(option.substr(pattern_speed_prefix.size()))), 0.0f);
    if (option.starts_with(bounce_prefix)) bounce_override = std::clamp(std::stof(std::string(option.substr(bounce_prefix.size()))), 0.0f, 1.0f);
    if (option.starts_with(medium_prefix)) medium_density = std::clamp(std::stof(std::string(option.substr(medium_prefix.size()))), 0.0f, 0.8f);
    if (option.starts_with(anisotropy_prefix)) medium_anisotropy = std::clamp(std::stof(std::string(option.substr(anisotropy_prefix.size()))), -0.85f, 0.85f);
    if (option.starts_with(god_rays_prefix)) god_ray_strength = std::clamp(std::stof(std::string(option.substr(god_rays_prefix.size()))), 0.0f, 3.0f);
    if (option.starts_with(motes_prefix)) mote_strength = std::clamp(std::stof(std::string(option.substr(motes_prefix.size()))), 0.0f, 2.0f);
    if (option.starts_with(flashlight_on_prefix)) flashlight_on_frame = uint32_t(std::stoul(std::string(option.substr(flashlight_on_prefix.size()))));
    if (option.starts_with(flashlight_off_prefix)) flashlight_off_frame = uint32_t(std::stoul(std::string(option.substr(flashlight_off_prefix.size()))));
    if (option.starts_with(exploration_on_prefix)) exploration_on_frame = uint32_t(std::stoul(std::string(option.substr(exploration_on_prefix.size()))));
    if (option.starts_with(frames_prefix)) frame_limit = uint32_t(std::stoul(std::string(option.substr(frames_prefix.size()))));
    if (option.starts_with(dump_prefix)) dump_path = std::string(option.substr(dump_prefix.size()));
  }
  if (!dump_path.empty() && frame_limit == 0) frame_limit = 1;
  utils::info(
    "PF06 config: lighting={}, flashlight={}, medium={}, fixed_step={}",
    mode_name(lighting_mode), flashlight_enabled, medium_enabled, fixed_step);

  input::init input_runtime(&error_callback);
  input::events::init();
  painter::load_dispatcher1();
  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "PF06 Submarine Light Room";
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
      utils::error{}("PF06 requested unavailable validation layers");
    }
    instance_info.enabledLayerCount = uint32_t(painter::default_validation_layers.size());
    instance_info.ppEnabledLayerNames = painter::default_validation_layers.data();
  }
  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);
  const VkDebugUtilsMessengerEXT messenger = validation ? painter::create_debug_messenger(instance) : VK_NULL_HANDLE;
  GLFWwindow* window = input::create_window(initial_width, initial_height, "PF06 — submarine light room");
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (input::create_window_surface(instance, window, nullptr, &surface) != uint32_t(vk::Result::eSuccess)) {
    utils::error{}("PF06 could not create Vulkan surface");
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
  const VkDevice device = device_maker.create({}, "pf06.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();
    const std::string resource_root = std::string(PF06_RESOURCE_ROOT) + "/";
    const std::string cache_path = utils::cache_folder() + "pf06_submarine_room.pipeline_cache";
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("pf06_submarine_room");
    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0) utils::error{}("PF06 could not commit render graph");
    base.set_surface(surface, initial_width, initial_height);
    base.resize_viewport(initial_width, initial_height);
    base.populate_constant_default_values();
    base.change_render_graph(base.find_render_graph("pf06_submarine_room"));

    painter::assets_base assets(device, physical.handle);
    assets.create_fence();
    assets.create_allocator(instance);
    assets.create_command_buffer(queues.transfer, queues.graphics);
    assets.set_graphics_base(&base);
    assets.create_default_texture();

    const auto cube = make_unit_cube();
    const auto cube_mesh = assets.register_buffer_storage("pf06.unit_cube");
    assets.create_buffer_storage(
      cube_mesh, painter::buffer_create_info{"scene_geometry", uint32_t(cube.size()), 0});
    assets.populate_buffer_storage(
      cube_mesh,
      std::span(reinterpret_cast<const uint8_t*>(cube.data()), cube.size() * sizeof(cube[0])),
      std::span<const uint8_t>{});
    assets.mark_ready_buffer_slot(cube_mesh);

    const auto scene_group = base.find_draw_group("scene_draw_group");
    const uint32_t scene_pair = base.register_pair(scene_group, cube_mesh, 32);
    const auto scene_instances = make_room_instances();
    write_pair(base, scene_pair, scene_instances, uint32_t(cube.size()));

    const std::string common_resources = std::string(PLAYGROUND_COMMON_RESOURCE_ROOT) + "/";
    playground::visage_overlay overlay(
      common_resources + "fonts/crimson.roman.ttf",
      common_resources + "ui/lab_overlay.lua",
      playground::overlay_description{
        "PF06 — Submarine light room",
        "Project-look slice: direct light, room irradiance and low-light pattern",
        "WASD/QE + mouse · L lighting state · F flashlight · Esc"});
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
    lighting_key = input::glfw_key_from_canonical("key_l");
    flashlight_key = input::glfw_key_from_canonical("key_f");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    if (fixed_camera) input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_NORMAL);
    else {
      input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_DISABLED);
      input::set_raw_mouse_motion(window);
    }

    playground::free_camera camera;
    camera.position = {0.0f, 0.15f, 3.25f};
    auto [mouse_x, mouse_y] = input::cursor_pos(window);
    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;
    playground::frame_pacer frame_pacer(uncapped ? 0u : 60u);
    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;
    uint32_t frames_total = 0;
    double measured_seconds = 0.0;
    uint32_t measured_frames = 0;
    // Startup begins in its requested steady state; later L/F changes visibly propagate through the room.
    float weak_source = lighting_mode == blackout_mode ? 0.0f : 1.0f;
    float safe_source = lighting_mode == safe_mode ? 1.0f : 0.0f;
    float flashlight_source = flashlight_enabled ? 1.0f : 0.0f;

    while (!input::should_close(window)) {
      input::poll_events();
      const auto now = std::chrono::steady_clock::now();
      const float dt = std::clamp(std::chrono::duration<float>(now - previous_time).count(), 0.0f, 0.1f);
      previous_time = now;
      if (uncapped && frames_total >= 60) {
        measured_seconds += double(dt);
        measured_frames += 1;
      }
      input::events::update(size_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<float>(dt)).count()));
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
      if (!fixed_camera) {
        motion.look_delta = {float(next_mouse_x - mouse_x), float(next_mouse_y - mouse_y)};
        camera.update(motion, dt);
      }
      mouse_x = next_mouse_x;
      mouse_y = next_mouse_y;
      if (!base.can_draw()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }

      base.prepare_frame();
      if (frames_total == flashlight_on_frame) flashlight_enabled = true;
      if (frames_total == flashlight_off_frame) flashlight_enabled = false;
      if (frames_total == exploration_on_frame) lighting_mode = exploration_mode;
      const float aspect = float(std::max(pending_width, 1u)) / float(std::max(pending_height, 1u));
      const glm::mat4 view = camera.view();
      const glm::mat4 projection = playground::infinite_reverse_z_projection(glm::radians(65.0f), aspect, near_plane);
      const camera_block camera_data{
        projection * view,
        view,
        glm::vec4(camera.position, 1.0f),
        glm::vec4(float(pending_width), float(pending_height), near_plane, 0.0f)};
      write_current_buffer(base, "camera_buffer", &camera_data, sizeof(camera_data));

      const float simulation_dt = fixed_step ? 1.0f / 60.0f : dt;
      weak_source = advance_source(
        weak_source, lighting_mode != blackout_mode, simulation_dt, 1.40f, 0.24f);
      safe_source = advance_source(
        safe_source, lighting_mode == safe_mode, simulation_dt, 1.10f, 0.18f);
      flashlight_source = advance_source(
        flashlight_source, flashlight_enabled, simulation_dt, 1.80f, 0.14f);
      const float weak_gain = glm::smoothstep(0.0f, 0.22f, weak_source);
      const float safe_gain = glm::smoothstep(0.0f, 0.22f, safe_source);
      const float flashlight_gain = glm::smoothstep(0.0f, 0.18f, flashlight_source);
      const float weak_weight = (lighting_mode == safe_mode ? 0.28f : 0.72f) * weak_gain;
      const float safe_weight = safe_gain;
      const float room_source = glm::smoothstep(
        0.0f, 1.0f, std::max({weak_source, safe_source, flashlight_source}));
      const float weak_reach = 5.2f * propagation_curve(weak_source);
      const float safe_reach = 7.0f * propagation_curve(safe_source);
      const float flashlight_reach = 12.0f * propagation_curve(flashlight_source);
      const glm::vec3 weak_gi{0.060f, 0.074f, 0.066f};
      const glm::vec3 safe_gi{0.120f, 0.080f, 0.048f};
      const glm::vec3 flashlight_gi{0.072f, 0.078f, 0.070f};
      const float gi_weight = weak_source + safe_source + flashlight_source;
      const glm::vec3 room_gi = gi_weight > 0.0001f
        ? (weak_gi * weak_source + safe_gi * safe_source + flashlight_gi * flashlight_source) / gi_weight
        : weak_gi;
      float bounce = lighting_mode == safe_mode ? 0.30f : 0.23f;
      if (bounce_override >= 0.0f) bounce = bounce_override;
      const float time_seconds = fixed_step
        ? float(frames_total) / 60.0f
        : std::chrono::duration<float>(now - start_time).count();
      const lighting_block lighting{
        glm::vec4(weak_weight, safe_weight, bounce, flashlight_gain),
        glm::vec4(exposure, time_seconds, pattern_strength, pattern_speed),
        glm::vec4(-3.55f, 0.45f, -2.0f, 5.2f),
        glm::vec4(0.28f, 0.46f, 0.50f, 7.5f),
        glm::vec4(0.0f, 2.55f, -1.4f, 7.0f),
        glm::vec4(1.00f, 0.64f, 0.34f, 13.0f),
        glm::vec4(camera.forward(), 0.90f),
        glm::vec4(0.72f, 0.82f, 0.84f, 12.0f),
        glm::vec4(room_gi, 1.0f),
        glm::vec4(weak_reach, safe_reach, flashlight_reach, room_source),
        glm::vec4(
          medium_enabled ? medium_density : 0.0f,
          medium_anisotropy,
          god_ray_strength,
          mote_strength),
        glm::vec4(0.82f, 0.34f, 0.18f, 0.0f),
        glm::vec4(0.14f, 0.27f, 0.30f, 0.82f)};
      write_current_buffer(base, "lighting_buffer", &lighting, sizeof(lighting));

      const uint64_t frame_delta_us = uint64_t(std::max(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<float>(dt)).count(),
        int64_t{1}));
      const uint64_t timestamp_us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count());
      const std::array<std::string, 10> details{
        std::format("Lighting: {} · weak {:.2f} · safe {:.2f}", mode_name(lighting_mode), weak_weight, safe_weight),
        std::format("Room irradiance: fixed {:.2f} · source presence {:.2f}", bounce, room_source),
        std::format("Point reach: weak {:.1f} m · safe {:.1f} m", weak_reach, safe_reach),
        std::format("Flashlight: {} · ease-out front {:.1f}/12.0 m", flashlight_enabled ? "ON" : "OFF", flashlight_reach),
        std::format("Pattern: organic peripheral flow · strength {:.2f} · speed {:.3f}", pattern_strength, pattern_speed),
        std::format("Medium: {} · density {:.3f} · g {:.2f}", medium_enabled ? "ON" : "OFF", medium_density, medium_anisotropy),
        std::format("Volume: god rays {:.2f} · motes {:.2f}", god_ray_strength, mote_strength),
        std::format("Exposure: fixed {:.2f}; blackout is never auto-lifted", exposure),
        std::format("Scene: {} axis-aligned instances · per-pixel lighting", scene_instances.size()),
        "Half-resolution medium: 20 samples; window shadow map remains next"};
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
          dump_final_image(base, dump_path);
        }
        utils::info("PF06 reached requested {} frames", frame_limit);
        if (measured_frames != 0) {
          const double average_ms = measured_seconds * 1000.0 / double(measured_frames);
          utils::info(
            "PF06 uncapped steady frame: {:.3f} ms ({:.1f} FPS, {} samples)",
            average_ms, 1000.0 / average_ms, measured_frames);
        }
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
