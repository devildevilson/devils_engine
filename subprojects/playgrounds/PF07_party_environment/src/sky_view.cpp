#include "sky_view.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <span>
#include <thread>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "devils_engine/input/core.h"
#include "devils_engine/input/events.h"
#include "devils_engine/painter/assets_base.h"
#include "devils_engine/painter/auxiliary.h"
#include "devils_engine/painter/gpu_timing.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/makers.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/playground/frame_pacer.h"
#include "devils_engine/playground/free_camera.h"
#include "devils_engine/playground/visage_overlay.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"

namespace devils_engine::pf07 {
namespace {

constexpr float near_plane = 0.1f;

uint32_t pending_width = 1280;
uint32_t pending_height = 720;
bool resize_pending = false;
bool paused = false;
bool pause_requested = false;
double time_scale_step = 0.0;
int32_t escape_key = -1;
int32_t pause_key = -1;
int32_t faster_key = -1;
int32_t slower_key = -1;
int32_t exposure_up_key = -1;
int32_t exposure_down_key = -1;
double exposure_step = 0.0;

struct alignas(16) camera_block {
  glm::mat4 view_projection;
  glm::mat4 view;
  glm::vec4 camera_position;
  glm::vec4 viewport_near;
};
static_assert(sizeof(camera_block) == 160);

void error_callback(const int error, const char* message) noexcept {
  utils::warn("PF07 input error {}: {}", error, message);
}

void key_callback(GLFWwindow* window, const int key, const int scancode, const int action, const int) noexcept {
  input::events::update_key(scancode, action);
  if (key == escape_key && action == 1) input::set_should_close(window, true);
  if (key == pause_key && action == 1) pause_requested = true;
  if (key == faster_key && action == 1) time_scale_step += 1.0;
  if (key == slower_key && action == 1) time_scale_step -= 1.0;
  if (key == exposure_up_key && action == 1) exposure_step += 1.0;
  if (key == exposure_down_key && action == 1) exposure_step -= 1.0;
}

void mouse_button_callback(GLFWwindow*, const int button, const int action, const int) noexcept {
  input::events::update_mouse_button(button, action);
}

void framebuffer_callback(GLFWwindow*, const int width, const int height) noexcept {
  if (width <= 0 || height <= 0) return;
  pending_width = uint32_t(width);
  pending_height = uint32_t(height);
  resize_pending = true;
}

double star_separation_deg(const sky_state& state) {
  const auto& a = state.stars[0].direction;
  const auto& b = state.stars[1].direction;
  const auto cross = glm::dvec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
  const double cross_length = std::sqrt(cross.x * cross.x + cross.y * cross.y + cross.z * cross.z);
  return std::atan2(cross_length, a.x * b.x + a.y * b.y + a.z * b.z) * 180.0 / 3.14159265358979323846;
}

void bind_key(const std::string_view event, const std::string_view canonical) {
  const auto [key, scancode] = input::key_from_canonical(canonical);
  if (key < 0 || scancode < 0) utils::error{}("PF07 could not resolve input key '{}'", canonical);
  input::events::set_key(event, scancode, key);
}

void write_current_buffer(painter::graphics_base& base, const std::string_view name, const void* data,
                          const size_t bytes) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) utils::error{}("PF07 buffer '{}' is absent", name);

  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}("PF07 cannot write '{}' (capacity {}, requested {})", name, frame.sub.size, bytes);
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
    utils::error{}("PF07 cannot write UI commands (capacity {}, requested {})", frame.sub.size, bytes);
  }

  auto* destination = static_cast<uint8_t*>(frame.mapped) + frame.sub.offset;
  std::memcpy(destination, &count, sizeof(count));
  if (!commands.empty()) std::memcpy(destination + sizeof(count), commands.data(), commands.size_bytes());
}

void bind_texture_descriptor(painter::graphics_base& base, const painter::assets_base& assets,
                             const std::string_view descriptor_name) {
  const uint32_t slot = base.find_descriptor(descriptor_name);
  if (slot == painter::invalid_resource_slot) utils::error{}("PF07 descriptor '{}' is absent", descriptor_name);

  auto& descriptor = base.descriptors[slot];
  const vk::ImageView fallback(assets.default_texture_view());
  std::vector<vk::DescriptorImageInfo> images(descriptor.texture_count);
  for (uint32_t i = 0; i < descriptor.texture_count; ++i) {
    vk::ImageView view;
    if (i < assets.texture_slots.size()) view = assets.texture_slots[i].view;
    images[i] =
      vk::DescriptorImageInfo(vk::Sampler{}, view ? view : fallback, vk::ImageLayout::eShaderReadOnlyOptimal);
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
    while ((normalized & 0x400u) == 0) {
      normalized <<= 1;
      shift += 1;
    }
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
      utils::error{}("PF07 could not submit image dump");
    }
  }
  if (device.waitForFences(fence, true, size_t(1000) * 1000 * 1000) != vk::Result::eSuccess) {
    utils::error{}("PF07 image dump timed out");
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
    utils::error{}("PF07 could not write dump '{}'", path);
  }

  device.destroy(fence);
  device.freeCommandBuffers(base.command_pool, task);
  allocator.destroyBuffer(staging, allocation);
  utils::info("PF07 dumped {}x{} final frame to '{}'", width, height, path);
}

// Один кадр слишком шумный, чтобы решать, стоит ли перестраивать проход. Минимум полезен тем, что
// посторонняя нагрузка может только добавить время, а максимум показывает срывы.
struct pass_timing_accumulator {
  struct entry {
    std::string name;
    double total = 0.0;
    double minimum = std::numeric_limits<double>::max();
    double maximum = 0.0;
  };

  std::vector<entry> passes;
  double frame_total = 0.0;
  double frame_minimum = std::numeric_limits<double>::max();
  uint64_t samples = 0;

  void add(const std::span<const painter::gpu_pass_timing>& timings, const double frame_ms) {
    if (passes.empty()) {
      passes.resize(timings.size());
      for (size_t i = 0; i < timings.size(); ++i) passes[i].name = timings[i].name;
    }
    if (passes.size() != timings.size()) {
      utils::error{}("PF07 pass timing count changed mid-run: {} -> {}", passes.size(), timings.size());
    }

    for (size_t i = 0; i < timings.size(); ++i) {
      const double value = timings[i].milliseconds;
      passes[i].total += value;
      passes[i].minimum = std::min(passes[i].minimum, value);
      passes[i].maximum = std::max(passes[i].maximum, value);
    }
    frame_total += frame_ms;
    frame_minimum = std::min(frame_minimum, frame_ms);
    samples += 1;
  }

  void report() const {
    if (samples == 0) {
      utils::info("PF07 GPU timings: no samples collected");
      return;
    }

    const double count = double(samples);
    utils::info("PF07 GPU timings over {} frames (ms: average / minimum / maximum):", samples);
    for (const auto& pass : passes) {
      utils::info("  {:<24} {:6.3f} / {:6.3f} / {:6.3f}", pass.name, pass.total / count, pass.minimum, pass.maximum);
    }
    utils::info("  {:<24} {:6.3f} / {:6.3f}", "TOTAL ON GPU", frame_total / count, frame_minimum);
  }
};

} // namespace

int run_sky_view(const celestial_system& system, const view_options& options) {
  pending_width = options.width;
  pending_height = options.height;
  paused = options.paused;

  input::init input_runtime(&error_callback);
  input::events::init();
  painter::load_dispatcher1();

  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "PF07 party environment";
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
      utils::error{}("PF07 requested unavailable validation layers");
    }
    instance_info.enabledLayerCount = uint32_t(painter::default_validation_layers.size());
    instance_info.ppEnabledLayerNames = painter::default_validation_layers.data();
  }

  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);
  const VkDebugUtilsMessengerEXT messenger =
    options.validation ? painter::create_debug_messenger(instance) : VK_NULL_HANDLE;

  GLFWwindow* window = input::create_window(options.width, options.height, "PF07 — party environment");
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (input::create_window_surface(instance, window, nullptr, &surface) != uint32_t(vk::Result::eSuccess)) {
    utils::error{}("PF07 could not create Vulkan surface");
  }

  painter::system_info system_devices(instance);
  system_devices.check_devices_surface_capability(surface);
  const auto physical = system_devices.choose_physical_device();
  painter::system_info::print_choosed_device(physical.handle);
  const auto queue_plan = painter::make_device_queue_plan(physical);
  painter::device_maker device_maker(instance);
  device_maker.beginDevice(physical.handle);
  for (uint32_t i = 0; i < queue_plan.request_count; ++i) {
    device_maker.createQueue(queue_plan.requests[i].family, queue_plan.requests[i].count);
  }
  device_maker.features(vk::PhysicalDevice(physical.handle).getFeatures());
  device_maker.setExtensions(painter::default_device_extensions);
  const VkDevice device = device_maker.create({}, "pf07.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();

    const std::string resource_root = std::string(PF07_RESOURCE_ROOT) + "/";
    const std::string cache_path = utils::cache_folder() + "pf07_party_sky.pipeline_cache";
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("pf07_party_sky");
    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0) utils::error{}("PF07 could not commit render graph");
    base.set_surface(surface, options.width, options.height);
    base.resize_viewport(options.width, options.height);
    base.populate_constant_default_values();
    base.change_render_graph(base.find_render_graph("pf07_party_sky"));

    painter::assets_base assets(device, physical.handle);
    assets.create_fence();
    assets.create_allocator(instance);
    assets.create_command_buffer(queues.transfer, queues.graphics);
    assets.set_graphics_base(&base);
    assets.create_default_texture();

    const std::string common_resources = std::string(PLAYGROUND_COMMON_RESOURCE_ROOT) + "/";
    playground::visage_overlay overlay(
      common_resources + "fonts/crimson.roman.ttf", resource_root + "ui/pf07_controls.lua",
      playground::overlay_description{
        "PF07 — Party environment", "Slice 2: physical sky over a binary system, march + transmittance table",
        "WASD/QE look · Space pause · [ ] time speed · - = exposure"});
    const auto atlas = overlay.font_atlas();
    const auto font_texture = assets.register_texture_storage("playground.crimson_roman");
    assets.create_texture_storage(font_texture,
                                  painter::texture_create_info{{atlas.width, atlas.height, 1},
                                                               VK_FORMAT_R8G8B8A8_UNORM});
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
    pause_key = input::glfw_key_from_canonical("space");
    slower_key = input::glfw_key_from_canonical("left_bracket");
    faster_key = input::glfw_key_from_canonical("right_bracket");
    exposure_down_key = input::glfw_key_from_canonical("minus");
    exposure_up_key = input::glfw_key_from_canonical("equal");
    input::set_window_callback(window, &key_callback);
    input::set_window_callback(window, &mouse_button_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_DISABLED);
    input::set_raw_mouse_motion(window);

    playground::free_camera camera;
    // Камера смотрит чуть выше горизонта: срез 2 показывает небо, а не землю.
    camera.position = {0.0f, 0.0f, 0.0f};
    if (options.fixed_look) {
      // Азимут отсчитывается от севера через восток, мир имеет X на восток и Z на юг, поэтому
      // направление взгляда равно (sin A, tan h, -cos A) до нормировки, а yaw камеры — его угол в
      // плоскости XZ.
      const double azimuth = options.look_azimuth_deg * 3.14159265358979323846 / 180.0;
      camera.yaw = float(std::atan2(-std::cos(azimuth), std::sin(azimuth)));
      camera.pitch = float(options.look_altitude_deg * 3.14159265358979323846 / 180.0);
    }
    auto [mouse_x, mouse_y] = input::cursor_pos(window);
    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;
    playground::frame_pacer frame_pacer(options.uncapped ? 0u : 60u);
    painter::gpu_timestamp_profiler gpu_profiler(base);
    pass_timing_accumulator gpu_timings;
    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;
    context.set_gpu_profiler(&gpu_profiler);
    if (!gpu_profiler.available()) {
      utils::warn("PF07 GPU timestamps are unavailable on this device; per-pass cost will stay unmeasured");
    }

    double game_time_days = options.start_time_days;
    double time_scale = options.time_scale;
    double exposure = options.output.exposure;
    uint32_t frames_total = 0;

    while (!input::should_close(window)) {
      input::poll_events();
      const auto now = std::chrono::steady_clock::now();
      const float dt = std::clamp(std::chrono::duration<float>(now - previous_time).count(), 0.0f, 0.1f);
      previous_time = now;
      input::events::update(
        size_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<float>(dt)).count()));

      if (resize_pending) {
        vk::Device(device).waitIdle();
        base.resize_viewport(pending_width, pending_height);
        resize_pending = false;
      }
      if (pause_requested) {
        paused = !paused;
        pause_requested = false;
      }
      if (time_scale_step != 0.0) {
        time_scale *= std::pow(2.0, time_scale_step);
        time_scale = std::clamp(time_scale, 1.0e-4, 4.0);
        time_scale_step = 0.0;
        utils::info("PF07 time scale {:.4f} game days per real second", time_scale);
      }
      if (exposure_step != 0.0) {
        exposure *= std::pow(2.0, exposure_step);
        exposure_step = 0.0;
        utils::info("PF07 exposure {:.3e}", exposure);
      }

      auto [next_mouse_x, next_mouse_y] = input::cursor_pos(window);
      playground::camera_motion motion;
      motion.forward =
        float(input::events::is_pressed("camera_forward")) - float(input::events::is_pressed("camera_back"));
      motion.right =
        float(input::events::is_pressed("camera_right")) - float(input::events::is_pressed("camera_left"));
      motion.up = float(input::events::is_pressed("camera_up")) - float(input::events::is_pressed("camera_down"));
      motion.fast = input::events::is_pressed("camera_fast");
      if (!options.fixed_look) {
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
      if (gpu_profiler.has_results()) gpu_timings.add(gpu_profiler.passes(), gpu_profiler.frame_milliseconds());

      // При фиксированном числе кадров время идёт постоянным шагом: дамп обязан быть воспроизводимым,
      // а привязка к реальному dt сделала бы его зависимым от нагрузки машины.
      if (!paused) {
        const double advance = options.frames != 0 ? time_scale / 60.0 : time_scale * double(dt);
        game_time_days += advance;
      }
      const auto state = system.evaluate(game_time_days);

      const float aspect = float(std::max(pending_width, 1u)) / float(std::max(pending_height, 1u));
      const glm::mat4 view = camera.view();
      const float vertical_fov = glm::radians(65.0f);
      const glm::mat4 projection = playground::infinite_reverse_z_projection(vertical_fov, aspect, near_plane);
      // Тангенс полу-FOV передаётся явно. Выводить его из матрицы вида нельзя: там лежит поворот,
      // а не проекция, и такая попытка уже стоила кривых направлений лучей и рваного горизонта.
      const camera_block camera_data{
        projection * view, view, glm::vec4(camera.position, 1.0f),
        glm::vec4(float(pending_width), float(pending_height), near_plane, std::tan(vertical_fov * 0.5f))};
      write_current_buffer(base, "camera_buffer", &camera_data, sizeof(camera_data));

      auto output = options.output;
      output.exposure = exposure;
      // Звёздное поле берёт базис горизонта с замедленного времени: сутки идут за двадцать четыре
      // реальные минуты, и честное вращение неба выглядит вертолётом. Светила и луны при этом
      // продолжают идти по-настоящему, поэтому замедление касается только рисунка созвездий.
      const double star_time =
        options.start_time_days + (game_time_days - options.start_time_days) * output.star_rotation_scale;
      const auto star_frame = system.evaluate(star_time);
      const auto sky_block = pack_sky_block(state, star_frame, options.atmosphere, options.march, output,
                                            system.config().planet.radius_km, system.config().moons);
      write_current_buffer(base, "sky_buffer", &sky_block, sizeof(sky_block));

      const double day_fraction = game_time_days - std::floor(game_time_days);
      const std::array<std::string, 6> details{
        std::format("Time: day {:.0f} {:02}:{:02} · {:.4f} game days per real second{}", std::floor(game_time_days),
                    int32_t(day_fraction * 24.0), int32_t(std::fmod(day_fraction * 24.0, 1.0) * 60.0), time_scale,
                    paused ? " · PAUSED" : ""),
        std::format("{}: altitude {:.2f}° · {:.0f} lx{}", state.stars[0].name, state.stars[0].altitude_deg,
                    state.stars[0].illuminance_lx,
                    state.stars[0].occluded_fraction > 1e-4
                      ? std::format(" · eclipsed {:.3f}", state.stars[0].occluded_fraction) : ""),
        std::format("{}: altitude {:.2f}° · {:.0f} lx{}", state.stars[1].name, state.stars[1].altitude_deg,
                    state.stars[1].illuminance_lx,
                    state.stars[1].occluded_fraction > 1e-4
                      ? std::format(" · eclipsed {:.3f}", state.stars[1].occluded_fraction) : ""),
        std::format("Horizontal illuminance: {:.4g} lx · separation {:.2f}°", state.horizontal_illuminance_lx,
                    star_separation_deg(state)),
        std::format("Exposure: {:.3e} · atmosphere march {} steps + transmittance LUT", exposure,
                    options.march.primary_steps),
        "Transmittance table replaces the nested light march: 36.5 ms -> 12.4 ms at 720p"};
      overlay.set_detail_lines(details);
      const uint64_t frame_delta_us = uint64_t(std::max(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<float>(dt)).count(),
        int64_t{1}));
      const uint64_t timestamp_us =
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count());
      if (!overlay.update(frame_delta_us, timestamp_us)) utils::warn("PF07 Visage overlay update failed");
      write_overlay_buffers(base, overlay);

      context.prepare();
      context.draw();
      base.submit_frame();
      frame_pacer.wait();
      frames_total += 1;
      if (options.frames != 0 && frames_total >= options.frames) break;
    }

    vk::Device(device).waitIdle();
    if (!options.dump_path.empty()) dump_final_image(base, options.dump_path);
    gpu_timings.report();
    base.dump_cache_on_disk(cache_path);
  }

  vk::Instance(instance).destroy(surface);
  input::destroy(window);
  vk::Device(device).destroy();
  painter::destroy_debug_messenger(instance, messenger);
  vk::Instance(instance).destroy();
  return 0;
}

} // namespace devils_engine::pf07
