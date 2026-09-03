#include "viewer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <glm/detail/type_half.hpp>
#include <glm/geometric.hpp>
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

namespace devils_engine::gn03 {
namespace {

uint32_t pending_width = 1280;
uint32_t pending_height = 720;
bool resize_pending = false;
int32_t escape_key = -1;

void error_callback(const int error, const char* message) noexcept {
  utils::warn("GN03 input error {}: {}", error, message);
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
    utils::error{}("GN03 could not resolve input key '{}'", canonical);
  }
  input::events::set_key(event, scancode, key);
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

// Блок камеры. Раскладка ОДНА на все шейдеры и лежит одним файлом (shaders/camera_block.glsl):
// std140 читается по смещениям, поэтому пропуск поля в одной копии не даёт ни ошибки, ни
// предупреждения — все следующие поля просто читаются с чужого места.
struct alignas(16) camera_block {
  glm::mat4 view_projection;
  glm::mat4 inverse_view_projection;
  glm::vec4 camera_position;
  glm::vec4 sun_direction;
  glm::vec4 sky_colour;
  // Размер чанка в метрах, дальность видимости, режим представления, сила сетки чанков.
  glm::vec4 params;
  glm::vec4 viewport_near;
  glm::vec4 reserved[3];
};
static_assert(sizeof(camera_block) == 256);

void write_buffer(painter::graphics_base& base, const std::string_view name, const void* data, const size_t bytes) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("GN03 buffer '{}' is absent", name);
  }
  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}("GN03 cannot write '{}': capacity {}, requested {}", name, frame.sub.size, bytes);
  }
  if (bytes != 0) {
    std::memcpy(static_cast<uint8_t*>(frame.mapped) + frame.sub.offset, data, bytes);
  }
}

void write_overlay(painter::graphics_base& base, const playground::visage_overlay& overlay, const bool visible) {
  write_buffer(base, "ui_vertices", overlay.vertices().data(), overlay.vertices().size());
  write_buffer(base, "ui_indices", overlay.indices().data(), overlay.indices().size());
  const auto commands = overlay.commands();
  const uint32_t slot = base.find_resource("ui_commands");
  const auto frame = base.get_current_buffer_resource_frame(slot);
  const uint32_t count = visible ? uint32_t(commands.size()) : 0u;
  auto* destination = static_cast<uint8_t*>(frame.mapped) + frame.sub.offset;
  std::memcpy(destination, &count, sizeof(count));
  if (count != 0) {
    std::memcpy(destination + sizeof(count), commands.data(), commands.size_bytes());
  }
}

void bind_texture_descriptor(painter::graphics_base& base, const painter::assets_base& assets) {
  const uint32_t slot = base.find_descriptor("ui_textures");
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("GN03 descriptor 'ui_textures' is absent");
  }
  auto& descriptor = base.descriptors[slot];
  const vk::ImageView fallback(assets.default_texture_view());
  std::vector<vk::DescriptorImageInfo> images(descriptor.texture_count);
  for (uint32_t i = 0; i < descriptor.texture_count; ++i) {
    vk::ImageView view;
    if (i < assets.texture_slots.size()) {
      view = assets.texture_slots[i].view;
    }
    images[i] = vk::DescriptorImageInfo(vk::Sampler{}, view ? view : fallback,
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
      utils::error{}("GN03 could not submit the frame dump");
    }
  }
  if (device.waitForFences(fence, true, size_t(1000) * 1000 * 1000) != vk::Result::eSuccess) {
    utils::error{}("GN03 frame dump timed out");
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
    utils::error{}("GN03 could not write the dump '{}'", path);
  }
  device.destroy(fence);
  device.free(base.command_pool, buffers);
  allocator.destroyBuffer(staging, allocation);
}

} // namespace

int run_viewer(const viewer_options& options, const factory_builder& builder) {
  pending_width = options.width;
  pending_height = options.height;

  const double chunk_span = double(options.chunk_cells) * options.cell_size;

  input::init input_runtime(&error_callback);
  input::events::init();
  painter::load_dispatcher1();

  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "GN03 streaming volume";
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
      utils::error{}("GN03 requested validation, but the layers are unavailable");
    }
    instance_info.enabledLayerCount = uint32_t(painter::default_validation_layers.size());
    instance_info.ppEnabledLayerNames = painter::default_validation_layers.data();
  }

  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);
  const VkDebugUtilsMessengerEXT messenger =
    options.validation ? painter::create_debug_messenger(instance) : VK_NULL_HANDLE;
  GLFWwindow* window = input::create_window(options.width, options.height, "GN03 — streaming volume");
  VkSurfaceKHR surface_handle = VK_NULL_HANDLE;
  if (input::create_window_surface(instance, window, nullptr, &surface_handle) != uint32_t(vk::Result::eSuccess)) {
    utils::error{}("GN03 could not create the window surface");
  }

  painter::system_info system(instance);
  system.check_devices_surface_capability(surface_handle);
  const auto physical = system.choose_physical_device();
  const auto queue_plan = painter::make_device_queue_plan(physical);
  painter::device_maker device_maker(instance);
  device_maker.beginDevice(physical.handle);
  for (uint32_t i = 0; i < queue_plan.request_count; ++i) {
    device_maker.createQueue(queue_plan.requests[i].family, queue_plan.requests[i].count);
  }
  device_maker.features(vk::PhysicalDevice(physical.handle).getFeatures());
  device_maker.setExtensions(painter::default_device_extensions);
  const VkDevice device = device_maker.create({}, "gn03.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  int exit_code = 0;
  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();

    const std::string resource_root = std::string(GN03_RESOURCE_ROOT) + "/";
    const std::string cache_path = utils::cache_folder() + "gn03_streaming_volume.pipeline_cache";
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("gn03_volume");
    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0) {
      utils::error{}("GN03 could not commit the render graph from '{}'", resource_root);
    }
    if (options.uncapped) {
      base.set_present_mode(painter::physical_device_present_mode::values::mailbox,
                            painter::physical_device_present_mode::values::immediate);
    }
    base.set_surface(surface_handle, options.width, options.height);
    base.resize_viewport(options.width, options.height);
    base.populate_constant_default_values();
    const uint32_t graph = base.find_render_graph("gn03_volume");
    if (graph == painter::invalid_resource_slot) {
      utils::error{}("GN03 render graph was not found");
    }
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
        "GN03 streaming volume",
        "marching cubes over a chunked density field, generated around the camera in worker threads",
        "WASD/QE fly | Shift fast | M view | G chunk grid | Tab/-/= tune | Enter apply | N new seed | U overlay | Esc quit"});
    const auto atlas = overlay.font_atlas();
    const auto font_texture = assets.register_texture_storage("playground.crimson_roman");
    assets.create_texture_storage(font_texture,
                                  painter::texture_create_info{{atlas.width, atlas.height, 1},
                                                               VK_FORMAT_R8G8B8A8_UNORM});
    assets.populate_texture_storage(font_texture, atlas.bytes);
    assets.mark_ready_texture_slot(font_texture);
    overlay.set_font_texture(font_texture);
    bind_texture_descriptor(base, assets);

    // Арена не может быть больше объявленного буфера. Объявление живёт в конфиге графа, потому что
    // это цена по видеопамяти, и генератор с площадкой здесь одинаковы: свою стоимость надо назвать
    // до запуска, а не выяснять на середине полёта.
    const uint32_t arena_slot = base.find_resource("surface_vertices");
    if (arena_slot == painter::invalid_resource_slot) {
      utils::error{}("GN03 buffer 'surface_vertices' is absent");
    }
    const auto arena_frame = base.get_current_buffer_resource_frame(arena_slot);
    const size_t declared_vertices = arena_frame.sub.size / sizeof(gpu_vertex);
    size_t arena_vertices = options.arena_vertices;
    if (arena_vertices > declared_vertices) {
      utils::warn("GN03: arena asked for {} vertices, but the render config declares room for {} — using the "
                  "declared number",
                  arena_vertices, declared_vertices);
      arena_vertices = declared_vertices;
    }

    // Отрезок, возвращённый ареной, ждёт столько кадров, сколько их бывает в полёте: устройство
    // читает арену, и кадры, уже отданные ему, ссылаются на прежние вершины.
    vertex_arena arena(arena_vertices, 768, base.frames_in_flight() + 1);

    // ТАБЛИЦА СМЕЩЕНИЙ ЧАНКОВ — то, что делает «всё относительно камеры» дешёвым. Вершина знает
    // только свои координаты внутри чанка и номер слота; куда этот чанк попадает относительно чанка
    // камеры, говорит эта таблица, и она пишется целиком каждый кадр — по одному вектору на чанк, а
    // не на вершину. Поэтому уход камеры из чанка стоит килобайты записи вместо десятков мегабайт.
    const uint32_t offsets_slot = base.find_resource("chunk_offsets");
    if (offsets_slot == painter::invalid_resource_slot) {
      utils::error{}("GN03 buffer 'chunk_offsets' is absent");
    }
    const size_t declared_slots = base.get_current_buffer_resource_frame(offsets_slot).sub.size / sizeof(glm::vec4);
    if (declared_slots < max_chunk_slots) {
      utils::error{}("GN03: the render config declares room for {} chunk offsets, but the arena hands out {} "
                     "slots — the two numbers must agree",
                     declared_slots, max_chunk_slots);
    }
    std::vector<glm::vec4> chunk_offsets(max_chunk_slots, glm::vec4(0.0f));

    // СУЩНОСТИ ЖИВУТ С ЧАНКОМ, поэтому и хранятся по его ключу: выгрузка чанка — это и их выгрузка.
    // Второй арены им не нужно, и это не экономия, а разница масштаба: вершин миллионы, сущностей
    // десяток на чанк, поэтому дешевле каждый кадр привести их к системе чанка камеры на процессоре,
    // чем заводить им слоты, таблицу смещений и отложенное освобождение.
    std::unordered_map<originator::chunk_key, std::vector<chunk_prop>, chunk_key_hash> chunk_props;
    const uint32_t props_slot = base.find_resource("prop_instances");
    if (props_slot == painter::invalid_resource_slot) {
      utils::error{}("GN03 buffer 'prop_instances' is absent");
    }
    const size_t prop_instance_limit =
      base.get_current_buffer_resource_frame(props_slot).sub.size / sizeof(gpu_prop);
    std::vector<gpu_prop> prop_instances;
    prop_instances.reserve(prop_instance_limit);
    size_t dropped_props = 0;

    if (options.memory == nullptr) {
      utils::error{}("GN03 viewer needs the world memory: a world without one is a different contract");
    }
    auto& memory = *options.memory;
    std::vector<world_memory::joined_prop> visible;
    // Ближайшая веха и её имя: взаимодействие всегда с одной, и она же подписана в оверлее, иначе
    // «нажал и ничего не произошло» не отличить от «нажал не на то».
    prop_id nearest_id{};
    bool nearest_found = false;
    float nearest_distance = 0.0f;
    bool nearest_marked = false;

    std::vector<tunable_value> tunables = options.tunables;
    uint64_t seed = options.seed;
    const auto overrides_of = [&tunables]() {
      std::vector<std::pair<std::string, double>> result;
      result.reserve(tunables.size());
      for (const auto& tunable : tunables) {
        result.emplace_back(tunable.range.name, tunable.value);
      }
      return result;
    };

    auto window_state = options.window;
    auto streamer = std::make_unique<chunk_streamer>(builder(overrides_of(), seed), options.workers);

    input::events::clear_bindings();
    bind_key("camera_forward", "key_w");
    bind_key("camera_back", "key_s");
    bind_key("camera_left", "key_a");
    bind_key("camera_right", "key_d");
    bind_key("camera_down", "key_q");
    bind_key("camera_up", "key_e");
    bind_key("camera_fast", "left_shift");
    bind_key("mode_next", "key_m");
    bind_key("toggle_grid", "key_g");
    bind_key("toggle_overlay", "key_u");
    bind_key("setting_next", "tab");
    bind_key("setting_decrease", "minus");
    bind_key("setting_increase", "equal");
    bind_key("apply", "enter");
    bind_key("new_seed", "key_n");
    // ВЗАИМОДЕЙСТВИЕ, ради которого память и заводится: забрать веху и переключить пометку. Обе
    // операции меняют не мир, а ОТЛИЧИЕ мира от выводимого.
    bind_key("collect", "key_f");
    bind_key("mark", "key_r");
    escape_key = input::glfw_key_from_canonical("escape");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_DISABLED);
    input::set_raw_mouse_motion(window);

    playground::free_camera camera;
    // КАМЕРА ХРАНИТ ПОЗИЦИЮ ВНУТРИ СВОЕГО ЧАНКА, а не в мире: мировое место — это целый ключ чанка
    // плюс это смещение. Поэтому у полёта нет предела точности: сколько бы наблюдатель ни улетел,
    // числа кадра остаются в пределах одного чанка, а ключ растёт целыми.
    //
    // Начинает камера НАД поверхностью и смотрит вниз-вдоль. Первая попытка ставила её на высоту
    // трёх четвертей чанка, и это оказалось ВНУТРИ горы: рельеф доходит до 1.35 амплитуды, то есть
    // до тридцати с лишним метров, и первый кадр выходил чёрным — понять, генерируется ли мир,
    // было нельзя. Два чанка вверх выше любой вершины при настройках по умолчанию.
    // НАКОПИТЕЛЬ ПОЗИЦИИ — В DOUBLE, и это отдельная болезнь от плавающего начала координат.
    // Плавающее начало лечит РАЗРЕШЕНИЕ (числа остаются малыми), но не НАКОПЛЕНИЕ: сложение во float
    // округляется на каждом кадре, и миллион кадров дал 0.36 метра сноса — за пару игровых сессий
    // видимую величину. Ширина накопителя это и убирает: в double остаётся 10^-9 метра.
    //
    // Камере при этом отдаётся float ОДНИМ преобразованием на кадр — ей нужна матрица вида, а не
    // накопление, и величина там мала, поэтому преобразование точно.
    local_frame observer;
    observer.key = options.start;
    observer.position = options.start_offset_valid ? options.start_offset
                                                   : glm::dvec3(chunk_span * 0.5, 0.0, chunk_span * 0.5);
    camera.position = glm::vec3(observer.position);
    camera.pitch = options.start_offset_valid ? options.start_pitch : -0.45f;
    if (options.start_offset_valid) {
      camera.yaw = options.start_yaw;
    }
    camera.move_speed = float(chunk_span) * 0.6f;
    camera.fast_multiplier = 5.0f;

    auto [mouse_x, mouse_y] = input::cursor_pos(window);
    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;
    playground::frame_pacer pacer(options.uncapped ? 0u : 60u);

    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;

    static constexpr std::array<const char*, 3> mode_names{{"shaded", "normals", "slope"}};
    size_t mode = std::min(options.mode, mode_names.size() - 1);
    bool grid_visible = options.grid;
    bool overlay_visible = true;
    size_t setting = 0;
    bool settings_dirty = false;
    size_t rejected_chunks = 0;
    // Посчитанная геометрия, для которой пока нет места в арене.
    std::vector<chunk_mesh> postponed;
    uint32_t drawn_frames = 0;

    std::array<bool, 12> latches{};
    const auto pressed_once = [&](const std::string_view event, const size_t slot) {
      if (slot >= latches.size()) {
        utils::error{}("GN03 viewer: key latch {} for '{}' is outside the {} declared latches", slot, event,
                       latches.size());
      }
      const bool down = input::events::is_pressed(event);
      const bool fired = down && !latches[slot];
      latches[slot] = down;
      return fired;
    };

    window_state.centre = observer.key;
    streamer->set_window(window_state);

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

      const auto [next_mouse_x, next_mouse_y] = input::cursor_pos(window);
      playground::camera_motion motion;
      motion.forward =
        float(input::events::is_pressed("camera_forward")) - float(input::events::is_pressed("camera_back"));
      motion.right =
        float(input::events::is_pressed("camera_right")) - float(input::events::is_pressed("camera_left"));
      motion.up = float(input::events::is_pressed("camera_up")) - float(input::events::is_pressed("camera_down"));
      motion.fast = input::events::is_pressed("camera_fast");
      motion.look_delta = {float(next_mouse_x - mouse_x), float(next_mouse_y - mouse_y)};
      mouse_x = next_mouse_x;
      mouse_y = next_mouse_y;
      // Взгляд применяется камерой, а движение НАКАПЛИВАЕТСЯ ЗДЕСЬ, в double. Порядок тот же, что у
      // `update`: сначала взгляд, потом смещение по новому направлению.
      camera.look(motion);
      observer.position += glm::dvec3(camera.displacement(motion, dt));

      // ВЫХОД ИЗ ЧАНКА: ключ меняется, а позиция заворачивается внутрь. Это и есть весь механизм
      // плавающего начала координат — ни арену, ни таблицу пересчитывать не нужно, потому что
      // таблица и так пишется каждый кадр, а вершины лежат в координатах своих чанков.
      rebase(observer, chunk_span);
      camera.position = glm::vec3(observer.position);

      if (pressed_once("mode_next", 0)) {
        mode = (mode + 1) % mode_names.size();
      }
      if (pressed_once("toggle_grid", 1)) {
        grid_visible = !grid_visible;
      }
      if (pressed_once("toggle_overlay", 2)) {
        overlay_visible = !overlay_visible;
      }
      if (!tunables.empty()) {
        if (pressed_once("setting_next", 3)) {
          setting = (setting + 1) % tunables.size();
        }
        auto& current = tunables[setting];
        if (pressed_once("setting_decrease", 4)) {
          const double moved = current.range.advance(current.value, -1);
          settings_dirty = settings_dirty || moved != current.value;
          current.value = moved;
        }
        if (pressed_once("setting_increase", 5)) {
          const double moved = current.range.advance(current.value, 1);
          settings_dirty = settings_dirty || moved != current.value;
          current.value = moved;
        }
      }

      bool rebuild = false;
      if (pressed_once("new_seed", 6)) {
        // Хеш, а не +1: соседние зёрна дают похожие первые решения, и «другой мир» получается
        // подозрительно знакомым.
        uint64_t next = seed + 0x9e3779b97f4a7c15ull;
        next = (next ^ (next >> 30)) * 0xbf58476d1ce4e5b9ull;
        next = (next ^ (next >> 27)) * 0x94d049bb133111ebull;
        seed = (next ^ (next >> 31)) & 0x7fffffffull;
        rebuild = true;
      }
      if (pressed_once("apply", 7) && settings_dirty) {
        rebuild = true;
      }

      if (rebuild) {
        // Другие числа — другой мир, значит вся посчитанная геометрия устарела целиком. Стример
        // пересобирается вместе с пайплайнами рабочих потоков, арена очищается, и мир строится
        // заново от камеры: половина старого мира рядом с половиной нового — это не «быстрее», это
        // мир, которого нет.
        vk::Device(device).waitIdle();
        streamer.reset();
        arena = vertex_arena(arena_vertices, 768, base.frames_in_flight() + 1);
        chunk_props.clear();
        streamer = std::make_unique<chunk_streamer>(builder(overrides_of(), seed), options.workers);
        window_state.centre = observer.key;
        streamer->set_window(window_state);
        settings_dirty = false;
        rejected_chunks = 0;
        postponed.clear();
      }

      // Окно чанков переставляется только при СМЕНЕ ключа: пересчёт списка на каждый кадр означал бы
      // сортировку сотен ключей ради того же самого набора. Ключ камеры — он же и есть центр окна,
      // потому что окно теперь трёхмерно во все стороны.
      if (!(observer.key == window_state.centre)) {
        window_state.centre = observer.key;
        streamer->set_window(window_state);
      }

      for (const auto& key : streamer->take_evicted()) {
        arena.remove(key);
        chunk_props.erase(key);
      }

      // ОТЛОЖЕННЫЕ ЧАНКИ ЖДУТ МЕСТА, А НЕ СЧИТАЮТСЯ ЗАНОВО. Первая версия возвращала не поместившийся
      // чанк в очередь стримера, и это оказалось молотилкой: место не появлялось, чанк считался
      // снова, и за две с половиной секунды рабочие потоки посчитали 717 чанков вместо 324 — то есть
      // грели машину вместо того, чтобы досчитать остальной мир. Геометрия уже посчитана, стоит она
      // мегабайты в оперативной памяти, и правильное решение — держать её и попробовать позже.
      const auto try_insert = [&](chunk_mesh& mesh) {
        if (arena.insert(mesh.key, mesh.vertices)) {
          // Сущности принимаются вместе с геометрией и только вместе с ней: чанк, которого нет в
          // арене, не должен показывать свои вехи — иначе они висели бы над пустотой.
          if (!mesh.props.empty()) {
            chunk_props[mesh.key] = std::move(mesh.props);
          }
          return true;
        }
        if (rejected_chunks == 0) {
          utils::warn("GN03: the vertex arena is full ({} of {} vertices used) — the chunk at ({}, {}, {}) "
                      "waits for room; lower --radius or raise --arena",
                      arena.used(), arena.capacity(), mesh.key.x, mesh.key.y, mesh.key.z);
        }
        return false;
      };

      // Сначала те, что уже ждут: иначе новый чанк занимал бы освободившееся место перед тем, кто
      // стоит в очереди дольше, и ждущий не попал бы в мир никогда.
      for (auto it = postponed.begin(); it != postponed.end();) {
        if (!window_state.contains(it->key)) {
          // Окно ушло: геометрия устарела, и держать её незачем. Стример при этом обязан узнать, что
          // чанка в мире нет, иначе он останется «присутствующим» без единого треугольника.
          streamer->forget(it->key);
          it = postponed.erase(it);
          continue;
        }
        if (!try_insert(*it)) {
          break;
        }
        it = postponed.erase(it);
      }

      // За кадр забирается ограниченное число чанков: копирование мегабайта в арену — это работа
      // кадра, и отдать ей весь кадр значило бы получить рывок ровно в тот момент, когда мир
      // появляется. Остальное подождёт следующего кадра, оно уже посчитано.
      constexpr size_t uploads_per_frame = 6;
      constexpr size_t postponed_limit = 96;
      chunk_mesh ready;
      for (size_t i = 0; i < uploads_per_frame && postponed.empty() && streamer->pop_ready(ready); ++i) {
        if (try_insert(ready)) {
          continue;
        }
        if (postponed.size() < postponed_limit) {
          postponed.push_back(std::move(ready));
        } else {
          // Список ждущих тоже стоит памяти. Дальше держать бессмысленно: столько чанков за раз в
          // арену не поместится ни при каком порядке, и это уже не задержка, а неверный размер арены.
          streamer->forget(ready.key);
        }
        ++rejected_chunks;
        break;
      }

      if (!base.can_draw()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }

      base.prepare_frame();

      // Изменившиеся отрезки арены — и только они. Заливать всю арену каждый кадр значило бы везти
      // десятки мегабайт ради нескольких новых чанков.
      const auto dirty = arena.take_dirty();
      if (!dirty.empty()) {
        const auto frame = base.get_current_buffer_resource_frame(arena_slot);
        if (frame.mapped == nullptr) {
          utils::error{}("GN03: the vertex arena is not host visible");
        }
        auto* destination = static_cast<uint8_t*>(frame.mapped) + frame.sub.offset;
        const auto mirror = arena.mirror();
        for (const auto& region : dirty) {
          std::memcpy(destination + region.first * sizeof(gpu_vertex), mirror.data() + region.first,
                      region.count * sizeof(gpu_vertex));
        }
      }

      // Таблица смещений: по одной строке на чанк, лежащий в арене. Пишется целиком, потому что
      // после ухода камеры из чанка меняются ВСЕ строки сразу — и это дешевле любого учёта, кто
      // именно изменился: четыре тысячи векторов это 64 килобайта.
      for (const auto& live : arena.live()) {
        if (live.slot < chunk_offsets.size()) {
          chunk_offsets[live.slot] = glm::vec4(chunk_offset(live.key, observer.key, chunk_span), 0.0f);
        }
      }
      write_buffer(base, "chunk_offsets", chunk_offsets.data(), chunk_offsets.size() * sizeof(glm::vec4));

      // Сущности собираются заново каждый кадр и СРАЗУ в системе чанка камеры: их немного, поэтому
      // смещение применяется здесь, на процессоре, и шейдеру не нужны ни слот, ни таблица.
      //
      // СОЕДИНЕНИЕ С ПАМЯТЬЮ ПРОИСХОДИТ ЗДЕСЬ ЖЕ, каждый кадр, из (выводимое + склад). Хранить
      // «уже соединённое» было бы вторым представлением того же, и оно однажды разъехалось бы со
      // складом; а так соединение остаётся ФУНКЦИЕЙ, и это ровно то свойство, которое проверяется.
      prop_instances.clear();
      dropped_props = 0;
      nearest_found = false;
      nearest_distance = 0.0f;
      for (const auto& [key, list] : chunk_props) {
        const auto offset = chunk_offset(key, observer.key, chunk_span);
        memory.join(key, list, visible);
        for (const auto& entry : visible) {
          if (prop_instances.size() >= prop_instance_limit) {
            ++dropped_props;
            continue;
          }
          const auto& prop = *entry.prop;
          gpu_prop instance;
          for (uint32_t axis = 0; axis < 3; ++axis) {
            instance.position[axis] = offset[axis] + float(prop.position[axis]);
            instance.normal[axis] = float(prop.normal[axis]);
          }
          instance.scale = float(prop.size);
          // Род и пометка в одном слове: род в младшем байте, пометка выше. Пометка — это ПАМЯТЬ, а
          // не свойство мира, поэтому она приезжает из склада, а не из генератора.
          instance.kind = prop.kind | (entry.delta.marked ? 0x100u : 0u);
          prop_instances.push_back(instance);

          const glm::vec3 to_prop = glm::vec3(instance.position[0], instance.position[1], instance.position[2]) -
                                    camera.position;
          const float distance = glm::length(to_prop);
          if (!nearest_found || distance < nearest_distance) {
            nearest_found = true;
            nearest_distance = distance;
            nearest_id = prop_id{key, prop.origin};
            nearest_marked = entry.delta.marked;
          }
        }
      }
      write_buffer(base, "prop_instances", prop_instances.data(), prop_instances.size() * sizeof(gpu_prop));

      // Взаимодействие идёт ПОСЛЕ сборки кадра, потому что «ближайшая» известна только после неё, и
      // изменение склада увидит следующий кадр — так же, как его увидит вернувшийся чанк.
      static constexpr float reach = 12.0f;
      if (nearest_found && nearest_distance <= reach) {
        if (pressed_once("collect", 8)) {
          memory.take(nearest_id);
        }
        if (pressed_once("mark", 9)) {
          memory.mark(nearest_id);
        }
      }

      const float aspect = float(std::max(pending_width, 1u)) / float(std::max(pending_height, 1u));
      const float near_plane = 0.1f;
      const auto view = camera.view();
      const auto projection = playground::infinite_reverse_z_projection(glm::radians(70.0f), aspect, near_plane);

      camera_block block{};
      block.view_projection = projection * view;
      block.inverse_view_projection = glm::inverse(block.view_projection);
      block.camera_position = glm::vec4(camera.position, 1.0f);
      block.sun_direction = glm::vec4(glm::normalize(glm::vec3(0.42f, 0.70f, 0.28f)), 0.0f);
      block.sky_colour = glm::vec4(0.52f, 0.62f, 0.78f, 1.0f);
      // Дальность видимости — из окна чанков: туман обязан прятать ровно ту границу, за которой
      // геометрии нет. Иначе на краю окна видно, как мир кончается ступенькой.
      const float view_distance = float(double(window_state.horizontal_radius) * chunk_span);
      block.params = glm::vec4(float(chunk_span), view_distance, float(mode), grid_visible ? 1.0f : 0.0f);
      block.viewport_near =
        glm::vec4(float(pending_width), float(pending_height), near_plane, float(options.chunk_cells));
      write_buffer(base, "camera_buffer", &block, sizeof(block));

      const VkDrawIndirectCommand surface_command{uint32_t(arena.high_water()), 1u, 0u, 0u};
      base.write_constant_data(base.find_constant("surface_draw"), surface_command);
      // Двенадцать вершин на веху, экземпляров — сколько сущностей в окне.
      const VkDrawIndirectCommand prop_command{12u, uint32_t(prop_instances.size()), 0u, 0u};
      base.write_constant_data(base.find_constant("prop_draw"), prop_command);
      base.update_event();

      const auto world_camera = absolute_position(observer, chunk_span);
      const auto stats = streamer->stats();
      const auto seconds = std::chrono::duration<double>(now - start_time).count();
      std::vector<std::string> detail{
        std::format("chunk {} m, window {}x{}x{} chunks", chunk_span, window_state.horizontal_radius * 2 + 1,
                    window_state.vertical_radius * 2 + 1, window_state.horizontal_radius * 2 + 1),
        std::format("chunks present {}, pending {}, in flight {}", stats.present, stats.pending, stats.in_flight),
        std::format("chunk cost {:.1f} ms, {} generated in {:.1f} s", stats.last_milliseconds, stats.generated,
                    seconds),
        std::format("arena {}/{} vertices, holes {}, drawn {}", arena.used(), arena.capacity(),
                    arena.hole_vertices(), arena.high_water()),
        std::format("densest chunk {} vertices, waiting for room {} (rejected {})", stats.largest_chunk_vertices,
                    postponed.size(), rejected_chunks),
        std::format("entities {} in {} chunks{}", prop_instances.size(), chunk_props.size(),
                    dropped_props != 0 ? std::format(", {} over the buffer", dropped_props) : std::string{}),
        // Склад памяти: сколько сущностей мир помнит ИЗМЕНЁННЫМИ. Это же и размер сохранения.
        std::format("world memory {} entries; nearest {}", memory.size(),
                    nearest_found && nearest_distance <= reach
                      ? std::format("{:.1f} m, chunk {} {} {} #{}{} (F take, R mark)", nearest_distance,
                                    nearest_id.chunk.x, nearest_id.chunk.y, nearest_id.chunk.z,
                                    nearest_id.origin, nearest_marked ? ", marked" : "")
                      : std::string("out of reach")),
        // Мировое место камеры собирается из ключа и локального смещения: сама камера мировых
        // координат не хранит вовсе.
        std::format("view {} | seed {} | chunk {} {} {} | camera {:.0f} {:.0f} {:.0f}", mode_names[mode], seed,
                    observer.key.x, observer.key.y, observer.key.z, world_camera.x, world_camera.y,
                    world_camera.z),
      };
      if (!tunables.empty()) {
        const auto& current = tunables[setting];
        detail.push_back(std::format("{} = {:.3f} [{:.2f}..{:.2f}] {}", current.range.name, current.value,
                                     current.range.minimum, current.range.maximum,
                                     settings_dirty ? "(Enter to apply)" : ""));
      }
      overlay.set_detail_lines(detail);

      const uint64_t delta_us = uint64_t(std::max(dt, 1.0e-6f) * 1.0e6f);
      const uint64_t stamp_us = uint64_t(seconds * 1.0e6);
      overlay.update(delta_us, stamp_us);
      write_overlay(base, overlay, overlay_visible);

      context.prepare();
      context.draw();
      base.submit_frame();
      arena.advance_frame();
      pacer.wait();

      ++drawn_frames;
      if (options.frames != 0 && drawn_frames >= options.frames) {
        break;
      }
    }

    vk::Device(device).waitIdle();
    if (!options.dump_path.empty()) {
      dump_scene(base, options.dump_path);
      utils::info("GN03 viewer: frame saved to '{}'", options.dump_path);
    }
    base.dump_cache_on_disk(cache_path);
    streamer.reset();
  }

  vk::Instance(instance).destroy(surface_handle);
  input::destroy(window);
  vk::Device(device).destroy();
  painter::destroy_debug_messenger(instance, messenger);
  vk::Instance(instance).destroy();
  return exit_code;
}

} // namespace devils_engine::gn03
