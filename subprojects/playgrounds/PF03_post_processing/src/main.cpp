#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
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
#include "devils_engine/playground/free_camera.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"

using namespace devils_engine;

namespace {

constexpr uint32_t initial_width = 1280;
constexpr uint32_t initial_height = 720;
constexpr uint32_t dispatch_tile = 8;
constexpr float near_plane = 0.1f;
// Детерминированный шаг для орбиты: положение камеры становится чистой функцией номера кадра, поэтому два
// прогона с одинаковым --frames дают одинаковую картинку и ошибку репроекции можно сравнивать численно.
constexpr float orbit_step_seconds = 1.0f / 60.0f;

// Отладочные виды; порядок обязан совпадать с PF03_DEBUG_* в resources/shaders/pf03_frame.glsl
constexpr std::array<std::string_view, 17> debug_names = {
  "shaded", "depth", "normal", "motion", "reprojected", "error(motion)", "error(no motion)",
  "clipping", "calibration", "exposure", "transmittance", "ao", "ao raw", "taa rejection",
  "bloom", "shafts", "sharpen"};

uint32_t pending_width = initial_width;
uint32_t pending_height = initial_height;
bool resize_pending = false;
bool reset_requested = false;
int32_t escape_key = -1;
int32_t reset_key = -1;
std::array<int32_t, debug_names.size()> debug_keys{};
uint32_t debug_mode = 0;

void error_callback(const int error, const char* message) noexcept {
  utils::warn("PF03 input error {}: {}", error, message);
}

void key_callback(GLFWwindow* window, const int key, const int scancode, const int action, const int) noexcept {
  input::events::update_key(scancode, action);
  if (action != 1) {
    return;
  }
  if (key == escape_key) {
    input::set_should_close(window, true);
  }
  if (key == reset_key) {
    reset_requested = true;
  }
  for (uint32_t i = 0; i < debug_keys.size(); ++i) {
    if (key == debug_keys[i]) {
      debug_mode = i;
      utils::info("PF03 debug view {}: {}", i, debug_names[i]);
    }
  }
}

void framebuffer_callback(GLFWwindow*, const int width, const int height) noexcept {
  pending_width = width > 0 ? uint32_t(width) : 0u;
  pending_height = height > 0 ? uint32_t(height) : 0u;
  resize_pending = pending_width != 0 && pending_height != 0;
}

// Последовательность Халтона: низкодискрепансный набор субпиксельных смещений. Случайный джиттер сходился бы
// хуже — точки сбиваются в кучки и часть площади пикселя не сэмплируется вовсе, а регулярная сетка даёт
// видимую периодичность. Основания 2 и 3 взаимно просты, поэтому пара координат не коррелирует.
float halton(uint index, const uint base) {
  float result = 0.0f;
  float inverse = 1.0f / float(base);
  while (index > 0u) {
    result += float(index % base) * inverse;
    index /= base;
    inverse /= float(base);
  }
  return result;
}

// Раскладка обязана совпадать с PF03_FRAME_BLOCK_BODY (resources/shaders/pf03_frame.glsl)
struct alignas(16) frame_block {
  glm::mat4 view_projection;
  glm::mat4 previous_view_projection;
  glm::mat4 inverse_view_projection;
  glm::vec4 camera_position;
  glm::vec4 viewport_near;
  glm::vec4 controls;
  glm::vec4 light_direction;
  glm::vec4 tonemap;
  glm::vec4 exposure_limits;
  glm::vec4 fog_params;
  glm::vec4 fog_color;
  glm::vec4 ao_params;
  glm::vec4 taa_params;
  glm::vec4 taa_jitter;
  glm::vec4 bloom_params;
  glm::vec4 shaft_params;
  glm::vec4 lens_params;
  glm::vec4 output_params;
};
static_assert(sizeof(frame_block) == 432);

struct vertex {
  float px, py, pz;
  float nx, ny, nz;
};

void add_quad(
  std::vector<vertex>& out,
  const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d,
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

// Комната статична: она даёт motion только от камеры и держит шахматный пол как стресс-паттерн.
std::vector<vertex> make_room() {
  std::vector<vertex> out;
  constexpr float room_x = 9.0f;
  constexpr float room_y = 3.0f;
  constexpr float room_z = 9.0f;
  add_quad(out, {-room_x, -room_y, room_z}, {room_x, -room_y, room_z}, {room_x, -room_y, -room_z}, {-room_x, -room_y, -room_z}, {0, 1, 0});
  add_quad(out, {-room_x, -room_y, -room_z}, {room_x, -room_y, -room_z}, {room_x, room_y, -room_z}, {-room_x, room_y, -room_z}, {0, 0, 1});
  add_quad(out, {room_x, -room_y, room_z}, {-room_x, -room_y, room_z}, {-room_x, room_y, room_z}, {room_x, room_y, room_z}, {0, 0, -1});
  add_quad(out, {-room_x, -room_y, room_z}, {-room_x, -room_y, -room_z}, {-room_x, room_y, -room_z}, {-room_x, room_y, room_z}, {1, 0, 0});

  // Колонны у дальней стены. Нужны не для красоты: экранные световые лучи возникают там, где свет проходит
  // МЕЖДУ затенителями. Радиальное размытие большого сплошного неба даёт просто засветку кадра, а не шахты —
  // без разрывов в маске эффект нечем показать и нечем измерить.
  for (uint32_t i = 0; i < 5; ++i) {
    const float x = -6.0f + float(i) * 3.0f;
    add_box(out, {x, 0.4f, -7.5f}, {0.45f, 3.4f, 0.45f});
  }
  return out;
}

std::vector<vertex> make_cube() {
  std::vector<vertex> out;
  add_box(out, {0.0f, 0.0f, 0.0f}, {0.7f, 0.7f, 0.7f});
  return out;
}

// Запись трансформа объекта в per_frame ресурсе object_transforms. Прошлый кадр здесь НЕ дублируется:
// вершинный шейдер берёт его из истории того же ресурса, потому что период у него честно кадровый.
struct object_transform {
  glm::vec4 offset; // xyz — смещение, w — угол поворота вокруг Y
  glm::vec4 reserved;
};

constexpr uint32_t mover_count = 6;

// Движение объектов — чистая функция НОМЕРА кадра, как и орбита камеры: только так дамп двух режимов
// сравним попиксельно.
//
// Перенос и вращение разведены по отдельным ручкам сознательно. Вращение меняет нормаль, а от неё зависит
// затенение, поэтому у вращающейся поверхности точка меняет цвет между кадрами — репроекция такое не
// исправляет ПО ОПРЕДЕЛЕНИЮ (этим займётся neighbourhood clamp в TAA). Чтобы мерить сами векторы, нужен
// инвариантный сигнал, то есть чистый перенос: --object-spin=0.
glm::vec4 mover_offset(const uint32_t index, const int64_t frame_index, const float speed, const float spin) {
  const float phase = float(frame_index) * orbit_step_seconds * speed;
  const float lane = float(index);
  const float radius = 1.6f + 0.45f * lane;
  const float angle = phase * (0.6f + 0.18f * lane) + lane * 1.05f;
  return glm::vec4(
    std::cos(angle) * radius,
    -2.3f + 0.55f * std::sin(phase * 1.7f + lane),
    std::sin(angle) * radius - 1.0f,
    float(frame_index) * orbit_step_seconds * spin * (0.9f + 0.25f * lane));
}

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

void update_dispatch_constant(
  painter::graphics_base& base, const std::string_view name, const uint32_t width, const uint32_t height) {
  const uint32_t slot = base.find_constant(name);
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF03 constant '{}' is absent from the configured graph", name);
  }
  const VkDispatchIndirectCommand command{
    (width + dispatch_tile - 1u) / dispatch_tile,
    (height + dispatch_tile - 1u) / dispatch_tile,
    1u};
  base.write_constant_data(slot, command);
}

void update_screen_dispatch(painter::graphics_base& base, const uint32_t width, const uint32_t height) {
  update_dispatch_constant(base, "screen_dispatch", width, height);
  // SSAO считается в половинном разрешении, поэтому у него своя сетка групп
  update_dispatch_constant(base, "half_dispatch", (width + 1u) / 2u, (height + 1u) / 2u);
  // Уровни пирамиды: сетка групп на каждый свой размер, иначе шаг либо не покроет уровень, либо выйдет за него
  update_dispatch_constant(base, "quarter_dispatch", (width + 3u) / 4u, (height + 3u) / 4u);
  update_dispatch_constant(base, "eighth_dispatch", (width + 7u) / 8u, (height + 7u) / 8u);
  update_dispatch_constant(base, "sixteenth_dispatch", (width + 15u) / 16u, (height + 15u) / 16u);
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

// sf4 хранится половинной точностью, поэтому дамп сам разворачивает half в float: тащить ради этого
// зависимость не за чем, а формат фиксированный (IEEE binary16).
float half_to_float(const uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1fu;
  const uint32_t mantissa = h & 0x3ffu;

  if (exponent == 0) {
    if (mantissa == 0) {
      return std::bit_cast<float>(sign);
    }
    // денормал: нормализуем сдвигом
    uint32_t e = 0;
    uint32_t m = mantissa;
    while ((m & 0x400u) == 0) {
      m <<= 1;
      e += 1;
    }
    m &= 0x3ffu;
    const uint32_t bits = sign | ((127 - 15 - e + 1) << 23) | (m << 13);
    return std::bit_cast<float>(bits);
  }

  if (exponent == 0x1fu) {
    return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));
  }

  const uint32_t bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  return std::bit_cast<float>(bits);
}

// Кадр-точный дамп итоговой картинки в PPM. Скриншот внешним инструментом для измерений не годится:
// он приходит в произвольный момент, а сравнивать режимы надо на ОДНОМ И ТОМ ЖЕ кадре — иначе камера
// успевает уехать, и разница между «ошибка с motion» и «ошибка без motion» тонет в разнице фаз.
void dump_composed_image(painter::graphics_base& base, const std::string& path) {
  const uint32_t slot = base.find_resource("composed_color");
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF03 resource 'composed_color' is absent from the configured graph");
  }

  const auto frame = base.get_current_image_resource_frame(slot);
  const auto [width, height] = base.swapchain_extent();
  const size_t texel_size = sizeof(uint16_t) * 4; // sf4 = R16G16B16A16_SFLOAT
  const size_t bytes = size_t(width) * size_t(height) * texel_size;

  vma::Allocator allocator(base.allocator);
  vk::BufferCreateInfo bci{};
  bci.usage = vk::BufferUsageFlagBits::eTransferDst;
  bci.size = bytes;
  vma::AllocationCreateInfo aci{};
  aci.usage = vma::MemoryUsage::eGpuToCpu;
  aci.flags = vma::AllocationCreateFlagBits::eMapped;
  auto [staging, allocation] = allocator.createBuffer(bci, aci);

  vk::Device dev(base.device);
  vk::CommandBufferAllocateInfo cbai{};
  cbai.commandPool = base.command_pool;
  cbai.level = vk::CommandBufferLevel::ePrimary;
  cbai.commandBufferCount = 1;
  const auto buffers = dev.allocateCommandBuffers(cbai);
  vk::CommandBuffer task(buffers[0]);
  task.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

  // Пасс презентации оставляет composed_color в transfer_src, поэтому переход layout не нужен — нужна
  // только видимость записи для копирования.
  vk::ImageMemoryBarrier before{};
  before.srcAccessMask = vk::AccessFlagBits::eTransferRead;
  before.dstAccessMask = vk::AccessFlagBits::eTransferRead;
  before.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
  before.newLayout = vk::ImageLayout::eTransferSrcOptimal;
  before.image = frame.handle;
  before.subresourceRange = std::bit_cast<vk::ImageSubresourceRange>(frame.sub);
  task.pipelineBarrier(
    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
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
    vk::SubmitInfo si{};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &task;
    const auto queue_lock = base.graphics.lock();
    if (vk::Queue(base.graphics.handle()).submit(1, &si, fence) != vk::Result::eSuccess) {
      utils::error{}("PF03 could not submit the image dump copy");
    }
  }
  if (dev.waitForFences(fence, true, size_t(1000) * 1000 * 1000) != vk::Result::eSuccess) {
    utils::error{}("PF03 image dump did not finish in time");
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
  // аргументы file_io::write — (bytes, path): порядок обратный привычному, поэтому пишем явно
  if (!file_io::write(std::span<const char>(ppm.data(), ppm.size()), path, file_io::type::binary)) {
    utils::error{}("PF03 could not write the dump to '{}'", path);
  }

  dev.destroy(fence);
  dev.freeCommandBuffers(base.command_pool, task);
  allocator.destroyBuffer(staging, allocation);
  utils::info("PF03 dumped {}x{} composed frame to '{}'", width, height, path);
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

void bind_key(const std::string_view event, const std::string_view canonical) {
  const auto [key, scancode] = input::key_from_canonical(canonical);
  if (key < 0 || scancode < 0) {
    utils::error{}("PF03 could not resolve input key '{}'", canonical);
  }
  input::events::set_key(event, scancode, key);
}

} // namespace

int main(int argc, char** argv) {
  bool validation = false;
  bool uncapped = false;
  bool verbose = false;
  bool camera_locked = false;
  uint32_t frame_limit = 0; // 0 — до Esc; иначе выходим сами (детерминированные прогоны)
  float orbit_speed = 0.0f;  // > 0 — камера идёт по орбите как функция НОМЕРА кадра
  float object_speed = 1.0f; // скорость переноса движущихся кубов; 0 — сцена полностью статична
  float object_spin = 0.6f;  // скорость вращения кубов; 0 нужен для измерения самих motion-векторов
  uint32_t tonemap_operator = 3; // 0 none, 1 reinhard, 2 hable, 3 aces
  float manual_exposure = 0.0f;  // > 0 — фиксированная экспозиция вместо автоматической
  float adapt_rate = 2.2f;       // скорость привыкания, 1/секунда
  float sun_intensity = 6.0f;    // яркость солнца: задаёт динамический диапазон кадра
  // Доля неба в освещении. SSAO модулирует ИМЕННО ambient, поэтому в солнечно-доминированной сцене он
  // слаб по построению, а в пасмурной или в помещении становится главным источником объёма.
  float ambient_fraction = 0.12f;
  bool encode_srgb = false;      // тракт презентации сам кодирует sRGB — измерено видом 8
  float fog_density = 0.035f;    // плотность тумана у опорной высоты, 1/метр
  float fog_falloff = 6.0f;      // масштаб спада плотности по высоте, метры
  float fog_height = -3.0f;      // опорная высота: пол комнаты
  float fog_anisotropy = 0.62f;  // > 0 — рассеяние вперёд, туман светится в сторону солнца
  float ao_radius = 0.55f;       // радиус выборки SSAO в метрах
  float ao_intensity = 1.8f;     // сила затенения
  float ao_power = 1.8f;         // показатель контраста: компенсирует систематическую недооценку оценщика
  bool taa_enabled = true;
  // Режим отбраковки истории: 0 — нет, 1 — жёсткая коробка min/max, 2 — клип по дисперсии в YCoCg.
  // Жёсткая коробка отбраковывает историю при любом движении камеры и возвращает ступеньку на далёких
  // кромках; клип по дисперсии допускает правдоподобное отклонение.
  // По умолчанию жёсткая коробка: с фильтром Catmull-Rom она ИЗМЕРЕННО точнее (536 против 554 по отклонению
  // от эталона). Режим 3 (по скорости) держит больше истории — меньше ступенек на движении, но больше отставания.
  uint32_t taa_reject = 1;
  // Фильтр выборки истории: bilinear копит размытие под движением, Catmull-Rom его компенсирует
  bool taa_catmull_rom = true;
  float bloom_intensity = 0.06f;   // вклад пирамиды в кадр
  float bloom_threshold = 1.0f;    // порог в единицах ПОСЛЕ экспозиции: «ярче, чем зритель считает белым»
  float bloom_knee = 0.5f;         // мягкость колена порога
  float bloom_up_weight = 0.75f;   // вес каждого шага подъёма: меньше — компактнее свечение
  float shaft_intensity = 0.08f;   // сила лучей: это доля рассеянного света, а не яркость сама по себе
  float shaft_falloff = 2.5f;      // затухание вдоль луча
  float sharpen = 0.35f;           // резкость после накопления: TAA неизбежно размывает историю
  float vignette = 0.25f;          // затемнение краёв
  float aberration = 0.0f;         // хроматическая аберрация; по умолчанию выключена как приём на вкус
  float grain = 0.0f;              // зерно; тоже на вкус
  bool dither = true;              // дизер перед 8-битным выводом: почти бесплатно, убирает бандинг
  float taa_weight = 0.92f;       // вес истории: больше — стабильнее и мылее
  uint32_t taa_phases = 8;        // длина последовательности джиттера
  float jitter_scale = 1.0f;      // 0 — джиттер выключен (тогда накапливать нечего)
  float ao_bias = 0.08f;         // порог по касательной плоскости: ниже него затенитель считается компланарным
  bool ao_enabled = true;
  uint32_t ao_samples = 0; // != 0 — переопределяем specialization-константу шага до сборки графа
  glm::vec3 sun_direction{0.45f, 0.82f, 0.35f}; // направление НА солнце; у горизонта туман выглядит иначе
  std::string dump_path;    // непусто — на последнем кадре пишем итоговую картинку в PPM
  float motion_gain = 6.0f;
  float error_gain = 8.0f;
  glm::vec3 start_position{0.0f, -0.4f, 6.5f};
  float start_yaw = -1.5707963f;
  float start_pitch = -0.05f;

  for (int i = 1; i < argc; ++i) {
    const std::string_view option(argv[i]);
    validation = validation || option == "--validation";
    uncapped = uncapped || option == "--uncapped";
    verbose = verbose || option == "--verbose";
    camera_locked = camera_locked || option == "--lock-camera";

    constexpr std::string_view frames_prefix = "--frames=";
    if (option.starts_with(frames_prefix)) {
      frame_limit = uint32_t(std::stoul(std::string(option.substr(frames_prefix.size()))));
    }
    constexpr std::string_view debug_prefix = "--debug=";
    if (option.starts_with(debug_prefix)) {
      debug_mode = std::min<uint32_t>(uint32_t(std::stoul(std::string(option.substr(debug_prefix.size())))), debug_names.size() - 1);
    }
    constexpr std::string_view sharpen_prefix = "--sharpen=";
    if (option.starts_with(sharpen_prefix)) {
      sharpen = std::stof(std::string(option.substr(sharpen_prefix.size())));
    }
    constexpr std::string_view vignette_prefix = "--vignette=";
    if (option.starts_with(vignette_prefix)) {
      vignette = std::stof(std::string(option.substr(vignette_prefix.size())));
    }
    constexpr std::string_view aberration_prefix = "--aberration=";
    if (option.starts_with(aberration_prefix)) {
      aberration = std::stof(std::string(option.substr(aberration_prefix.size())));
    }
    constexpr std::string_view grain_prefix = "--grain=";
    if (option.starts_with(grain_prefix)) {
      grain = std::stof(std::string(option.substr(grain_prefix.size())));
    }
    constexpr std::string_view dither_prefix = "--dither=";
    if (option.starts_with(dither_prefix)) {
      dither = std::stoi(std::string(option.substr(dither_prefix.size()))) != 0;
    }
    constexpr std::string_view bloom_prefix = "--bloom=";
    if (option.starts_with(bloom_prefix)) {
      bloom_intensity = std::stof(std::string(option.substr(bloom_prefix.size())));
    }
    constexpr std::string_view bloom_threshold_prefix = "--bloom-threshold=";
    if (option.starts_with(bloom_threshold_prefix)) {
      bloom_threshold = std::stof(std::string(option.substr(bloom_threshold_prefix.size())));
    }
    constexpr std::string_view bloom_up_prefix = "--bloom-spread=";
    if (option.starts_with(bloom_up_prefix)) {
      bloom_up_weight = std::stof(std::string(option.substr(bloom_up_prefix.size())));
    }
    constexpr std::string_view shafts_prefix = "--shafts=";
    if (option.starts_with(shafts_prefix)) {
      shaft_intensity = std::stof(std::string(option.substr(shafts_prefix.size())));
    }
    constexpr std::string_view shaft_falloff_prefix = "--shafts-falloff=";
    if (option.starts_with(shaft_falloff_prefix)) {
      shaft_falloff = std::stof(std::string(option.substr(shaft_falloff_prefix.size())));
    }
    constexpr std::string_view taa_prefix = "--taa=";
    if (option.starts_with(taa_prefix)) {
      taa_enabled = std::stoi(std::string(option.substr(taa_prefix.size()))) != 0;
    }
    constexpr std::string_view taa_clamp_prefix = "--taa-clamp=";
    if (option.starts_with(taa_clamp_prefix)) {
      const auto name = option.substr(taa_clamp_prefix.size());
      if (name == "0" || name == "none") taa_reject = 0;
      else if (name == "1" || name == "minmax") taa_reject = 1;
      else if (name == "2" || name == "variance") taa_reject = 2;
      else if (name == "3" || name == "velocity") taa_reject = 3;
      else utils::error{}("PF03 unknown TAA rejection mode '{}' (none|minmax|variance|velocity)", name);
    }
    constexpr std::string_view taa_filter_prefix = "--taa-filter=";
    if (option.starts_with(taa_filter_prefix)) {
      const auto name = option.substr(taa_filter_prefix.size());
      if (name == "bilinear") taa_catmull_rom = false;
      else if (name == "catmullrom") taa_catmull_rom = true;
      else utils::error{}("PF03 unknown TAA history filter '{}' (bilinear|catmullrom)", name);
    }
    constexpr std::string_view taa_weight_prefix = "--taa-weight=";
    if (option.starts_with(taa_weight_prefix)) {
      taa_weight = std::stof(std::string(option.substr(taa_weight_prefix.size())));
    }
    constexpr std::string_view jitter_prefix = "--jitter=";
    if (option.starts_with(jitter_prefix)) {
      jitter_scale = std::stof(std::string(option.substr(jitter_prefix.size())));
    }
    constexpr std::string_view ao_prefix = "--ao=";
    if (option.starts_with(ao_prefix)) {
      ao_enabled = std::stoi(std::string(option.substr(ao_prefix.size()))) != 0;
    }
    constexpr std::string_view ao_samples_prefix = "--ao-samples=";
    if (option.starts_with(ao_samples_prefix)) {
      ao_samples = uint32_t(std::stoul(std::string(option.substr(ao_samples_prefix.size()))));
    }
    constexpr std::string_view ao_radius_prefix = "--ao-radius=";
    if (option.starts_with(ao_radius_prefix)) {
      ao_radius = std::stof(std::string(option.substr(ao_radius_prefix.size())));
    }
    constexpr std::string_view ao_intensity_prefix = "--ao-intensity=";
    if (option.starts_with(ao_intensity_prefix)) {
      ao_intensity = std::stof(std::string(option.substr(ao_intensity_prefix.size())));
    }
    constexpr std::string_view ao_power_prefix = "--ao-power=";
    if (option.starts_with(ao_power_prefix)) {
      ao_power = std::stof(std::string(option.substr(ao_power_prefix.size())));
    }
    constexpr std::string_view ao_bias_prefix = "--ao-bias=";
    if (option.starts_with(ao_bias_prefix)) {
      ao_bias = std::stof(std::string(option.substr(ao_bias_prefix.size())));
    }
    constexpr std::string_view sun_dir_prefix = "--sun-dir=";
    if (option.starts_with(sun_dir_prefix)) {
      const auto text = std::string(option.substr(sun_dir_prefix.size()));
      std::array<float, 3> values{sun_direction.x, sun_direction.y, sun_direction.z};
      size_t begin = 0;
      for (uint32_t v = 0; v < values.size() && begin <= text.size(); ++v) {
        const auto end = text.find(',', begin);
        values[v] = std::stof(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
      }
      sun_direction = {values[0], values[1], values[2]};
    }
    constexpr std::string_view fog_prefix = "--fog=";
    if (option.starts_with(fog_prefix)) {
      fog_density = std::stof(std::string(option.substr(fog_prefix.size())));
    }
    constexpr std::string_view fog_height_prefix = "--fog-height=";
    if (option.starts_with(fog_height_prefix)) {
      fog_falloff = std::stof(std::string(option.substr(fog_height_prefix.size())));
    }
    constexpr std::string_view fog_aniso_prefix = "--fog-anisotropy=";
    if (option.starts_with(fog_aniso_prefix)) {
      fog_anisotropy = std::stof(std::string(option.substr(fog_aniso_prefix.size())));
    }
    constexpr std::string_view tonemap_prefix = "--tonemap=";
    if (option.starts_with(tonemap_prefix)) {
      const auto name = option.substr(tonemap_prefix.size());
      if (name == "none") tonemap_operator = 0;
      else if (name == "reinhard") tonemap_operator = 1;
      else if (name == "hable") tonemap_operator = 2;
      else if (name == "aces") tonemap_operator = 3;
      else utils::error{}("PF03 unknown tonemap operator '{}' (none|reinhard|hable|aces)", name);
    }
    constexpr std::string_view exposure_prefix = "--exposure=";
    if (option.starts_with(exposure_prefix)) {
      manual_exposure = std::stof(std::string(option.substr(exposure_prefix.size())));
    }
    constexpr std::string_view adapt_prefix = "--adapt-rate=";
    if (option.starts_with(adapt_prefix)) {
      adapt_rate = std::stof(std::string(option.substr(adapt_prefix.size())));
    }
    constexpr std::string_view ambient_prefix = "--ambient=";
    if (option.starts_with(ambient_prefix)) {
      ambient_fraction = std::stof(std::string(option.substr(ambient_prefix.size())));
    }
    constexpr std::string_view sun_prefix = "--sun=";
    if (option.starts_with(sun_prefix)) {
      sun_intensity = std::stof(std::string(option.substr(sun_prefix.size())));
    }
    constexpr std::string_view srgb_prefix = "--encode-srgb=";
    if (option.starts_with(srgb_prefix)) {
      encode_srgb = std::stoi(std::string(option.substr(srgb_prefix.size()))) != 0;
    }
    constexpr std::string_view object_prefix = "--object-speed=";
    if (option.starts_with(object_prefix)) {
      object_speed = std::stof(std::string(option.substr(object_prefix.size())));
    }
    constexpr std::string_view spin_prefix = "--object-spin=";
    if (option.starts_with(spin_prefix)) {
      object_spin = std::stof(std::string(option.substr(spin_prefix.size())));
    }
    constexpr std::string_view orbit_prefix = "--orbit=";
    if (option.starts_with(orbit_prefix)) {
      orbit_speed = std::stof(std::string(option.substr(orbit_prefix.size())));
    }
    constexpr std::string_view motion_gain_prefix = "--motion-gain=";
    if (option.starts_with(motion_gain_prefix)) {
      motion_gain = std::stof(std::string(option.substr(motion_gain_prefix.size())));
    }
    constexpr std::string_view error_gain_prefix = "--error-gain=";
    if (option.starts_with(error_gain_prefix)) {
      error_gain = std::stof(std::string(option.substr(error_gain_prefix.size())));
    }
    constexpr std::string_view dump_prefix = "--dump=";
    if (option.starts_with(dump_prefix)) {
      dump_path = std::string(option.substr(dump_prefix.size()));
    }
    constexpr std::string_view camera_prefix = "--camera-at=";
    if (option.starts_with(camera_prefix)) {
      const auto text = std::string(option.substr(camera_prefix.size()));
      std::array<float, 5> values{start_position.x, start_position.y, start_position.z, start_yaw, start_pitch};
      size_t begin = 0;
      for (uint32_t v = 0; v < values.size() && begin <= text.size(); ++v) {
        const auto end = text.find(',', begin);
        values[v] = std::stof(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) {
          break;
        }
        begin = end + 1;
      }
      start_position = {values[0], values[1], values[2]};
      start_yaw = values[3];
      start_pitch = values[4];
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

  GLFWwindow* window = input::create_window(initial_width, initial_height, "PF03 — thin G-buffer + reprojection");
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

    // Число проб SSAO — specialization-константа шага, поэтому переопределяется в РАСПАРСЕННОМ конфиге до
    // сборки графа: так тир качества меняется пересборкой pipeline, а не правкой файла на диске (приём из PF02).
    if (ao_samples != 0) {
      const auto slot = render_config.find_execution_step("compute_ao");
      if (slot == painter::invalid_resource_slot) {
        utils::error{}("PF03 step 'compute_ao' is absent from the configured graph");
      }
      auto& constants = render_config.steps[slot].shader_constants;
      const auto found = std::find_if(constants.begin(), constants.end(), [](const auto& entry) {
        return entry.first == "ao_samples";
      });
      if (found == constants.end()) {
        utils::error{}("PF03 step 'compute_ao' has no 'ao_samples' shader constant to override");
      }
      found->second = std::to_string(ao_samples);
      utils::info("PF03 shader constant override: ao_samples = {}", ao_samples);
    }

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
    const auto room_mesh = upload_mesh("pf03.room", room);
    const auto cube_mesh = upload_mesh("pf03.cube", cube);

    const uint32_t draw_group = base.find_draw_group("scene_draw_group");
    const uint32_t room_pair = base.register_pair(draw_group, room_mesh, 1);
    const uint32_t mover_pair = base.register_pair(draw_group, cube_mesh, mover_count);

    // В инстанс-лейн пишем ТОЛЬКО индекс объекта, и один раз: это константа, поэтому общий на все кадры в
    // полёте per_update буфер тут безопасен. Всё, что меняется каждый кадр, живёт в object_transforms.
    for (uint32_t offset = 0; offset < base.frames_in_flight(); ++offset) {
      const auto room_instances = base.get_current_instance_resource_frame(room_pair, offset);
      const glm::vec4 room_index{0.0f, 0.0f, 0.0f, 0.0f};
      std::memcpy(static_cast<uint8_t*>(room_instances.mapped) + room_instances.sub.offset, &room_index, sizeof(room_index));

      const auto mover_instances = base.get_current_instance_resource_frame(mover_pair, offset);
      std::array<glm::vec4, mover_count> mover_indices{};
      for (uint32_t i = 0; i < mover_count; ++i) {
        mover_indices[i] = glm::vec4(float(i + 1), 0.0f, 0.0f, 0.0f);
      }
      std::memcpy(static_cast<uint8_t*>(mover_instances.mapped) + mover_instances.sub.offset, mover_indices.data(), sizeof(mover_indices));

      const auto room_indirect = base.get_current_indirect_resource_frame(room_pair, offset);
      VkDrawIndirectCommand room_command{};
      room_command.vertexCount = uint32_t(room.size());
      room_command.instanceCount = 1;
      std::memcpy(static_cast<uint8_t*>(room_indirect.mapped) + room_indirect.sub.offset, &room_command, sizeof(room_command));

      const auto mover_indirect = base.get_current_indirect_resource_frame(mover_pair, offset);
      VkDrawIndirectCommand mover_command{};
      mover_command.vertexCount = uint32_t(cube.size());
      mover_command.instanceCount = mover_count;
      std::memcpy(static_cast<uint8_t*>(mover_indirect.mapped) + mover_indirect.sub.offset, &mover_command, sizeof(mover_command));
    }

    input::events::clear_bindings();
    bind_key("camera_forward", "key_w");
    bind_key("camera_back", "key_s");
    bind_key("camera_left", "key_a");
    bind_key("camera_right", "key_d");
    bind_key("camera_down", "key_q");
    bind_key("camera_up", "key_e");
    bind_key("camera_fast", "left_shift");
    escape_key = input::glfw_key_from_canonical("escape");
    reset_key = input::glfw_key_from_canonical("key_r");
    for (uint32_t i = 0; i < debug_keys.size(); ++i) {
      debug_keys[i] = input::glfw_key_from_canonical("key_" + std::to_string(i));
    }
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    if (!camera_locked) {
      input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_DISABLED);
      input::set_raw_mouse_motion(window);
    }

    playground::free_camera camera;
    camera.position = start_position;
    camera.yaw = start_yaw;
    camera.pitch = start_pitch;

    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;

    report_resource_copies(base, "scene_color");
    report_resource_copies(base, "gbuffer_motion");
    utils::info("PF03 graph: thin G-buffer (depth+normal+motion) -> shade -> compose/reproject -> present");
    utils::info(
      "PF03 views: 0 shaded, 1 depth, 2 normal, 3 motion, 4 reprojected, 5 error(motion), 6 error(no motion), "
      "7 clipping, 8 calibration, 9 exposure, 10 transmittance, 11 ao, 12 ao raw, 13 taa rejection, "
      "14 bloom, 15 shafts, 16 sharpen");
    utils::info(
      "PF03 SSAO: {}, radius {} m, intensity {}, bias {} (half resolution + depth-aware blur)",
      ao_enabled ? "on" : "off", ao_radius, ao_intensity, ao_bias);
    utils::info("PF03 SSAO contrast power {}", ao_power);
    utils::info(
      "PF03 TAA: {}, history weight {}, rejection {}, jitter phases {}",
      taa_enabled ? "on" : "off", taa_weight,
      taa_reject == 0 ? "none" : (taa_reject == 1 ? "minmax box" : (taa_reject == 2 ? "variance clip (YCoCg)" : "velocity-scaled clip (YCoCg)")),
      taa_phases);
    utils::info("PF03 TAA history filter: {}", taa_catmull_rom ? "Catmull-Rom" : "bilinear");
    utils::info(
      "PF03 bloom: intensity {}, threshold {}, knee {}, spread {} (4-level pyramid)",
      bloom_intensity, bloom_threshold, bloom_knee, bloom_up_weight);
    utils::info("PF03 light shafts: intensity {}, falloff {} (screen-space, sun must be on screen)", shaft_intensity, shaft_falloff);
    utils::info(
      "PF03 output: sharpen {}, vignette {}, aberration {}, grain {}, dither {}",
      sharpen, vignette, aberration, grain, dither ? "on" : "off");
    utils::info("PF03 lighting: sun {}, ambient fraction {} (SSAO модулирует только ambient)", sun_intensity, ambient_fraction);
    utils::info(
      "PF03 fog: density {}, height falloff {} m, reference {} m, anisotropy {}",
      fog_density, fog_falloff, fog_height, fog_anisotropy);
    utils::info(
      "PF03 tone mapping: operator {}, exposure {}, adapt rate {}/s, sun {}",
      tonemap_operator, manual_exposure > 0.0f ? std::to_string(manual_exposure) : std::string("auto"),
      adapt_rate, sun_intensity);
    utils::info("PF03 controls: WASD/QE move, mouse look, R reset history, Esc exit");
    utils::info("PF03 scene: static room (camera motion only) + {} moving cubes (per-object motion)", mover_count);

    auto previous_time = std::chrono::steady_clock::now();
    const auto loop_start = previous_time;
    auto [mouse_x, mouse_y] = input::cursor_pos(window);
    playground::frame_pacer frame_pacer(uncapped ? 0u : 60u);
    uint32_t frames_since_reset = 0;
    uint32_t frames_total = 0;
    glm::mat4 previous_view_projection(1.0f);
    glm::vec2 previous_jitter_uv(0.0f);

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

      if (orbit_speed > 0.0f) {
        // Орбита — функция номера кадра, а не стенных часов: только так две записи сравнимы попиксельно
        const float phase = float(frames_total) * orbit_step_seconds * orbit_speed;
        camera.position = glm::vec3(std::sin(phase) * 5.5f, -0.4f + std::sin(phase * 0.7f) * 0.35f, std::cos(phase) * 5.5f);
        camera.yaw = -phase - 1.5707963f;
        camera.pitch = -0.06f;
      } else if (!camera_locked) {
        const auto [next_mouse_x, next_mouse_y] = input::cursor_pos(window);
        playground::camera_motion motion;
        motion.forward = float(input::events::is_pressed("camera_forward")) - float(input::events::is_pressed("camera_back"));
        motion.right = float(input::events::is_pressed("camera_right")) - float(input::events::is_pressed("camera_left"));
        motion.up = float(input::events::is_pressed("camera_up")) - float(input::events::is_pressed("camera_down"));
        motion.fast = input::events::is_pressed("camera_fast");
        motion.look_delta = {float(next_mouse_x - mouse_x), float(next_mouse_y - mouse_y)};
        mouse_x = next_mouse_x;
        mouse_y = next_mouse_y;
        camera.update(motion, dt);
      }

      if (!base.can_draw()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }

      base.prepare_frame();

      const float aspect = float(std::max(pending_width, 1u)) / float(std::max(pending_height, 1u));
      const auto view = camera.view();
      auto projection = playground::infinite_reverse_z_projection(glm::radians(68.0f), aspect, near_plane);

      // Субпиксельный джиттер — это НЕ постобработка, а смещение самой проекции: каждый кадр сцена
      // сэмплируется в других точках внутри пикселя, и накопление превращает это в настоящий суперсэмплинг.
      // Сдвиг добавляется в третий столбец, поэтому масштабируется вместе с w и работает на любой глубине.
      const uint32_t phase = frames_total % std::max(taa_phases, 1u);
      // Джиттер НЕ привязан к включённости накопления сознательно: «джиттер без TAA» — это отдельное
      // состояние, которое надо уметь показать, потому что именно оно демонстрирует, что джиттер сам по себе
      // не сглаживает, а даёт дрожание.
      const glm::vec2 jitter_pixels = jitter_scale > 0.0f
        ? glm::vec2(halton(phase + 1u, 2u) - 0.5f, halton(phase + 1u, 3u) - 0.5f) * jitter_scale
        : glm::vec2(0.0f);
      const glm::vec2 jitter_uv{
        jitter_pixels.x / float(std::max(pending_width, 1u)),
        jitter_pixels.y / float(std::max(pending_height, 1u))};
      // Знак определён ИЗМЕРЕНИЕМ, а не выводом из формы матрицы: инвариант такой — на полностью статичной
      // сцене со статичной камерой motion обязан быть нулём при включённом джиттере. При обратном знаке он
      // оказывался 1.6 px, джиттер протекал в векторы, TAA репроецировал по ним и в итоге давал картинку
      // ДАЛЬШЕ от суперсэмплированной, чем один кадр.
      projection[2][0] -= jitter_uv.x * 2.0f;
      projection[2][1] -= jitter_uv.y * 2.0f;

      const auto view_projection = projection * view;

      // На первом кадре и после сброса прошлой матрицы нет: берём текущую, тогда motion = 0, и «истории
      // нет» читается как нулевое движение, а не как случайный вектор.
      if (frames_since_reset == 0) {
        previous_view_projection = view_projection;
        previous_jitter_uv = jitter_uv;
      }

      frame_block frame_data{};
      frame_data.view_projection = view_projection;
      frame_data.previous_view_projection = previous_view_projection;
      frame_data.inverse_view_projection = glm::inverse(view_projection);
      frame_data.camera_position = glm::vec4(camera.position, 1.0f);
      frame_data.viewport_near = glm::vec4(float(pending_width), float(pending_height), near_plane, float(frames_since_reset));
      frame_data.controls = glm::vec4(float(debug_mode), motion_gain, error_gain, encode_srgb ? 1.0f : 0.0f);
      frame_data.light_direction = glm::vec4(glm::normalize(sun_direction), ambient_fraction);
      frame_data.tonemap = glm::vec4(float(tonemap_operator), manual_exposure, adapt_rate, dt);
      // границы log2 средней яркости, ключ (средний серый) и яркость солнца
      frame_data.exposure_limits = glm::vec4(-6.0f, 8.0f, 0.18f, sun_intensity);
      frame_data.fog_params = glm::vec4(fog_density, fog_falloff, fog_height, fog_anisotropy);
      // Цвет рассеяния масштабируется солнцем: туман светится тем же светом, что и всё остальное, поэтому
      // при смене яркости освещения он не должен внезапно становиться чёрным или выжженным.
      frame_data.fog_color = glm::vec4(glm::vec3(0.42f, 0.52f, 0.72f) * sun_intensity * 0.45f, 0.75f);
      frame_data.ao_params = glm::vec4(ao_radius, ao_intensity, ao_bias, ao_enabled ? ao_power : 0.0f);
      // Временной сдвиг вращения выборки AO имеет смысл только вместе с накоплением: иначе это просто шум,
      // который меняется каждый кадр и мерцает.
      const float ao_temporal = taa_enabled ? float(phase) / float(std::max(taa_phases, 1u)) : 0.0f;
      frame_data.taa_params = glm::vec4(
        taa_enabled ? taa_weight : 0.0f,
        float(taa_reject),
        taa_enabled ? (taa_catmull_rom ? 2.0f : 1.0f) : 0.0f,
        ao_temporal);
      frame_data.taa_jitter = glm::vec4(jitter_uv, previous_jitter_uv);
      frame_data.bloom_params = glm::vec4(bloom_intensity, bloom_threshold, bloom_knee, bloom_up_weight);

      // Положение солнца на экране: проецируем точку далеко по направлению НА светило. Если она за камерой
      // (w <= 0) либо ушла далеко за пределы кадра, лучи гасим — экранный метод их всё равно не построит.
      const auto sun_world = camera.position + glm::normalize(sun_direction) * 1000.0f;
      const auto sun_clip = view_projection * glm::vec4(sun_world, 1.0f);
      glm::vec2 sun_uv(0.0f);
      float shafts_strength = 0.0f;
      if (sun_clip.w > 0.0f) {
        sun_uv = glm::vec2(sun_clip.x / sun_clip.w, sun_clip.y / sun_clip.w) * 0.5f + 0.5f;
        const float margin = 0.35f;
        const bool visible = sun_uv.x > -margin && sun_uv.x < 1.0f + margin &&
                             sun_uv.y > -margin && sun_uv.y < 1.0f + margin;
        shafts_strength = visible ? shaft_intensity : 0.0f;
      }
      frame_data.shaft_params = glm::vec4(sun_uv, shafts_strength, shaft_falloff);
      frame_data.lens_params = glm::vec4(sharpen, vignette, aberration, grain);
      // Семя зерна и дизера меняется по кадрам: статичный шум читается как грязь на экране, а не как зерно.
      // Оно же остаётся детерминированным (функция номера кадра), поэтому дампы сравнимы.
      frame_data.output_params = glm::vec4(dither ? 1.0f : 0.0f, float(frames_total % 64u), 0.0f, 0.0f);
      write_current_buffer(base, "frame_buffer", &frame_data, sizeof(frame_data));

      {
        // Только текущие трансформы: индекс 0 — неподвижная комната, дальше движущиеся кубы. Прошлый кадр
        // движок отдаст сам из истории этого ресурса, поэтому дублировать его здесь нечего.
        std::array<object_transform, 1 + mover_count> transforms{};
        transforms[0].offset = glm::vec4(0.0f);
        for (uint32_t i = 0; i < mover_count; ++i) {
          transforms[1 + i].offset = mover_offset(i, int64_t(frames_total), object_speed, object_spin);
        }
        write_current_buffer(base, "object_transforms", transforms.data(), sizeof(transforms));
      }

      context.prepare();
      context.draw();
      base.submit_frame();
      frame_pacer.wait();

      previous_view_projection = view_projection;
      previous_jitter_uv = jitter_uv;
      frames_since_reset += 1;
      frames_total += 1;
      if (frame_limit != 0 && frames_total >= frame_limit) {
        if (!dump_path.empty()) {
          vk::Device(device).waitIdle();
          dump_composed_image(base, dump_path);
        }
        // Среднее время кадра печатается всегда: это единственная цена, которую площадка может измерить без
        // GPU-таймеров, и её достаточно, чтобы сравнить тиры качества между собой на одной машине.
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - loop_start).count();
        utils::info(
          "PF03 reached the requested {} frames in {:.3f} s, average frame {:.3f} ms{}",
          frame_limit, elapsed, 1000.0 * elapsed / double(frame_limit), uncapped ? " (uncapped)" : " (paced)");
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
