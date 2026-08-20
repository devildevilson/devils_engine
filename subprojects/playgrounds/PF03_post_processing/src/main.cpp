#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <chrono>
#include <limits>
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
#include "devils_engine/painter/gpu_timing.h"
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
// Гистограмма считается группами 16x16: по потоку на корзину, то есть одно глобальное атомарное сложение на поток
constexpr uint32_t histogram_tile = 16;
constexpr float near_plane = 0.1f;
// Детерминированный шаг для орбиты: положение камеры становится чистой функцией номера кадра, поэтому два
// прогона с одинаковым --frames дают одинаковую картинку и ошибку репроекции можно сравнивать численно.
constexpr float orbit_step_seconds = 1.0f / 60.0f;

// Отладочные виды; порядок обязан совпадать с PF03_DEBUG_* в resources/shaders/pf03_frame.glsl
constexpr std::array<std::string_view, 24> debug_names = {
  "shaded", "depth", "normal", "motion", "reprojected", "error(motion)", "error(no motion)",
  "clipping", "calibration", "exposure", "transmittance", "ao", "ao raw", "taa rejection",
  "bloom", "shafts", "sharpen", "histogram", "histogram plot", "luminance",
  "grade delta", "lut error", "gamut", "lut strip"};

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

// Разбор списка «x,y,z» из аргумента. Незаданные компоненты остаются от значения по умолчанию, поэтому
// '--grade-slope=1.05' задаёт только красный канал.
glm::vec3 parse_vec3(const std::string_view text, const glm::vec3 fallback) {
  std::array<float, 3> values{fallback.x, fallback.y, fallback.z};
  const std::string str(text);
  size_t begin = 0;
  for (uint32_t v = 0; v < values.size() && begin <= str.size(); ++v) {
    const auto end = str.find(',', begin);
    values[v] = std::stof(str.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return {values[0], values[1], values[2]};
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
  glm::vec4 metering;
  glm::vec4 grade_balance;
  glm::vec4 grade_tone;
  glm::vec4 grade_slope;
  glm::vec4 grade_offset;
  glm::vec4 grade_power;
  glm::vec4 grade_filter;
  glm::vec4 lut_params;
};
static_assert(sizeof(frame_block) == 560);

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

// Размер группы — ПАРАМЕТР, а не константа функции. До этого функция всегда делила на 8, и вызов для
// гистограммы, которой нужна сетка 16x16, получил двойное деление: сетка вышла 10x6 вместо 80x45, то есть в
// гистограмму попадали первые шестнадцать строк кадра вместо всего кадра. Симптом был крайне обманчивым —
// распределение выглядело вырожденным (четыре корзины подряд), потому что верхняя полоса кадра это ровное небо.
void update_dispatch_constant(
  painter::graphics_base& base, const std::string_view name,
  const uint32_t width, const uint32_t height, const uint32_t tile) {
  const uint32_t slot = base.find_constant(name);
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF03 constant '{}' is absent from the configured graph", name);
  }
  const VkDispatchIndirectCommand command{
    (width + tile - 1u) / tile,
    (height + tile - 1u) / tile,
    1u};
  base.write_constant_data(slot, command);
}

// Масштаб сетки замера: 1 — по каждому пикселю, 2 — по каждому второму и так далее. Ручкой, а не константой,
// потому что вопрос «сколько стоит замер» теперь можно измерить, а не оценивать.
uint32_t metering_scale = 2;

// Сторона куба таблицы грейда. Живёт здесь, а не в main, потому что от неё зависит и размер ресурса
// (переопределение распарсенного конфига), и сетка запекающего dispatch.
uint32_t lut_grid = 32;

void update_screen_dispatch(painter::graphics_base& base, const uint32_t width, const uint32_t height) {
  // Каждой цели передаётся её РАЗМЕР В ПИКСЕЛЯХ и размер группы её шейдера — так двойное деление становится
  // невыразимым, а не подстерегающим.
  update_dispatch_constant(base, "screen_dispatch", width, height, dispatch_tile);
  // SSAO и пирамида считаются в пониженных разрешениях, группа у них та же 8x8
  update_dispatch_constant(base, "half_dispatch", (width + 1u) / 2u, (height + 1u) / 2u, dispatch_tile);
  update_dispatch_constant(base, "quarter_dispatch", (width + 3u) / 4u, (height + 3u) / 4u, dispatch_tile);
  update_dispatch_constant(base, "eighth_dispatch", (width + 7u) / 8u, (height + 7u) / 8u, dispatch_tile);
  update_dispatch_constant(base, "sixteenth_dispatch", (width + 15u) / 16u, (height + 15u) / 16u, dispatch_tile);
  // Гистограмма считается по сетке вдвое реже кадра, группами 16x16 (по потоку на корзину)
  const uint32_t metering_divisor = std::max(metering_scale, 1u);
  update_dispatch_constant(
    base, "histogram_dispatch",
    (width + metering_divisor - 1u) / metering_divisor,
    (height + metering_divisor - 1u) / metering_divisor,
    histogram_tile);
  // Таблица грейда от разрешения кадра не зависит вовсе — её сетка задаётся размером самой таблицы. Константа
  // ставится здесь просто чтобы все dispatch'и считались в одном месте, а не в двух.
  update_dispatch_constant(base, "lut_dispatch", lut_grid * lut_grid, lut_grid, dispatch_tile);
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

// Накопитель времён пассов. Среднее по прогону, а не мгновенное значение: одиночный кадр на этом железе
// шумит сильнее, чем стоят измеряемые эффекты, и именно поэтому суждения о цене до сих пор были угадыванием.
// Минимум держится отдельно — он менее подвержен помехам от чужой нагрузки, потому что шум только добавляет.
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
      for (size_t i = 0; i < timings.size(); ++i) {
        passes[i].name = timings[i].name;
      }
    }
    if (passes.size() != timings.size()) {
      utils::error{}("PF03 pass timing count changed mid-run: {} -> {}", passes.size(), timings.size());
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
      utils::info("PF03 GPU timings: no samples collected");
      return;
    }

    const double count = double(samples);
    utils::info("PF03 GPU timings over {} frames (мс: среднее / минимум / максимум):", samples);
    for (const auto& pass : passes) {
      utils::info(
        "  {:<20} {:6.3f} / {:6.3f} / {:6.3f}",
        pass.name, pass.total / count, pass.minimum, pass.maximum);
    }
    utils::info("  {:<20} {:6.3f} / {:6.3f}", "ВСЕГО НА GPU", frame_total / count, frame_minimum);
  }
};

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
  float adapt_up = 2.2f;         // привыкание К ЯРКОМУ, 1/с: у глаза это секунды
  float adapt_down = 0.35f;      // привыкание К ТЁМНОМУ: десятки секунд, отсюда ощущение ослепления
  float metering_low = 0.45f;    // нижний перцентиль: отбрасываем тени, они не определяют экспозицию
  float metering_high = 0.95f;   // верхний перцентиль: отбрасываем выбросы — блики и источники
  float center_weight = 0.4f;    // центровзвешенность замера
  float lamp_intensity = 40.0f;  // яркость светящейся панели: ручка для изоляции влияния выброса на замер
  // Границы замера в стопах. Это и есть вход для «эффекта тёмной комнаты»: авторски ограниченная экспозиция
  // не даёт вытянуть тёмное помещение, и узкий проход работает визуальным барьером. Волюмы в движке жить не
  // должны — он берёт границы данными, а расставляет их прикладной слой.
  float exposure_min = -6.0f;
  float exposure_max = 8.0f;
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
  // Грейд включён по умолчанию и НЕЙТРАЛЕН. Это не «эффект на вкус», а тождество: при нейтральных ручках
  // табличный путь обязан совпасть с кадром без грейда, и держать эту проверку в штатном прогоне полезнее,
  // чем включать её отдельным тестом раз в срез.
  bool grade_enabled = true;
  bool grade_display_space = false; // false — scene-referred (до кривой), true — display-referred (после)
  bool grade_by_lut = true;         // true — таблица, false — та же функция аналитически; чистое A/B
  bool wb_naive = false;            // наивный баланс белого усилением каналов: нужен для измерения его цены
  // Объявленная температура ОСВЕЩЕНИЯ сцены (как у баланса белого в камере), не «температура картинки»:
  // выше 6500 — сцена считается освещённой холоднее, и картинка теплеет. Направление контринтуитивное, но
  // это единственная трактовка, в которой число совпадает с числом на реальном источнике.
  float temperature = 6500.0f;
  float tint = 0.0f;               // ось зелёный–пурпурный, перпендикулярно локусу
  float contrast = 1.0f;
  float contrast_pivot = 0.18f;    // средний серый: то же число, что ключ замера экспозиции
  float saturation = 1.0f;
  glm::vec3 grade_slope{1.0f};    // ASC CDL
  glm::vec3 grade_offset{0.0f};
  glm::vec3 grade_power{1.0f};
  glm::vec3 color_filter{1.0f};
  float filter_strength = 1.0f;
  bool lut_linear_shaper = false; // линейный shaper вместо log2: показывает, куда уходит сетка таблицы
  float lut_min_stop = -12.0f;    // границы shaper'а в стопах: область определения таблицы
  float lut_max_stop = 10.0f;
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
    // Пресет «взгляда»: ставит сразу несколько ручек, поэтому в командной строке должен идти ПЕРЕД точечными —
    // разбор идёт по порядку аргументов, и последнее упоминание выигрывает.
    constexpr std::string_view look_prefix = "--look=";
    if (option.starts_with(look_prefix)) {
      const auto name = option.substr(look_prefix.size());
      if (name == "neutral") {
        temperature = 6500.0f; tint = 0.0f; contrast = 1.0f; saturation = 1.0f;
        grade_slope = glm::vec3(1.0f); grade_offset = glm::vec3(0.0f); grade_power = glm::vec3(1.0f);
        color_filter = glm::vec3(1.0f);
      } else if (name == "warm") {
        temperature = 8200.0f; tint = -0.04f; contrast = 1.06f; saturation = 1.06f;
        grade_slope = glm::vec3(1.03f, 1.0f, 0.96f);
      } else if (name == "cool") {
        temperature = 5200.0f; tint = 0.03f; contrast = 1.06f; saturation = 0.95f;
        grade_offset = glm::vec3(0.0f, 0.004f, 0.018f);
      } else if (name == "bleach") {
        // Сильный грейд специально: он и нагружает табулирование, и выгоняет каналы за гамму
        contrast = 1.35f; saturation = 0.55f; grade_power = glm::vec3(0.95f, 0.95f, 1.0f);
      } else if (name == "teal_orange") {
        contrast = 1.15f; saturation = 1.18f;
        grade_slope = glm::vec3(1.06f, 1.0f, 0.92f);
        grade_offset = glm::vec3(-0.006f, 0.0f, 0.02f);
      } else {
        utils::error{}("PF03 unknown look '{}' (neutral|warm|cool|bleach|teal_orange)", name);
      }
    }
    constexpr std::string_view grade_prefix = "--grade=";
    if (option.starts_with(grade_prefix)) {
      grade_enabled = std::stoi(std::string(option.substr(grade_prefix.size()))) != 0;
    }
    constexpr std::string_view grade_space_prefix = "--grade-space=";
    if (option.starts_with(grade_space_prefix)) {
      const auto name = option.substr(grade_space_prefix.size());
      if (name == "scene") grade_display_space = false;
      else if (name == "display") grade_display_space = true;
      else utils::error{}("PF03 unknown grade space '{}' (scene|display)", name);
    }
    constexpr std::string_view lut_prefix = "--lut=";
    if (option.starts_with(lut_prefix)) {
      grade_by_lut = std::stoi(std::string(option.substr(lut_prefix.size()))) != 0;
    }
    constexpr std::string_view lut_size_prefix = "--lut-size=";
    if (option.starts_with(lut_size_prefix)) {
      lut_grid = uint32_t(std::stoul(std::string(option.substr(lut_size_prefix.size()))));
      if (lut_grid < 2 || lut_grid > 128) {
        utils::error{}("PF03 lut size {} is out of the supported 2..128 range", lut_grid);
      }
    }
    constexpr std::string_view lut_shaper_prefix = "--lut-shaper=";
    if (option.starts_with(lut_shaper_prefix)) {
      const auto name = option.substr(lut_shaper_prefix.size());
      if (name == "log" || name == "log2") lut_linear_shaper = false;
      else if (name == "linear") lut_linear_shaper = true;
      else utils::error{}("PF03 unknown lut shaper '{}' (log|linear)", name);
    }
    constexpr std::string_view lut_range_prefix = "--lut-range=";
    if (option.starts_with(lut_range_prefix)) {
      const auto range = parse_vec3(option.substr(lut_range_prefix.size()), glm::vec3(lut_min_stop, lut_max_stop, 0.0f));
      lut_min_stop = range.x;
      lut_max_stop = range.y;
    }
    constexpr std::string_view temperature_prefix = "--temperature=";
    if (option.starts_with(temperature_prefix)) {
      temperature = std::stof(std::string(option.substr(temperature_prefix.size())));
    }
    constexpr std::string_view tint_prefix = "--tint=";
    if (option.starts_with(tint_prefix)) {
      tint = std::stof(std::string(option.substr(tint_prefix.size())));
    }
    constexpr std::string_view wb_prefix = "--wb=";
    if (option.starts_with(wb_prefix)) {
      const auto name = option.substr(wb_prefix.size());
      if (name == "adapt") wb_naive = false;
      else if (name == "naive") wb_naive = true;
      else utils::error{}("PF03 unknown white balance mode '{}' (adapt|naive)", name);
    }
    constexpr std::string_view contrast_prefix = "--contrast=";
    if (option.starts_with(contrast_prefix)) {
      contrast = std::stof(std::string(option.substr(contrast_prefix.size())));
    }
    constexpr std::string_view pivot_prefix = "--contrast-pivot=";
    if (option.starts_with(pivot_prefix)) {
      contrast_pivot = std::stof(std::string(option.substr(pivot_prefix.size())));
    }
    constexpr std::string_view saturation_prefix = "--saturation=";
    if (option.starts_with(saturation_prefix)) {
      saturation = std::stof(std::string(option.substr(saturation_prefix.size())));
    }
    constexpr std::string_view slope_prefix = "--grade-slope=";
    if (option.starts_with(slope_prefix)) {
      grade_slope = parse_vec3(option.substr(slope_prefix.size()), grade_slope);
    }
    constexpr std::string_view offset_prefix = "--grade-offset=";
    if (option.starts_with(offset_prefix)) {
      grade_offset = parse_vec3(option.substr(offset_prefix.size()), grade_offset);
    }
    constexpr std::string_view power_prefix = "--grade-power=";
    if (option.starts_with(power_prefix)) {
      grade_power = parse_vec3(option.substr(power_prefix.size()), grade_power);
    }
    constexpr std::string_view filter_prefix = "--color-filter=";
    if (option.starts_with(filter_prefix)) {
      color_filter = parse_vec3(option.substr(filter_prefix.size()), color_filter);
    }
    constexpr std::string_view filter_strength_prefix = "--color-filter-strength=";
    if (option.starts_with(filter_strength_prefix)) {
      filter_strength = std::stof(std::string(option.substr(filter_strength_prefix.size())));
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
      sun_direction = parse_vec3(option.substr(sun_dir_prefix.size()), sun_direction);
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
      adapt_up = std::stof(std::string(option.substr(adapt_prefix.size())));
      adapt_down = adapt_up; // общий темп: нужен для A/B против асимметричного
    }
    constexpr std::string_view adapt_up_prefix = "--adapt-up=";
    if (option.starts_with(adapt_up_prefix)) {
      adapt_up = std::stof(std::string(option.substr(adapt_up_prefix.size())));
    }
    constexpr std::string_view adapt_down_prefix = "--adapt-down=";
    if (option.starts_with(adapt_down_prefix)) {
      adapt_down = std::stof(std::string(option.substr(adapt_down_prefix.size())));
    }
    constexpr std::string_view mscale_prefix = "--metering-scale=";
    if (option.starts_with(mscale_prefix)) {
      metering_scale = uint32_t(std::stoul(std::string(option.substr(mscale_prefix.size()))));
    }
    constexpr std::string_view range_prefix = "--exposure-range=";
    if (option.starts_with(range_prefix)) {
      const auto text = std::string(option.substr(range_prefix.size()));
      const auto comma = text.find(',');
      exposure_min = std::stof(text.substr(0, comma));
      if (comma != std::string::npos) {
        exposure_max = std::stof(text.substr(comma + 1));
      }
    }
    constexpr std::string_view lamp_prefix = "--lamp=";
    if (option.starts_with(lamp_prefix)) {
      lamp_intensity = std::stof(std::string(option.substr(lamp_prefix.size())));
    }
    constexpr std::string_view low_prefix = "--metering-low=";
    if (option.starts_with(low_prefix)) {
      metering_low = std::stof(std::string(option.substr(low_prefix.size())));
    }
    constexpr std::string_view high_prefix = "--metering-high=";
    if (option.starts_with(high_prefix)) {
      metering_high = std::stof(std::string(option.substr(high_prefix.size())));
    }
    constexpr std::string_view center_prefix = "--metering-center=";
    if (option.starts_with(center_prefix)) {
      center_weight = std::stof(std::string(option.substr(center_prefix.size())));
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

    // Размер таблицы грейда — это РАЗМЕР РЕСУРСА, а не число в UBO и не константа шага. Поэтому он
    // переопределяется в объявленном значении до сборки графа: сменить его живьём нельзя, нужно пересоздать
    // картинку. Ровно тот рычаг «размеры целей», про который говорит аудит.
    if (lut_grid != 32) {
      const auto slot = render_config.find_constant_value("color_lut_size");
      if (slot == painter::invalid_resource_slot) {
        utils::error{}("PF03 declared value 'color_lut_size' is absent from the configured graph");
      }
      auto& value = render_config.constant_values[slot];
      value.value = std::make_tuple(lut_grid * lut_grid, lut_grid, 0u);
      value.current_value = value.value;
      utils::info("PF03 resource size override: color_lut = {}x{} (LUT {}^3)", lut_grid * lut_grid, lut_grid, lut_grid);
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

    // Профилировщик создаётся ПОСЛЕ смены графа: ему нужно знать число пассов и их имена
    painter::gpu_timestamp_profiler gpu_profiler(base);
    pass_timing_accumulator timings;

    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;
    context.set_gpu_profiler(&gpu_profiler);
    if (!gpu_profiler.available()) {
      utils::warn("PF03 GPU timestamps are unavailable on this device; per-pass cost will stay unmeasured");
    }

    report_resource_copies(base, "scene_color");
    report_resource_copies(base, "gbuffer_motion");
    utils::info("PF03 graph: thin G-buffer (depth+normal+motion) -> shade -> compose/reproject -> present");
    utils::info(
      "PF03 views: 0 shaded, 1 depth, 2 normal, 3 motion, 4 reprojected, 5 error(motion), 6 error(no motion), "
      "7 clipping, 8 calibration, 9 exposure, 10 transmittance, 11 ao, 12 ao raw, 13 taa rejection, "
      "14 bloom, 15 shafts, 16 sharpen, 17 histogram state, 18 histogram plot, 19 metered luminance, "
      "20 grade delta, 21 lut error, 22 gamut, 23 lut strip");
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
      adapt_up, sun_intensity);
    utils::info(
      "PF03 metering: histogram percentiles {}..{}, center weight {}, adaptation up {}/s down {}/s",
      metering_low, metering_high, center_weight, adapt_up, adapt_down);
    utils::info("PF03 metering range: {}..{} stops (границы для «тёмной комнаты»)", exposure_min, exposure_max);
    utils::info(
      "PF03 grade: {}, {}-referred, path {}, wb {} ({} K, tint {}), contrast {} @ {}, saturation {}",
      grade_enabled ? "on" : "off", grade_display_space ? "display" : "scene",
      grade_by_lut ? "LUT" : "analytic", wb_naive ? "naive gains" : "von Kries",
      temperature, tint, contrast, contrast_pivot, saturation);
    utils::info(
      "PF03 grade CDL: slope [{} {} {}], offset [{} {} {}], power [{} {} {}]",
      grade_slope.x, grade_slope.y, grade_slope.z,
      grade_offset.x, grade_offset.y, grade_offset.z,
      grade_power.x, grade_power.y, grade_power.z);
    utils::info(
      "PF03 LUT: {}^3 (strip {}x{}), shaper {} over {}..{} stops",
      lut_grid, lut_grid * lut_grid, lut_grid, lut_linear_shaper ? "linear" : "log2",
      lut_min_stop, lut_max_stop);
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

      // Результаты запросов забираются в prepare_frame после ожидания фенса слота, поэтому читать их надо
      // сразу здесь: это времена кадра, отправленного frames_in_flight кадров назад.
      if (gpu_profiler.has_results()) {
        timings.add(gpu_profiler.passes(), gpu_profiler.frame_milliseconds());
      }

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
      frame_data.tonemap = glm::vec4(float(tonemap_operator), manual_exposure, adapt_up, dt);
      // границы log2 средней яркости, ключ (средний серый) и яркость солнца
      frame_data.exposure_limits = glm::vec4(exposure_min, exposure_max, 0.18f, sun_intensity);
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
      frame_data.output_params = glm::vec4(dither ? 1.0f : 0.0f, float(frames_total % 64u), lamp_intensity, 0.0f);
      frame_data.metering = glm::vec4(metering_low, metering_high, center_weight, adapt_down);
      frame_data.grade_balance = glm::vec4(temperature, tint, wb_naive ? 1.0f : 0.0f, contrast);
      frame_data.grade_tone = glm::vec4(
        saturation, contrast_pivot, grade_enabled ? 1.0f : 0.0f, grade_display_space ? 1.0f : 0.0f);
      frame_data.grade_slope = glm::vec4(grade_slope, 0.0f);
      frame_data.grade_offset = glm::vec4(grade_offset, 0.0f);
      frame_data.grade_power = glm::vec4(grade_power, 0.0f);
      frame_data.grade_filter = glm::vec4(color_filter, filter_strength);
      frame_data.lut_params = glm::vec4(
        grade_by_lut ? 1.0f : 0.0f, lut_linear_shaper ? 1.0f : 0.0f, lut_min_stop, lut_max_stop);
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
        timings.report();
        break;
      }
    }

    if (frame_limit == 0) {
      timings.report(); // выход по Esc: отчёт всё равно нужен
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
