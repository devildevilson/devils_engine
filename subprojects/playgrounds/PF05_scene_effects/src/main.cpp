#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

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
#include "devils_engine/utf/utf.hpp"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/shared.h"
#include "devils_engine/visage/font.h"

using namespace devils_engine;

namespace {

constexpr uint32_t initial_width = 1280;
constexpr uint32_t initial_height = 720;
constexpr float near_plane = 0.1f;
constexpr float billboard_spherical = 0.0f;
constexpr float billboard_cylindrical_y = 1.0f;
constexpr float billboard_screen_size = 2.0f;

uint32_t pending_width = initial_width;
uint32_t pending_height = initial_height;
bool resize_pending = false;
int32_t escape_key = -1;
int32_t decal_key = -1;
bool decals_enabled = true;

void error_callback(const int error, const char* message) noexcept {
  utils::warn("PF05 input error {}: {}", error, message);
}

void key_callback(GLFWwindow* window, const int key, const int scancode, const int action, const int) noexcept {
  input::events::update_key(scancode, action);
  if (key == escape_key && action == 1) {
    input::set_should_close(window, true);
  }
  if (key == decal_key && action == 1) {
    decals_enabled = !decals_enabled;
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
    utils::error{}("PF05 could not resolve input key '{}'", canonical);
  }
  input::events::set_key(event, scancode, key);
}

struct scene_vertex {
  float px, py, pz;
  float nx, ny, nz;
};

struct glyph_vertex {
  float x, y;
};

struct decal_vertex {
  float x, y, z;
};

struct glyph_style {
  glm::vec4 fill;
  glm::vec4 outline;
  float boldness;
  float outline_width;
  float softness;
  uint32_t detail_texture = 0;
  float detail_mix = 0.0f;
};

struct glyph_instance {
  glm::mat4 transform;
  glm::vec4 uv_rect;
  glm::vec4 fill;
  glm::vec4 outline;
  glm::vec4 effect;
};
static_assert(sizeof(glyph_instance) == 128);

struct billboard_glyph_instance {
  glm::mat4 transform;
  glm::vec4 uv_rect;
  glm::vec4 fill;
  glm::vec4 outline;
  glm::vec4 effect;
  glm::vec4 anchor;
};
static_assert(sizeof(billboard_glyph_instance) == 144);

struct decal_instance {
  glm::mat4 decal_to_world;
  glm::mat4 world_to_decal;
  glm::vec4 uv_rect;
  glm::vec4 fill;
  glm::vec4 effect;
};
static_assert(sizeof(decal_instance) == 176);

struct alignas(16) camera_block {
  glm::mat4 view_projection;
  glm::mat4 view;
  glm::vec4 camera_position;
  glm::vec4 viewport_near;
  glm::mat4 inverse_view_projection;
  glm::vec4 effect_params;
};
static_assert(sizeof(camera_block) == 240);

void add_quad(
  std::vector<scene_vertex>& out,
  const glm::vec3& a,
  const glm::vec3& b,
  const glm::vec3& c,
  const glm::vec3& d,
  const glm::vec3& normal) {
  const auto push = [&out, normal](const glm::vec3& p) {
    out.push_back(scene_vertex{p.x, p.y, p.z, normal.x, normal.y, normal.z});
  };
  push(a); push(b); push(c);
  push(a); push(c); push(d);
}

void add_box(std::vector<scene_vertex>& out, const glm::vec3& center, const glm::vec3& half) {
  const glm::vec3 mn = center - half;
  const glm::vec3 mx = center + half;
  add_quad(out, {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}, {0, 0, 1});
  add_quad(out, {mx.x, mn.y, mn.z}, {mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}, {0, 0, -1});
  add_quad(out, {mx.x, mn.y, mx.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}, {1, 0, 0});
  add_quad(out, {mn.x, mn.y, mn.z}, {mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z}, {-1, 0, 0});
  add_quad(out, {mn.x, mx.y, mx.z}, {mx.x, mx.y, mx.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z}, {0, 1, 0});
  add_quad(out, {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z}, {0, -1, 0});
}

std::vector<scene_vertex> make_room() {
  std::vector<scene_vertex> out;
  add_quad(out, {-5, -1.5f, 3}, {5, -1.5f, 3}, {5, -1.5f, -5}, {-5, -1.5f, -5}, {0, 1, 0});
  add_quad(out, {-5, -1.5f, -5}, {5, -1.5f, -5}, {5, 3.5f, -5}, {-5, 3.5f, -5}, {0, 0, 1});
  add_quad(out, {-5, -1.5f, 3}, {-5, -1.5f, -5}, {-5, 3.5f, -5}, {-5, 3.5f, 3}, {1, 0, 0});
  add_quad(out, {5, -1.5f, -5}, {5, -1.5f, 3}, {5, 3.5f, 3}, {5, 3.5f, -5}, {-1, 0, 0});
  return out;
}

std::vector<scene_vertex> make_cube() {
  std::vector<scene_vertex> out;
  add_box(out, {}, {0.65f, 0.65f, 0.65f});
  return out;
}

std::vector<decal_vertex> make_decal_cube() {
  const auto scene_cube = make_cube();
  std::vector<decal_vertex> out;
  out.reserve(scene_cube.size());
  for (const auto& vertex : scene_cube) {
    // make_cube has a 0.65 half extent; decal local space is exactly [-0.5, +0.5].
    out.push_back(decal_vertex{vertex.px / 1.3f, vertex.py / 1.3f, vertex.pz / 1.3f});
  }
  return out;
}

struct quadratic_path {
  glm::vec3 p0;
  glm::vec3 p1;
  glm::vec3 p2;
  glm::vec3 plane_normal;
  std::array<float, 65> lengths{};
  float length = 0.0f;

  glm::vec3 point(const float t) const noexcept {
    return glm::vec3(utils::shared::quadratic_bezier(
      glm::vec4(p0, 0.0f), glm::vec4(p1, 0.0f), glm::vec4(p2, 0.0f), t));
  }

  glm::vec3 tangent(const float t) const noexcept {
    return glm::normalize(glm::vec3(utils::shared::quadratic_bezier_derivative(
      glm::vec4(p0, 0.0f), glm::vec4(p1, 0.0f), glm::vec4(p2, 0.0f), t)));
  }

  void build_lengths() noexcept {
    lengths[0] = 0.0f;
    glm::vec3 previous = p0;
    for (size_t i = 1; i < lengths.size(); ++i) {
      const glm::vec3 current = point(float(i) / float(lengths.size() - 1));
      lengths[i] = lengths[i - 1] + glm::length(current - previous);
      previous = current;
    }
    length = lengths.back();
  }

  float parameter_at(const float distance) const noexcept {
    const float target = std::clamp(distance, 0.0f, length);
    const auto upper = std::lower_bound(lengths.begin(), lengths.end(), target);
    if (upper == lengths.begin()) return 0.0f;
    if (upper == lengths.end()) return 1.0f;
    const size_t hi = size_t(upper - lengths.begin());
    const size_t lo = hi - 1;
    const float segment = std::max(lengths[hi] - lengths[lo], 1e-6f);
    const float fraction = (target - lengths[lo]) / segment;
    return (float(lo) + fraction) / float(lengths.size() - 1);
  }
};

glm::mat4 glyph_transform(
  const glm::vec3& origin,
  const glm::vec3& right,
  const glm::vec3& up,
  const float width,
  const float height) noexcept {
  return glm::mat4{
    glm::vec4(right * width, 0.0f),
    glm::vec4(up * height, 0.0f),
    glm::vec4(glm::normalize(glm::cross(right, up)), 0.0f),
    glm::vec4(origin, 1.0f)};
}

glyph_instance make_glyph_instance(
  const visage::font_t& font,
  const visage::font_t::glyph_t& glyph,
  const glm::mat4& transform,
  const glyph_style& style,
  const uint32_t atlas_slot) {
  if (atlas_slot > 255u || style.detail_texture > 255u) {
    utils::error{}("PF05 packed glyph texture slots must fit in 8 bits");
  }
  const uint32_t packed_textures =
    (atlas_slot & 0xffu) |
    ((style.detail_texture & 0xffu) << 8u) |
    (uint32_t(std::clamp(style.detail_mix, 0.0f, 1.0f) * 255.0f + 0.5f) << 16u);
  return glyph_instance{
    transform,
    glm::vec4(
      float(glyph.al / double(font.width)),
      float(glyph.ab / double(font.height)),
      float(glyph.ar / double(font.width)),
      float(glyph.at / double(font.height))),
    style.fill,
    style.outline,
    glm::vec4(style.boldness, style.outline_width, style.softness, std::bit_cast<float>(packed_textures))};
}

struct text_layout_result {
  std::vector<glyph_instance> glyphs;
  float resolved_height = 0.0f;
  float consumed_length = 0.0f;
  size_t source_characters = 0;
  size_t consumed_characters = 0;
};

text_layout_result layout_on_path(
  const visage::font_t& font,
  const std::string_view text,
  const quadratic_path& path,
  const float max_length,
  const float requested_height,
  const glyph_style& style,
  const uint32_t atlas_slot) {
  text_layout_result result;
  const std::u32string codepoints = utf::as_u32(text);
  result.source_characters = codepoints.size();
  const float unit_width = float(font.text_width(1.0, text));
  result.resolved_height = requested_height > 0.0f
                             ? requested_height
                             : max_length / std::max(unit_width, 1e-6f);
  float cursor = 0.0f;
  for (const char32_t character : codepoints) {
    const auto* glyph = font.find_glyph(uint32_t(character));
    if (glyph == nullptr) continue;
    const float advance = float(glyph->advance) * result.resolved_height;
    if (requested_height > 0.0f && cursor + advance > max_length) break;
    if (glyph->w > 0 && glyph->h > 0) {
      const float sample_distance = std::min(cursor + advance * 0.5f, path.length);
      const float t = path.parameter_at(sample_distance);
      const glm::vec3 right = path.tangent(t);
      const glm::vec3 up = glm::normalize(glm::cross(path.plane_normal, right));
      const glm::vec3 pen = path.point(path.parameter_at(cursor));
      const float left = float(glyph->pl) * result.resolved_height;
      const float bottom = float(glyph->pb) * result.resolved_height;
      const float width = float(glyph->pr - glyph->pl) * result.resolved_height;
      const float height = float(glyph->pt - glyph->pb) * result.resolved_height;
      const glm::vec3 origin = pen + right * left + up * bottom;
      result.glyphs.push_back(make_glyph_instance(
        font, *glyph, glyph_transform(origin, right, up, width, height), style, atlas_slot));
    }
    cursor += advance;
    result.consumed_characters += 1;
  }
  result.consumed_length = cursor;
  return result;
}

std::vector<std::string> wrap_words(
  const visage::font_t& font,
  const std::string_view text,
  const float height,
  const float max_width) {
  std::istringstream stream{std::string(text)};
  std::vector<std::string> lines;
  std::string line;
  std::string word;
  while (stream >> word) {
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && font.text_width(height, candidate) > max_width) {
      lines.push_back(line);
      line = word;
    } else {
      line = candidate;
    }
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

std::vector<billboard_glyph_instance> layout_billboard(
  const visage::font_t& font,
  const std::string_view text,
  const float height,
  const float max_width,
  const glm::vec4& anchor,
  const glyph_style& style,
  const uint32_t atlas_slot) {
  std::vector<billboard_glyph_instance> out;
  const auto lines = wrap_words(font, text, height, max_width);
  const float line_height = float(font.metrics.line_height) * height;
  float baseline = float(lines.size() - 1) * line_height * 0.5f;
  for (const auto& line : lines) {
    const float line_width = float(font.text_width(height, line));
    float cursor = -std::min(line_width, max_width) * 0.5f;
    for (const char32_t character : utf::as_u32(line)) {
      const auto* glyph = font.find_glyph(uint32_t(character));
      if (glyph == nullptr) continue;
      const float advance = float(glyph->advance) * height;
      if (cursor + advance > max_width * 0.5f) break;
      if (glyph->w > 0 && glyph->h > 0) {
        const float left = float(glyph->pl) * height;
        const float bottom = float(glyph->pb) * height;
        const float width = float(glyph->pr - glyph->pl) * height;
        const float glyph_height = float(glyph->pt - glyph->pb) * height;
        const glm::mat4 transform = glyph_transform(
          glm::vec3{cursor + left, baseline + bottom, 0.0f},
          glm::vec3{1, 0, 0}, glm::vec3{0, 1, 0}, width, glyph_height);
        const auto base = make_glyph_instance(font, *glyph, transform, style, atlas_slot);
        out.push_back(billboard_glyph_instance{
          base.transform, base.uv_rect, base.fill, base.outline, base.effect, anchor});
      }
      cursor += advance;
    }
    baseline -= line_height;
  }
  return out;
}

std::vector<decal_instance> layout_surface_decal(
  const visage::font_t& font,
  const std::string_view text,
  const glm::vec3& baseline_origin,
  const glm::vec3& right_axis,
  const glm::vec3& up_axis,
  const glm::vec3& projection_normal,
  const float font_height,
  const float max_length,
  const float volume_depth,
  const glyph_style& style,
  const uint32_t atlas_slot) {
  const glm::vec3 right = glm::normalize(right_axis);
  const glm::vec3 up = glm::normalize(up_axis);
  const glm::vec3 normal = glm::normalize(projection_normal);
  std::vector<decal_instance> out;
  float cursor = 0.0f;
  for (const char32_t character : utf::as_u32(text)) {
    const auto* glyph = font.find_glyph(uint32_t(character));
    if (glyph == nullptr) continue;
    const float advance = float(glyph->advance) * font_height;
    if (cursor + advance > max_length) break;
    if (glyph->w > 0 && glyph->h > 0) {
      const float left = float(glyph->pl) * font_height;
      const float bottom = float(glyph->pb) * font_height;
      const float width = float(glyph->pr - glyph->pl) * font_height;
      const float height = float(glyph->pt - glyph->pb) * font_height;
      const glm::vec3 lower_left = baseline_origin + right * (cursor + left) + up * bottom;
      const glm::vec3 center = lower_left + right * (width * 0.5f) + up * (height * 0.5f);
      const glm::mat4 decal_to_world{
        glm::vec4(right * width, 0.0f),
        glm::vec4(up * height, 0.0f),
        glm::vec4(normal * volume_depth, 0.0f),
        glm::vec4(center, 1.0f)};
      out.push_back(decal_instance{
        decal_to_world,
        glm::inverse(decal_to_world),
        glm::vec4(
          float(glyph->al / double(font.width)),
          float(glyph->ab / double(font.height)),
          float(glyph->ar / double(font.width)),
          float(glyph->at / double(font.height))),
        style.fill,
        glm::vec4(float(atlas_slot), style.boldness, style.softness, 0.55f)});
    }
    cursor += advance;
  }
  return out;
}

std::vector<uint8_t> make_weathered_stone_texture(const uint32_t width, const uint32_t height) {
  std::vector<uint8_t> pixels(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t state = x * 0x9e3779b9u ^ y * 0x85ebca6bu ^ 0x51f15e5du;
      state ^= state >> 16;
      state *= 0x7feb352du;
      state ^= state >> 15;
      const float noise = float(state & 255u) / 255.0f;
      const bool crack = ((x + y * 3u + (state >> 8)) % 29u) < 2u ||
                         ((x * 5u + height - y + (state >> 16)) % 47u) == 0u;
      const float stone = crack ? 0.16f : 0.48f + noise * 0.46f;
      const size_t i = (size_t(y) * width + x) * 4;
      pixels[i + 0] = uint8_t(std::clamp(stone * 1.05f, 0.0f, 1.0f) * 255.0f);
      pixels[i + 1] = uint8_t(std::clamp(stone * 0.92f, 0.0f, 1.0f) * 255.0f);
      pixels[i + 2] = uint8_t(std::clamp(stone * 0.72f, 0.0f, 1.0f) * 255.0f);
      pixels[i + 3] = 255u;
    }
  }
  return pixels;
}

void write_current_buffer(
  painter::graphics_base& base,
  const std::string_view name,
  const void* data,
  const size_t bytes) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF05 buffer '{}' is absent", name);
  }
  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}("PF05 cannot write '{}' (capacity {}, requested {})", name, frame.sub.size, bytes);
  }
  if (bytes != 0) {
    std::memcpy(static_cast<uint8_t*>(frame.mapped) + frame.sub.offset, data, bytes);
  }
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
    utils::error{}("PF05 cannot write UI commands (capacity {}, requested {})", frame.sub.size, bytes);
  }
  auto* destination = static_cast<uint8_t*>(frame.mapped) + frame.sub.offset;
  std::memcpy(destination, &count, sizeof(count));
  if (!commands.empty()) {
    std::memcpy(destination + sizeof(count), commands.data(), commands.size_bytes());
  }
}

void bind_texture_descriptor(
  painter::graphics_base& base,
  const painter::assets_base& assets,
  const std::string_view descriptor_name) {
  const uint32_t slot = base.find_descriptor(descriptor_name);
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF05 texture descriptor '{}' is absent", descriptor_name);
  }
  auto& descriptor = base.descriptors[slot];
  const vk::ImageView fallback(assets.default_texture_view());
  std::vector<vk::DescriptorImageInfo> images(descriptor.texture_count);
  for (uint32_t i = 0; i < descriptor.texture_count; ++i) {
    vk::ImageView view;
    if (i < assets.texture_slots.size()) view = assets.texture_slots[i].view;
    images[i] = vk::DescriptorImageInfo(
      vk::Sampler{}, view ? view : fallback, vk::ImageLayout::eShaderReadOnlyOptimal);
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

template <typename Instance>
void write_pair(
  painter::graphics_base& base,
  const uint32_t pair,
  const std::span<const Instance> instances,
  const uint32_t vertex_count) {
  for (uint32_t offset = 0; offset < base.frames_in_flight(); ++offset) {
    const auto instance_frame = base.get_current_instance_resource_frame(pair, offset);
    const auto indirect_frame = base.get_current_indirect_resource_frame(pair, offset);
    if (instances.size_bytes() > instance_frame.sub.size) {
      utils::error{}("PF05 pair {} exceeds its instance budget", pair);
    }
    std::memcpy(
      static_cast<uint8_t*>(instance_frame.mapped) + instance_frame.sub.offset,
      instances.data(), instances.size_bytes());
    const VkDrawIndirectCommand command{vertex_count, uint32_t(instances.size()), 0, 0};
    std::memcpy(
      static_cast<uint8_t*>(indirect_frame.mapped) + indirect_frame.sub.offset,
      &command, sizeof(command));
  }
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
    while ((normalized & 0x400u) == 0) { normalized <<= 1; shift += 1; }
    normalized &= 0x3ffu;
    return std::bit_cast<float>(sign | ((127 - 15 - shift + 1) << 23) | (normalized << 13));
  }
  if (exponent == 0x1fu) return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));
  return std::bit_cast<float>(sign | ((exponent + 127 - 15) << 23) | (mantissa << 13));
}

void dump_scene_image(painter::graphics_base& base, const std::string& path) {
  const uint32_t slot = base.find_resource("scene_color");
  const auto frame = base.get_current_image_resource_frame(slot);
  const auto [width, height] = base.swapchain_extent();
  const size_t bytes = size_t(width) * size_t(height) * sizeof(uint16_t) * 4;
  vma::Allocator allocator(base.allocator);
  vk::BufferCreateInfo buffer_info{};
  buffer_info.usage = vk::BufferUsageFlagBits::eTransferDst;
  buffer_info.size = bytes;
  vma::AllocationCreateInfo allocation_info{};
  allocation_info.usage = vma::MemoryUsage::eGpuToCpu;
  allocation_info.flags = vma::AllocationCreateFlagBits::eMapped;
  auto [staging, allocation] = allocator.createBuffer(buffer_info, allocation_info);
  vk::Device dev(base.device);
  vk::CommandBufferAllocateInfo command_info{};
  command_info.commandPool = base.command_pool;
  command_info.level = vk::CommandBufferLevel::ePrimary;
  command_info.commandBufferCount = 1;
  const auto buffers = dev.allocateCommandBuffers(command_info);
  vk::CommandBuffer task(buffers[0]);
  task.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
  vk::BufferImageCopy region{};
  region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.imageSubresource.layerCount = 1;
  region.imageSubresource.baseArrayLayer = frame.sub.base_array_layer;
  region.imageExtent = vk::Extent3D{width, height, 1};
  task.copyImageToBuffer(frame.handle, vk::ImageLayout::eTransferSrcOptimal, staging, region);
  task.end();
  const vk::Fence fence = dev.createFence({});
  vk::SubmitInfo submit{};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &task;
  {
    const auto lock = base.graphics.lock();
    if (vk::Queue(base.graphics.handle()).submit(1, &submit, fence) != vk::Result::eSuccess) {
      utils::error{}("PF05 could not submit image dump");
    }
  }
  if (dev.waitForFences(fence, true, size_t(1000) * 1000 * 1000) != vk::Result::eSuccess) {
    utils::error{}("PF05 image dump timed out");
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
  if (!file_io::write(std::span<const char>(ppm.data(), ppm.size()), path, file_io::type::binary)) {
    utils::error{}("PF05 could not write dump '{}'", path);
  }
  dev.destroy(fence);
  dev.freeCommandBuffers(base.command_pool, task);
  allocator.destroyBuffer(staging, allocation);
  utils::info("PF05 dumped {}x{} scene frame to '{}'", width, height, path);
}

} // namespace

int main(int argc, char** argv) {
  bool validation = false;
  bool uncapped = false;
  bool fixed_camera = false;
  bool start_without_decals = false;
  uint32_t frame_limit = 0;
  std::string dump_path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option(argv[i]);
    validation = validation || option == "--validation";
    uncapped = uncapped || option == "--uncapped";
    fixed_camera = fixed_camera || option == "--fixed-camera";
    start_without_decals = start_without_decals || option == "--no-decals";
    constexpr std::string_view frames_prefix = "--frames=";
    constexpr std::string_view dump_prefix = "--dump=";
    if (option.starts_with(frames_prefix)) {
      frame_limit = uint32_t(std::stoul(std::string(option.substr(frames_prefix.size()))));
    }
    if (option.starts_with(dump_prefix)) {
      dump_path = std::string(option.substr(dump_prefix.size()));
    }
  }
  if (!dump_path.empty() && frame_limit == 0) frame_limit = 1;
  decals_enabled = !start_without_decals;

  input::init input_runtime(&error_callback);
  input::events::init();
  painter::load_dispatcher1();
  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "PF05 Scene Effects";
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
      utils::error{}("PF05 requested unavailable validation layers");
    }
    instance_info.enabledLayerCount = uint32_t(painter::default_validation_layers.size());
    instance_info.ppEnabledLayerNames = painter::default_validation_layers.data();
  }
  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);
  const VkDebugUtilsMessengerEXT messenger = validation
    ? painter::create_debug_messenger(instance) : VK_NULL_HANDLE;
  GLFWwindow* window = input::create_window(initial_width, initial_height, "PF05 — world MSDF text");
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  const auto surface_result = input::create_window_surface(instance, window, nullptr, &surface);
  if (surface_result != uint32_t(vk::Result::eSuccess)) {
    utils::error{}("PF05 could not create Vulkan surface");
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
  const VkDevice device = device_maker.create({}, "pf05.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();
    const std::string resource_root = std::string(PF05_RESOURCE_ROOT) + "/";
    const std::string cache_path = utils::cache_folder() + "pf05_scene_effects.pipeline_cache";
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("pf05_scene_effects");
    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0) {
      utils::error{}("PF05 could not commit render graph");
    }
    base.set_surface(surface, initial_width, initial_height);
    base.resize_viewport(initial_width, initial_height);
    base.populate_constant_default_values();
    const uint32_t graph = base.find_render_graph("pf05_scene_effects");
    base.change_render_graph(graph);

    painter::assets_base assets(device, physical.handle);
    assets.create_fence();
    assets.create_allocator(instance);
    assets.create_command_buffer(queues.transfer, queues.graphics);
    assets.set_graphics_base(&base);
    assets.create_default_texture();

    const auto upload_scene_mesh = [&assets](const std::string& id, const std::vector<scene_vertex>& data) {
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
    const auto decal_cube = make_decal_cube();
    const auto room_mesh = upload_scene_mesh("pf05.room", room);
    const auto cube_mesh = upload_scene_mesh("pf05.cube", cube);
    const auto decal_mesh = assets.register_buffer_storage("pf05.decal_cube");
    assets.create_buffer_storage(
      decal_mesh, painter::buffer_create_info{"decal_volume_geometry", uint32_t(decal_cube.size()), 0});
    assets.populate_buffer_storage(
      decal_mesh,
      std::span(reinterpret_cast<const uint8_t*>(decal_cube.data()), decal_cube.size() * sizeof(decal_cube[0])),
      std::span<const uint8_t>{});
    assets.mark_ready_buffer_slot(decal_mesh);
    const std::array<glyph_vertex, 6> glyph_quad{
      glyph_vertex{0, 0}, glyph_vertex{1, 0}, glyph_vertex{1, 1},
      glyph_vertex{0, 0}, glyph_vertex{1, 1}, glyph_vertex{0, 1}};
    const auto glyph_mesh = assets.register_buffer_storage("pf05.glyph_quad");
    assets.create_buffer_storage(
      glyph_mesh, painter::buffer_create_info{"glyph_geometry", uint32_t(glyph_quad.size()), 0});
    assets.populate_buffer_storage(
      glyph_mesh,
      std::span(reinterpret_cast<const uint8_t*>(glyph_quad.data()), sizeof(glyph_quad)),
      std::span<const uint8_t>{});
    assets.mark_ready_buffer_slot(glyph_mesh);

    const std::string common_resources = std::string(PLAYGROUND_COMMON_RESOURCE_ROOT) + "/";
    playground::visage_overlay overlay(
      common_resources + "fonts/crimson.roman.ttf",
      common_resources + "ui/lab_overlay.lua",
      playground::overlay_description{
        "PF05 — Scene effects / MSDF",
        "Crimson atlas: world paths, three billboard modes and screen-space decals",
        "WASD/QE + mouse · F toggles decals · fixed-size text clips · Esc"});
    const auto atlas = overlay.font_atlas();
    const auto font_texture = assets.register_texture_storage("playground.crimson_roman");
    assets.create_texture_storage(
      font_texture,
      painter::texture_create_info{{atlas.width, atlas.height, 1}, VK_FORMAT_R8G8B8A8_UNORM});
    assets.populate_texture_storage(font_texture, atlas.bytes);
    assets.mark_ready_texture_slot(font_texture);
    overlay.set_font_texture(font_texture);
    constexpr uint32_t detail_width = 64;
    constexpr uint32_t detail_height = 64;
    const auto weathered_pixels = make_weathered_stone_texture(detail_width, detail_height);
    const auto weathered_texture = assets.register_texture_storage("pf05.weathered_stone");
    assets.create_texture_storage(
      weathered_texture,
      painter::texture_create_info{{detail_width, detail_height, 1}, VK_FORMAT_R8G8B8A8_UNORM});
    assets.populate_texture_storage(weathered_texture, weathered_pixels);
    assets.mark_ready_texture_slot(weathered_texture);
    bind_texture_descriptor(base, assets, "text_textures");

    const auto& font = overlay.font_metrics();
    quadratic_path fixed_line{
      glm::vec3{-2.7f, 0.15f, -0.7f}, glm::vec3{-0.3f, 0.15f, -0.7f}, glm::vec3{2.1f, 0.15f, -0.7f},
      glm::vec3{0, 0, 1}};
    fixed_line.build_lengths();
    quadratic_path curve{
      glm::vec3{-3.1f, 1.25f, -2.0f}, glm::vec3{0.0f, 2.7f, -2.0f}, glm::vec3{3.1f, 1.25f, -2.0f},
      glm::vec3{0, 0, 1}};
    curve.build_lengths();
    const auto fixed_text = layout_on_path(
      font, "FIXED SIZE CLIPS THIS LONG STRING", fixed_line, 4.8f, 0.52f,
      glyph_style{glm::vec4{0.95f, 0.94f, 0.86f, 1}, glm::vec4{0.04f, 0.05f, 0.08f, 1}, 0, 0.10f, 0},
      font_texture);
    const auto curve_text = layout_on_path(
      font, "FIT TO BEZIER LENGTH", curve, curve.length, 0.0f,
      glyph_style{
        glm::vec4{1.0f, 0.58f, 0.12f, 1}, glm::vec4{0.25f, 0.03f, 0.01f, 1},
        0.015f, 0.12f, 0.08f, weathered_texture, 0.78f},
      font_texture);
    const auto back_wall_decal = layout_surface_decal(
      font, "TRUE SCREEN SPACE DECAL",
      glm::vec3{-3.45f, -0.30f, -4.96f},
      glm::vec3{1, 0, 0}, glm::vec3{0, 1, 0}, glm::vec3{0, 0, 1},
      0.43f, 6.9f, 0.30f,
      glyph_style{glm::vec4{0.18f, 0.95f, 0.83f, 0.92f}, {}, 0.015f, 0, 0.03f},
      font_texture);
    const auto right_wall_decal = layout_surface_decal(
      font, "ORIENTED VOLUME",
      glm::vec3{4.96f, 0.30f, -3.80f},
      glm::vec3{0, 0, 1}, glm::vec3{0, 1, 0}, glm::vec3{-1, 0, 0},
      0.36f, 3.4f, 0.30f,
      glyph_style{glm::vec4{1.0f, 0.42f, 0.64f, 0.90f}, {}, 0.01f, 0, 0.02f},
      font_texture);
    if (std::abs(curve_text.consumed_length - curve.length) > 0.01f) {
      utils::error{}(
        "PF05 fit-to-length invariant failed: consumed {}, requested {}",
        curve_text.consumed_length, curve.length);
    }
    if (fixed_text.consumed_length > 4.8f + 1e-4f ||
        fixed_text.consumed_characters >= fixed_text.source_characters) {
      utils::error{}(
        "PF05 fixed-size clipping invariant failed: consumed {} m and {}/{} characters",
        fixed_text.consumed_length,
        fixed_text.consumed_characters,
        fixed_text.source_characters);
    }
    const auto spherical_text = layout_billboard(
      font, "SPHERICAL BILLBOARD WRAPS ABOVE THIS OBJECT", 0.30f, 2.15f,
      glm::vec4{2.25f, 0.45f, -2.9f, billboard_spherical},
      glyph_style{glm::vec4{0.76f, 0.94f, 1.0f, 1}, glm::vec4{0.01f, 0.04f, 0.12f, 1}, 0, 0.11f, 0.04f},
      font_texture);
    const auto cylindrical_text = layout_billboard(
      font, "Y AXIS LOCKED", 0.38f, 2.4f,
      glm::vec4{4.10f, 0.80f, -2.60f, billboard_cylindrical_y},
      glyph_style{
        glm::vec4{0.95f, 0.76f, 0.36f, 1}, glm::vec4{0.10f, 0.035f, 0.01f, 1},
        0, 0.10f, 0.02f, weathered_texture, 0.88f},
      font_texture);
    const auto screen_text = layout_billboard(
      font, "WORLD ANCHOR / 38 PX", 38.0f, 330.0f,
      glm::vec4{0.0f, -0.60f, 0.0f, billboard_screen_size},
      glyph_style{glm::vec4{0.92f, 0.46f, 0.72f, 1}, glm::vec4{0.10f, 0.01f, 0.06f, 1}, 0.01f, 0.10f, 0.02f},
      font_texture);
    std::vector<billboard_glyph_instance> billboard_glyphs;
    billboard_glyphs.reserve(spherical_text.size() + cylindrical_text.size() + screen_text.size());
    billboard_glyphs.insert(billboard_glyphs.end(), spherical_text.begin(), spherical_text.end());
    billboard_glyphs.insert(billboard_glyphs.end(), cylindrical_text.begin(), cylindrical_text.end());
    billboard_glyphs.insert(billboard_glyphs.end(), screen_text.begin(), screen_text.end());
    std::vector<glyph_instance> world_glyphs;
    world_glyphs.reserve(fixed_text.glyphs.size() + curve_text.glyphs.size());
    world_glyphs.insert(world_glyphs.end(), fixed_text.glyphs.begin(), fixed_text.glyphs.end());
    world_glyphs.insert(world_glyphs.end(), curve_text.glyphs.begin(), curve_text.glyphs.end());
    std::vector<decal_instance> decal_glyphs;
    decal_glyphs.reserve(back_wall_decal.size() + right_wall_decal.size());
    decal_glyphs.insert(decal_glyphs.end(), back_wall_decal.begin(), back_wall_decal.end());
    decal_glyphs.insert(decal_glyphs.end(), right_wall_decal.begin(), right_wall_decal.end());

    const uint32_t scene_group = base.find_draw_group("scene_draw_group");
    const uint32_t decal_group = base.find_draw_group("decal_draw_group");
    const uint32_t world_group = base.find_draw_group("world_glyph_draw_group");
    const uint32_t billboard_group = base.find_draw_group("billboard_glyph_draw_group");
    const uint32_t room_pair = base.register_pair(scene_group, room_mesh, 1);
    const uint32_t props_pair = base.register_pair(scene_group, cube_mesh, 2);
    const uint32_t decal_pair = base.register_pair(decal_group, decal_mesh, uint32_t(decal_glyphs.size()));
    const uint32_t world_pair = base.register_pair(world_group, glyph_mesh, uint32_t(world_glyphs.size()));
    const uint32_t billboard_pair = base.register_pair(
      billboard_group, glyph_mesh, uint32_t(billboard_glyphs.size()));
    const std::array<glm::vec4, 1> room_instances{glm::vec4{0, 0, 0, 0}};
    const std::array<glm::vec4, 2> prop_instances{
      glm::vec4{-1.6f, -0.85f, -2.7f, 1}, glm::vec4{2.25f, -0.85f, -2.9f, 2}};
    write_pair(base, room_pair, std::span<const glm::vec4>(room_instances), uint32_t(room.size()));
    write_pair(base, props_pair, std::span<const glm::vec4>(prop_instances), uint32_t(cube.size()));
    write_pair(base, decal_pair, std::span<const decal_instance>(decal_glyphs), uint32_t(decal_cube.size()));
    write_pair(base, world_pair, std::span<const glyph_instance>(world_glyphs), uint32_t(glyph_quad.size()));
    write_pair(base, billboard_pair, std::span<const billboard_glyph_instance>(billboard_glyphs), uint32_t(glyph_quad.size()));

    input::events::clear_bindings();
    bind_key("camera_forward", "key_w");
    bind_key("camera_back", "key_s");
    bind_key("camera_left", "key_a");
    bind_key("camera_right", "key_d");
    bind_key("camera_down", "key_q");
    bind_key("camera_up", "key_e");
    bind_key("camera_fast", "left_shift");
    escape_key = input::glfw_key_from_canonical("escape");
    decal_key = input::glfw_key_from_canonical("key_f");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    if (fixed_camera) {
      input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_NORMAL);
    } else {
      input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_DISABLED);
      input::set_raw_mouse_motion(window);
    }

    playground::free_camera camera;
    camera.position = {0.0f, 0.25f, 5.3f};
    auto [mouse_x, mouse_y] = input::cursor_pos(window);
    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;
    playground::frame_pacer frame_pacer(uncapped ? 0u : 60u);
    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;
    uint32_t frames_total = 0;
    while (!input::should_close(window)) {
      input::poll_events();
      const auto now = std::chrono::steady_clock::now();
      const float dt = std::clamp(std::chrono::duration<float>(now - previous_time).count(), 0.0f, 0.1f);
      previous_time = now;
      input::events::update(size_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<float>(dt)).count()));
      if (resize_pending) {
        vk::Device(device).waitIdle();
        base.resize_viewport(pending_width, pending_height);
        resize_pending = false;
      }
      const auto [next_mouse_x, next_mouse_y] = input::cursor_pos(window);
      playground::camera_motion motion;
      motion.forward = float(input::events::is_pressed("camera_forward")) - float(input::events::is_pressed("camera_back"));
      motion.right = float(input::events::is_pressed("camera_right")) - float(input::events::is_pressed("camera_left"));
      motion.up = float(input::events::is_pressed("camera_up")) - float(input::events::is_pressed("camera_down"));
      motion.fast = input::events::is_pressed("camera_fast");
      if (!fixed_camera) {
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
      const float aspect = float(std::max(pending_width, 1u)) / float(std::max(pending_height, 1u));
      const glm::mat4 view = camera.view();
      const glm::mat4 projection = playground::infinite_reverse_z_projection(
        glm::radians(65.0f), aspect, near_plane);
      const camera_block camera_data{
        projection * view,
        view,
        glm::vec4(camera.position, 1),
        glm::vec4(float(pending_width), float(pending_height), near_plane, 0),
        glm::inverse(projection * view),
        glm::vec4(decals_enabled ? 1.0f : 0.0f, 0, 0, 0)};
      write_current_buffer(base, "camera_buffer", &camera_data, sizeof(camera_data));
      const uint64_t frame_delta_us = uint64_t(std::max(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<float>(dt)).count(),
        int64_t{1}));
      const uint64_t timestamp_us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count());
      const std::array<std::string, 10> details{
        std::format("Atlas: {}x{}, {} glyph metrics", atlas.width, atlas.height, font.glyphs.size()),
        std::format("Fixed: height {:.2f}, limit 4.80, consumed {}/{} characters",
          fixed_text.resolved_height, fixed_text.consumed_characters, fixed_text.source_characters),
        std::format("Fit: length {:.2f} -> derived height {:.3f}", curve.length, curve_text.resolved_height),
        std::format("Bezier: {} independent glyph world matrices", curve_text.glyphs.size()),
        std::format("Spherical: {} glyphs, camera right + up", spherical_text.size()),
        std::format("Cylindrical: {} glyphs, world Y locked", cylindrical_text.size()),
        std::format("Screen-size: {} glyphs, constant 38 px at world anchor", screen_text.size()),
        std::format("Screen decals: {} glyph volumes, depth reconstruction [{}]",
          decal_glyphs.size(), decals_enabled ? "ON" : "OFF"),
        std::format("Custom fill: packed detail slot {}, per-text mix", weathered_texture),
        "World/billboard glyph coverage writes depth; transparent quad pixels are discarded"};
      overlay.set_detail_lines(details);
      overlay.update(frame_delta_us, timestamp_us);
      write_overlay_buffers(base, overlay);
      context.prepare();
      context.draw();
      base.submit_frame();
      frame_pacer.wait();
      frames_total += 1;
      if (frame_limit != 0 && frames_total >= frame_limit) {
        if (!dump_path.empty()) {
          vk::Device(device).waitIdle();
          dump_scene_image(base, dump_path);
        }
        utils::info("PF05 reached requested {} frames", frame_limit);
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
