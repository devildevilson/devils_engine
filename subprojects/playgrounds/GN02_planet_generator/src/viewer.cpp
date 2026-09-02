#include "viewer.h"

#include <algorithm>
#include <array>
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
#include "devils_engine/painter/makers.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/playground/frame_pacer.h"
#include "devils_engine/playground/free_camera.h"
#include "devils_engine/playground/visage_overlay.h"
#include "devils_engine/visage/font.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"

#include "names.h"
#include "visual.h"

using namespace devils_engine;

namespace devils_engine::gn02 {
namespace {

uint32_t pending_width = 1280;
uint32_t pending_height = 720;
bool resize_pending = false;
int32_t escape_key = -1;
double scroll_accumulator = 0.0;

void error_callback(const int error, const char* message) noexcept {
  utils::warn("GN02 input error {}: {}", error, message);
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

void scroll_callback(GLFWwindow*, const double, const double y) noexcept {
  scroll_accumulator += y;
}

// Нажатие мыши копится флагом, а не читается опросом: клик — это СОБЫТИЕ, и между двумя кадрами его
// можно пропустить целиком, если спрашивать «нажата ли кнопка сейчас».
bool click_pending = false;

void mouse_callback(GLFWwindow*, const int button, const int action, const int) noexcept {
  if (button == 0 && action == 1) {
    click_pending = true;
  }
}

void bind_key(const std::string_view event, const std::string_view canonical) {
  const auto [key, scancode] = input::key_from_canonical(canonical);
  if (key < 0 || scancode < 0) {
    utils::error{}("GN02 could not resolve input key '{}'", canonical);
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

// Блок камеры. Держится маленьким намеренно: у просмотрщика один проход и один материал, и всё, что
// ему нужно знать про кадр, здесь.
struct alignas(16) camera_block {
  glm::mat4 view_projection;
  // Обратная матрица нужна небу: направление луча на пиксель восстанавливается из экранных
  // координат. Считается на CPU один раз за кадр, а не в шейдере на каждый пиксель.
  glm::mat4 inverse_view_projection;
  glm::mat4 planet_to_world;
  glm::vec4 camera_position;
  glm::vec4 light_direction;
  glm::vec4 params;        // масштаб рельефа, плоская закраска, радиус, режим
  glm::vec4 viewport_near; // ширина, высота, ближняя плоскость, резкость смешивания клеток
};
static_assert(sizeof(camera_block) == 256);

void write_buffer(painter::graphics_base& base, const std::string_view name, const void* data, const size_t bytes) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("GN02 buffer '{}' is absent", name);
  }
  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}("GN02 cannot write '{}': capacity {}, requested {}", name, frame.sub.size, bytes);
  }
  if (bytes != 0) {
    std::memcpy(static_cast<uint8_t*>(frame.mapped) + frame.sub.offset, data, bytes);
  }
}

// Глиф подписи места. Шестьдесят четыре байта: якорь с высотой, прямоугольник глифа в долях высоты,
// координаты в атласе и цвет с номером текстуры.
struct alignas(16) label_glyph {
  glm::vec4 anchor{};
  glm::vec4 rect{};
  glm::vec4 uv{};
  glm::vec4 tint{};
};
static_assert(sizeof(label_glyph) == 64);

// Подпись, выложенная глифами вокруг якоря.
//
// Про вертикаль стоит знать одно: `pb`/`pt` шрифта — это смещения ВНИЗ от верха строки, а не «низ и
// верх» глифа (так их упаковал атлас под y-вниз, и у nuklear они читаются так же). Экранные
// координаты Vulkan тоже идут вниз, поэтому пересчёта знака не нужно; нужна только поправка на то,
// что em-квадрат выше видимой строки — заглавная буква занимает примерно от 0.28 до 1.0 em, поэтому
// центр видимого текста лежит на 0.64 em ниже верха строки, а не на 0.5.
void append_label(std::vector<label_glyph>& out, const visage::font_t& font, const std::string_view text,
                  const glm::vec3& anchor, const float height, const uint32_t texture_slot,
                  const glm::vec3& colour) {
  const float width = float(font.text_width(1.0, text));
  float cursor = -width * 0.5f;
  for (const unsigned char character : text) {
    const auto* glyph = font.find_glyph(uint32_t(character));
    if (glyph == nullptr) {
      continue;
    }
    if (glyph->w > 0 && glyph->h > 0) {
      out.push_back(label_glyph{
        glm::vec4(anchor, height),
        glm::vec4(cursor + float(glyph->pl), float(glyph->pb) - 0.64f, float(glyph->pr - glyph->pl),
                  float(glyph->pt - glyph->pb)),
        glm::vec4(float(glyph->al / double(font.width)), float(glyph->ab / double(font.height)),
                  float(glyph->ar / double(font.width)), float(glyph->at / double(font.height))),
        glm::vec4(colour, float(texture_slot))});
    }
    cursor += float(glyph->advance);
  }
}

void write_overlay(painter::graphics_base& base, const playground::visage_overlay& overlay,
                   const bool visible) {
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
    utils::error{}("GN02 descriptor 'ui_textures' is absent");
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
      utils::error{}("GN02 could not submit the frame dump");
    }
  }
  if (device.waitForFences(fence, true, size_t(1000) * 1000 * 1000) != vk::Result::eSuccess) {
    utils::error{}("GN02 frame dump timed out");
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
    utils::error{}("GN02 could not write the dump '{}'", path);
  }
  device.destroy(fence);
  device.free(base.command_pool, buffers);
  allocator.destroyBuffer(staging, allocation);
}

// Предел камеры ПО ОСИ ВРАЩЕНИЯ. Выражен компонентой, а не углом, потому что ломается именно она: у
// lookAt с мировым «вверх» базис в полюсе вырождается, картинка прыгает, а обратная задача (луч из
// пикселя в поверхность) теряет решение. 0.94 — это примерно 70 градусов широты: полярную шапку видно
// целиком, но камера в неё не упирается.
constexpr float camera_axis_limit = 0.94f;

glm::vec3 clamp_to_limit(const glm::vec3 direction) {
  const glm::vec3 unit = glm::normalize(direction);
  if (std::abs(unit.y) <= camera_axis_limit) {
    return unit;
  }
  // Прижимаем ось и восстанавливаем длину горизонтальной части, а не просто нормируем: иначе на
  // пределе камера уезжала бы по долготе вслед за обрезанием.
  const float sign = unit.y >= 0.0f ? 1.0f : -1.0f;
  const glm::vec3 flat{unit.x, 0.0f, unit.z};
  const float flat_length = std::sqrt(std::max(1.0f - camera_axis_limit * camera_axis_limit, 0.0f));
  const glm::vec3 plane = glm::dot(flat, flat) > 1.0e-8f ? glm::normalize(flat) : glm::vec3(1.0f, 0.0f, 0.0f);
  return glm::normalize(plane * flat_length + glm::vec3(0.0f, sign * camera_axis_limit, 0.0f));
}

// Орбита вокруг планеты. Полюс не достигается никогда — см. camera_axis_limit.
glm::vec3 orbit(const glm::vec3 direction, const float horizontal, const float vertical, const float step) {
  const glm::vec3 radial = glm::normalize(direction);
  const glm::vec3 world_up{0.0f, 1.0f, 0.0f};
  glm::vec3 east = glm::cross(world_up, radial);
  if (glm::dot(east, east) < 1.0e-6f) {
    east = glm::vec3(1.0f, 0.0f, 0.0f);
  }
  east = glm::normalize(east);
  const glm::vec3 north = glm::normalize(glm::cross(radial, east));

  const glm::vec3 moved = radial + east * (horizontal * step) + north * (vertical * step);
  return clamp_to_limit(moved);
}

} // namespace

// Хеш зерна для кнопки «другая планета». Именно хеш, а не +1: соседние зёрна дают похожие первые
// решения (те же затравки плит смещаются на клетку), и «другой мир» получается подозрительно знакомым.
uint64_t next_seed(const uint64_t seed) noexcept {
  uint64_t x = seed + 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return (x ^ (x >> 31)) & 0x7fffffffull;
}

int run_viewer(const viewer_options& options, std::vector<tunable_value> tunables,
               const regenerate_fn& regenerate) {
  pending_width = options.width;
  pending_height = options.height;

  const auto mode_table = view_modes();
  size_t mode = std::min(options.mode, mode_table.size() - 1);

  // Выделение области. Держится НОМЕРОМ ОБЛАСТИ, а не номером клетки: выделяется место, а не точка,
  // и что считать местом, решает выбранное представление. Ноль означает «ничего не выбрано».
  float hovered_area = 0.0f;
  float selected_area = 0.0f;
  uint32_t hovered_cell = UINT32_MAX;

  const auto build_request = [&](const size_t step_limit, const uint64_t seed) {
    generation_request request;
    request.step_limit = step_limit;
    request.seed = seed;
    request.overrides.reserve(tunables.size());
    for (const auto& entry : tunables) {
      request.overrides.emplace_back(entry.range.name, entry.value);
    }
    return request;
  };

  size_t setting = 0;
  bool settings_dirty = false;
  uint64_t seed = options.seed;

  // Мир считается ДО открытия окна: если генерация упадёт, окно не должно успеть появиться.
  auto world = regenerate(build_request(options.step_limit, seed));
  size_t step_limit = world.executed_steps;
  seed = world.seed;

  // Подразделения по числу клеток: цель — около четырёх треугольников на клетку. Меньше — и берег
  // рисуется лестницей самой сетки; больше — платим вершинами за то, чего в данных нет.
  uint32_t subdivisions = options.subdivisions;
  if (subdivisions == 0) {
    const double cells = double(world.line->find_buffer("cells")->count());
    const double wanted = std::max(1.0, 4.0 * cells / 20.0);
    subdivisions = uint32_t(std::clamp(std::llround(std::log2(wanted) * 0.5), 3ll, 7ll));
  }

  auto surface = build_surface(*world.line, subdivisions);
  auto visuals = build_cell_visuals(*world.line, mode, world.line->find_buffer("cells")->count());
  bool visuals_dirty = true;

  // Поиск клетки по направлению и названия мест: и то и другое строится один раз на мир и живёт до
  // следующего пересчёта.
  cell_locator locator(*world.line);
  auto names = build_place_names(*world.line, options.continent_min_provinces, options.ocean_zones);

  // Области для подписей: собираются по УРОВНЮ текущего представления и сортируются по размеру.
  // Сортировка один раз на смену представления, а не на кадр: набор не меняется от поворота планеты,
  // меняется только то, какие из них видны, и это решает шейдер.
  auto labelled = collect_labelled_areas(*world.line, mode_table[mode].level);
  const auto sort_labelled = [](std::vector<labelled_area>& areas) {
    std::sort(areas.begin(), areas.end(),
              [](const labelled_area& a, const labelled_area& b) { return a.cells > b.cells; });
  };
  sort_labelled(labelled);
  std::vector<label_glyph> label_glyphs;
  label_glyphs.reserve(4096);

  // Название выделенной области ЗАВИСИТ ОТ ПРЕДСТАВЛЕНИЯ, и это главное свойство выделения: одна и та
  // же клетка под курсором в политике даёт герцогство, в географии — историческую область, а на
  // рельефе не даёт ничего, потому что у рельефа названных мест нет.
  const auto describe_area = [&](const float area) {
    const auto index = size_t(area + 0.5f);
    if (index == 0) {
      return std::string("nothing");
    }
    const auto pick = [&](const std::vector<std::string>& list, const std::string_view kind) {
      if (index < list.size() && !list[index].empty()) {
        return std::format("{} ({})", list[index], kind);
      }
      return std::format("{} #{}", kind, index);
    };
    switch (mode_table[mode].level) {
      case view_mode::named::province: return pick(names.provinces, "county");
      case view_mode::named::land_mass: return pick(names.land_masses, "land mass");
      case view_mode::named::continent: return pick(names.continents, "continent");
      case view_mode::named::historical_region: return pick(names.historical_regions, "historical region");
      case view_mode::named::ocean: return pick(names.oceans, "ocean");
      case view_mode::named::duchy: return pick(names.duchies, "duchy");
      case view_mode::named::empire: return pick(names.empires, "empire");
      case view_mode::named::realm: return pick(names.realms, "realm");
      case view_mode::named::sea_zone: return std::format("oceanic region #{}", index);
      case view_mode::named::landform: return std::format("landform kind #{}", index - 1);
      case view_mode::named::none: break;
    }
    return std::format("area #{}", index);
  };

  input::init input_runtime(&error_callback);
  input::events::init();
  painter::load_dispatcher1();

  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "GN02 planet generator";
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
      utils::error{}("GN02 requested validation, but the layers are unavailable");
    }
    instance_info.enabledLayerCount = uint32_t(painter::default_validation_layers.size());
    instance_info.ppEnabledLayerNames = painter::default_validation_layers.data();
  }

  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);
  const VkDebugUtilsMessengerEXT messenger =
    options.validation ? painter::create_debug_messenger(instance) : VK_NULL_HANDLE;
  GLFWwindow* window = input::create_window(options.width, options.height, "GN02 — planet generator");
  VkSurfaceKHR surface_handle = VK_NULL_HANDLE;
  if (input::create_window_surface(instance, window, nullptr, &surface_handle) != uint32_t(vk::Result::eSuccess)) {
    utils::error{}("GN02 could not create the window surface");
  }

  painter::system_info system(instance);
  system.check_devices_surface_capability(surface_handle);
  const auto physical = system.choose_physical_device();
  const auto queue_plan = painter::make_device_queue_plan(physical);
  painter::device_maker device_maker(instance);
  device_maker.beginDevice(physical.handle);
  // Именно request_count, а не размер массива: массив запросов фиксированный, и заполнены в нём
  // только первые request_count элементов. Обход всего массива отдаёт драйверу семейство 0xffffffff.
  for (uint32_t i = 0; i < queue_plan.request_count; ++i) {
    device_maker.createQueue(queue_plan.requests[i].family, queue_plan.requests[i].count);
  }
  device_maker.features(vk::PhysicalDevice(physical.handle).getFeatures());
  device_maker.setExtensions(painter::default_device_extensions);
  const VkDevice device = device_maker.create({}, "gn02.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();

    const std::string resource_root = std::string(GN02_RESOURCE_ROOT) + "/";
    const std::string cache_path = utils::cache_folder() + "gn02_planet_generator.pipeline_cache";
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("gn02_planet");
    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0) {
      utils::error{}("GN02 could not commit the render graph from '{}'", resource_root);
    }
    // Режим представления задаётся ДО свопчейна: по умолчанию это Fifo, то есть вертикальная
    // синхронизация. Измерено: с одним снятым ограничителем кадров частота осталась 63 в секунду —
    // её держал именно режим представления, а не производящий цикл. Значит снимать надо оба, иначе
    // флаг «без ограничения» ничего не снимает.
    if (options.uncapped) {
      base.set_present_mode(painter::physical_device_present_mode::values::mailbox,
                            painter::physical_device_present_mode::values::immediate);
    }
    base.set_surface(surface_handle, options.width, options.height);
    base.resize_viewport(options.width, options.height);
    base.populate_constant_default_values();
    const uint32_t graph = base.find_render_graph("gn02_planet");
    if (graph == painter::invalid_resource_slot) {
      utils::error{}("GN02 render graph was not found");
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
      // Текст оверлея латиницей: атлас шрифта площадок покрывает ASCII, и кириллица в нём просто
      // не отрисовывается — пустая панель вместо подписей. Русский остаётся в отчёте и в конфигах.
      playground::overlay_description{
        "GN02 planet generator",
        "fibonacci cell lattice + CSR adjacency, nine steps from tectonics to named places",
        "WASD orbit | wheel zoom | R spin | H relief | U overlay | Esc quit"});
    const auto atlas = overlay.font_atlas();
    const auto font_texture = assets.register_texture_storage("playground.crimson_roman");
    assets.create_texture_storage(font_texture,
                                  painter::texture_create_info{{atlas.width, atlas.height, 1},
                                                               VK_FORMAT_R8G8B8A8_UNORM});
    assets.populate_texture_storage(font_texture, atlas.bytes);
    assets.mark_ready_texture_slot(font_texture);
    overlay.set_font_texture(font_texture);
    bind_texture_descriptor(base, assets);

    // Сетка поверхности загружается один раз: она зависит только от положений клеток, а те
    // считаются первым шагом и дальше не меняются.
    write_buffer(base, "surface_vertices", surface.data(), surface.size() * sizeof(surface_vertex));

    input::events::clear_bindings();
    bind_key("rotate_up", "key_w");
    bind_key("rotate_down", "key_s");
    bind_key("rotate_left", "key_a");
    bind_key("rotate_right", "key_d");
    bind_key("toggle_rotation", "key_r");
    bind_key("toggle_relief", "key_h");
    bind_key("toggle_overlay", "key_u");
    bind_key("mode_next", "key_m");
    bind_key("mode_previous", "key_n");
    bind_key("step_next", "right_bracket");
    bind_key("step_previous", "left_bracket");
    bind_key("setting_next", "tab");
    bind_key("setting_decrease", "minus");
    bind_key("setting_increase", "equal");
    bind_key("regenerate", "enter");
    bind_key("new_seed", "key_g");
    for (uint32_t i = 0; i < 10; ++i) {
      static constexpr std::array<const char*, 10> digits{"key_0", "key_1", "key_2", "key_3", "key_4",
                                                          "key_5", "key_6", "key_7", "key_8", "key_9"};
      bind_key(std::string("mode_digit_") + std::to_string(i), digits[i]);
    }
    escape_key = input::glfw_key_from_canonical("escape");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    input::set_window_callback(window, &scroll_callback);
    input::set_window_callback(window, &mouse_callback);

    float planet_yaw = 0.0f;
    glm::vec3 camera_direction = clamp_to_limit(glm::vec3(0.35f, 0.28f, 1.0f));
    float camera_distance = options.camera_distance;
    bool auto_rotate = !options.fixed_rotation;
    bool relief = true;
    // Оверлей включён, но выключаем его одной кнопкой: подписи занимают четверть кадра, а вопрос «на
    // что похожа планета» лучше всего задаётся кадром без единой буквы.
    bool overlay_visible = true;

    std::array<bool, 24> latches{};
    const auto pressed_once = [&](const std::string_view event, const size_t slot) {
      const bool down = input::events::is_pressed(event);
      const bool fired = down && !latches[slot];
      latches[slot] = down;
      return fired;
    };

    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;
    playground::frame_pacer pacer(options.uncapped ? 0u : 60u);
    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;
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
        camera_distance = std::clamp(camera_distance * float(std::pow(0.88, scroll_accumulator)), 1.05f, 6.0f);
        scroll_accumulator = 0.0;
      }

      if (pressed_once("toggle_rotation", 0)) {
        auto_rotate = !auto_rotate;
      }
      if (pressed_once("toggle_relief", 1)) {
        relief = !relief;
      }
      if (pressed_once("toggle_overlay", 23)) {
        overlay_visible = !overlay_visible;
      }

      const size_t previous_mode = mode;
      if (pressed_once("mode_next", 2)) {
        mode = (mode + 1) % mode_table.size();
      }
      if (pressed_once("mode_previous", 3)) {
        mode = (mode + mode_table.size() - 1) % mode_table.size();
      }
      for (uint32_t digit = 0; digit < 10; ++digit) {
        if (pressed_once(std::string("mode_digit_") + std::to_string(digit), 4 + digit)) {
          // Цифры 1..9 выбирают первые девять полей, ноль — десятое: так привычнее, чем считать с нуля.
          const size_t requested = digit == 0 ? 9 : digit - 1;
          if (requested < mode_table.size()) {
            mode = requested;
          }
        }
      }
      if (mode != previous_mode) {
        visuals_dirty = true;
        labelled = collect_labelled_areas(*world.line, mode_table[mode].level);
        sort_labelled(labelled);
        // Выделение снимается при смене представления: номер области у другого уровня означает другое
        // место, и удержать «то же самое» нельзя — это и есть суть того, что выделяется ПРЕДСТАВЛЕНИЕМ.
        selected_area = 0.0f;
      }

      // Настройки генератора. Границы и шаг пришли из конфига, поэтому просмотрщик крутит значение,
      // ничего не зная о его смысле: он не может ни вывести величину за пределы, ни сойти с сетки шага.
      if (!tunables.empty()) {
        if (pressed_once("setting_next", 4 + 10)) {
          setting = (setting + 1) % tunables.size();
        }
        auto& current = tunables[setting];
        if (pressed_once("setting_decrease", 5 + 10)) {
          const double moved = current.range.advance(current.value, -1);
          settings_dirty = settings_dirty || moved != current.value;
          current.value = moved;
        }
        if (pressed_once("setting_increase", 6 + 10)) {
          const double moved = current.range.advance(current.value, 1);
          settings_dirty = settings_dirty || moved != current.value;
          current.value = moved;
        }
      }

      // Смена шага, настроек или зерна пересчитывает мир. Это дорого (доли секунды) и делается прямо в
      // кадре: моргнуть одним кадром честнее, чем держать семь готовых копий планеты в памяти.
      const size_t total_steps = world.step_names.size();
      size_t requested_step = step_limit;
      uint64_t requested_seed = seed;
      bool rebuild = false;

      if (pressed_once("step_next", 7 + 10)) {
        requested_step = std::min(step_limit + 1, total_steps);
      }
      if (pressed_once("step_previous", 8 + 10)) {
        requested_step = step_limit > 1 ? step_limit - 1 : 1;
      }
      if (pressed_once("new_seed", 9 + 10)) {
        requested_seed = next_seed(seed);
      }
      if (pressed_once("regenerate", 10 + 10)) {
        rebuild = true;
      }

      if (requested_step != step_limit || requested_seed != seed || rebuild) {
        vk::Device(device).waitIdle();
        world = regenerate(build_request(requested_step, requested_seed));
        step_limit = world.executed_steps;
        seed = world.seed;
        settings_dirty = false;
        surface = build_surface(*world.line, subdivisions);
        write_buffer(base, "surface_vertices", surface.data(), surface.size() * sizeof(surface_vertex));
        locator = cell_locator(*world.line);
        names = build_place_names(*world.line, options.continent_min_provinces, options.ocean_zones);
        labelled = collect_labelled_areas(*world.line, mode_table[mode].level);
        sort_labelled(labelled);
        // Выделение снимается при пересчёте: номер области у нового мира означает другое место.
        selected_area = 0.0f;
        visuals_dirty = true;
      }

      if (!options.fixed_rotation) {
        if (auto_rotate) {
          planet_yaw += dt * 0.09f;
        }
        const float horizontal = float(input::events::is_pressed("rotate_right")) -
                                 float(input::events::is_pressed("rotate_left"));
        const float vertical = float(input::events::is_pressed("rotate_up")) -
                               float(input::events::is_pressed("rotate_down"));
        if (horizontal != 0.0f || vertical != 0.0f) {
          camera_direction = orbit(camera_direction, horizontal, vertical, dt * 0.85f);
        }
      }

      const size_t cells = world.line->find_buffer("cells")->count();
      if (visuals_dirty) {
        visuals = build_cell_visuals(*world.line, mode, cells);
      }

      const auto& current_mode = mode_table[mode];
      detail.clear();
      // Клавиши генерации отдельной строкой, а не в общей подписи: та рисуется одной строкой и
      // обрезается по ширине панели, а обрезанная подсказка хуже, чем никакой.
      detail.emplace_back("keys: 1-9,0 field | M/N cycle | [ ] step | Tab setting | -/= value | "
                          "Enter apply | G new seed");
      detail.push_back(std::format("step {}/{}: {} ({:.0f} ms to regenerate)", step_limit, total_steps,
                                   step_limit == 0 ? std::string("none") : world.step_names[step_limit - 1],
                                   world.milliseconds));
      detail.push_back(std::format("field {}/{}: {} - {}", mode + 1, mode_table.size(), current_mode.name,
                                   current_mode.hint));
      if (step_limit < current_mode.ready_after_step) {
        detail.push_back(std::format("this field is written by step {} ({}); it is still all zero",
                                     current_mode.ready_after_step,
                                     world.step_names[current_mode.ready_after_step - 1]));
      }
      detail.push_back(std::format("cells {}, mesh triangles {}, seed {}", cells, surface.size() / 3, seed));
      if (!tunables.empty()) {
        const auto& current = tunables[setting];
        detail.push_back(std::format("setting {}/{}: {} = {:g}  [{:g} .. {:g} step {:g}]{}", setting + 1,
                                     tunables.size(), current.range.name, current.value, current.range.minimum,
                                     current.range.maximum, current.range.step,
                                     settings_dirty ? "  (Enter to apply)" : ""));
      }
      detail.push_back(std::format("relief {}, spin {}, distance {:.2f} R", relief ? "on" : "off",
                                   auto_rotate ? "on" : "off", camera_distance));

      // ВЫДЕЛЕНИЕ. Строка идёт последней намеренно: это единственная строка оверлея, которая
      // отвечает не на «что нагенерировано», а на «что я сейчас показываю пальцем», и именно её и не
      // хватало, чтобы планета перестала быть полем цветов.
      detail.push_back(std::format("under cursor: {}   selected: {}", describe_area(hovered_area),
                                   describe_area(selected_area)));
      overlay.set_detail_lines(detail);

      if (!base.can_draw()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }
      base.prepare_frame();

      const glm::mat4 planet_to_world = glm::rotate(glm::mat4(1.0f), planet_yaw, glm::vec3(0.0f, 1.0f, 0.0f));
      const glm::vec3 eye = camera_direction * camera_distance;
      const glm::vec3 target{0.0f};
      const float aspect = float(std::max(pending_width, 1u)) / float(std::max(pending_height, 1u));
      const float fov = glm::radians(48.0f);
      const auto view = glm::lookAtRH(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
      const auto projection = playground::infinite_reverse_z_projection(fov, aspect, 0.05f);
      const glm::mat4 camera_view_projection = projection * view;

      // ПОДБОР ОБЛАСТИ ЛУЧОМ. Луч строится из базиса камеры, а не из обратной матрицы: обратная нужна
      // небу на каждый пиксель, а здесь пиксель один, и базис короче.
      //
      // Пересечение считается со СФЕРОЙ ЕДИНИЧНОГО РАДИУСА, а не с рельефом: вопрос «какая клетка в
      // этом направлении» — про направление, и он тот же, который задаёт сетка и любой потребитель
      // пакета. Рельеф сдвинул бы ответ на доли клетки и потребовал бы марша по поверхности.
      {
        const glm::vec3 forward = glm::normalize(target - eye);
        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 up = glm::cross(right, forward);
        const auto [mouse_x, mouse_y] = input::cursor_pos(window);
        const float ndc_x = float(mouse_x / double(std::max(pending_width, 1u)) * 2.0 - 1.0);
        const float ndc_y = float(mouse_y / double(std::max(pending_height, 1u)) * 2.0 - 1.0);
        const float tangent = std::tan(fov * 0.5f);
        const glm::vec3 ray =
          glm::normalize(forward + right * (ndc_x * tangent * aspect) - up * (ndc_y * tangent));

        hovered_cell = UINT32_MAX;
        hovered_area = 0.0f;
        const float along = -glm::dot(eye, ray);
        const float closest = glm::length(eye + ray * along);
        if (closest < 1.0f) {
          const float offset = std::sqrt(std::max(1.0f - closest * closest, 0.0f));
          const glm::vec3 hit = eye + ray * (along - offset);
          // Точка попадания переводится в planet-local: клетки лежат в системе планеты, а вращение
          // планеты живёт в матрице.
          const glm::vec3 local = glm::vec3(glm::inverse(planet_to_world) * glm::vec4(hit, 1.0f));
          hovered_cell = locator.locate(local);
          if (hovered_cell < visuals.size()) {
            hovered_area = visuals[hovered_cell].area;
          }
        }
        if (click_pending) {
          // Повторный клик по той же области снимает выделение: иначе снять его нечем.
          selected_area = selected_area == hovered_area ? 0.0f : hovered_area;
          click_pending = false;
        }
      }

      camera_block camera{};
      camera.view_projection = camera_view_projection;
      camera.inverse_view_projection = glm::inverse(camera.view_projection);
      camera.planet_to_world = planet_to_world;
      camera.camera_position = glm::vec4(eye, 1.0f);
      // Четвёртая компонента направления света — НОМЕР ВЫДЕЛЕННОЙ ОБЛАСТИ. Свободный слот вместо
      // нового поля блока: блок повторяется в четырёх шейдерах байт в байт, и каждое новое поле — это
      // четыре места, где о нём можно забыть (на одном таком уже пропадал весь оверлей).
      camera.light_direction = glm::vec4(glm::normalize(glm::vec3(-0.42f, -0.35f, -0.84f)), selected_area);
      // Масштаб рельефа в радиусах планеты на метр высоты: 6000 м при масштабе 12 дают 1.2% радиуса.
      // Без преувеличения рельеф на глобусе не виден вовсе — Земля с точностью до процента гладкая.
      camera.params = glm::vec4(relief ? options.relief_scale / 6371000.0f : 0.0f,
                                current_mode.categorical ? 1.0f : 0.0f, 1.0f, float(mode));
      // Четвёртая компонента — резкость смешивания четырёх ближайших клеток, и она приходит РЕЖИМОМ:
      // непрерывное поле сглаживается полностью, категориальное почти не смешивается.
      camera.viewport_near =
        glm::vec4(float(pending_width), float(pending_height), 0.05f, current_mode.blend_sharpness());
      write_buffer(base, "camera_buffer", &camera, sizeof(camera));
      write_buffer(base, "cell_visuals", visuals.data(), visuals.size() * sizeof(cell_visual));
      visuals_dirty = false;

      // ПОДПИСИ МЕСТ. Собираются каждый кадр, и это не расточительство: набор зависит от расстояния до
      // камеры, а оно меняется колесом. Подписей десятки, глифов сотни — сборка стоит микросекунды.
      //
      // Порог по РАЗМЕРУ НА ЭКРАНЕ, а не по размеру области: подпись мельче трёх процентов экрана
      // нечитаема, и сотня нечитаемых подписей скрывает как раз то, что подписывает. Угловой радиус
      // области выводится из её площади (доля площади = доля полного угла), а масштаб экрана — из
      // того, какую часть кадра занимает планета.
      label_glyphs.clear();
      if (!labelled.empty()) {
        const float planet_ndc = std::tan(std::asin(std::min(1.0f / std::max(camera_distance, 1.01f), 1.0f))) /
                                 std::tan(fov * 0.5f);
        const double total_cells = double(cells);
        // РАЗВЕДЕНИЕ ПО ЭКРАНУ. Порог по размеру области отсеивает нечитаемо мелкое, но не спасает от
        // главного: у горизонта диск сжимается, и десяток крупных областей проецируется в одну полосу.
        // Там подписи налезали друг на друга и не читалась ни одна. Поэтому подпись принимается только
        // если её экранное место свободно, а порядок обхода — по убыванию площади, то есть более
        // крупное место побеждает более мелкое.
        // Занятое место хранится ПРЯМОУГОЛЬНИКОМ, а не точкой: у длинного названия и короткого разная
        // ширина, и проверка по расстоянию между центрами пропускала налезание длинных подписей.
        struct occupied {
          glm::vec2 centre;
          glm::vec2 half;
        };
        std::vector<occupied> placed;
        placed.reserve(64);
        size_t shown = 0;
        for (const auto& area : labelled) {
          if (shown >= 48 || label_glyphs.size() + 32 > 8192) {
            break;
          }
          const double share = double(area.cells) / std::max(total_cells, 1.0);
          const float angular = float(std::sqrt(std::max(share, 0.0) * 4.0));
          const float screen = angular * planet_ndc;
          const bool chosen = selected_area > 0.5f && std::abs(area.area - selected_area) < 0.5f;
          if (screen < 0.055f && !chosen) {
            continue;
          }

          // Горизонт и место на экране считаются ЗДЕСЬ, хотя горизонт проверяет и шейдер: шейдер
          // решает, рисовать ли глиф, а здесь решается, тратить ли на подпись бюджет и место.
          const glm::vec3 world_direction =
            glm::normalize(glm::vec3(planet_to_world * glm::vec4(area.centre, 0.0f)));
          if (glm::dot(world_direction, glm::normalize(eye)) < 0.30f) {
            continue;
          }
          const glm::vec4 clip = camera_view_projection * planet_to_world * glm::vec4(area.centre, 1.0f);
          if (clip.w <= 0.0f) {
            continue;
          }
          const glm::vec2 ndc = glm::vec2(clip) / clip.w;
          const auto describe = [&](const std::vector<std::string>& list) -> std::string_view {
            return area.area < list.size() ? std::string_view(list[area.area]) : std::string_view{};
          };
          std::string_view text;
          switch (mode_table[mode].level) {
            case view_mode::named::province: text = describe(names.provinces); break;
            case view_mode::named::land_mass: text = describe(names.land_masses); break;
            case view_mode::named::continent: text = describe(names.continents); break;
            case view_mode::named::historical_region: text = describe(names.historical_regions); break;
            case view_mode::named::ocean: text = describe(names.oceans); break;
            case view_mode::named::duchy: text = describe(names.duchies); break;
            case view_mode::named::empire: text = describe(names.empires); break;
            case view_mode::named::realm: text = describe(names.realms); break;
            default: break;
          }
          if (text.empty()) {
            continue;
          }
          const float height = std::clamp(screen * 0.42f, 0.020f, 0.055f);
          const occupied box{ndc, glm::vec2(float(overlay.font_metrics().text_width(1.0, text)) * height *
                                              0.5f / aspect,
                                            height * 0.75f)};
          bool free_space = true;
          for (const auto& other : placed) {
            if (std::abs(other.centre.x - box.centre.x) < other.half.x + box.half.x &&
                std::abs(other.centre.y - box.centre.y) < other.half.y + box.half.y) {
              free_space = false;
              break;
            }
          }
          if (!free_space && !chosen) {
            continue;
          }
          placed.push_back(box);

          append_label(label_glyphs, overlay.font_metrics(), text, area.centre, height, font_texture,
                       chosen ? glm::vec3(1.0f, 0.90f, 0.62f) : glm::vec3(0.96f, 0.96f, 0.93f));
          ++shown;
        }
      }
      write_buffer(base, "label_glyphs", label_glyphs.data(), label_glyphs.size() * sizeof(label_glyph));
      const VkDrawIndirectCommand label_command{uint32_t(label_glyphs.size() * 6), 1u, 0u, 0u};
      base.write_constant_data(base.find_constant("label_draw"), label_command);

      const VkDrawIndirectCommand planet_command{uint32_t(surface.size()), 1u, 0u, 0u};
      base.write_constant_data(base.find_constant("planet_draw"), planet_command);
      base.update_event();

      const uint64_t delta_us = uint64_t(std::max(real_dt, 1.0e-6f) * 1.0e6f);
      const uint64_t stamp_us =
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count());
      overlay.update(delta_us, stamp_us);
      // Спрятанный оверлей — это НОЛЬ КОМАНД отрисовки, а не прозрачный цвет: команд нет, значит нет
      // и вершин, и шаг рисует пустоту за одну проверку вместо целого атласа полупрозрачных четвёрок.
      write_overlay(base, overlay, overlay_visible);
      context.prepare();
      context.draw();
      base.submit_frame();
      pacer.wait();

      ++drawn_frames;
      if (options.frames != 0 && drawn_frames >= options.frames) {
        break;
      }
    }

    vk::Device(device).waitIdle();
    if (!options.dump_path.empty()) {
      dump_scene(base, options.dump_path);
      utils::info("GN02 viewer: frame saved to '{}'", options.dump_path);
    }
    base.dump_cache_on_disk(cache_path);
  }

  vk::Instance(instance).destroy(surface_handle);
  input::destroy(window);
  vk::Device(device).destroy();
  painter::destroy_debug_messenger(instance, messenger);
  vk::Instance(instance).destroy();
  return 0;
}

} // namespace devils_engine::gn02
