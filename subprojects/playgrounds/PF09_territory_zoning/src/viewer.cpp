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
#include "devils_engine/visage/font.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"

using namespace devils_engine;
#include "devils_engine/utils/hash.h"

#include "navigate.h"
#include "tactics.h"
#include "titles.h"
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

// Вершина мирового интерфейса. Якорь — в мире, раскладка — в пикселях: чисто мировой прямоугольник
// растягивается наклоном камеры и перестаёт быть текстом, а чисто экранный отрывается от персонажа.
struct label_vertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  uint32_t tint = 0;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  float u = -1.0f;   // отрицательный u — сплошная заливка, а не глиф
  float v = -1.0f;
};
static_assert(sizeof(label_vertex) == 32);

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
    case zone_kind::crossroad: return pack(168, 163, 152, 255);
    case zone_kind::square: return pack(186, 178, 162, 255);
    case zone_kind::yard: return pack(132, 156, 112, 255);
    case zone_kind::hall: return pack(196, 172, 140, 255);
    case zone_kind::wall: return pack(112, 96, 82, 255);
    case zone_kind::door: return pack(214, 150, 90, 255);
    case zone_kind::stair: return pack(206, 196, 120, 255);
    case zone_kind::landmark: return pack(214, 150, 90, 255);
    case zone_kind::settlement: return pack(96, 104, 126, 255);
    default: return pack(110, 110, 120, 255);
  }
}

// Раскраска по контролю. Не декоративный режим: «кто держит район» — вопрос, на который зонирование
// отвечает подъёмом по вложенности, и увидеть ответ надо ровно там, где он берётся, — на местах, а не
// на абстрактном узле, у которого и формы-то нет.
uint32_t faction_tint(const uint32_t faction) {
  switch (faction) {
    case 0: return pack(120, 120, 128, 255);
    case 1: return pack(92, 126, 190, 255);
    case 2: return pack(96, 160, 112, 255);
    case 3: return pack(186, 154, 82, 255);
    case 4: return pack(150, 110, 168, 255);
    default: return pack(196, 76, 68, 255);   // преступные силы — все оттенки одного красного
  }
}

uint32_t crime_tint(const uint32_t crime) {
  const float t = std::clamp(float(crime) / 1000.0f, 0.0f, 1.0f);
  return pack(uint32_t(70.0f + 170.0f * t), uint32_t(150.0f - 100.0f * t), uint32_t(110.0f - 70.0f * t), 255);
}

// Раскраска по праву: чья это земля и чей закон здесь в силе. Разные вопросы и разные картинки — первая
// про собственность, вторая про то, докуда достаёт власть.
uint32_t owner_tint(const uint32_t owner) {
  if (owner == 0) return pack(120, 120, 128, 255);
  const auto h = utils::splitmix(uint64_t(owner) + 1ull);
  return pack(96 + uint32_t(h & 0x7f), 96 + uint32_t((h >> 21) & 0x7f), 96 + uint32_t((h >> 42) & 0x7f), 255);
}

uint32_t law_tint(const title_book& titles, const title_id source) {
  if (source == invalid_title) return pack(40, 40, 46, 255);   // сюда не достаёт ничей закон
  const auto* record = titles.find(source);
  switch (record == nullptr ? title_rank::count : record->rank) {
    case title_rank::realm: return pack(96, 150, 200, 255);
    case title_rank::city: return pack(120, 180, 130, 255);
    case title_rank::district: return pack(206, 140, 70, 255);
    default: return pack(120, 120, 128, 255);
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
  std::vector<label_vertex> labels;
  std::vector<zone_key> slots;   // slot -> ключ зоны, для наведения и выделения
};

// Прямоугольник мирового интерфейса в пикселях от якоря. Координаты экранные, поэтому y растёт ВНИЗ:
// так же, как в clip-пространстве Vulkan, и незачем переворачивать знак в трёх местах.
void emit_label_quad(frame_geometry& out, const glm::vec3 anchor, const glm::vec2 lower, const glm::vec2 upper,
                     const uint32_t tint) {
  const auto corner = [&](const float px, const float py) {
    out.labels.push_back({anchor.x, anchor.y, anchor.z, tint, px, py, -1.0f, -1.0f});
  };
  corner(lower.x, lower.y);
  corner(upper.x, lower.y);
  corner(upper.x, upper.y);
  corner(lower.x, lower.y);
  corner(upper.x, upper.y);
  corner(lower.x, upper.y);
}

// Строка тем же MSDF-атласом, что и экранный overlay: второй атлас ради подписей в мире не нужен, а его
// метрики движок и так отдаёт наружу. Возвращает ширину, чтобы вызывающий мог отцентрировать подложку.
float emit_label_text(frame_geometry& out, const visage::font_t& font, const glm::vec3 anchor,
                      const std::string_view text, const float left_px, const float baseline_px,
                      const float size_px, const uint32_t tint) {
  float cursor = left_px;
  for (const unsigned char symbol : text) {
    const auto* glyph = font.find_glyph(uint32_t(symbol));
    if (glyph == nullptr) continue;

    if (glyph->w > 0 && glyph->h > 0) {
      // ВАЖНО про метрики. `pb`/`pt` в этом шрифте уже зеркалены упаковщиком атласа под y-вниз:
      // хранится `pb = 1 - pt_вверх`, `pt = 1 - pb_вверх`. То есть это НЕ «низ и верх» в привычном
      // смысле, а смещения вниз от верха строки, отстоящего от базовой линии на один em. Прочитать их
      // как y-вверх — и высокие буквы встают правильно, а короткие уезжают вверх: у них маленький `pt`,
      // и квад цепляется не за ту сторону. Выглядит это как пляшущая строка, а не как явная ошибка.
      const float line_top = baseline_px - size_px;
      const float left = cursor + float(glyph->pl) * size_px;
      const float right = cursor + float(glyph->pr) * size_px;
      const float top = line_top + float(glyph->pb) * size_px;
      const float bottom = line_top + float(glyph->pt) * size_px;

      // А вот atlas-bounds НЕ зеркалены: сырые строки атласа лежат в текстуре как есть, поэтому верх
      // квада сэмплит `at/H` (большее v), низ — `ab/H`.
      const float u0 = float(glyph->al / double(font.width));
      const float u1 = float(glyph->ar / double(font.width));
      const float v_bottom = float(glyph->ab / double(font.height));
      const float v_top = float(glyph->at / double(font.height));

      const auto corner = [&](const float px, const float py, const float u, const float v) {
        out.labels.push_back({anchor.x, anchor.y, anchor.z, tint, px, py, u, v});
      };
      corner(left, bottom, u0, v_bottom);
      corner(right, bottom, u1, v_bottom);
      corner(right, top, u1, v_top);
      corner(left, bottom, u0, v_bottom);
      corner(right, top, u1, v_top);
      corner(left, top, u0, v_top);
    }
    cursor += float(glyph->advance) * size_px;
  }
  return cursor - left_px;
}

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

// Предмет: коробка со стороной в два радиуса. Круг рисовать незачем — предмет и в модели круг лишь для
// расчёта обхода, а на экране важно, что он стоит именно здесь и именно такой высоты.
void emit_box(frame_geometry& out, const glm::vec2 centre, const float radius, const float low,
              const float high, const uint32_t top_tint, const uint32_t side_tint, const uint32_t slot);

void emit_line(frame_geometry& out, const glm::vec2 a, const glm::vec2 b, const float height, const uint32_t tint) {
  out.lines.push_back({a.x, height, a.y, tint, no_slot, 0, 0, 0});
  out.lines.push_back({b.x, height, b.y, tint, no_slot, 0, 0, 0});
}

void emit_box(frame_geometry& out, const glm::vec2 centre, const float radius, const float low,
              const float high, const uint32_t top_tint, const uint32_t side_tint, const uint32_t slot) {
  const glm::vec2 corners[4] = {{centre.x - radius, centre.y - radius},
                                {centre.x + radius, centre.y - radius},
                                {centre.x + radius, centre.y + radius},
                                {centre.x - radius, centre.y + radius}};
  push(out, corners[0], high, top_tint, slot);
  push(out, corners[1], high, top_tint, slot);
  push(out, corners[2], high, top_tint, slot);
  push(out, corners[0], high, top_tint, slot);
  push(out, corners[2], high, top_tint, slot);
  push(out, corners[3], high, top_tint, slot);
  for (uint32_t i = 0; i < 4; ++i) {
    emit_wall(out, corners[i], corners[(i + 1) % 4], low, high, side_tint, slot);
  }
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
        "WASD pan | wheel zoom | LMB select | QE yaw | RF pitch | ZX floor | C cutaway | V route | N names | "
        "O props | B tactics | M control map | K toggle door | G agents | P portals | H walls | Esc quit"});

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

    // Центр поселения — это чаще всего площадь, и окно открывалось на пустом пятне мостовой. Смещаемся к
    // ближайшему ЗДАНИЮ: смотреть тут интересно на застройку, а не на то, что она окружает.
    {
      const auto* home = store.sector(sector_of(centre.x), sector_of(centre.y));
      double best = 1.0e30;
      glm::dvec2 target = centre;
      if (home != nullptr) {
        for (const auto& record : home->zones) {
          if (record.kind != zone_kind::hall || record.floor != 0) continue;
          const glm::dvec2 spot{(record.bounds.lower.x + record.bounds.upper.x) * 0.5,
                                (record.bounds.lower.z + record.bounds.upper.z) * 0.5};
          const double distance = (spot.x - centre.x) * (spot.x - centre.x) + (spot.y - centre.y) * (spot.y - centre.y);
          if (distance < best) {
            best = distance;
            target = spot;
          }
        }
      }
      centre = target;
      store.focus(centre);
    }

    input::events::clear_bindings();
    bind_key("pan_up", "key_w");
    bind_key("pan_down", "key_s");
    bind_key("pan_left", "key_a");
    bind_key("pan_right", "key_d");
    bind_key("toggle_agents", "key_g");
    bind_key("toggle_portals", "key_p");
    bind_key("toggle_walls", "key_h");
    bind_key("floor_down", "key_z");
    bind_key("floor_up", "key_x");
    bind_key("toggle_cutaway", "key_c");
    bind_key("toggle_routes", "key_v");
    bind_key("toggle_names", "key_n");
    bind_key("toggle_door", "key_k");
    bind_key("toggle_props", "key_o");
    bind_key("toggle_tactics", "key_b");
    bind_key("cycle_control", "key_m");
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
    bool show_props = true;
    bool show_tactics = options.start_tactics;
    // 0 — по виду места, 1 — кто держит район, 2 — насколько тут опасно.
    uint32_t control_mode = options.start_control_mode;

    // Право живёт отдельным файлом и целиком в памяти: «кто правит этим городом» обязано отвечаться и
    // тогда, когда сам город ещё не подгружен.
    title_book titles;
    titles.load(titles_path(options.world_root));
    const realm_view realm{&store, &titles};
    bool control_latch = false;
    bool walls_latch = false;
    bool props_latch = false;
    bool tactics_latch = false;
    double pitch_deg = 42.0;
    float yaw_rad = 0.0f;
    bool agents_latch = false;
    bool portals_latch = false;

    int32_t current_floor = options.start_floor;
    bool cutaway = options.start_cutaway;
    bool show_routes = true;
    bool show_names = true;
    bool floor_down_latch = false;
    bool floor_up_latch = false;
    bool cutaway_latch = false;
    bool routes_latch = false;
    bool names_latch = false;
    bool door_latch = false;
    uint32_t toggled_doors = 0;

    // От чьего лица задаются вопросы про вход. Партия — посторонний: так видно, где её не ждут.
    constexpr uint32_t party_actor = 777;

    std::vector<agent> walkers;
    std::vector<zone_key> walkable;
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

      const auto latched = [](const std::string_view name, bool& latch) {
        const bool pressed = input::events::is_pressed(name);
        const bool fired = pressed && !latch;
        latch = pressed;
        return fired;
      };
      if (latched("floor_down", floor_down_latch)) current_floor = std::max(current_floor - 1, 0);
      if (latched("floor_up", floor_up_latch)) current_floor = std::min(current_floor + 1, 3);
      if (latched("toggle_cutaway", cutaway_latch)) cutaway = !cutaway;
      if (latched("toggle_routes", routes_latch)) show_routes = !show_routes;
      if (latched("toggle_names", names_latch)) show_names = !show_names;
      if (latched("toggle_props", props_latch)) show_props = !show_props;
      if (latched("toggle_tactics", tactics_latch)) show_tactics = !show_tactics;
      if (latched("cycle_control", control_latch)) control_mode = (control_mode + 1) % 5;
      const bool door_key = latched("toggle_door", door_latch);

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
      // Луч пересекается с плоскостью ТЕКУЩЕГО ЭТАЖА, а не с землёй: на втором этаже курсор обязан
      // указывать на второй этаж, иначе выбирается комната под ногами у того, кто стоит наверху.
      const float floor_base = float(current_floor) * storey_pitch_m;
      pointer = glm::vec3{float(centre.x), floor_base, float(centre.y)};
      if (std::abs(ray.y) > 1.0e-5f) {
        const float t = (floor_base - eye.y) / ray.y;
        if (t > 0.0f) pointer = eye + ray * t;
      }
      pointer.y = floor_base + 0.05f;

      const auto hovered_part = store.pick(pointer, level);
      const auto* hovered_zone = hovered_part.valid() ? store.find(hovered_part.zone) : nullptr;
      if (click_pending) {
        selected = hovered_zone == nullptr ? invalid_key : hovered_zone->key;
        click_pending = false;
      }

      // Переключение двери — это переключение ПРОХОДИМОСТИ МЕСТА, а не свойства ребра. Одно действие
      // закрывает сразу все её проходы, и маршруты меняются со следующего же поиска.
      if (door_key && hovered_zone != nullptr && hovered_zone->kind == zone_kind::door) {
        store.set_closed(hovered_zone->key, !store.closed(hovered_zone->key));
        ++toggled_doors;
        for (auto& walker : walkers) {
          walker.path.clear();
          walker.arrived = true;
        }
      }

      // --- сборка геометрии кадра ---

      geometry.fill.clear();
      geometry.lines.clear();
      geometry.labels.clear();
      geometry.slots.clear();

      // Срез переднего плана. Партийная камера смотрит под углом, и здание перед группой закрывает её
      // целиком; убирать надо не «всё высокое», а ровно то, что стоит МЕЖДУ камерой и группой. Поэтому
      // правило геометрическое: клин от группы назад к камере, и стены внутри клина не поднимаются.
      const glm::vec2 flat_forward =
        glm::normalize(glm::vec2{forward.x, forward.z} + glm::vec2{1.0e-6f, 0.0f});
      // Точка отсчёта — цель камеры, а не первый попавшийся персонаж: камера в партийной РПГ и так
      // стоит на группе, и «между камерой и группой» это ровно «между камерой и целью».
      const glm::vec2 party{float(centre.x), float(centre.y)};
      const float cut_depth = float(span_m * 0.8);
      const float cut_near = float(span_m * 0.05);
      const float cut_far = float(span_m * 0.22);
      const auto in_cutaway = [&](const glm::vec2 point) {
        if (!cutaway) return false;
        const auto delta = point - party;
        const float along = glm::dot(delta, flat_forward);
        if (along >= 0.0f || along <= -cut_depth) return false;

        // Клин РАСШИРЯЕТСЯ к камере: чем ближе стена, тем больше экрана она закрывает, и постоянная
        // ширина срезала бы дальнее вместе с ближним или не срезала бы ничего.
        const float side = std::abs(delta.x * flat_forward.y - delta.y * flat_forward.x);
        const float reach = -along / cut_depth;
        return side < cut_near + (cut_far - cut_near) * reach;
      };

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

            // Этажи. Выше текущего не рисуется ничего — иначе крыша закрывает то, ради чего на этаж
            // переключились; ровно один этаж ниже остаётся приглушённым, чтобы было видно, где стоишь
            // относительно улицы, а глубже уже мешает.
            if (record.floor > current_floor) continue;
            const bool below = record.floor < current_floor;
            if (record.floor < current_floor - 1) continue;

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

              const auto centre_of_part = glm::vec2{(part.bounds.lower.x + part.bounds.upper.x) * 0.5f,
                                                    (part.bounds.lower.z + part.bounds.upper.z) * 0.5f};
              const bool sliced = in_cutaway(centre_of_part);

              uint32_t floor_tint = kind_tint(record.kind);
              if (control_mode == 1) {
                floor_tint = faction_tint(store.control_at(record.key, control_field::faction).faction);
              } else if (control_mode == 2) {
                floor_tint = crime_tint(store.control_at(record.key, control_field::crime).crime);
              } else if (control_mode == 3) {
                floor_tint = owner_tint(de_facto_holder(realm, record.key));
              } else if (control_mode == 4) {
                floor_tint = law_tint(titles, law_over(realm, record.key));
              }
              if (below) {
                // Нижний этаж уходит в полупрозрачность, а не в другой цвет: он должен читаться как
                // «то же самое, но не здесь», а перекраска читалась бы как другой вид места.
                floor_tint = (floor_tint & 0x00ffffffu) | (110u << 24);
              }
              emit_floor(geometry, outline, low, floor_tint, slot);

              // Стена теперь ЧИТАЕТСЯ ИЗ ДАННЫХ, а не выводится из отсутствия прохода. Прежнее правило
              // «ребро без прохода — стена» перестало работать в тот момент, когда стена стала зоной: у
              // непроходимого места проходы есть, они просто ведут в него, а не сквозь. Поэтому
              // непроходимая зона поднимается целиком, а у проходимой вертикаль остаётся только там, где
              // проёма действительно нет.
              if (show_walls && !below && !sliced && high - low > 0.5f) {
                const uint32_t tint = wall_tint(record.kind);
                for (size_t i = 0; i < outline.size(); ++i) {
                  const auto a = outline[i];
                  const auto b = outline[(i + 1) % outline.size()];
                  if (record.impassable()) {
                    emit_wall(geometry, a, b, low, high, tint, slot);
                  } else {
                    emit_open_wall(geometry, a, b, portals, low, high, tint, slot);
                  }
                }
              }

              // Предметы места. Рисуются только на своём этаже и только когда не срезаны: срез убирает
              // не только стену, но и шкаф, иначе он остаётся висеть посреди среза.
              if (show_props && !below && !sliced) {
                for (const auto& prop : sector->props_of(record)) {
                  if (prop.part != index) continue;
                  const uint32_t top = prop.blocks_sight() ? pack(126, 96, 70, 255) : pack(158, 132, 96, 255);
                  const uint32_t side = prop.climbable() ? pack(104, 84, 62, 255) : pack(82, 64, 48, 255);
                  emit_box(geometry, prop.position, prop.radius, low, low + prop.height, top, side, slot);
                }
              }

              if (!show_portals || below) continue;
              for (const auto& portal : portals) {
                if (!portal.geometric()) continue;
                if (portal.other < record.key || (portal.other == record.key && portal.other_part < index)) continue;

                // Красным помечается то, через что сейчас НЕ ПРОЙТИ, откуда бы это ни следовало: замок на
                // самом проходе или закрытая дверь по ту сторону. Игроку важно первое, а не то, где живёт
                // флаг, и разделять эти два случая цветом значило бы показывать устройство данных.
                const auto* neighbour = store.find(portal.other);
                const bool open = portal.passable() && (neighbour == nullptr || store.passable(*neighbour));
                const uint32_t tint = open ? pack(245, 245, 235, 220) : pack(230, 70, 70, 240);
                emit_line(geometry, portal.from, portal.to, low + 0.06f, tint);
              }
            }
          }
        }
      }

      // --- персонажи ---

      if (show_agents) {
        // Ходить можно только по ПРОХОДИМОМУ. Раньше сюда шёл весь список нарисованных зон, и персонаж
        // заводился внутри стены: снаружи это выглядело как имя, висящее над кладкой.
        walkable.clear();
        for (const auto key : geometry.slots) {
          const auto* zone = store.find(key);
          if (zone != nullptr && store.passable(*zone)) walkable.push_back(key);
        }

        while (walkers.size() < options.agent_count && !walkable.empty()) {
          const auto key = walkable[utils::splitmix(walkers.size() + 1ull, uint64_t(drawn_frames) + 1ull) %
                                    walkable.size()];
          agent walker{};
          walker.location = {key, 0};
          if (!interior_point(store, walker.location, walker.position)) break;
          walkers.push_back(walker);
        }

        for (auto& walker : walkers) {
          if (walker.arrived || walker.path.empty()) {
            if (walkable.empty()) continue;
            const auto goal = walkable[utils::splitmix(uint64_t(&walker - walkers.data()) + 1ull,
                                                       uint64_t(drawn_frames) + 7ull) %
                                       walkable.size()];
            walker.path = find_path(store, walker.location, {goal, 0});
            walker.cursor = 0;
            walker.arrived = walker.path.empty();
          }
          step_agent(store, walker, std::max(dt, 0.001f) * 3.2f);
        }

        const float marker = float(std::max(span_m * 0.003, 0.30));
        for (uint32_t index = 0; index < walkers.size(); ++index) {
          const auto& walker = walkers[index];
          const auto* part = store.part_of(walker.location);
          const auto* zone = store.find(walker.location.zone);
          if (part == nullptr || zone == nullptr) continue;
          if (zone->floor > current_floor || zone->floor < current_floor - 1) continue;

          const bool leader = index == 0;
          const bool downstairs = zone->floor < current_floor;
          const float height = part->bounds.lower.y + 0.1f;
          const uint32_t body = leader ? pack(255, 214, 96, 255) : pack(255, 90, 200, 255);
          emit_marker(geometry, walker.position, leader ? marker * 1.4f : marker, height,
                      downstairs ? ((body & 0x00ffffffu) | (110u << 24)) : body);

          // Маршрут рисуется ИЗ ТОГО ЖЕ пути, по которому персонаж шагает. Отдельная «визуальная»
          // ломаная разошлась бы с движением, и картинка врала бы ровно там, где на неё смотрят.
          if (show_routes && !downstairs) {
            const auto points = route_points(store, walker);
            const uint32_t tint = leader ? pack(255, 226, 130, 220) : pack(120, 200, 255, 150);
            for (size_t i = 1; i < points.size(); ++i) {
              emit_line(geometry, points[i - 1], points[i], height + 0.05f, tint);
            }
          }

          // Мировой интерфейс. Показывается не всегда: на общем плане города имена превращаются в кашу,
          // а порог по ширине обзора — это то же правило, по которому переключается уровень карты.
          if (show_names && !downstairs && span_m <= 900.0 && index < 12) {
            static constexpr std::string_view names[] = {"Aldric", "Bran",  "Cedric", "Doran",
                                                         "Edrin",  "Falk",  "Gorm",   "Hale",
                                                         "Ivar",   "Jory",  "Kessa",  "Lowen"};
            const auto name = names[index % (sizeof(names) / sizeof(names[0]))];
            const auto status = zone_kind_name(zone->kind);
            const float progress = walker.path.empty()
                                     ? 1.0f
                                     : float(walker.cursor) / float(std::max<size_t>(walker.path.size(), 1));

            const glm::vec3 anchor{walker.position.x, height + 1.8f, walker.position.y};
            const auto& font = overlay.font_metrics();
            const float name_px = 15.0f;
            const float status_px = 11.0f;
            const float name_width = float(font.text_width(name_px, name));
            const float status_width = float(font.text_width(status_px, status));
            const float half = std::max(name_width, status_width) * 0.5f + 7.0f;

            emit_label_quad(geometry, anchor, {-half, -56.0f}, {half, -10.0f}, pack(18, 20, 26, 200));
            emit_label_quad(geometry, anchor, {-half, -56.0f}, {half, -53.0f},
                            leader ? pack(255, 214, 96, 235) : pack(120, 200, 255, 200));
            emit_label_text(geometry, font, anchor, name, -name_width * 0.5f, -38.0f, name_px,
                            pack(240, 236, 226, 255));
            emit_label_quad(geometry, anchor, {-half + 5.0f, -33.0f}, {half - 5.0f, -28.0f},
                            pack(52, 56, 64, 220));
            emit_label_quad(geometry, anchor, {-half + 5.0f, -33.0f},
                            {-half + 5.0f + (2.0f * half - 10.0f) * progress, -28.0f}, pack(120, 210, 130, 235));
            emit_label_text(geometry, font, anchor, status, -status_width * 0.5f, -14.0f, status_px,
                            pack(178, 186, 198, 255));
          }
        }
      } else {
        walkers.clear();
      }

      // --- тактическая картина ---
      //
      // Ровно те же вызовы, которыми пользуется игра, и ровно на тех данных, по которым ходят. Отдельный
      // «отладочный» расчёт показал бы красивую картинку, ничего не говорящую о том, что решит ИИ.
      if (show_tactics && hovered_zone != nullptr && !hovered_zone->abstract()) {
        const auto place = hovered_zone->key;
        glm::vec2 threat = walkers.empty() ? glm::vec2{float(centre.x), float(centre.y)} : walkers.front().position;

        // Угроза должна быть ВНУТРИ места, иначе видимость не определена и ответ выродится в список
        // предметов. Если группа снаружи — берём проём, через который она войдёт.
        uint32_t dummy = 0;
        (void)dummy;
        if (!settle_into_place(store, place, threat)) {
          const auto edges = store.perimeter(place);
          for (const auto& portal : edges) {
            if (!portal.geometric()) continue;
            threat = portal.middle();
            if (settle_into_place(store, place, threat)) break;
          }
        }

        const float mark_height = float(current_floor) * storey_pitch_m + 0.2f;
        emit_marker(geometry, threat, 0.45f, mark_height, pack(255, 80, 60, 235));

        for (const auto& spot : cover_spots(store, place, threat, 8)) {
          // За предметом и за формой места — разные вещи, и цветом они разные: первое можно потерять
          // вместе с предметом, второе нет.
          emit_marker(geometry, spot.position, 0.40f, mark_height,
                      spot.sheltered() ? pack(90, 220, 120, 230) : pack(150, 200, 180, 200));
          emit_line(geometry, threat, spot.position, mark_height, pack(90, 220, 120, 110));
        }
        for (const auto& spot : watch_spots(store, place, 4)) {
          emit_marker(geometry, spot.position, 0.40f, mark_height, pack(110, 170, 255, 230));
          for (const auto& portal : store.perimeter(place)) {
            if (!portal.geometric()) continue;
            if (!visible(store, place, spot.position, portal.middle())) continue;
            emit_line(geometry, spot.position, portal.middle(), mark_height, pack(110, 170, 255, 130));
          }
        }

        std::vector<glm::vec2> group;
        for (uint32_t i = 0; i < walkers.size() && group.size() < 4; ++i) {
          group.push_back(walkers[i].position);
        }
        for (const auto& order : fan_out(store, place, order_kind::hold, threat, group)) {
          emit_line(geometry, group[order.agent], order.position, mark_height + 0.05f,
                    pack(255, 214, 96, 200));
        }
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
        detail.push_back(std::format("props: {} here", store.props_of(shown->key).size()));
        {
          // Право показывается ЧЕТЫРЬМЯ разными фактами, потому что это четыре разных вопроса: чьё по
          // праву, кто держит на деле, чей закон достаёт и пустят ли сюда. Свести их в один — значит
          // потерять ровно те различия, ради которых титулы и заведены.
          const auto* deed = titles.find(shown->title);
          const auto law = law_over(realm, shown->key);
          const auto* law_record = law == invalid_title ? nullptr : titles.find(law);
          const auto verdict = may_enter(realm, shown->key, party_actor);
          detail.push_back(std::format("de jure: {} '{}' of {}  de facto: {}",
                                       deed == nullptr ? "none" : title_rank_name(deed->rank),
                                       deed == nullptr ? "" : titles.name_of(shown->title),
                                       deed == nullptr ? 0u : deed->de_jure_holder,
                                       de_facto_holder(realm, shown->key)));
          detail.push_back(std::format("law here: {}  entry for {}: {}",
                                       law_record == nullptr
                                         ? std::string("nobody's — no law reaches here")
                                         : std::format("{} '{}'", title_rank_name(law_record->rank),
                                                       titles.name_of(law)),
                                       party_actor, verdict.allowed ? "allowed" : "trespass"));
        }
        {
          const auto* holder = store.carrier_of(shown->key, control_field::faction);
          const auto faction = store.control_at(shown->key, control_field::faction);
          const auto crime = store.control_at(shown->key, control_field::crime);
          const auto wealth = store.control_at(shown->key, control_field::prosperity);
          detail.push_back(std::format("held by faction {} (via {} '{}')  crime {}  prosperity {}",
                                       faction.faction,
                                       holder == nullptr ? "nothing" : zone_kind_name(holder->kind),
                                       holder == nullptr ? "" : store.name_of(*holder), crime.crime,
                                       wealth.prosperity));
        }
        detail.push_back(std::format("floor: {}  passable: {}{}", shown->floor,
                                     store.passable(*shown) ? "yes" : "no",
                                     shown->kind == zone_kind::door
                                       ? (store.closed(shown->key) ? "  (K opens this door)"
                                                                   : "  (K closes this door)")
                                       : ""));

        const auto* parent = store.find(shown->parent);
        detail.push_back(parent == nullptr ? std::string("part of: none (parent sector not resident)")
                                           : std::format("part of: {} '{}'", zone_kind_name(parent->kind),
                                                         store.name_of(*parent)));
      }
      detail.push_back(std::format("view {:.0f} m  pitch {:.0f} deg  map level {}  zones in frame {}", span_m,
                                   pitch_deg, zone_level_name(level), geometry.slots.size()));
      static constexpr const char* control_names[] = {"kind", "faction", "crime", "owner", "law"};
      detail.push_back(std::format("map colours by {}  ({} titles loaded)", control_names[control_mode % 5],
                                   titles.size()));
      detail.push_back(std::format("floor {}  cutaway {}  routes {}  names {}  props {}  tactics {}  "
                                   "doors toggled {}",
                                   current_floor, cutaway ? "on" : "off", show_routes ? "on" : "off",
                                   show_names ? "on" : "off", show_props ? "on" : "off",
                                   show_tactics ? "on" : "off", toggled_doors));
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
      // В `w` едет слот атласа шрифта: мировым надписям он нужен во фрагменте, а заводить ради одного
      // числа ещё один набор дескрипторов дороже, чем занять свободное поле уже существующего блока.
      camera.viewport_near =
        glm::vec4(float(pending_width), float(pending_height), 0.1f, float(font_texture));
      write_buffer(base, "camera_buffer", &camera, sizeof(camera));

      write_buffer(base, "fill_vertices", geometry.fill.data(), geometry.fill.size() * sizeof(stream_vertex));
      write_buffer(base, "line_vertices", geometry.lines.data(), geometry.lines.size() * sizeof(stream_vertex));
      write_buffer(base, "label_vertices", geometry.labels.data(), geometry.labels.size() * sizeof(label_vertex));

      const VkDrawIndirectCommand fill_command{uint32_t(geometry.fill.size()), 1, 0, 0};
      const VkDrawIndirectCommand line_command{uint32_t(geometry.lines.size()), 1, 0, 0};
      const VkDrawIndirectCommand label_command{uint32_t(geometry.labels.size()), 1, 0, 0};
      base.write_constant_data(base.find_constant("fill_draw"), fill_command);
      base.write_constant_data(base.find_constant("line_draw"), line_command);
      base.write_constant_data(base.find_constant("label_draw"), label_command);
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
