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
#include <glm/mat4x4.hpp>

#include "devils_engine/input/core.h"
#include "devils_engine/input/events.h"
#include "devils_engine/painter/assets_base.h"
#include "devils_engine/painter/auxiliary.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/makers.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/playground/frame_pacer.h"
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

struct stream_vertex {
  float x = 0.0f;
  float y = 0.0f;
  uint32_t tint = 0;
  uint32_t slot = 0xffffffffu;
};
static_assert(sizeof(stream_vertex) == 16);

constexpr uint32_t no_slot = 0xffffffffu;

uint32_t pack(const uint32_t r, const uint32_t g, const uint32_t b, const uint32_t a) {
  return r | (g << 8) | (b << 16) | (a << 24);
}

uint32_t kind_tint(const zone_kind kind) {
  switch (kind) {
    case zone_kind::street: return pack(150, 146, 138, 255);
    case zone_kind::yard: return pack(132, 156, 112, 255);
    case zone_kind::room: return pack(196, 172, 140, 255);
    case zone_kind::landmark: return pack(214, 150, 90, 255);
    case zone_kind::settlement: return pack(96, 104, 126, 255);
    default: return pack(110, 110, 120, 255);
  }
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

void emit_outline(frame_geometry& out, const std::span<const glm::vec2> outline, const uint32_t tint,
                  const uint32_t slot) {
  if (outline.size() < 3) return;
  for (size_t i = 1; i + 1 < outline.size(); ++i) {
    out.fill.push_back({outline[0].x, outline[0].y, tint, slot});
    out.fill.push_back({outline[i].x, outline[i].y, tint, slot});
    out.fill.push_back({outline[i + 1].x, outline[i + 1].y, tint, slot});
  }
}

void emit_line(frame_geometry& out, const glm::vec2 a, const glm::vec2 b, const uint32_t tint) {
  out.lines.push_back({a.x, a.y, tint, no_slot});
  out.lines.push_back({b.x, b.y, tint, no_slot});
}

void emit_marker(frame_geometry& out, const glm::vec2 centre, const float radius, const uint32_t tint) {
  const glm::vec2 corners[4] = {{centre.x - radius, centre.y - radius},
                                {centre.x + radius, centre.y - radius},
                                {centre.x + radius, centre.y + radius},
                                {centre.x - radius, centre.y + radius}};
  out.fill.push_back({corners[0].x, corners[0].y, tint, no_slot});
  out.fill.push_back({corners[1].x, corners[1].y, tint, no_slot});
  out.fill.push_back({corners[2].x, corners[2].y, tint, no_slot});
  out.fill.push_back({corners[0].x, corners[0].y, tint, no_slot});
  out.fill.push_back({corners[2].x, corners[2].y, tint, no_slot});
  out.fill.push_back({corners[3].x, corners[3].y, tint, no_slot});
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
        "WASD pan | wheel zoom | LMB select | G agents | P portals | Esc quit"});

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
    escape_key = input::glfw_key_from_canonical("escape");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    input::set_window_callback(window, &scroll_callback);
    input::set_window_callback(window, &mouse_callback);

    double span_m = options.start_span_m;
    zone_key selected = invalid_key;
    bool show_agents = true;
    bool show_portals = true;
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

      store.focus(centre);

      const double aspect = double(std::max(pending_width, 1u)) / double(std::max(pending_height, 1u));
      const double half_w = span_m * 0.5;
      const double half_h = half_w / aspect;
      const auto level = level_for_span(span_m);

      const auto [mouse_x, mouse_y] = input::cursor_pos(window);
      const glm::vec3 pointer{
        float(centre.x + (mouse_x / double(std::max(pending_width, 1u)) * 2.0 - 1.0) * half_w), 0.5f,
        float(centre.y + (mouse_y / double(std::max(pending_height, 1u)) * 2.0 - 1.0) * half_h)};

      const auto* hovered_zone = store.pick(pointer, level);
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

            emit_outline(geometry, sector->outline_of(record), kind_tint(record.kind), slot);

            if (!show_portals) continue;
            for (const auto& portal : sector->portals_of(record)) {
              if (!portal.geometric() || portal.other < record.key) continue;
              const uint32_t tint = portal.passable() ? pack(245, 245, 235, 200) : pack(210, 70, 70, 230);
              emit_line(geometry, portal.from, portal.to, tint);
            }
          }
        }
      }

      // --- персонажи ---

      if (show_agents) {
        while (walkers.size() < options.agent_count && !geometry.slots.empty()) {
          const auto key = geometry.slots[utils::splitmix(walkers.size() + 1ull, uint64_t(drawn_frames) + 1ull) %
                                          geometry.slots.size()];
          const auto* home = store.find(key);
          agent walker{};
          walker.zone = key;
          if (home == nullptr || !interior_point(store, *home, walker.position)) break;
          walkers.push_back(walker);
        }

        for (auto& walker : walkers) {
          if (walker.arrived || walker.path.empty()) {
            const auto* here = store.find(walker.zone);
            if (here == nullptr || geometry.slots.empty()) continue;

            const auto goal = geometry.slots[utils::splitmix(uint64_t(&walker - walkers.data()) + 1ull,
                                                             uint64_t(drawn_frames) + 7ull) %
                                             geometry.slots.size()];
            walker.path = find_path(store, walker.zone, goal);
            walker.cursor = 0;
            walker.arrived = walker.path.empty();
          }
          step_agent(store, walker, std::max(dt, 0.001f) * 3.2f);
        }

        const float marker = float(std::max(span_m * 0.004, 0.35));
        for (const auto& walker : walkers) {
          emit_marker(geometry, walker.position, marker, pack(255, 90, 200, 255));
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
        for (const auto& portal : store.portals_of(*shown)) {
          portal.passable() ? ++open_gates : ++locked_gates;
        }
        detail.push_back(std::format("portals: {} open, {} locked", open_gates, locked_gates));

        const auto* parent = store.find(shown->parent);
        detail.push_back(parent == nullptr ? std::string("part of: none (parent sector not resident)")
                                           : std::format("part of: {} '{}'", zone_kind_name(parent->kind),
                                                         store.name_of(*parent)));
      }
      detail.push_back(std::format("view {:.0f} m  map level {}  zones in frame {}", span_m,
                                   zone_level_name(level), geometry.slots.size()));
      detail.push_back(std::format("sectors resident {}  {:.0f} KB  agents {}", store.resident_sectors(),
                                   double(store.resident_bytes()) / 1024.0, walkers.size()));
      overlay.set_detail_lines(detail);

      if (!base.can_draw()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }
      base.prepare_frame();

      glm::mat4 projection(0.0f);
      projection[0][0] = float(1.0 / half_w);
      projection[2][1] = float(1.0 / half_h);
      projection[3][0] = float(-centre.x / half_w);
      projection[3][1] = float(-centre.y / half_h);
      projection[3][2] = 0.5f;
      projection[3][3] = 1.0f;

      camera_block camera{};
      camera.view_projection = projection;
      camera.view = glm::mat4(1.0f);
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
