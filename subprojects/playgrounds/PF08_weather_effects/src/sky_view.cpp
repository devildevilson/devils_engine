#include "sky_view.h"

#include "fixture.h"
#include "shadows.h"
#include "foliage.h"
#include "terrain.h"

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
#include "devils_engine/painter/atlas_layout.h"
#include "devils_engine/painter/region_draw.h"
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

namespace devils_engine::pf08 {
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
int32_t weather_cycle_key = -1;
double exposure_step = 0.0;
bool weather_cycle_requested = false;

struct alignas(16) camera_block {
  glm::mat4 view_projection;
  glm::mat4 view;
  glm::vec4 camera_position;
  glm::vec4 viewport_near;
};
static_assert(sizeof(camera_block) == 160);

void error_callback(const int error, const char* message) noexcept {
  utils::warn("PF08 input error {}: {}", error, message);
}

void key_callback(GLFWwindow* window, const int key, const int scancode, const int action, const int) noexcept {
  input::events::update_key(scancode, action);
  if (key == escape_key && action == 1) input::set_should_close(window, true);
  if (key == pause_key && action == 1) pause_requested = true;
  if (key == faster_key && action == 1) time_scale_step += 1.0;
  if (key == slower_key && action == 1) time_scale_step -= 1.0;
  if (key == exposure_up_key && action == 1) exposure_step += 1.0;
  if (key == exposure_down_key && action == 1) exposure_step -= 1.0;
  if (key == weather_cycle_key && action == 1) weather_cycle_requested = true;
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

// Экспозиция по замеру ЯРКОСТИ кадра.
//
// Прошлая версия мерила горизонтальную освещённость — падающий свет, как фотограф с полусферой на
// объективе. Для сцены, которую свет освещает, это верно, но небо светом не освещается: оно само есть
// источник и в сумерках занимает весь кадр. На рассвете при светиле на -6° падающий замер давал
// 17 лк, экспозиция открывалась почти на три ступени сверх нужного, и небо выходило белым при
// формально верных числах. Поправкой это не лечится: ошибочна сама измеряемая величина, а не её
// множитель — уменьшение замера делает картинку ЕЩЁ светлее.
//
// Теперь яркость кадра считает видеокарта (см. meter.comp.glsl) и кладёт в буфер, видимый хосту.
// Формула — обычный отражённый замер при ISO 100 с калибровочной постоянной 12.5: яркость сцены
// отображается в средне-серое. Прямой свет светил в замер больше не входит вообще: диск в кадре
// учтён самой картинкой, а за кадром он на экспозицию влиять и не должен.
double target_ev100(const double scene_luminance_nits, const exposure_settings& settings) {
  const double measured = std::log2(std::max(scene_luminance_nits, 1e-6) * 100.0 / 12.5);
  // Полная адаптация вывела бы измеренную яркость в средне-серое и стёрла разницу между полднем и
  // полночью. Доля меньше единицы оставляет тёмным состояниям их темноту: кадр отходит от точки
  // отсчёта на ту же долю, на какую от неё отошла сцена.
  const double ev100 = settings.reference_ev100 + settings.bias_stops +
                       settings.adaptation_strength * (measured - settings.reference_ev100);
  return std::clamp(ev100, settings.min_ev100, settings.max_ev100);
}

// Средняя геометрическая яркость кадра, посчитанная видеокардой. Копия берётся ТЕКУЩАЯ: её видеокарта
// заполняла столько кадров назад, сколько держится копий ресурса, и та работа давно завершена — ждать
// забора нечего. Пока замер не пришёл (первые кадры, ресурс ещё нулевой), возвращается ноль, и вызов
// сам решает, что делать.
double read_scene_luminance(painter::graphics_base& base) {
  const uint32_t slot = base.find_resource("exposure_meter");
  if (slot == painter::invalid_resource_slot) return 0.0;

  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || frame.sub.size < sizeof(float)) return 0.0;

  float value = 0.0f;
  std::memcpy(&value, static_cast<const uint8_t*>(frame.mapped) + frame.sub.offset, sizeof(value));
  if (!std::isfinite(value) || value <= 0.0f) return 0.0;
  return double(value);
}

// Множитель вывода из EV100. Яркость 1.2 * 2^EV100 отображается в единицу — обычная фотографическая
// нормировка, а не подобранное число.
double exposure_from_ev100(const double ev100) {
  return 1.0 / (1.2 * std::pow(2.0, ev100));
}

// Значения цветового сценария на заданной высоте главного светила. Между ключами линейная
// интерполяция; за краями — крайние ключи без экстраполяции, чтобы сценарий не улетал на полюсах.
void evaluate_colour_script(const colour_script& script, const double sun_altitude_deg,
                            output_settings& output) {
  if (script.keys.empty()) return;

  const auto apply = [&output](const colour_script_key& key) {
    output.grade_tint = key.tint.size() >= 3
                          ? glm::vec3(float(key.tint[0]), float(key.tint[1]), float(key.tint[2]))
                          : glm::vec3(1.0f);
    output.grade_saturation = key.saturation;
    output.grade_contrast = key.contrast;
  };

  if (sun_altitude_deg <= script.keys.front().sun_altitude_deg) {
    apply(script.keys.front());
    return;
  }
  if (sun_altitude_deg >= script.keys.back().sun_altitude_deg) {
    apply(script.keys.back());
    return;
  }

  for (size_t i = 1; i < script.keys.size(); ++i) {
    const auto& previous = script.keys[i - 1];
    const auto& current = script.keys[i];
    if (sun_altitude_deg > current.sun_altitude_deg) continue;

    const double span = current.sun_altitude_deg - previous.sun_altitude_deg;
    const double weight = span > 0.0 ? (sun_altitude_deg - previous.sun_altitude_deg) / span : 0.0;
    const auto blend = [weight](const double a, const double b) { return a + (b - a) * weight; };

    const bool has_tint = previous.tint.size() >= 3 && current.tint.size() >= 3;
    output.grade_tint = has_tint ? glm::vec3(float(blend(previous.tint[0], current.tint[0])),
                                             float(blend(previous.tint[1], current.tint[1])),
                                             float(blend(previous.tint[2], current.tint[2])))
                                 : glm::vec3(1.0f);
    output.grade_saturation = blend(previous.saturation, current.saturation);
    output.grade_contrast = blend(previous.contrast, current.contrast);
    return;
  }
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
  if (key < 0 || scancode < 0) utils::error{}("PF08 could not resolve input key '{}'", canonical);
  input::events::set_key(event, scancode, key);
}

void write_current_buffer(painter::graphics_base& base, const std::string_view name, const void* data,
                          const size_t bytes) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) utils::error{}("PF08 buffer '{}' is absent", name);

  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}("PF08 cannot write '{}' (capacity {}, requested {})", name, frame.sub.size, bytes);
  }
  if (bytes != 0) std::memcpy(static_cast<uint8_t*>(frame.mapped) + frame.sub.offset, data, bytes);
}

// Инстансы опорной геометрии пишутся во ВСЕ копии кадра сразу и один раз: фикстура статична, а
// копий у ресурса столько же, сколько кадров в полёте, и незаполненная копия дала бы мигание раз в
// два-три кадра.
void write_fixture(painter::graphics_base& base, const uint32_t pair,
                   const std::span<const scene_instance> instances, const uint32_t vertex_count) {
  for (uint32_t offset = 0; offset < base.frames_in_flight(); ++offset) {
    const auto instance_frame = base.get_current_instance_resource_frame(pair, offset);
    const auto indirect_frame = base.get_current_indirect_resource_frame(pair, offset);
    if (instances.size_bytes() > instance_frame.sub.size) {
      utils::error{}("PF08 fixture instance budget exceeded ({} bytes, capacity {})",
                     instances.size_bytes(), instance_frame.sub.size);
    }
    std::memcpy(static_cast<uint8_t*>(instance_frame.mapped) + instance_frame.sub.offset, instances.data(),
                instances.size_bytes());
    const VkDrawIndirectCommand command{vertex_count, uint32_t(instances.size()), 0, 0};
    std::memcpy(static_cast<uint8_t*>(indirect_frame.mapped) + indirect_frame.sub.offset, &command,
                sizeof(command));
  }
}

// Инстанс, меняющийся каждый кадр, пишется только в ТЕКУЩУЮ копию: остальные принадлежат кадрам,
// которые ещё в полёте, и трогать их значит менять уже отданную видеокарте работу.
void write_pair_current(painter::graphics_base& base, const uint32_t pair,
                        const std::span<const scene_instance> instances, const uint32_t vertex_count) {
  const auto instance_frame = base.get_current_instance_resource_frame(pair, 0);
  const auto indirect_frame = base.get_current_indirect_resource_frame(pair, 0);
  if (instances.size_bytes() > instance_frame.sub.size) {
    utils::error{}("PF08 pair {} instance budget exceeded", pair);
  }
  std::memcpy(static_cast<uint8_t*>(instance_frame.mapped) + instance_frame.sub.offset, instances.data(),
              instances.size_bytes());
  const VkDrawIndirectCommand command{vertex_count, uint32_t(instances.size()), 0, 0};
  std::memcpy(static_cast<uint8_t*>(indirect_frame.mapped) + indirect_frame.sub.offset, &command,
              sizeof(command));
}

// Команды регионов: по одной на каскад. Каждая несёт свой viewport в атласе и индекс записи каскада,
// поэтому шесть карт рисуются ОДНИМ проходом, а не шестью, и порядок здесь — это просто порядок
// записей в буфере, а не знание шейдера о раскладке атласа.
// Что именно рисуется в тень: пара и сколько её инстансов. Список СВОЙ у каждого каскада, и это не
// мелочь: заросли имеет смысл класть только в ближние каскады. Шесть тысяч кустов в дальний каскад
// стоят полного прохода по ним ради теней, которые на своей дальности занимают доли пикселя.
struct shadow_caster {
  uint32_t pair = 0;
  uint32_t instance_count = 0;
  uint32_t vertex_count = 0;
};

void write_shadow_regions(painter::graphics_base& base,
                          const std::span<const std::vector<shadow_caster>> casters,
                          const std::span<const painter::atlas_region> regions,
                          const std::span<const bool> slot_active) {
  const uint32_t region_count = uint32_t(regions.size());
  uint32_t span_total = 0;
  for (uint32_t index = 0; index < region_count; ++index) {
    if (slot_active[index / cascade_count]) span_total += uint32_t(casters[index].size());
  }
  std::array<uint8_t, 2048> bytes{};
  const size_t needed = painter::region_draw_buffer_size(region_count, span_total);
  if (needed > bytes.size()) utils::error{}("PF08 shadow region buffer too small ({} bytes)", needed);

  painter::region_draw_header header{};
  header.region_count = region_count;
  header.span_count = span_total;
  header.region_stride = sizeof(painter::region_draw_command);
  header.span_stride = sizeof(painter::region_draw_span);
  std::memcpy(bytes.data(), &header, sizeof(header));

  size_t command_offset = sizeof(header);
  size_t span_offset = command_offset + size_t(region_count) * sizeof(painter::region_draw_command);
  uint32_t span_cursor = 0;
  for (uint32_t index = 0; index < region_count; ++index) {
    painter::region_draw_command command{};
    command.viewport_x = float(regions[index].x);
    command.viewport_y = float(regions[index].y);
    command.viewport_width = float(regions[index].size);
    command.viewport_height = float(regions[index].size);
    command.min_depth = 0.0f;
    command.max_depth = 1.0f;
    command.scissor_x = int32_t(regions[index].x);
    command.scissor_y = int32_t(regions[index].y);
    command.scissor_width = regions[index].size;
    command.scissor_height = regions[index].size;
    // Пустой слот не рисует ничего вовсе: у него просто нет пролётов. Тайл при этом остаётся
    // очищенным, а сила источника в записи каскада равна нулю, поэтому читать его никто не станет.
    const bool active = slot_active[index / cascade_count];
    command.data_index = index;
    command.first_span = span_cursor;
    command.span_count = active ? uint32_t(casters[index].size()) : 0u;
    std::memcpy(bytes.data() + command_offset, &command, sizeof(command));
    command_offset += sizeof(command);

    if (!active) continue;
    for (const auto& caster : casters[index]) {
      painter::region_draw_span span{};
      span.pair_index = caster.pair;
      span.first_instance = 0;
      span.instance_count = caster.instance_count;
      std::memcpy(bytes.data() + span_offset, &span, sizeof(span));
      span_offset += sizeof(span);
      span_cursor += 1;
    }
  }
  write_current_buffer(base, "shadow_region_commands", bytes.data(), needed);
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
    utils::error{}("PF08 cannot write UI commands (capacity {}, requested {})", frame.sub.size, bytes);
  }

  auto* destination = static_cast<uint8_t*>(frame.mapped) + frame.sub.offset;
  std::memcpy(destination, &count, sizeof(count));
  if (!commands.empty()) std::memcpy(destination + sizeof(count), commands.data(), commands.size_bytes());
}

void clear_overlay_commands(painter::graphics_base& base) {
  const uint32_t count = 0;
  write_current_buffer(base, "ui_commands", &count, sizeof(count));
}

void bind_texture_descriptor(painter::graphics_base& base, const painter::assets_base& assets,
                             const std::string_view descriptor_name) {
  const uint32_t slot = base.find_descriptor(descriptor_name);
  if (slot == painter::invalid_resource_slot) utils::error{}("PF08 descriptor '{}' is absent", descriptor_name);

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
      utils::error{}("PF08 could not submit image dump");
    }
  }
  if (device.waitForFences(fence, true, size_t(1000) * 1000 * 1000) != vk::Result::eSuccess) {
    utils::error{}("PF08 image dump timed out");
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
    utils::error{}("PF08 could not write dump '{}'", path);
  }

  device.destroy(fence);
  device.freeCommandBuffers(base.command_pool, task);
  allocator.destroyBuffer(staging, allocation);
  utils::info("PF08 dumped {}x{} final frame to '{}'", width, height, path);
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
      utils::error{}("PF08 pass timing count changed mid-run: {} -> {}", passes.size(), timings.size());
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
      utils::info("PF08 GPU timings: no samples collected");
      return;
    }

    const double count = double(samples);
    utils::info("PF08 GPU timings over {} frames (ms: average / minimum / maximum):", samples);
    for (const auto& pass : passes) {
      utils::info("  {:<24} {:6.3f} / {:6.3f} / {:6.3f}", pass.name, pass.total / count, pass.minimum, pass.maximum);
    }
    utils::info("  {:<24} {:6.3f} / {:6.3f}", "TOTAL ON GPU", frame_total / count, frame_minimum);
  }
};

} // namespace

int run_sky_view(const celestial_system& system, const view_options& raw_options) {
  view_options options = raw_options;

  if (!options.preset.empty()) {
    const std::string presets_path = std::string(PF08_RESOURCE_ROOT) + "/celestial/presets.tavl";
    const std::string text = file_io::read(presets_path, file_io::type::text);
    if (text.empty()) utils::error{}("PF08 could not read presets '{}'", presets_path);

    view_preset_list list;
    std::string diagnostics;
    if (!parse_view_presets(text, list, diagnostics)) {
      utils::error{}("PF08 presets '{}' have tavl diagnostics:\n{}", presets_path, diagnostics);
    }

    bool found = false;
    for (const auto& preset : list.presets) {
      if (preset.name != options.preset) continue;
      options.start_time_days = system.from_calendar(preset.year, preset.day, preset.hour);
      options.look_azimuth_deg = preset.look_azimuth_deg;
      options.look_altitude_deg = preset.look_altitude_deg;
      options.fixed_look = true;
      options.time_scale = 0.0;
      options.exposure.manual = true;
      options.exposure.manual_ev100 = preset.ev100;
      found = true;
      utils::info("PF08 preset '{}': year {} day {} {:.2f}h, camera {:.1f}/{:.1f}, fixed EV100 {:+.2f}",
                  preset.name, preset.year, preset.day, preset.hour,
                  preset.look_azimuth_deg, preset.look_altitude_deg, preset.ev100);
      break;
    }
    if (!found) utils::error{}("PF08 preset '{}' is not declared in presets.tavl", options.preset);
  }

  const std::string weather_path = std::string(PF08_RESOURCE_ROOT) + "/weather/presets.tavl";
  const std::string weather_text = file_io::read(weather_path, file_io::type::text);
  if (weather_text.empty()) utils::error{}("PF08 could not read weather presets '{}'", weather_path);

  weather_preset_list weather_presets;
  std::string weather_diagnostics;
  if (!parse_weather_presets(weather_text, weather_presets, weather_diagnostics)) {
    utils::error{}("PF08 weather presets '{}' have diagnostics:\n{}", weather_path, weather_diagnostics);
  }
  const weather_preset* initial_weather = find_weather_preset(weather_presets, options.weather_preset);
  if (initial_weather == nullptr) {
    utils::error{}("PF08 weather '{}' is not declared in weather/presets.tavl", options.weather_preset);
  }
  if (options.weather_transition_overridden &&
      (!std::isfinite(options.weather_transition_seconds) || options.weather_transition_seconds < 0.0)) {
    utils::error{}("PF08 weather transition must be finite and non-negative");
  }

  const auto apply_weather_overrides = [&options](weather_state state) {
    if (options.turbidity_overridden) state.aerosol_turbidity = options.atmosphere.turbidity;
    if (options.wind_direction_overridden) state.wind_direction_deg = options.wind_direction_deg;
    if (options.wind_strength_overridden) state.wind_strength_m = options.wind_strength_m;
    state.wind_direction_deg = normalize_weather_direction(state.wind_direction_deg);
    if (!std::isfinite(state.aerosol_turbidity) || state.aerosol_turbidity <= 0.0 ||
        !std::isfinite(state.wind_direction_deg) || !std::isfinite(state.wind_strength_m) ||
        state.wind_strength_m < 0.0) {
      utils::error{}("PF08 effective weather state is invalid: turbidity {}, wind {} deg / {} m",
                     state.aerosol_turbidity, state.wind_direction_deg, state.wind_strength_m);
    }
    return state;
  };

  weather_transition weather;
  weather.snap(initial_weather->name, apply_weather_overrides(state_from_preset(*initial_weather)));
  size_t weather_index = size_t(initial_weather - weather_presets.presets.data());
  const double weather_transition_seconds = options.weather_transition_overridden
                                              ? options.weather_transition_seconds
                                              : weather_presets.transition_seconds;
  utils::info("PF08 weather '{}': aerosol {:.2f}, wind {:.0f} deg / {:.2f} m, transition {:.1f} s",
              initial_weather->name, weather.state().aerosol_turbidity, weather.state().wind_direction_deg,
              weather.state().wind_strength_m, weather_transition_seconds);

  pending_width = options.width;
  pending_height = options.height;
  paused = options.paused;
  weather_cycle_requested = false;

  input::init input_runtime(&error_callback);
  input::events::init();
  painter::load_dispatcher1();

  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "PF08 weather effects";
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
      utils::error{}("PF08 requested unavailable validation layers");
    }
    instance_info.enabledLayerCount = uint32_t(painter::default_validation_layers.size());
    instance_info.ppEnabledLayerNames = painter::default_validation_layers.data();
  }

  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);
  const VkDebugUtilsMessengerEXT messenger =
    options.validation ? painter::create_debug_messenger(instance) : VK_NULL_HANDLE;

  GLFWwindow* window = input::create_window(options.width, options.height, "PF08 — weather effects");
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (input::create_window_surface(instance, window, nullptr, &surface) != uint32_t(vk::Result::eSuccess)) {
    utils::error{}("PF08 could not create Vulkan surface");
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
  const VkDevice device = device_maker.create({}, "pf08.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();

    const std::string resource_root = std::string(PF08_RESOURCE_ROOT) + "/";
    const std::string cache_path = utils::cache_folder() + "pf08_weather_sky.pipeline_cache";
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("pf08_weather_sky");
    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0) utils::error{}("PF08 could not commit render graph");
    base.set_surface(surface, options.width, options.height);
    base.resize_viewport(options.width, options.height);
    base.populate_constant_default_values();
    base.change_render_graph(base.find_render_graph("pf08_weather_sky"));

    painter::assets_base assets(device, physical.handle);
    assets.create_fence();
    assets.create_allocator(instance);
    assets.create_command_buffer(queues.transfer, queues.graphics);
    assets.set_graphics_base(&base);
    assets.create_default_texture();

    // Опорная геометрия среза 4. Меш ровно один — единичный куб; всё остальное делает инстанс.
    const auto cube = make_unit_cube();
    const auto cube_mesh = assets.register_buffer_storage("pf08.unit_cube");
    assets.create_buffer_storage(cube_mesh,
                                 painter::buffer_create_info{"scene_geometry", uint32_t(cube.size()), 0});
    assets.populate_buffer_storage(
      cube_mesh, std::span(reinterpret_cast<const uint8_t*>(cube.data()), cube.size() * sizeof(cube[0])),
      std::span<const uint8_t>{});
    assets.mark_ready_buffer_slot(cube_mesh);

    const auto scene_group = base.find_draw_group("scene_draw_group");
    // Резерв пары считается в ИНСТАНСАХ и занимает место в буфере отрисовок под каждый из них, поэтому
    // просить с запасом здесь не бесплатно: соседняя пара сдвигается ровно на этот резерв. Фикстуре
    // хватает тридцати двух при двенадцати используемых.
    const uint32_t scene_pair = base.register_pair(scene_group, cube_mesh, 32);
    const auto fixture = make_fixture_instances();
    double caster_height = fixture_caster_height(fixture);
    write_fixture(base, scene_pair, fixture, uint32_t(cube.size()));

    // Земля — вторая пара в той же группе отрисовки. Проход рисует ВСЕ пары группы, поэтому диск
    // попадает в кадр сам; а в проход теней он не попадает, потому что там пролёты заданы явно и
    // ссылаются только на пару кастеров. Земля обязана тени принимать, но отбрасывать ей нечего.
    const auto ground = make_ground_disc(options.ground_radius_m, system.config().planet.radius_km * 1000.0,
                                         ground_ring_count, ground_segment_count);
    const auto ground_mesh = assets.register_buffer_storage("pf08.ground_disc");
    assets.create_buffer_storage(ground_mesh,
                                 painter::buffer_create_info{"scene_geometry", uint32_t(ground.size()), 0});
    assets.populate_buffer_storage(
      ground_mesh, std::span(reinterpret_cast<const uint8_t*>(ground.data()), ground.size() * sizeof(ground[0])),
      std::span<const uint8_t>{});
    assets.mark_ready_buffer_slot(ground_mesh);
    const uint32_t ground_pair = base.register_pair(scene_group, ground_mesh, 1);
    utils::info("PF08 ground disc: radius {:.0f} m, {} vertices", options.ground_radius_m, ground.size());

    // Участок долины — СТАТИЧЕСКИЙ меш у начала координат, третья пара группы. Рисуется раньше диска,
    // чтобы ранний тест глубины отсёк те его пиксели, что окажутся под участком.
    const auto valley = make_valley_patch();
    for (const auto& vertex : valley) caster_height = std::max(caster_height, double(vertex.py));
    const auto valley_mesh = assets.register_buffer_storage("pf08.valley_patch");
    assets.create_buffer_storage(valley_mesh,
                                 painter::buffer_create_info{"scene_geometry", uint32_t(valley.size()), 0});
    assets.populate_buffer_storage(
      valley_mesh, std::span(reinterpret_cast<const uint8_t*>(valley.data()), valley.size() * sizeof(valley[0])),
      std::span<const uint8_t>{});
    assets.mark_ready_buffer_slot(valley_mesh);
    const uint32_t valley_pair = base.register_pair(scene_group, valley_mesh, 1);
    {
      // Участок приподнят на два сантиметра над диском: к своему краю рельеф выходит в ноль, и там обе
      // поверхности совпадают. Без подъёма они дерутся за глубину и покрываются рябью.
      const std::array<scene_instance, 1> instance{
        scene_instance{glm::vec4(0.0f, 0.02f, 0.0f, 0.0f), glm::vec4(0.5f, 0.5f, 0.5f, 0.0f),
                       glm::vec4(glm::vec3(float(options.atmosphere.ground_albedo)), 0.0f)}};
      for (uint32_t offset = 0; offset < base.frames_in_flight(); ++offset) {
        const auto frame = base.get_current_instance_resource_frame(valley_pair, offset);
        const auto indirect = base.get_current_indirect_resource_frame(valley_pair, offset);
        std::memcpy(static_cast<uint8_t*>(frame.mapped) + frame.sub.offset, instance.data(),
                    instance.size() * sizeof(instance[0]));
        const VkDrawIndirectCommand command{uint32_t(valley.size()), 1, 0, 0};
        std::memcpy(static_cast<uint8_t*>(indirect.mapped) + indirect.sub.offset, &command, sizeof(command));
      }
    }
    utils::info("PF08 valley patch: {:.0f} m across, {} vertices", valley_half_size * 2.0, valley.size());

    // Заросли: два меша и две пары. LOD здесь — это ВЫБОР ПАРЫ, а не упрощение на лету: дальние кусты
    // рисуются мешем с двумя лезвиями вместо пяти, и разбор идёт на хосте по дальности от камеры.
    const auto shrub_near_mesh_data = make_shrub(shrub_blades_near);
    const auto shrub_far_mesh_data = make_shrub(shrub_blades_far);
    const auto shrub_near_mesh = assets.register_buffer_storage("pf08.shrub_near");
    const auto shrub_far_mesh = assets.register_buffer_storage("pf08.shrub_far");
    for (const auto& [slot, data] : std::initializer_list<std::pair<uint32_t, const std::vector<scene_vertex>&>>{
           {shrub_near_mesh, shrub_near_mesh_data}, {shrub_far_mesh, shrub_far_mesh_data}}) {
      assets.create_buffer_storage(slot,
                                   painter::buffer_create_info{"scene_geometry", uint32_t(data.size()), 0});
      assets.populate_buffer_storage(
        slot, std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(data[0])),
        std::span<const uint8_t>{});
      assets.mark_ready_buffer_slot(slot);
    }
    const uint32_t shrub_near_pair = base.register_pair(scene_group, shrub_near_mesh, options.foliage_count);
    const uint32_t shrub_far_pair = base.register_pair(scene_group, shrub_far_mesh, options.foliage_count);
    const auto shrubs = scatter_shrubs(options.foliage_count);
    for (const auto& item : shrubs) caster_height = std::max(caster_height, double(item.y + item.height));
    utils::info("PF08 foliage: {} shrubs, near mesh {} vertices, far mesh {}", shrubs.size(),
                shrub_near_mesh_data.size(), shrub_far_mesh_data.size());
    std::vector<scene_instance> shrub_near_instances;
    std::vector<scene_instance> shrub_far_instances;
    shrub_near_instances.reserve(shrubs.size());
    shrub_far_instances.reserve(shrubs.size());

    // Раскладка атласа теней считается ОДИН раз: размеры тайлов постоянны, а укладка детерминирована,
    // поэтому пересчитывать её каждый кадр значило бы каждый кадр получать тот же ответ.
    std::array<painter::atlas_region, cascade_total> shadow_regions{};
    uint32_t shadow_atlas_width = 0;
    uint32_t shadow_atlas_height = 0;
    {
      std::array<uint32_t, cascade_total> sizes{};
      sizes.fill(cascade_tile_size);
      // Размер атласа читается из ОБЪЯВЛЕНИЯ, а не повторяется здесь константой: разойдись эти два
      // числа, укладка молча съехала бы, и каскады читались бы из чужих тайлов.
      const uint32_t size_index = base.find_constant_value("shadow_atlas_size");
      if (size_index == UINT32_MAX) utils::error{}("PF08 value 'shadow_atlas_size' is absent");
      const auto [atlas_width, atlas_height, unused_depth] =
        base.constant_values[size_index].current_value;
      if (!painter::allocate_atlas_regions(atlas_width, atlas_height, sizes, shadow_regions)) {
        utils::error{}("PF08 shadow atlas {}x{} cannot hold {} tiles of {}", atlas_width, atlas_height,
                       cascade_total, cascade_tile_size);
      }
      shadow_atlas_width = atlas_width;
      shadow_atlas_height = atlas_height;
    }

    const std::string common_resources = std::string(PLAYGROUND_COMMON_RESOURCE_ROOT) + "/";
    playground::visage_overlay overlay(
      common_resources + "fonts/crimson.roman.ttf", resource_root + "ui/pf08_controls.lua",
      playground::overlay_description{
        "PF08 — Weather effects",
        "Slice 1: one weather state feeds atmosphere and the shared foliage/shadow wind field",
        "WASD/QE look · Space pause · T weather · [ ] time speed · - = exposure"});
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
    weather_cycle_key = input::glfw_key_from_canonical("key_t");
    input::set_window_callback(window, &key_callback);
    input::set_window_callback(window, &mouse_button_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_DISABLED);
    input::set_raw_mouse_motion(window);

    playground::free_camera camera;
    // Камера смотрит чуть выше горизонта: здесь одновременно читаются небо, долина и движение зарослей.
    // Глаз стоит НАД сценой, а не в её плоскости. Пока камера была в нуле, атмосфера считала
    // наблюдателя на двух метрах, а геометрия рисовалась с уровня земли: основания предметов ложились
    // ровно на линию горизонта, и всё выглядело стоящим бесконечно далеко. Сцена в метрах, высота
    // атмосферы в километрах — отсюда и множитель.
    camera.position = {0.0f, float(options.march.camera_height_km * 1000.0), 0.0f};
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
      utils::warn("PF08 GPU timestamps are unavailable on this device; per-pass cost will stay unmeasured");
    }

    // Таблица многократного рассеяния живёт на условном счётчике: она функция параметров среды и
    // ничего больше. Хост двигает счётчик при старте и при изменении аэрозоля weather state;
    // следующие срезы расширят invalidation только параметрами, от которых LUT действительно зависит.
    const uint32_t atmosphere_counter = base.find_counter("atmosphere_cache");
    if (atmosphere_counter == painter::invalid_resource_slot) {
      utils::error{}("PF08 counter 'atmosphere_cache' is absent from the configured graph");
    }
    double baked_turbidity = -1.0;

    colour_script script;
    {
      const std::string script_path = std::string(PF08_RESOURCE_ROOT) + "/celestial/colour_script.tavl";
      const std::string text = file_io::read(script_path, file_io::type::text);
      if (text.empty()) utils::error{}("PF08 could not read colour script '{}'", script_path);

      std::string diagnostics;
      if (!parse_colour_script(text, script, diagnostics)) {
        utils::error{}("PF08 colour script '{}' has tavl diagnostics:\n{}", script_path, diagnostics);
      }
    }

    // Календарный старт перекрывает абсолютный: «третий год, сотые сутки» задаётся человеком, а
    // время в сутках от эпохи — машиной.
    double game_time_days = options.start_year != 0
                              ? system.from_calendar(options.start_year, options.start_day_of_year,
                                                     options.start_hour)
                              : options.start_time_days;
    double time_scale = options.time_scale;
    // Адаптация обязана стартовать уже привыкшей, иначе любой дамп ловит её переходный процесс. Но
    // замер приходит с видеокарты и на первом кадре его ещё нет, поэтому вместо угадывания начальное
    // значение ПРИЩЁЛКИВАЕТСЯ к первому же настоящему замеру, а до него экспозиция не двигается.
    double current_ev100 = options.exposure.manual_ev100;
    bool exposure_settled = options.exposure.manual;
    double exposure_bias_stops = 0.0;
    double wall_seconds = 0.0;
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
        utils::info("PF08 time scale {:.4f} game days per real second", time_scale);
      }
      if (exposure_step != 0.0) {
        exposure_bias_stops += exposure_step;
        exposure_step = 0.0;
        utils::info("PF08 exposure compensation {:+.1f} stops", exposure_bias_stops);
      }
      if (weather_cycle_requested) {
        weather_index = (weather_index + 1) % weather_presets.presets.size();
        const auto& next = weather_presets.presets[weather_index];
        weather.set_target(next.name, apply_weather_overrides(state_from_preset(next)), weather_transition_seconds);
        weather_cycle_requested = false;
        utils::info("PF08 weather transition '{}' -> '{}' over {:.1f} s", weather.source_name(),
                    weather.target_name(), weather_transition_seconds);
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

      // Fixed-frame mode advances weather by the same deterministic clock as wind, astronomy and
      // exposure. A transition dump must not depend on how quickly this machine compiled a shader.
      const double step_seconds = options.frames != 0 ? 1.0 / 60.0 : double(dt);
      weather.advance(step_seconds);
      auto frame_atmosphere = options.atmosphere;
      frame_atmosphere.turbidity = weather.state().aerosol_turbidity;

      base.prepare_frame();
      if (baked_turbidity != frame_atmosphere.turbidity) {
        base.inc_counter(atmosphere_counter);
        baked_turbidity = frame_atmosphere.turbidity;
      }
      if (gpu_profiler.has_results()) gpu_timings.add(gpu_profiler.passes(), gpu_profiler.frame_milliseconds());

      // Шаг времени кадра. При фиксированном числе кадров он постоянный: дамп обязан быть
      // воспроизводимым, а привязка к реальному dt сделала бы его зависимым от нагрузки машины.
      //
      // Значение ОДНО и на игровое время, и на адаптацию экспозиции. Разойдясь, они дают тихо неверный
      // дамп: игровое время шло бы как на шестидесяти кадрах, а глаз привыкал бы со скоростью машины,
      // и восход в дампе проходил бы с совсем другой экспозицией, чем в окне.
      // Часы РЕАЛЬНОГО времени для ветра. При фиксированном числе кадров они идут постоянным шагом
      // вместе с игровыми: дамп обязан быть воспроизводимым целиком, а не наполовину.
      wall_seconds += step_seconds;
      if (!paused) game_time_days += time_scale * step_seconds;
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

      // Диск земли следует за камерой по горизонтали. Для сферы это точно, а не приближённо:
      // наблюдатель всегда стоит на её вершине, и «вершина» — это ровно то место, где он стоит.
      // Разбор зарослей по дальности. Делается каждый кадр и намеренно в лоб: шесть тысяч кустов —
      // это порядок, на котором простой проход по массиву ещё дешевле любой структуры, и цена этого
      // видна в отчёте наравне с проходами видеокарты.
      shrub_near_instances.clear();
      shrub_far_instances.clear();
      const float range_squared = float(options.foliage_range_m * options.foliage_range_m);
      const float lod_squared = float(options.foliage_lod_m * options.foliage_lod_m);
      for (const auto& item : shrubs) {
        const float dx = item.x - camera.position.x;
        const float dz = item.z - camera.position.z;
        const float distance_squared = dx * dx + dz * dz;
        if (distance_squared > range_squared) continue;
        const scene_instance instance{
          glm::vec4(item.x, item.y, item.z, 1.0f),
          glm::vec4(item.width * 0.5f, item.height * 0.5f, item.width * 0.5f, item.phase),
          glm::vec4(item.albedo, item.yaw)};
        (distance_squared <= lod_squared ? shrub_near_instances : shrub_far_instances).push_back(instance);
      }
      write_pair_current(base, shrub_near_pair, shrub_near_instances, uint32_t(shrub_near_mesh_data.size()));
      write_pair_current(base, shrub_far_pair, shrub_far_instances, uint32_t(shrub_far_mesh_data.size()));

      const std::array<scene_instance, 1> ground_instance{make_ground_instance(
        camera.position.x, camera.position.z, glm::vec3(float(options.atmosphere.ground_albedo)))};
      write_pair_current(base, ground_pair, ground_instance, uint32_t(ground.size()));

      // Теневые источники выбираются КАЖДЫЙ кадр по яркости, а не назначаются раз и навсегда. Днём
      // это обе звезды, в сумерках может оказаться компаньон и луна, ночью — две луны. Одно правило
      // вместо трёх режимов; передача главенства получается сама, потому что вклад каждого источника
      // умножается на его долю света, и перестановка тел между слотами ничего не меняет.
      const auto shadow_sources = select_shadow_sources(state);
      std::array<cascade_record, cascade_total> cascades{};
      std::array<bool, shadow_source_count> slot_active{};
      for (uint32_t slot = 0; slot < shadow_source_count; ++slot) {
        slot_active[slot] = slot < options.shadow_sources && shadow_sources[slot].active &&
                            shadow_sources[slot].strength > 0.0;
        if (!slot_active[slot]) continue;
        build_cascades(shadow_sources[slot], camera, aspect, vertical_fov, near_plane,
                       float(options.shadow_far_m), float(caster_height),
                       float(options.cascade_split_lambda), std::span(shadow_regions).subspan(slot * cascade_count, cascade_count),
                       shadow_atlas_width, shadow_atlas_height,
                       std::span(cascades).subspan(slot * cascade_count, cascade_count));
      }
      write_current_buffer(base, "cascade_buffer", cascades.data(), sizeof(cascades));
      // Кто бросает тень в каждый каскад. Коробки и рельеф — во все: гребень долины обязан затенять
      // проход, а без рельефа в карте тень от него просто не появится. Заросли — только в ближние
      // каскады: дальше их тень занимает доли пикселя, а стоит полного прохода по тысячам инстансов.
      std::array<std::vector<shadow_caster>, cascade_total> casters{};
      for (uint32_t index = 0; index < cascade_total; ++index) {
        auto& list = casters[index];
        list.clear();
        list.push_back({scene_pair, uint32_t(fixture.size()), uint32_t(cube.size())});
        list.push_back({valley_pair, 1u, uint32_t(valley.size())});
        // Заросли бросают тень в столько ближних каскадов, сколько попросили, и ОБЕИМИ парами LOD.
        // Прежде здесь стоял только ближний каскад и только ближняя пара — то есть тень получали
        // 584 куста из 4696 нарисованных, и только те, что ближе 3.7 м. Ограничение выглядело
        // экономией, а на деле просто прятало эффект: чтобы увидеть тень травы, надо было в неё
        // почти упереться.
        if (index % cascade_count < options.foliage_shadow_cascades) {
          if (!shrub_near_instances.empty()) {
            list.push_back({shrub_near_pair, uint32_t(shrub_near_instances.size()),
                            uint32_t(shrub_near_mesh_data.size())});
          }
          if (!shrub_far_instances.empty()) {
            list.push_back({shrub_far_pair, uint32_t(shrub_far_instances.size()),
                            uint32_t(shrub_far_mesh_data.size())});
          }
        }
      }
      write_shadow_regions(base, casters, shadow_regions, slot_active);

      // Экспозиция догоняет цель по-разному вверх и вниз. Постоянные заданы в реальных секундах, а не
      // в игровых: привыкает глаз игрока, а не планета.
      // Замер учитывает и прямой свет, и рассеянный. Второе слагаемое решающее в сумерках: прямого
      // света там нет вовсе, а небо продолжает светить.
      const double scene_luminance = read_scene_luminance(base);
      const bool metered = !options.exposure.manual && scene_luminance > 0.0;
      const double goal_ev100 = options.exposure.manual ? options.exposure.manual_ev100
                                                        : target_ev100(scene_luminance, options.exposure);
      if (metered) {
        if (exposure_settled) {
          // Ускоренное время сжимает адаптацию во столько же раз. На паузе мир не ускорен вовсе,
          // поэтому берётся ДЕЙСТВУЮЩИЙ темп, а не выставленный: иначе остановленное на перемотке
          // время оставило бы мгновенный глаз, и поворот камеры менял бы экспозицию скачком.
          // Множитель ограничен единицей сверху — замедление не имеет права делать глаз медленнее
          // штатного, ему и так некуда торопиться.
          const double effective_scale = paused ? 0.0 : time_scale;
          const double compression =
            std::min(1.0, options.exposure.reference_time_scale / std::max(effective_scale, 1e-12));
          const double seconds = (goal_ev100 > current_ev100 ? options.exposure.adapt_brighter_seconds
                                                             : options.exposure.adapt_darker_seconds) *
                                 compression;
          const double rate = 1.0 - std::exp(-step_seconds / std::max(seconds, 1e-6));
          current_ev100 += (goal_ev100 - current_ev100) * rate;
        } else {
          current_ev100 = goal_ev100;
          exposure_settled = true;
        }
      } else if (options.exposure.manual) {
        current_ev100 = goal_ev100;
      }

      auto output = options.output;
      output.exposure = exposure_from_ev100(current_ev100 - exposure_bias_stops);
      // Первые кадры замера ещё нет, и печатать для них цель бессмысленно: она посчитана от нулевой
      // яркости и к экспозиции отношения не имеет.
      if (options.trace_exposure && (metered || options.exposure.manual)) {
        utils::info("PF08 t={:.5f} · {:5.2f}° · L {:.4g} nits · target {:+.2f} · EV {:+.2f} · lag {:+.2f} stops",
                    game_time_days, state.stars[0].altitude_deg, scene_luminance, goal_ev100, current_ev100,
                    goal_ev100 - current_ev100);
      }
      evaluate_colour_script(script, state.stars[0].altitude_deg, output);
      // Звёздное поле берёт базис горизонта с замедленного времени: сутки идут за двадцать четыре
      // реальные минуты, и честное вращение неба выглядит вертолётом. Светила и луны при этом
      // продолжают идти по-настоящему, поэтому замедление касается только рисунка созвездий.
      const double star_time =
        options.start_time_days + (game_time_days - options.start_time_days) * output.star_rotation_scale;
      const auto star_frame = system.evaluate(star_time);
      // Высота наблюдателя для атмосферы берётся из КАМЕРЫ, а не из настройки: поднявшись, игрок
      // обязан увидеть, как отодвигается горизонт. Настройка задаёт только начальное положение.
      auto march = options.march;
      march.camera_height_km = double(camera.position.y) * 0.001;
      auto sky_block = pack_sky_block(state, star_frame, frame_atmosphere, march, output,
                                      system.config().planet.radius_km, system.config().moons);
      // Ветер: направление из настроек, время — РЕАЛЬНОЕ, а не игровое. Перемотка суток не имеет
      // права разгонять качание веток: ветер живёт в том же времени, что и глаз наблюдателя.
      const double wind_angle = weather.state().wind_direction_deg * 3.14159265358979323846 / 180.0;
      sky_block.wind_params =
        glm::vec4(float(std::sin(wind_angle)), float(-std::cos(wind_angle)),
                  float(weather.state().wind_strength_m), float(wall_seconds));
      sky_block.shadow_bodies = glm::vec4(slot_active[0] ? shadow_sources[0].body_code : -1.0f,
                                          slot_active[1] ? shadow_sources[1].body_code : -1.0f, 0.0f, 0.0f);
      write_current_buffer(base, "sky_buffer", &sky_block, sizeof(sky_block));

      const auto calendar = system.to_calendar(game_time_days);
      const std::array<std::string, 9> details{
        std::format("Year {} · beat {}/{} · day {}/{} · {:02}:{:02} · {:.4f} days per real second{}", calendar.year,
                    calendar.beat_year, system.binary_beat_years(), calendar.day,
                    uint32_t(system.planet_year_days()), calendar.hour,
                    calendar.minute, time_scale, paused ? " · PAUSED" : ""),
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
        std::format("Exposure: EV100 {:+.2f}{} · target {:+.2f} · bias {:+.1f} · multiplier {:.3e}",
                    current_ev100, options.exposure.manual ? " (fixed)" : "", goal_ev100,
                    options.exposure.bias_stops + exposure_bias_stops, output.exposure),
        std::format("Scene: direct {:.4g} lx · metered frame luminance {:.4g} nits{}",
                    state.horizontal_illuminance_lx, scene_luminance,
                    goal_ev100 <= options.exposure.min_ev100 + 1e-6 ? " · adaptation at its night floor" : ""),
        weather.active()
          ? std::format("Weather: {} -> {} · {:.0f}% · aerosol {:.2f} · wind {:.0f}° / {:.2f} m",
                        weather.source_name(), weather.target_name(), weather.progress() * 100.0,
                        weather.state().aerosol_turbidity, weather.state().wind_direction_deg,
                        weather.state().wind_strength_m)
          : std::format("Weather: {} · aerosol {:.2f} · wind {:.0f}° / {:.2f} m",
                        weather.target_name(), weather.state().aerosol_turbidity,
                        weather.state().wind_direction_deg, weather.state().wind_strength_m),
        std::format("Foliage: {} near + {} far of {} · LOD {:.0f} m · range {:.0f} m",
                    shrub_near_instances.size(), shrub_far_instances.size(), shrubs.size(),
                    options.foliage_lod_m, options.foliage_range_m),
        std::format("Colour script: tint ({:.2f} {:.2f} {:.2f}) · saturation {:.2f} · contrast {:.2f}",
                    output.grade_tint.x, output.grade_tint.y, output.grade_tint.z, output.grade_saturation,
                    output.grade_contrast)};
      if (options.show_overlay) {
        overlay.set_detail_lines(details);
        const uint64_t frame_delta_us = uint64_t(std::max(
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<float>(dt)).count(),
          int64_t{1}));
        const uint64_t timestamp_us =
          uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count());
        if (!overlay.update(frame_delta_us, timestamp_us)) utils::warn("PF08 Visage overlay update failed");
        write_overlay_buffers(base, overlay);
      } else {
        clear_overlay_commands(base);
      }

      context.prepare();
      context.draw();
      base.submit_frame();
      frame_pacer.wait();
      frames_total += 1;
      if (options.frames != 0 && frames_total >= options.frames) break;
    }

    vk::Device(device).waitIdle();
    // Состояние экспозиции печатается рядом с дампом: сравнивать кадры можно только зная, какой
    // экспозицией они сняты, а по самой картинке это не восстанавливается.
    utils::info("PF08 exposure at t={:.5f}: frame luminance {:.4g} nits · EV100 {:+.2f} · multiplier {:.4e}",
                game_time_days, read_scene_luminance(base), current_ev100,
                exposure_from_ev100(current_ev100 - exposure_bias_stops));
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

} // namespace devils_engine::pf08
