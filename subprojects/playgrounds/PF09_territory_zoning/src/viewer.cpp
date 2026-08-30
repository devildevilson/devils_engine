#include "viewer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include <glm/detail/type_half.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>

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
#include "devils_engine/utils/hash.h"

#include "navigate.h"
#include "zones.h"

namespace devils_engine::pf09 {

namespace {

uint32_t pending_width = 1280;
uint32_t pending_height = 720;
bool resize_pending = false;
int32_t escape_key = -1;
double scroll_accumulator = 0.0;
bool click_pending = false;

void error_callback(const int error, const char* message) noexcept {
  utils::warn("PF09 input error {}: {}", error, message);
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
  if (key < 0 || scancode < 0) utils::error{}("PF09 could not resolve input key '{}'", canonical);
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
  glm::mat4 view;
  glm::vec4 camera_position;
  glm::vec4 viewport_near;
};
static_assert(sizeof(camera_block) == 160);

// std430 выравнивает vec3 по 16 байтам, поэтому запись дополнена до 32: набивка объявлена явно, чтобы
// раскладка C++ и GLSL совпадали не по случайности.
struct stream_vertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  uint32_t tint = 0;
  uint32_t slot = 0xffffffffu;
  uint32_t pad0 = 0;
  uint32_t pad1 = 0;
  uint32_t pad2 = 0;
};
static_assert(sizeof(stream_vertex) == 32);

constexpr uint32_t no_slot = 0xffffffffu;

uint32_t pack(const uint32_t r, const uint32_t g, const uint32_t b, const uint32_t a) {
  return r | (g << 8) | (b << 16) | (a << 24);
}

// Стены темнее пола того же вида: без этого наклонная камера показывает сплошное пятно, потому что
// освещения тут нет и различать плоскости больше нечем.
uint32_t wall_tint(const zone_kind kind);

uint32_t kind_tint(const zone_kind kind) {
  switch (kind) {
    case zone_kind::street: return pack(150, 146, 138, 255);
    case zone_kind::yard: return pack(132, 156, 112, 255);
    case zone_kind::hall: return pack(196, 172, 140, 255);
    case zone_kind::landmark: return pack(214, 150, 90, 255);
    case zone_kind::settlement: return pack(96, 104, 126, 255);
    default: return pack(110, 110, 120, 255);
  }
}

uint32_t wall_tint(const zone_kind kind) {
  const uint32_t base_tint = kind_tint(kind);
  const auto shade = [&](const uint32_t shift) { return ((base_tint >> shift) & 0xffu) * 55u / 100u; };
  return pack(shade(0), shade(8), shade(16), 255);
}

void write_buffer(painter::graphics_base& base, const std::string_view name, const void* data, const size_t bytes) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) utils::error{}("PF09 buffer '{}' is absent from the graph", name);

  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}("PF09 cannot write '{}': capacity {}, requested {}", name, frame.sub.size, bytes);
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
  if (slot == painter::invalid_resource_slot) utils::error{}("PF09 descriptor 'ui_textures' is absent");

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

// Уровень выводится из ширины обзора, а не выбирается тумблером. Это и есть правило площадки: до пятна
// локальности показываем места, выше — поселения. Технический шов один, и он здесь.
zone_level level_for_span(const double span_m) {
  return span_m <= 2000.0 ? zone_level::interior : zone_level::local;
}

struct frame_geometry {
  std::vector<stream_vertex> fill;
  std::vector<stream_vertex> lines;
  std::vector<zone_key> slots;   // slot -> ключ зоны, для наведения и выделения
};

void push(frame_geometry& out, const glm::vec2 flat, const float height, const uint32_t tint, const uint32_t slot) {
  out.fill.push_back({flat.x, height, flat.y, tint, slot, 0, 0, 0});
}

void emit_floor(frame_geometry& out, const std::span<const glm::vec2> outline, const float height,
                const uint32_t tint, const uint32_t slot) {
  if (outline.size() < 3) return;
  for (size_t i = 1; i + 1 < outline.size(); ++i) {
    push(out, outline[0], height, tint, slot);
    push(out, outline[i], height, tint, slot);
    push(out, outline[i + 1], height, tint, slot);
  }
}

// Стена — это ребро, которое НИ ОДИН проход не занял. Отсюда архитектура появляется без единого
// авторского полигона стены: где нельзя пройти, там стоит стена, и это ровно то же утверждение, что
// связность живёт на рёбрах.
void emit_wall(frame_geometry& out, const glm::vec2 a, const glm::vec2 b, const float low, const float high,
               const uint32_t tint, const uint32_t slot) {
  push(out, a, low, tint, slot);
  push(out, b, low, tint, slot);
  push(out, b, high, tint, slot);
  push(out, a, low, tint, slot);
  push(out, b, high, tint, slot);
  push(out, a, high, tint, slot);
}

void emit_line(frame_geometry& out, const glm::vec2 a, const glm::vec2 b, const float height, const uint32_t tint) {
  out.lines.push_back({a.x, height, a.y, tint, no_slot, 0, 0, 0});
  out.lines.push_back({b.x, height, b.y, tint, no_slot, 0, 0, 0});
}

void emit_marker(frame_geometry& out, const glm::vec2 centre, const float radius, const float height,
                 const uint32_t tint) {
  const glm::vec2 corners[4] = {{centre.x - radius, centre.y - radius},
                                {centre.x + radius, centre.y - radius},
                                {centre.x + radius, centre.y + radius},
                                {centre.x - radius, centre.y + radius}};
  push(out, corners[0], height, tint, no_slot);
  push(out, corners[1], height, tint, no_slot);
  push(out, corners[2], height, tint, no_slot);
  push(out, corners[0], height, tint, no_slot);
  push(out, corners[2], height, tint, no_slot);
  push(out, corners[3], height, tint, no_slot);
}

// Части ребра, не занятые проходами. Отрезки проходов лежат на самом ребре, поэтому задача одномерная:
// вычесть из интервала ребра интервалы проёмов.
void emit_open_wall(frame_geometry& out, const glm::vec2 a, const glm::vec2 b,
                    const std::span<const zone_portal> portals, const float low, const float high,
                    const uint32_t tint, const uint32_t slot) {
  auto direction = b - a;
  const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
  if (length < 1.0e-4f) return;
  direction /= length;

  std::vector<std::pair<float, float>> gaps;
  for (const auto& portal : portals) {
    if (!portal.geometric()) continue;

    // Проём считается лежащим на ребре, если оба его конца отстоят от прямой ребра меньше допуска.
    const auto to_from = portal.from - a;
    const auto to_to = portal.to - a;
    const float side_from = std::abs(to_from.x * direction.y - to_from.y * direction.x);
    const float side_to = std::abs(to_to.x * direction.y - to_to.y * direction.x);
    if (side_from > 0.05f || side_to > 0.05f) continue;

    float begin = to_from.x * direction.x + to_from.y * direction.y;
    float end = to_to.x * direction.x + to_to.y * direction.y;
    if (begin > end) std::swap(begin, end);
    if (end <= 0.0f || begin >= length) continue;
    gaps.emplace_back(std::max(begin, 0.0f), std::min(end, length));
  }
  std::sort(gaps.begin(), gaps.end());

  float cursor = 0.0f;
  for (const auto& [begin, end] : gaps) {
    if (begin > cursor + 0.02f) emit_wall(out, a + direction * cursor, a + direction * begin, low, high, tint, slot);
    cursor = std::max(cursor, end);
  }
  if (cursor < length - 0.02f) emit_wall(out, a + direction * cursor, b, low, high, tint, slot);
}

// Снимок кадра. Нужен не для красоты: без него проверить, что окно рисует именно то, что лежит в
// хранилище, можно только глазами живого человека, а это плохая опора для площадки.
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
      utils::error{}("PF09 could not submit the frame dump");
    }
  }
  if (device.waitForFences(fence, true, size_t(1000) * 1000 * 1000) != vk::Result::eSuccess) {
    utils::error{}("PF09 frame dump timed out");
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
    utils::error{}("PF09 could not write the dump '{}'", path);
  }

  device.destroy(fence);
  device.free(base.command_pool, buffers);
  allocator.destroyBuffer(staging, allocation);
}

} // namespace

int run_viewer(const territory& map, const locality_config& local, const viewer_options& options) {
  (void)map;
  (void)local;

  pending_width = options.width;
  pending_height = options.height;

  input::init input_runtime(&error_callback);
  input::events::init();
  painter::load_dispatcher1();

  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "PF09 territory zoning";
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
      utils::error{}("PF09 requested validation, but the layers are unavailable");
    }
    instance_info.enabledLayerCount = uint32_t(painter::default_validation_layers.size());
    instance_info.ppEnabledLayerNames = painter::default_validation_layers.data();
  }

  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);
  const VkDebugUtilsMessengerEXT messenger =
    options.validation ? painter::create_debug_messenger(instance) : VK_NULL_HANDLE;

  GLFWwindow* window = input::create_window(options.width, options.height, "PF09 — territory zoning");
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (input::create_window_surface(instance, window, nullptr, &surface) != uint32_t(vk::Result::eSuccess)) {
    utils::error{}("PF09 could not create a Vulkan surface");
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
  const VkDevice device = device_maker.create({}, "pf09.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  int result = 0;
  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();

    const std::string resource_root = std::string(PF09_RESOURCE_ROOT) + "/";
    const std::string cache_path = utils::cache_folder() + "pf09_territory_zoning.pipeline_cache";
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("pf09_zones");

    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0) {
      utils::error{}("PF09 could not commit the render graph from '{}'", resource_root);
    }
    base.set_surface(surface, options.width, options.height);
    base.resize_viewport(options.width, options.height);
    base.populate_constant_default_values();

    const uint32_t graph = base.find_render_graph("pf09_zones");
    if (graph == painter::invalid_resource_slot) utils::error{}("PF09 render graph was not found");
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
      // Текст overlay латиницей не по небрежности: общий MSDF-атлас Crimson собран без кириллицы, и
      // русские строки вышли бы пустыми пятнами. Комментарии и отчёты остаются на русском.
      playground::overlay_description{
        "PF09 territory zoning",
        "sectors from disk -> polygon zones -> portals from shared edges",
        "WASD pan | wheel zoom | LMB select | QE yaw | RF pitch | G agents | P portals | H walls | Esc quit"});

    const auto atlas = overlay.font_atlas();
    const auto font_texture = assets.register_texture_storage("playground.crimson_roman");
    assets.create_texture_storage(font_texture,
                                  painter::texture_create_info{{atlas.width, atlas.height, 1},
                                                               VK_FORMAT_R8G8B8A8_UNORM});
    assets.populate_texture_storage(font_texture, atlas.bytes);
    assets.mark_ready_texture_slot(font_texture);
    overlay.set_font_texture(font_texture);
    bind_texture_descriptor(base, assets);

    zone_store store(options.world_root, options.stream_radius_m, options.stream_budget);

    // Камера начинает не в пустом поле, а в поселении: пустота — законный ответ модели, но не то, ради
    // чего открывают окно.
    glm::dvec2 centre{0.0, 0.0};
    {
      const auto probe = options.world_root;
      (void)probe;
      store.focus(centre);
      bool found = false;
      for (int32_t sy = -64; sy <= 64 && !found; ++sy) {
        for (int32_t sx = -64; sx <= 64 && !found; ++sx) {
          const auto* sector = store.sector(sx, sy);
          if (sector == nullptr) continue;
          for (const auto& record : sector->zones) {
            if (record.kind != zone_kind::settlement) continue;
            centre = {(record.bounds.lower.x + record.bounds.upper.x) * 0.5,
                      (record.bounds.lower.z + record.bounds.upper.z) * 0.5};
            found = true;
            break;
          }
        }
      }
      if (!found) {
        for (int32_t sy = 0; sy < 128 && !found; ++sy) {
          for (int32_t sx = 0; sx < 128 && !found; ++sx) {
            centre = {(double(sx) + 0.5) * sector_span_m, (double(sy) + 0.5) * sector_span_m};
            store.focus(centre);
            const auto* sector = store.sector(sx, sy);
            if (sector == nullptr || sector->zones.empty()) continue;
            for (const auto& record : sector->zones) {
              if (record.kind != zone_kind::settlement) continue;
              centre = {(record.bounds.lower.x + record.bounds.upper.x) * 0.5,
                        (record.bounds.lower.z + record.bounds.upper.z) * 0.5};
              found = true;
              break;
            }
          }
        }
      }
      if (!found) utils::error{}("PF09 viewer: no settlement found; build the world first");
    }
    store.focus(centre);

    input::events::clear_bindings();
    bind_key("pan_up", "key_w");
    bind_key("pan_down", "key_s");
    bind_key("pan_left", "key_a");
    bind_key("pan_right", "key_d");
    bind_key("toggle_agents", "key_g");
    bind_key("toggle_portals", "key_p");
    bind_key("toggle_walls", "key_h");
    bind_key("pitch_up", "key_r");
    bind_key("pitch_down", "key_f");
    bind_key("yaw_left", "key_q");
    bind_key("yaw_right", "key_e");
    escape_key = input::glfw_key_from_canonical("escape");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    input::set_window_callback(window, &scroll_callback);
    input::set_window_callback(window, &mouse_callback);

    double span_m = options.start_span_m;
    zone_key selected = invalid_key;
    bool show_agents = true;
    bool show_portals = true;
    bool show_walls = true;
    bool walls_latch = false;
    double pitch_deg = 42.0;
    float yaw_rad = 0.0f;
    bool agents_latch = false;
    bool portals_latch = false;

    std::vector<agent> walkers;
    frame_geometry geometry;
    std::vector<std::string> detail;

    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;
    playground::frame_pacer pacer(60u);

    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;

    utils::info("PF09 viewer: WASD pan, wheel zoom, LMB select, G agents, P portals, Esc quit");
    uint32_t drawn_frames = 0;

    while (!input::should_close(window)) {
      input::poll_events();
      const auto now = std::chrono::steady_clock::now();
      const float dt = std::clamp(std::chrono::duration<float>(now - previous_time).count(), 0.0f, 0.1f);
      previous_time = now;
      input::events::update(size_t(dt * 1.0e6f));

      if (resize_pending) {
        vk::Device(device).waitIdle();
        base.resize_viewport(pending_width, pending_height);
        resize_pending = false;
      }

      if (scroll_accumulator != 0.0) {
        span_m = std::clamp(span_m * std::pow(0.85, scroll_accumulator), 30.0, 400000.0);
        scroll_accumulator = 0.0;
      }

      const double pan = span_m * 0.6 * double(dt);
      centre.x += pan * (double(input::events::is_pressed("pan_right")) - double(input::events::is_pressed("pan_left")));
      centre.y += pan * (double(input::events::is_pressed("pan_down")) - double(input::events::is_pressed("pan_up")));

      const bool agents_key = input::events::is_pressed("toggle_agents");
      if (agents_key && !agents_latch) show_agents = !show_agents;
      agents_latch = agents_key;
      const bool portals_key = input::events::is_pressed("toggle_portals");
      if (portals_key && !portals_latch) show_portals = !show_portals;
      portals_latch = portals_key;
      const bool walls_key = input::events::is_pressed("toggle_walls");
      if (walls_key && !walls_latch) show_walls = !show_walls;
      walls_latch = walls_key;

      pitch_deg = std::clamp(pitch_deg + 40.0 * double(dt) *
                                           (double(input::events::is_pressed("pitch_up")) -
                                            double(input::events::is_pressed("pitch_down"))),
                             12.0, 89.0);
      yaw_rad += float(1.2 * double(dt) * (double(input::events::is_pressed("yaw_right")) -
                                           double(input::events::is_pressed("yaw_left"))));

      store.focus(centre);

      const double aspect = double(std::max(pending_width, 1u)) / double(std::max(pending_height, 1u));
      const double half_w = span_m * 0.5;
      const double half_h = half_w / aspect;
      const auto level = level_for_span(span_m);

      // Камера партийной РПГ: сверху под наклоном, чтобы читалась архитектура. Расстояние выводится из
      // ширины обзора, поэтому колесо остаётся зумом, а не полётом.
      const float fov = glm::radians(50.0f);
      const float pitch = glm::radians(pitch_deg);
      const float distance = float(half_w / (std::tan(fov * 0.5) * aspect));
      const glm::vec3 target{float(centre.x), 0.0f, float(centre.y)};
      const glm::vec3 forward{std::cos(pitch) * std::sin(yaw_rad), -std::sin(pitch), std::cos(pitch) * std::cos(yaw_rad)};
      const glm::vec3 eye = target - forward * distance;
      const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
      const glm::vec3 up = glm::cross(right, forward);

      const auto [mouse_x, mouse_y] = input::cursor_pos(window);
      const float ndc_x = float(mouse_x / double(std::max(pending_width, 1u)) * 2.0 - 1.0);
      const float ndc_y = float(mouse_y / double(std::max(pending_height, 1u)) * 2.0 - 1.0);
      const float tangent = std::tan(fov * 0.5f);

      // Луч под курсором пересекается с землёй: без плоской проекции наведение обязано считаться в 3D,
      // иначе выделялось бы не то, во что игрок целится.
      const glm::vec3 ray = glm::normalize(forward + right * (ndc_x * tangent * float(aspect)) - up * (ndc_y * tangent));
      glm::vec3 pointer{float(centre.x), 0.0f, float(centre.y)};
      if (std::abs(ray.y) > 1.0e-5f) {
        const float t = -eye.y / ray.y;
        if (t > 0.0f) pointer = eye + ray * t;
      }
      pointer.y = 0.05f;

      const auto hovered_part = store.pick(pointer, level);
      const auto* hovered_zone = hovered_part.valid() ? store.find(hovered_part.zone) : nullptr;
      if (click_pending) {
        selected = hovered_zone == nullptr ? invalid_key : hovered_zone->key;
        click_pending = false;
      }

      // --- сборка геометрии кадра ---

      geometry.fill.clear();
      geometry.lines.clear();
      geometry.slots.clear();

      const float cull_low_x = float(centre.x - half_w);
      const float cull_high_x = float(centre.x + half_w);
      const float cull_low_z = float(centre.y - half_h);
      const float cull_high_z = float(centre.y + half_h);

      uint32_t selected_slot = no_slot;
      uint32_t hovered_slot = no_slot;

      const int32_t reach = int32_t(std::ceil(std::max(half_w, half_h) / sector_span_m)) + 1;
      const int32_t sector_x = sector_of(centre.x);
      const int32_t sector_y = sector_of(centre.y);

      for (int32_t sy = sector_y - reach; sy <= sector_y + reach; ++sy) {
        for (int32_t sx = sector_x - reach; sx <= sector_x + reach; ++sx) {
          const auto* sector = store.sector(sx, sy);
          if (sector == nullptr) continue;

          for (const auto& record : sector->zones) {
            if (record.level != level || record.abstract()) continue;
            if (record.bounds.upper.x < cull_low_x || record.bounds.lower.x > cull_high_x) continue;
            if (record.bounds.upper.z < cull_low_z || record.bounds.lower.z > cull_high_z) continue;

            const uint32_t slot = uint32_t(geometry.slots.size());
            geometry.slots.push_back(record.key);
            if (record.key == selected) selected_slot = slot;
            if (hovered_zone != nullptr && record.key == hovered_zone->key) hovered_slot = slot;

            const auto parts = sector->parts_of(record);
            for (uint32_t index = 0; index < parts.size(); ++index) {
              const auto& part = parts[index];
              const auto outline = sector->outline_of(part);
              const auto portals = sector->portals_of(part);

              const float low = part.bounds.lower.y;
              const float high = part.bounds.upper.y;
              emit_floor(geometry, outline, low, kind_tint(record.kind), slot);

              // Стены поднимаются только там, где высота вообще есть. Улица высотой в двадцать
              // сантиметров стены не образует, и вертикали на ней были бы шумом, а не архитектурой.
              if (show_walls && high - low > 0.5f) {
                const uint32_t tint = wall_tint(record.kind);
                for (size_t i = 0; i < outline.size(); ++i) {
                  emit_open_wall(geometry, outline[i], outline[(i + 1) % outline.size()], portals, low, high, tint,
                                 slot);
                }
              }

              if (!show_portals) continue;
              for (const auto& portal : portals) {
                if (!portal.geometric()) continue;
                if (portal.other < record.key || (portal.other == record.key && portal.other_part < index)) continue;
                const uint32_t tint = portal.passable() ? pack(245, 245, 235, 220) : pack(230, 70, 70, 240);
                emit_line(geometry, portal.from, portal.to, low + 0.06f, tint);
              }
            }
          }
        }
      }

      // --- персонажи ---

      if (show_agents) {
        while (walkers.size() < options.agent_count && !geometry.slots.empty()) {
          const auto key = geometry.slots[utils::splitmix(walkers.size() + 1ull, uint64_t(drawn_frames) + 1ull) %
                                          geometry.slots.size()];
          agent walker{};
          walker.location = {key, 0};
          if (!interior_point(store, walker.location, walker.position)) break;
          walkers.push_back(walker);
        }

        for (auto& walker : walkers) {
          if (walker.arrived || walker.path.empty()) {
            if (geometry.slots.empty()) continue;
            const auto goal = geometry.slots[utils::splitmix(uint64_t(&walker - walkers.data()) + 1ull,
                                                             uint64_t(drawn_frames) + 7ull) %
                                             geometry.slots.size()];
            walker.path = find_path(store, walker.location, {goal, 0});
            walker.cursor = 0;
            walker.arrived = walker.path.empty();
          }
          step_agent(store, walker, std::max(dt, 0.001f) * 3.2f);
        }

        const float marker = float(std::max(span_m * 0.003, 0.30));
        for (const auto& walker : walkers) {
          const auto* part = store.part_of(walker.location);
          const float height = part == nullptr ? 0.1f : part->bounds.lower.y + 0.1f;
          emit_marker(geometry, walker.position, marker, height, pack(255, 90, 200, 255));
        }
      } else {
        walkers.clear();
      }

      // --- метаинформация о выбранной зоне ---

      detail.clear();
      const auto* shown = selected != invalid_key ? store.find(selected) : hovered_zone;
      if (shown == nullptr) {
        detail.emplace_back("zone: none (a gap between places, a legal answer)");
      } else {
        detail.push_back(std::format("zone: {} '{}'", zone_kind_name(shown->kind), store.name_of(*shown)));
        detail.push_back(std::format("level: {}  key: {}", zone_level_name(shown->level), shown->key));

        uint32_t open_gates = 0;
        uint32_t locked_gates = 0;
        for (uint32_t index = 0; index < shown->part_count; ++index) {
          for (const auto& portal : store.portals_of(part_ref{shown->key, index})) {
            portal.passable() ? ++open_gates : ++locked_gates;
          }
        }
        detail.push_back(std::format("parts: {}  portals: {} open, {} locked", shown->part_count, open_gates,
                                     locked_gates));

        const auto* parent = store.find(shown->parent);
        detail.push_back(parent == nullptr ? std::string("part of: none (parent sector not resident)")
                                           : std::format("part of: {} '{}'", zone_kind_name(parent->kind),
                                                         store.name_of(*parent)));
      }
      detail.push_back(std::format("view {:.0f} m  pitch {:.0f} deg  map level {}  zones in frame {}", span_m,
                                   pitch_deg, zone_level_name(level), geometry.slots.size()));
      detail.push_back(std::format("sectors resident {}  {:.0f} KB  agents {}", store.resident_sectors(),
                                   double(store.resident_bytes()) / 1024.0, walkers.size()));
      overlay.set_detail_lines(detail);

      if (!base.can_draw()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }
      base.prepare_frame();

      const auto view = glm::lookAtRH(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
      const auto projection = playground::infinite_reverse_z_projection(fov, float(aspect), 0.25f);

      camera_block camera{};
      camera.view_projection = projection * view;
      camera.view = view;
      camera.camera_position = glm::vec4(float(centre.x), float(centre.y),
                                         std::bit_cast<float>(selected_slot), std::bit_cast<float>(hovered_slot));
      camera.viewport_near = glm::vec4(float(pending_width), float(pending_height), 0.1f, 1.0f);
      write_buffer(base, "camera_buffer", &camera, sizeof(camera));

      write_buffer(base, "fill_vertices", geometry.fill.data(), geometry.fill.size() * sizeof(stream_vertex));
      write_buffer(base, "line_vertices", geometry.lines.data(), geometry.lines.size() * sizeof(stream_vertex));

      const VkDrawIndirectCommand fill_command{uint32_t(geometry.fill.size()), 1, 0, 0};
      const VkDrawIndirectCommand line_command{uint32_t(geometry.lines.size()), 1, 0, 0};
      base.write_constant_data(base.find_constant("fill_draw"), fill_command);
      base.write_constant_data(base.find_constant("line_draw"), line_command);
      base.update_event();

      const uint64_t delta_us = uint64_t(std::max(dt, 1.0e-6f) * 1.0e6f);
      const uint64_t stamp_us =
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count());
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
      utils::info("PF09 viewer: frame saved to '{}'", options.dump_path);
    }
    base.dump_cache_on_disk(cache_path);
  }

  vk::Instance(instance).destroy(surface);
  input::destroy(window);
  vk::Device(device).destroy();
  painter::destroy_debug_messenger(instance, messenger);
  vk::Instance(instance).destroy();
  return result;
}

} // namespace devils_engine::pf09
