#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glm/vec4.hpp>

#include "devils_engine/catalogue/logging.h"
#include "devils_engine/input/core.h"
#include "devils_engine/input/events.h"
#include "devils_engine/painter/assets_base.h"
#include "devils_engine/painter/auxiliary.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/makers.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/playground/frame_pacer.h"
#include "devils_engine/utils/core.h"

using namespace devils_engine;

namespace {

constexpr uint32_t initial_width = 1280;
constexpr uint32_t initial_height = 720;
constexpr uint32_t dispatch_tile = 8;

uint32_t pending_width = initial_width;
uint32_t pending_height = initial_height;
bool resize_pending = false;
bool reset_requested = false;
int32_t escape_key = -1;
int32_t reset_key = -1;

void error_callback(const int error, const char* message) noexcept {
  utils::warn("PF03 input error {}: {}", error, message);
}

void key_callback(GLFWwindow* window, const int key, const int scancode, const int action, const int) noexcept {
  input::events::update_key(scancode, action);
  if (key == escape_key && action == 1) {
    input::set_should_close(window, true);
  }
  if (key == reset_key && action == 1) {
    reset_requested = true;
  }
}

void framebuffer_callback(GLFWwindow*, const int width, const int height) noexcept {
  pending_width = width > 0 ? uint32_t(width) : 0u;
  pending_height = height > 0 ? uint32_t(height) : 0u;
  resize_pending = pending_width != 0 && pending_height != 0;
}

// Раскладка обязана совпадать с PF03_FRAME_BLOCK_BODY (resources/shaders/pf03_frame.glsl)
struct alignas(16) frame_block {
  glm::vec4 viewport_time;
  glm::vec4 controls;
};
static_assert(sizeof(frame_block) == 32);

void write_current_buffer(painter::graphics_base& base, const std::string_view name, const void* data, const size_t bytes) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF03 buffer '{}' is absent from the configured graph", name);
  }
  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}(
      "PF03 cannot write buffer '{}' (mapped {}, capacity {}, requested {})",
      name, frame.mapped != nullptr, frame.sub.size, bytes);
  }
  std::memcpy(static_cast<uint8_t*>(frame.mapped) + frame.sub.offset, data, bytes);
}

void update_screen_dispatch(painter::graphics_base& base, const uint32_t width, const uint32_t height) {
  const uint32_t slot = base.find_constant("screen_dispatch");
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF03 screen_dispatch constant is absent from the configured graph");
  }
  const VkDispatchIndirectCommand command{
    (width + dispatch_tile - 1u) / dispatch_tile,
    (height + dispatch_tile - 1u) / dispatch_tile,
    1u};
  base.write_constant_data(slot, command);
  base.update_event();
}

// Сколько копий движок в итоге завёл под ресурс и почему. Ровно та величина, которую конфиг больше не
// задаёт руками: период счётчика плюс запрошенная читателями глубина истории.
void report_resource_copies(const painter::graphics_base& base, const std::string_view name) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF03 resource '{}' is absent from the configured graph", name);
  }
  const auto& res = base.resources[slot];
  utils::info(
    "PF03 resource '{}': {} copies = period '{}' ({}) + history {}",
    res.name,
    res.compute_buffering(&base),
    painter::type::to_string(res.type),
    painter::type::compute_buffering(&base, res.type),
    res.history_depth);
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
  bool validation = false;
  bool uncapped = false;
  bool verbose = false;
  uint32_t frame_limit = 0; // 0 — до Esc; иначе выходим сами (детерминированные прогоны)
  float history_weight = 0.94f;
  float motion_speed = 1.0f;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option(argv[i]);
    validation = validation || option == "--validation";
    uncapped = uncapped || option == "--uncapped";
    verbose = verbose || option == "--verbose";

    constexpr std::string_view frames_prefix = "--frames=";
    if (option.starts_with(frames_prefix)) {
      frame_limit = uint32_t(std::stoul(std::string(option.substr(frames_prefix.size()))));
    }
    constexpr std::string_view weight_prefix = "--history-weight=";
    if (option.starts_with(weight_prefix)) {
      history_weight = std::clamp(std::stof(std::string(option.substr(weight_prefix.size()))), 0.0f, 0.999f);
    }
    constexpr std::string_view speed_prefix = "--motion-speed=";
    if (option.starts_with(speed_prefix)) {
      motion_speed = std::stof(std::string(option.substr(speed_prefix.size())));
    }
  }

  catalogue::register_engine_domains();
  if (verbose) {
    // Выведенные кросс-кадровые зависимости и инициализацию истории движок логирует на глубине flow:
    // площадке важно видеть, что порядок между кадрами реально появился, а не «кажется, работает».
    catalogue::logs().set_level("render", catalogue::log_depth::flow);
  }

  input::init input_runtime(&error_callback);
  input::events::init();
  painter::load_dispatcher1();

  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "PF03 post processing";
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
      utils::error{}("PF03 requested validation, but Vulkan validation layers are unavailable");
    }
    instance_info.enabledLayerCount = uint32_t(painter::default_validation_layers.size());
    instance_info.ppEnabledLayerNames = painter::default_validation_layers.data();
  }

  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);
  const VkDebugUtilsMessengerEXT messenger = validation ? painter::create_debug_messenger(instance) : VK_NULL_HANDLE;

  GLFWwindow* window = input::create_window(initial_width, initial_height, "PF03 — temporal history");
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  const auto surface_result = input::create_window_surface(instance, window, nullptr, &surface);
  if (surface_result != uint32_t(vk::Result::eSuccess)) {
    utils::error{}("PF03 could not create Vulkan surface: {}", vk::to_string(vk::Result(surface_result)));
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
  const VkDevice device = device_maker.create({}, "pf03.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();
    const std::string resource_root = std::string(PF03_RESOURCE_ROOT) + "/";
    const std::string cache_path = utils::cache_folder() + "pf03_post_processing.pipeline_cache";
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("pf03_post");

    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0) {
      utils::error{}("PF03 could not commit render graph from '{}'", resource_root);
    }
    base.set_surface(surface, initial_width, initial_height);
    base.resize_viewport(initial_width, initial_height);
    base.populate_constant_default_values();
    update_screen_dispatch(base, initial_width, initial_height);

    const uint32_t graph = base.find_render_graph("pf03_post");
    if (graph == painter::invalid_resource_slot) {
      utils::error{}("PF03 render graph was not found");
    }
    base.change_render_graph(graph);

    painter::assets_base assets(device, physical.handle);
    assets.create_fence();
    assets.create_allocator(instance);
    assets.create_command_buffer(queues.transfer, queues.graphics);
    assets.set_graphics_base(&base);
    assets.create_default_texture();

    input::events::clear_bindings();
    escape_key = input::glfw_key_from_canonical("escape");
    reset_key = input::glfw_key_from_canonical("key_r");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);

    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;

    report_resource_copies(base, "scene_color");
    report_resource_copies(base, "feedback_color");
    utils::info("PF03 frames in flight: {}", base.frames_in_flight());
    utils::info("PF03 graph: scene compute -> temporal accumulate (reads previous frame) -> blit present");
    utils::info("PF03 controls: R reset history, Esc exit");

    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;
    playground::frame_pacer frame_pacer(uncapped ? 0u : 60u);
    uint32_t frames_since_reset = 0;
    uint32_t frames_total = 0;

    while (!input::should_close(window)) {
      input::poll_events();
      const auto now = std::chrono::steady_clock::now();
      const float dt = std::clamp(std::chrono::duration<float>(now - previous_time).count(), 0.0f, 0.1f);
      previous_time = now;
      input::events::update(size_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<float>(dt)).count()));

      if (resize_pending) {
        vk::Device(device).waitIdle();
        base.resize_viewport(pending_width, pending_height);
        update_screen_dispatch(base, pending_width, pending_height);
        resize_pending = false;
        frames_since_reset = 0; // ресурсы пересозданы — истории больше нет
      }

      if (reset_requested) {
        frames_since_reset = 0;
        reset_requested = false;
      }

      if (!base.can_draw()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }

      base.prepare_frame();

      frame_block frame_data{};
      frame_data.viewport_time = glm::vec4(
        float(pending_width),
        float(pending_height),
        std::chrono::duration<float>(now - start_time).count(),
        float(frames_since_reset));
      frame_data.controls = glm::vec4(history_weight, motion_speed, 0.0f, 0.0f);
      write_current_buffer(base, "frame_buffer", &frame_data, sizeof(frame_data));

      context.prepare();
      context.draw();
      base.submit_frame();
      frame_pacer.wait();

      frames_since_reset += 1;
      frames_total += 1;
      if (frame_limit != 0 && frames_total >= frame_limit) {
        utils::info("PF03 reached the requested {} frames, exiting", frame_limit);
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
