#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec4.hpp>

#include "devils_engine/input/core.h"
#include "devils_engine/input/events.h"
#include "devils_engine/painter/assets_base.h"
#include "devils_engine/painter/auxiliary.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/gpu_timing.h"
#include "devils_engine/painter/makers.h"
#include "devils_engine/painter/region_draw.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/playground/frame_pacer.h"
#include "devils_engine/playground/free_camera.h"
#include "devils_engine/playground/visage_overlay.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"

using namespace devils_engine;

namespace {

constexpr uint32_t initial_width = 1280;
constexpr uint32_t initial_height = 720;
constexpr uint32_t shadow_resolution = 2048;
constexpr uint32_t caster_count = 5;
constexpr uint32_t spot_count = 4;
constexpr uint32_t cascade_count = 4;
constexpr uint32_t packed_caster_capacity = caster_count * (spot_count + 1);
constexpr float camera_near = 0.1f;
constexpr float cascade_far = 40.0f;
constexpr float cascade_split_lambda = 0.68f;
constexpr float cascade_blend_fraction = 0.12f;
constexpr float default_raster_bias_constant = -1.25f;
constexpr float default_raster_bias_slope = -1.75f;
constexpr float default_normal_bias_base = 0.20f;
constexpr float default_normal_bias_slope = 1.10f;
constexpr float default_receiver_plane_scale = 1.0f;

uint32_t pending_width = initial_width;
uint32_t pending_height = initial_height;
bool resize_pending = false;
int32_t escape_key = -1;

void error_callback(const int error, const char* message) noexcept {
  utils::warn("PF02 input error {}: {}", error, message);
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
    utils::error{}("PF02 could not resolve input key '{}'", canonical);
  }
  input::events::set_key(event, scancode, key);
}

struct vertex {
  float px, py, pz;
  float nx, ny, nz;
  float u, v;
};

void add_quad(
  std::vector<vertex>& vertices,
  const glm::vec3& a,
  const glm::vec3& b,
  const glm::vec3& c,
  const glm::vec3& d,
  const glm::vec3& normal) {
  const auto push = [&vertices, normal](const glm::vec3& p, const float u, const float v) {
    vertices.push_back(vertex{p.x, p.y, p.z, normal.x, normal.y, normal.z, u, v});
  };
  push(a, 0.0f, 0.0f);
  push(b, 1.0f, 0.0f);
  push(c, 1.0f, 1.0f);
  push(a, 0.0f, 0.0f);
  push(c, 1.0f, 1.0f);
  push(d, 0.0f, 1.0f);
}

void add_box(std::vector<vertex>& out, const glm::vec3& low, const glm::vec3& high) {
  add_quad(out, {low.x, low.y, high.z}, {high.x, low.y, high.z}, {high.x, high.y, high.z}, {low.x, high.y, high.z}, {0, 0, 1});
  add_quad(out, {high.x, low.y, low.z}, {low.x, low.y, low.z}, {low.x, high.y, low.z}, {high.x, high.y, low.z}, {0, 0, -1});
  add_quad(out, {low.x, low.y, low.z}, {low.x, low.y, high.z}, {low.x, high.y, high.z}, {low.x, high.y, low.z}, {-1, 0, 0});
  add_quad(out, {high.x, low.y, high.z}, {high.x, low.y, low.z}, {high.x, high.y, low.z}, {high.x, high.y, high.z}, {1, 0, 0});
  add_quad(out, {low.x, low.y, low.z}, {high.x, low.y, low.z}, {high.x, low.y, high.z}, {low.x, low.y, high.z}, {0, -1, 0});
  add_quad(out, {low.x, high.y, high.z}, {high.x, high.y, high.z}, {high.x, high.y, low.z}, {low.x, high.y, low.z}, {0, 1, 0});
}

std::vector<vertex> make_stage() {
  std::vector<vertex> out;
  out.reserve(60);
  add_quad(out, {-7, -2, 6}, {7, -2, 6}, {7, -2, -7}, {-7, -2, -7}, {0, 1, 0});
  add_quad(out, {-7, -2, -7}, {7, -2, -7}, {7, 5, -7}, {-7, 5, -7}, {0, 0, 1});
  add_quad(out, {-7, -2, -7}, {-7, -2, 6}, {-7, 5, 6}, {-7, 5, -7}, {1, 0, 0});

  // Bias fixtures: an oblique receiver makes slope acne visible, while the very thin upright box
  // exposes contact loss/peter-panning as raster or receiver bias grows.
  const glm::vec3 ramp_a{-5.8f, -1.96f, -1.4f};
  const glm::vec3 ramp_b{-2.7f, -1.96f, -1.4f};
  const glm::vec3 ramp_c{-2.7f, -0.05f, -4.7f};
  const glm::vec3 ramp_d{-5.8f, -0.05f, -4.7f};
  const glm::vec3 ramp_normal = glm::normalize(glm::cross(ramp_b - ramp_a, ramp_c - ramp_a));
  add_quad(out, ramp_a, ramp_b, ramp_c, ramp_d, ramp_normal);
  add_box(out, {-4.55f, -2.0f, 0.25f}, {-4.39f, 0.25f, 1.15f});
  return out;
}

std::vector<vertex> make_cube() {
  constexpr float h = 0.5f;
  std::vector<vertex> out;
  out.reserve(36);
  add_quad(out, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1});
  add_quad(out, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1});
  add_quad(out, {-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-1, 0, 0});
  add_quad(out, {h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}, {1, 0, 0});
  add_quad(out, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}, {0, -1, 0});
  add_quad(out, {-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}, {0, 1, 0});
  return out;
}

glm::mat4 reverse_z_ortho(
  const float left,
  const float right,
  const float bottom,
  const float top,
  const float near_plane,
  const float far_plane) noexcept {
  glm::mat4 projection{1.0f};
  projection[0][0] = 2.0f / (right - left);
  projection[1][1] = -2.0f / (top - bottom);
  projection[2][2] = 1.0f / (far_plane - near_plane);
  projection[3][0] = -(right + left) / (right - left);
  projection[3][1] = (top + bottom) / (top - bottom);
  projection[3][2] = far_plane / (far_plane - near_plane);
  return projection;
}

glm::mat4 reverse_z_perspective(
  const float vertical_fov,
  const float aspect,
  const float near_plane,
  const float far_plane) noexcept {
  const float f = 1.0f / std::tan(vertical_fov * 0.5f);
  glm::mat4 projection{0.0f};
  projection[0][0] = f / aspect;
  projection[1][1] = -f;
  projection[2][2] = near_plane / (far_plane - near_plane);
  projection[2][3] = -1.0f;
  projection[3][2] = near_plane * far_plane / (far_plane - near_plane);
  return projection;
}

struct alignas(16) scene_block {
  glm::mat4 view_projection;
  glm::mat4 view;
  glm::mat4 light_view_projection;
  glm::vec4 camera_position;
  glm::vec4 viewport_near;
  glm::vec4 light_direction;
  // x: atlas resolution, y/z: world-texel normal bias, w: vertex-layout guard (must stay zero).
  glm::vec4 shadow_params;
  glm::vec4 filter_params;
};
static_assert(sizeof(scene_block) == 272);

struct alignas(16) directional_cascade_record {
  glm::mat4 light_view_projection;
  // x/y: camera-space interval, z: blend-band start, w: debug tint enable.
  glm::vec4 split_depths;
  // x: world-space width of one texel, y: map visibility enable,
  // z: receiver-plane scale; w is reserved.
  glm::vec4 shadow_params;
};
static_assert(sizeof(directional_cascade_record) == 6 * sizeof(glm::vec4));

using directional_cascade_block = std::array<directional_cascade_record, cascade_count>;

directional_cascade_block make_directional_cascades(
  const playground::free_camera& camera,
  const float aspect,
  const float vertical_fov,
  const glm::vec3 light_direction,
  const bool debug_tint) {
  directional_cascade_block out{};
  std::array<float, cascade_count> split_fars{};
  for (uint32_t index = 0; index < cascade_count; ++index) {
    const float fraction = float(index + 1) / float(cascade_count);
    const float logarithmic = camera_near * std::pow(cascade_far / camera_near, fraction);
    const float uniform = camera_near + (cascade_far - camera_near) * fraction;
    split_fars[index] =
      logarithmic * cascade_split_lambda + uniform * (1.0f - cascade_split_lambda);
  }

  const glm::vec3 camera_forward = camera.forward();
  const glm::vec3 camera_right = camera.right();
  const glm::vec3 camera_up = glm::normalize(glm::cross(camera_right, camera_forward));
  const glm::mat4 light_view = glm::mat4(glm::mat3(glm::lookAtRH(
    -light_direction,
    glm::vec3{0.0f},
    glm::vec3{0.0f, 1.0f, 0.0f})));
  const float tangent = std::tan(vertical_fov * 0.5f);
  float split_near = camera_near;

  for (uint32_t index = 0; index < cascade_count; ++index) {
    const float split_far = split_fars[index];
    std::array<glm::vec3, 8> corners{};
    uint32_t corner_index = 0;
    for (const float distance : {split_near, split_far}) {
      const glm::vec3 center = camera.position + camera_forward * distance;
      const float half_height = tangent * distance;
      const float half_width = half_height * aspect;
      for (const float vertical : {-1.0f, 1.0f}) {
        for (const float horizontal : {-1.0f, 1.0f}) {
          corners[corner_index++] = center +
            camera_right * (horizontal * half_width) + camera_up * (vertical * half_height);
        }
      }
    }

    glm::vec3 center{0.0f};
    for (const auto corner : corners) center += corner;
    center /= float(corners.size());
    float radius = 0.0f;
    for (const auto corner : corners) radius = std::max(radius, glm::length(corner - center));
    // A rotation-independent sphere plus a quantized extent prevents the projection from breathing
    // while the camera rotates inside one split interval.
    radius = std::ceil(radius * 16.0f) / 16.0f;

    const glm::vec3 center_light = glm::vec3(light_view * glm::vec4(center, 1.0f));
    const float texel_world = (radius * 2.0f) / float(shadow_resolution / 2);
    const float snapped_x = std::round(center_light.x / texel_world) * texel_world;
    const float snapped_y = std::round(center_light.y / texel_world) * texel_world;

    float minimum_z = 1.0e30f;
    float maximum_z = -1.0e30f;
    for (const auto corner : corners) {
      const float light_z = (light_view * glm::vec4(corner, 1.0f)).z;
      minimum_z = std::min(minimum_z, light_z);
      maximum_z = std::max(maximum_z, light_z);
    }
    // The camera slice is a receiver volume. Extend it along the light direction so nearby objects
    // outside the frustum can still cast into the visible slice.
    constexpr float caster_depth_margin = 12.0f;
    const float near_distance = -maximum_z - caster_depth_margin;
    const float far_distance = -minimum_z + caster_depth_margin;
    const glm::mat4 light_projection = reverse_z_ortho(
      snapped_x - radius,
      snapped_x + radius,
      snapped_y - radius,
      snapped_y + radius,
      near_distance,
      far_distance);

    const float blend_start = split_far - (split_far - split_near) * cascade_blend_fraction;
    out[index].light_view_projection = light_projection * light_view;
    out[index].split_depths = glm::vec4(
      split_near,
      split_far,
      blend_start,
      debug_tint ? 1.0f : 0.0f);
    out[index].shadow_params = glm::vec4(texel_world, 0.0f, 0.0f, 0.0f);
    split_near = split_far;
  }
  return out;
}

constexpr uint32_t directional_span_count = cascade_count * 2;

struct alignas(16) directional_region_stream {
  painter::region_draw_header header;
  std::array<painter::region_draw_command, cascade_count> regions;
  std::array<painter::region_draw_span, directional_span_count> spans;
};
static_assert(
  sizeof(directional_region_stream) == painter::region_draw_buffer_size(cascade_count, directional_span_count));

directional_region_stream build_directional_regions(
  const uint32_t stage_pair,
  const uint32_t caster_pair,
  const float raster_bias_constant,
  const float raster_bias_slope) {
  directional_region_stream stream{};
  stream.header.region_count = cascade_count;
  stream.header.span_count = directional_span_count;
  stream.header.region_stride = sizeof(painter::region_draw_command);
  stream.header.span_stride = sizeof(painter::region_draw_span);

  constexpr uint32_t tile_resolution = shadow_resolution / 2;
  for (uint32_t cascade_index = 0; cascade_index < cascade_count; ++cascade_index) {
    auto& region = stream.regions[cascade_index];
    region.viewport_x = float((cascade_index & 1u) * tile_resolution);
    region.viewport_y = float((cascade_index >> 1u) * tile_resolution);
    region.viewport_width = float(tile_resolution);
    region.viewport_height = float(tile_resolution);
    region.scissor_x = int32_t((cascade_index & 1u) * tile_resolution);
    region.scissor_y = int32_t((cascade_index >> 1u) * tile_resolution);
    region.scissor_width = tile_resolution;
    region.scissor_height = tile_resolution;
    region.depth_bias_constant = raster_bias_constant;
    region.depth_bias_slope = raster_bias_slope;
    region.data_index = cascade_index;
    region.first_span = cascade_index * 2;
    region.span_count = 2;
    stream.spans[region.first_span] = painter::region_draw_span{stage_pair, 0, 1, 0};
    stream.spans[region.first_span + 1] = painter::region_draw_span{caster_pair, 0, caster_count, 0};
  }
  return stream;
}

struct alignas(16) spot_light_record {
  glm::mat4 light_view_projection;
  glm::vec4 position_range;
  glm::vec4 direction_outer;
  glm::vec4 color_intensity;
  // x: tangent of the spotlight half-FOV, y: map visibility enable,
  // z: receiver-plane scale; w is reserved.
  glm::vec4 shadow_params;
};
static_assert(sizeof(spot_light_record) == 8 * sizeof(glm::vec4));

using spot_light_block = std::array<spot_light_record, spot_count>;

spot_light_block make_spot_lights(const float seconds) {
  spot_light_block out{};
  const std::array<glm::vec3, spot_count> targets{
    glm::vec3{-2.2f, -1.8f, -0.8f},
    glm::vec3{2.0f, -1.8f, -1.5f},
    glm::vec3{-2.0f, -1.8f, -4.1f},
    glm::vec3{2.2f, -1.8f, -3.4f}};
  std::array<glm::vec3, spot_count> positions{
    glm::vec3{-4.0f, 4.4f, 2.7f},
    glm::vec3{4.2f, 4.1f, 2.0f},
    glm::vec3{-3.8f, 3.5f, -3.1f},
    glm::vec3{3.8f, 4.4f, -4.8f}};
  positions[0].x += std::sin(seconds * 0.55f) * 0.9f;
  positions[0].z += std::cos(seconds * 0.43f) * 0.6f;

  const std::array<glm::vec3, spot_count> colors{
    glm::vec3{1.0f, 0.20f, 0.10f},
    glm::vec3{0.12f, 0.42f, 1.0f},
    glm::vec3{0.16f, 1.0f, 0.30f},
    glm::vec3{0.95f, 0.16f, 0.85f}};
  constexpr float range = 12.0f;
  const float outer_cosine = std::cos(glm::radians(25.0f));
  const auto projection = reverse_z_perspective(glm::radians(52.0f), 1.0f, 0.15f, range);
  for (uint32_t i = 0; i < spot_count; ++i) {
    const glm::vec3 direction = glm::normalize(targets[i] - positions[i]);
    const glm::mat4 view = glm::lookAtRH(positions[i], positions[i] + direction, glm::vec3{0.0f, 1.0f, 0.0f});
    out[i].light_view_projection = projection * view;
    out[i].position_range = glm::vec4(positions[i], range);
    out[i].direction_outer = glm::vec4(direction, outer_cosine);
    out[i].color_intensity = glm::vec4(colors[i], 5.5f);
    out[i].shadow_params = glm::vec4(std::tan(glm::radians(52.0f) * 0.5f), 0.0f, 0.0f, 0.0f);
  }
  return out;
}

constexpr uint32_t spot_span_count = spot_count * 2;

struct alignas(16) spot_region_stream {
  painter::region_draw_header header;
  std::array<painter::region_draw_command, spot_count> regions;
  std::array<painter::region_draw_span, spot_span_count> spans;
};
static_assert(sizeof(spot_region_stream) == painter::region_draw_buffer_size(spot_count, spot_span_count));

bool spot_intersects_caster(const spot_light_record& light, const glm::vec3 caster_center) noexcept {
  constexpr float caster_radius = 0.8660254f;
  const glm::vec3 to_caster = caster_center - glm::vec3(light.position_range);
  const float distance = glm::length(to_caster);
  if (distance - caster_radius > light.position_range.w) return false;
  if (distance <= caster_radius) return true;

  // Expand the cone conservatively by the cube's bounding sphere. False positives merely draw an
  // extra caster; false negatives would remove a real shadow.
  const float center_cosine = glm::dot(to_caster / distance, glm::vec3(light.direction_outer));
  const float angular_allowance = std::min(caster_radius / distance, 1.0f);
  return center_cosine + angular_allowance >= light.direction_outer.w;
}

spot_region_stream build_spot_regions(
  const spot_light_block& lights,
  const std::array<glm::vec4, caster_count>& visible_casters,
  std::array<glm::vec4, packed_caster_capacity>& packed_casters,
  const uint32_t stage_pair,
  const uint32_t caster_pair,
  const float raster_bias_constant,
  const float raster_bias_slope) {
  spot_region_stream stream{};
  stream.header.region_count = spot_count;
  stream.header.span_count = spot_span_count;
  stream.header.region_stride = sizeof(painter::region_draw_command);
  stream.header.span_stride = sizeof(painter::region_draw_span);

  std::copy(visible_casters.begin(), visible_casters.end(), packed_casters.begin());
  uint32_t packed_offset = caster_count;
  constexpr uint32_t tile_resolution = shadow_resolution / 2;
  for (uint32_t light_index = 0; light_index < spot_count; ++light_index) {
    auto& region = stream.regions[light_index];
    region.viewport_x = float((light_index & 1u) * tile_resolution);
    region.viewport_y = float((light_index >> 1u) * tile_resolution);
    region.viewport_width = float(tile_resolution);
    region.viewport_height = float(tile_resolution);
    region.scissor_x = int32_t((light_index & 1u) * tile_resolution);
    region.scissor_y = int32_t((light_index >> 1u) * tile_resolution);
    region.scissor_width = tile_resolution;
    region.scissor_height = tile_resolution;
    region.depth_bias_constant = raster_bias_constant;
    region.depth_bias_slope = raster_bias_slope;
    region.data_index = light_index;
    region.first_span = light_index * 2;
    region.span_count = 2;

    stream.spans[region.first_span] = painter::region_draw_span{stage_pair, 0, 1, 0};
    const uint32_t first_caster = packed_offset;
    for (const auto& caster : visible_casters) {
      if (!spot_intersects_caster(lights[light_index], glm::vec3(caster))) continue;
      if (packed_offset >= packed_casters.size()) {
        utils::error{}("PF02 regional caster packing exceeded its fixed capacity");
      }
      packed_casters[packed_offset++] = caster;
    }
    stream.spans[region.first_span + 1] =
      painter::region_draw_span{caster_pair, first_caster, packed_offset - first_caster, 0};
  }
  return stream;
}

void write_current_buffer(painter::graphics_base& base, const std::string_view name, const void* data, const size_t bytes) {
  const uint32_t slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF02 buffer '{}' is absent from the configured graph", name);
  }
  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}("PF02 cannot write buffer '{}' (mapped {}, capacity {}, requested {})", name, frame.mapped != nullptr, frame.sub.size, bytes);
  }
  if (bytes != 0) {
    std::memcpy(static_cast<uint8_t*>(frame.mapped) + frame.sub.offset, data, bytes);
  }
}

void update_contact_dispatch(painter::graphics_base& base, const uint32_t width, const uint32_t height) {
  const uint32_t half_width = (width + 1u) / 2u;
  const uint32_t half_height = (height + 1u) / 2u;
  const uint32_t slot = base.find_constant("contact_dispatch");
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF02 contact_dispatch constant is absent from the configured graph");
  }
  const VkDispatchIndirectCommand command{(half_width + 7u) / 8u, (half_height + 7u) / 8u, 1u};
  base.write_constant_data(slot, command);
  base.update_event();
}

void write_overlay_buffers(painter::graphics_base& base, const playground::visage_overlay& overlay) {
  const auto vertices = overlay.vertices();
  const auto indices = overlay.indices();
  write_current_buffer(base, "ui_vertices", vertices.data(), vertices.size());
  write_current_buffer(base, "ui_indices", indices.data(), indices.size());

  const auto commands = overlay.commands();
  const uint32_t slot = base.find_resource("ui_commands");
  if (slot == painter::invalid_resource_slot) {
    utils::error{}("PF02 buffer 'ui_commands' is absent from the configured graph");
  }
  const auto frame = base.get_current_buffer_resource_frame(slot);
  const uint32_t count = uint32_t(commands.size());
  const size_t bytes = sizeof(count) + commands.size_bytes();
  if (frame.mapped == nullptr || bytes > frame.sub.size) {
    utils::error{}("PF02 cannot write UI commands (mapped {}, capacity {}, requested {})", frame.mapped != nullptr, frame.sub.size, bytes);
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
    utils::error{}("PF02 texture descriptor '{}' is absent", descriptor_name);
  }
  auto& descriptor = base.descriptors[slot];
  const vk::ImageView fallback(assets.default_texture_view());
  if (!fallback || descriptor.texture_count == 0) {
    utils::error{}("PF02 texture descriptor '{}' has no fallback or slots", descriptor_name);
  }

  std::vector<vk::DescriptorImageInfo> images(descriptor.texture_count);
  for (uint32_t i = 0; i < descriptor.texture_count; ++i) {
    vk::ImageView view;
    if (i < assets.texture_slots.size()) {
      view = assets.texture_slots[i].view;
    }
    images[i] = vk::DescriptorImageInfo(vk::Sampler{}, view ? view : fallback, vk::ImageLayout::eShaderReadOnlyOptimal);
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

uint32_t register_mesh_pair(
  painter::graphics_base& base,
  painter::assets_base& assets,
  const std::string_view storage_name,
  const std::string_view geometry_name,
  const std::string_view draw_group_name,
  const std::span<const uint8_t> vertices,
  const std::span<const glm::vec4> instances,
  const uint32_t vertex_count,
  const uint32_t visible_instance_count) {
  const auto mesh = assets.register_buffer_storage(std::string(storage_name));
  assets.create_buffer_storage(mesh, painter::buffer_create_info{std::string(geometry_name), vertex_count, 0});
  assets.populate_buffer_storage(mesh, vertices, std::span<const uint8_t>{});
  assets.mark_ready_buffer_slot(mesh);
  const uint32_t draw_group = base.find_draw_group(draw_group_name);
  if (draw_group == painter::invalid_resource_slot) {
    utils::error{}("PF02 draw group '{}' is absent", draw_group_name);
  }
  const uint32_t pair = base.register_pair(draw_group, mesh, uint32_t(instances.size()));
  for (uint32_t offset = 0; offset < base.frames_in_flight(); ++offset) {
    const auto instance_frame = base.get_current_instance_resource_frame(pair, offset);
    const auto indirect_frame = base.get_current_indirect_resource_frame(pair, offset);
    std::memcpy(static_cast<uint8_t*>(instance_frame.mapped) + instance_frame.sub.offset, instances.data(), instances.size_bytes());
    VkDrawIndirectCommand command{};
    command.vertexCount = vertex_count;
    command.instanceCount = visible_instance_count;
    std::memcpy(static_cast<uint8_t*>(indirect_frame.mapped) + indirect_frame.sub.offset, &command, sizeof(command));
  }
  return pair;
}

std::vector<const char*> instance_extensions(const bool validation) {
  uint32_t count = 0;
  const char** required = input::get_required_instance_extensions(&count);
  std::vector<const char*> extensions(required, required + count);
  if (validation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  return extensions;
}

} // namespace

int main(int argc, char** argv) {
  bool validation = false;
  bool uncapped = false;
  bool start_zero_bias = false;
  bool initial_cascade_debug = false;
  bool initial_contact = false;
  bool initial_map_shadows = true;
  bool initial_receiver_plane = true;
  uint32_t initial_lighting_mode = 0;
  uint32_t initial_aa_mode = 1;
  bool initial_pcss = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option(argv[i]);
    validation = validation || option == "--validation";
    uncapped = uncapped || option == "--uncapped";
    start_zero_bias = start_zero_bias || option == "--zero-bias";
    initial_cascade_debug = initial_cascade_debug || option == "--cascade-debug";
    if (option == "--no-contact") initial_contact = false;
    if (option == "--contact") initial_contact = true;
    if (option == "--no-map-shadows") initial_map_shadows = false;
    if (option == "--map-shadows") initial_map_shadows = true;
    if (option == "--no-receiver-plane") initial_receiver_plane = false;
    if (option == "--receiver-plane") initial_receiver_plane = true;
    if (option == "--all-lights") initial_lighting_mode = 0;
    if (option == "--directional-only") initial_lighting_mode = 1;
    if (option == "--spot-only") initial_lighting_mode = 2;
    if (option == "--hard") initial_aa_mode = 0;
    if (option == "--pcf") initial_aa_mode = 1;
    if (option == "--poisson") initial_aa_mode = 2;
    initial_pcss = initial_pcss || option == "--pcss";
  }

  input::init input_runtime(&error_callback);
  input::events::init();
  painter::load_dispatcher1();

  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "PF02 Shadows";
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
      utils::error{}("PF02 requested validation, but Vulkan validation layers are unavailable");
    }
    instance_info.enabledLayerCount = uint32_t(painter::default_validation_layers.size());
    instance_info.ppEnabledLayerNames = painter::default_validation_layers.data();
  }

  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);
  const VkDebugUtilsMessengerEXT messenger = validation ? painter::create_debug_messenger(instance) : VK_NULL_HANDLE;

  GLFWwindow* window = input::create_window(initial_width, initial_height, "PF02 — shadow maps + spot atlas");
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  const auto surface_result = input::create_window_surface(instance, window, nullptr, &surface);
  if (surface_result != uint32_t(vk::Result::eSuccess)) {
    utils::error{}("PF02 could not create Vulkan surface: {}", vk::to_string(vk::Result(surface_result)));
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
  const VkDevice device = device_maker.create({}, "pf02.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();
    const std::string resource_root = std::string(PF02_RESOURCE_ROOT) + "/";
    const std::string cache_path = utils::cache_folder() + "pf02_shadows.pipeline_cache";
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("pf02_shadows");

    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0) {
      utils::error{}("PF02 could not commit render graph from '{}'", resource_root);
    }
    base.set_surface(surface, initial_width, initial_height);
    base.resize_viewport(initial_width, initial_height);
    base.populate_constant_default_values();
    update_contact_dispatch(base, initial_width, initial_height);
    const uint32_t graph = base.find_render_graph("pf02_shadows");
    if (graph == painter::invalid_resource_slot) utils::error{}("PF02 render graph was not found");
    base.change_render_graph(graph);

    painter::assets_base assets(device, physical.handle);
    assets.create_fence();
    assets.create_allocator(instance);
    assets.create_command_buffer(queues.transfer, queues.graphics);
    assets.set_graphics_base(&base);
    assets.create_default_texture();

    const std::string common_resources = std::string(PLAYGROUND_COMMON_RESOURCE_ROOT) + "/";
    playground::visage_overlay overlay(
      common_resources + "fonts/crimson.roman.ttf",
      common_resources + "ui/lab_overlay.lua",
      playground::overlay_description{
        "PF02 — shadow maps + atlas",
        "4-cascade directional atlas + 2×2 spot atlas → runtime shadow filters",
        "WASD/QE + mouse · Shift boost · bias keys below · Esc exit"});
    const auto atlas = overlay.font_atlas();
    const auto font_texture = assets.register_texture_storage("playground.crimson_roman");
    assets.create_texture_storage(font_texture, painter::texture_create_info{{atlas.width, atlas.height, 1}, VK_FORMAT_R8G8B8A8_UNORM});
    assets.populate_texture_storage(font_texture, atlas.bytes);
    assets.mark_ready_texture_slot(font_texture);
    overlay.set_font_texture(font_texture);
    bind_texture_descriptor(base, assets, "ui_textures");

    const auto stage = make_stage();
    const std::array<glm::vec4, 1> stage_instances{glm::vec4(0.0f)};
    const uint32_t stage_pair = register_mesh_pair(
      base,
      assets,
      "pf02.stage",
      "scene_geometry",
      "scene_draw_group",
      std::span(reinterpret_cast<const uint8_t*>(stage.data()), stage.size() * sizeof(stage[0])),
      stage_instances,
      uint32_t(stage.size()),
      uint32_t(stage_instances.size()));

    const auto cube = make_cube();
    std::array<glm::vec4, caster_count> caster_instances{
      glm::vec4{-2.6f, -1.5f, 0.4f, 0.0f},
      glm::vec4{-0.5f, -1.5f, -2.2f, 0.0f},
      glm::vec4{1.8f, -1.5f, 0.0f, 0.0f},
      glm::vec4{3.4f, -0.2f, -3.8f, 0.0f},
      glm::vec4{0.7f, 0.5f, -4.8f, 0.0f}};
    std::array<glm::vec4, packed_caster_capacity> packed_caster_instances{};
    std::copy(caster_instances.begin(), caster_instances.end(), packed_caster_instances.begin());
    const uint32_t caster_pair = register_mesh_pair(
      base,
      assets,
      "pf02.casters",
      "scene_geometry",
      "scene_draw_group",
      std::span(reinterpret_cast<const uint8_t*>(cube.data()), cube.size() * sizeof(cube[0])),
      packed_caster_instances,
      uint32_t(cube.size()),
      caster_count);

    const std::array<glm::vec3, 3> fullscreen_triangle{
      glm::vec3{-1.0f, -1.0f, 0.0f}, glm::vec3{3.0f, -1.0f, 0.0f}, glm::vec3{-1.0f, 3.0f, 0.0f}};
    const std::array<glm::vec4, 1> debug_instances{glm::vec4(0.0f)};
    register_mesh_pair(
      base,
      assets,
      "pf02.shadow_debug",
      "shadow_debug_geometry",
      "shadow_debug_draw_group",
      std::span(reinterpret_cast<const uint8_t*>(fullscreen_triangle.data()), sizeof(fullscreen_triangle)),
      debug_instances,
      uint32_t(fullscreen_triangle.size()),
      uint32_t(debug_instances.size()));

    input::events::clear_bindings();
    bind_key("camera_forward", "key_w");
    bind_key("camera_back", "key_s");
    bind_key("camera_left", "key_a");
    bind_key("camera_right", "key_d");
    bind_key("camera_down", "key_q");
    bind_key("camera_up", "key_e");
    bind_key("camera_fast", "left_shift");
    bind_key("receiver_constant_down", "key_z");
    bind_key("receiver_constant_up", "key_x");
    bind_key("receiver_slope_down", "key_c");
    bind_key("receiver_slope_up", "key_v");
    bind_key("raster_constant_down", "key_b");
    bind_key("raster_constant_up", "key_n");
    bind_key("raster_slope_down", "key_m");
    bind_key("raster_slope_up", "comma");
    bind_key("bias_zero", "key_0");
    bind_key("bias_defaults", "key_1");
    bind_key("lighting_all", "key_2");
    bind_key("lighting_directional", "key_3");
    bind_key("lighting_spot", "key_4");
    bind_key("shadow_hard", "key_5");
    bind_key("shadow_pcf", "key_6");
    bind_key("shadow_poisson", "key_7");
    bind_key("shadow_pcss", "key_8");
    bind_key("aa_radius_down", "left_bracket");
    bind_key("aa_radius_up", "right_bracket");
    bind_key("emitter_radius_down", "semicolon");
    bind_key("emitter_radius_up", "apostrophe");
    bind_key("receiver_plane_down", "key_g");
    bind_key("receiver_plane_up", "key_h");
    bind_key("contact_toggle", "key_f");
    bind_key("map_shadow_toggle", "key_r");
    bind_key("cascade_debug", "key_9");
    escape_key = input::glfw_key_from_canonical("escape");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    input::set_cursor_input_mode(window, DEVILS_ENGINE_INPUT_CURSOR_DISABLED);
    input::set_raw_mouse_motion(window);

    playground::free_camera camera;
    camera.position = {0.0f, 1.0f, 7.5f};
    auto [mouse_x, mouse_y] = input::cursor_pos(window);
    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;
    playground::frame_pacer frame_pacer(uncapped ? 0u : 60u);
    float raster_bias_constant = start_zero_bias ? 0.0f : default_raster_bias_constant;
    float raster_bias_slope = start_zero_bias ? 0.0f : default_raster_bias_slope;
    float normal_bias_base = start_zero_bias ? 0.0f : default_normal_bias_base;
    float normal_bias_slope = start_zero_bias ? 0.0f : default_normal_bias_slope;
    float receiver_plane_scale = start_zero_bias || !initial_receiver_plane ?
      0.0f : default_receiver_plane_scale;
    uint32_t lighting_mode = initial_lighting_mode;
    uint32_t aa_mode = initial_aa_mode;
    float aa_radius = 1.0f;
    bool pcss_enabled = initial_pcss;
    float emitter_radius = 0.08f;
    bool contact_enabled = initial_contact;
    bool map_shadows_enabled = initial_map_shadows;
    float contact_distance = 0.24f;
    float contact_thickness = 0.055f;
    bool cascade_debug = initial_cascade_debug;

    const glm::vec3 light_direction = glm::normalize(glm::vec3{-0.55f, -1.0f, -0.38f});

    painter::gpu_timestamp_profiler gpu_profiler(base);
    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;
    context.set_gpu_profiler(&gpu_profiler);

    utils::info("PF02 bias controls: Z/X world-texel base, C/V slope, G/H receiver-plane scale, B/N raster constant, M/, raster slope");
    utils::info("PF02 graph: directional + spot atlases -> camera depth -> half-res contact masks -> shadowed forward -> debug/present");

    while (!input::should_close(window)) {
      input::poll_events();
      const auto now = std::chrono::steady_clock::now();
      const float dt = std::clamp(std::chrono::duration<float>(now - previous_time).count(), 0.0f, 0.1f);
      previous_time = now;
      input::events::update(size_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<float>(dt)).count()));

      if (resize_pending) {
        vk::Device(device).waitIdle();
        base.resize_viewport(pending_width, pending_height);
        update_contact_dispatch(base, pending_width, pending_height);
        resize_pending = false;
      }

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
      normal_bias_base = std::clamp(
        normal_bias_base +
          (float(input::events::is_pressed("receiver_constant_up")) -
           float(input::events::is_pressed("receiver_constant_down"))) * dt * 0.75f,
        0.0f,
        3.0f);
      normal_bias_slope = std::clamp(
        normal_bias_slope +
          (float(input::events::is_pressed("receiver_slope_up")) -
           float(input::events::is_pressed("receiver_slope_down"))) * dt * 1.5f,
        0.0f,
        6.0f);
      receiver_plane_scale = std::clamp(
        receiver_plane_scale +
          (float(input::events::is_pressed("receiver_plane_up")) -
           float(input::events::is_pressed("receiver_plane_down"))) * dt,
        0.0f,
        2.0f);
      raster_bias_constant = std::clamp(
        raster_bias_constant +
          (float(input::events::is_pressed("raster_constant_up")) -
           float(input::events::is_pressed("raster_constant_down"))) * dt * 2.0f,
        -8.0f,
        0.0f);
      raster_bias_slope = std::clamp(
        raster_bias_slope +
          (float(input::events::is_pressed("raster_slope_up")) -
           float(input::events::is_pressed("raster_slope_down"))) * dt * 2.5f,
        -8.0f,
        0.0f);
      if (input::events::is_pressed("bias_zero")) {
        raster_bias_constant = 0.0f;
        raster_bias_slope = 0.0f;
        normal_bias_base = 0.0f;
        normal_bias_slope = 0.0f;
        receiver_plane_scale = 0.0f;
      }
      if (input::events::is_pressed("bias_defaults")) {
        raster_bias_constant = default_raster_bias_constant;
        raster_bias_slope = default_raster_bias_slope;
        normal_bias_base = default_normal_bias_base;
        normal_bias_slope = default_normal_bias_slope;
        receiver_plane_scale = default_receiver_plane_scale;
      }
      if (input::events::is_pressed("lighting_all")) lighting_mode = 0;
      if (input::events::is_pressed("lighting_directional")) lighting_mode = 1;
      if (input::events::is_pressed("lighting_spot")) lighting_mode = 2;
      if (input::events::is_pressed("shadow_hard")) aa_mode = 0;
      if (input::events::is_pressed("shadow_pcf")) aa_mode = 1;
      if (input::events::is_pressed("shadow_poisson")) aa_mode = 2;
      if (input::events::check_event("shadow_pcss", input::event_state::press)) {
        pcss_enabled = !pcss_enabled;
      }
      if (input::events::check_event("contact_toggle", input::event_state::press)) {
        contact_enabled = !contact_enabled;
      }
      if (input::events::check_event("map_shadow_toggle", input::event_state::press)) {
        map_shadows_enabled = !map_shadows_enabled;
      }
      aa_radius = std::clamp(
        aa_radius +
          (float(input::events::is_pressed("aa_radius_up")) -
           float(input::events::is_pressed("aa_radius_down"))) * dt,
        0.25f,
        4.0f);
      emitter_radius = std::clamp(
        emitter_radius +
          (float(input::events::is_pressed("emitter_radius_up")) -
           float(input::events::is_pressed("emitter_radius_down"))) * dt * 0.08f,
        0.01f,
        0.50f);
      if (input::events::check_event("cascade_debug", input::event_state::press)) {
        cascade_debug = !cascade_debug;
      }

      if (!base.can_draw()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }

      base.prepare_frame();
      const float aspect = float(std::max(pending_width, 1u)) / float(std::max(pending_height, 1u));
      const auto view = camera.view();
      const float camera_fov = glm::radians(68.0f);
      const auto projection = playground::infinite_reverse_z_projection(camera_fov, aspect, camera_near);
      auto directional_cascades = make_directional_cascades(
        camera,
        aspect,
        camera_fov,
        light_direction,
        cascade_debug);
      for (auto& cascade : directional_cascades) {
        cascade.shadow_params.y = map_shadows_enabled ? 1.0f : 0.0f;
        cascade.shadow_params.z = receiver_plane_scale;
      }
      const auto directional_regions = build_directional_regions(
        stage_pair,
        caster_pair,
        raster_bias_constant,
        raster_bias_slope);
      scene_block scene{};
      scene.view_projection = projection * view;
      scene.view = view;
      scene.light_view_projection = directional_cascades[0].light_view_projection;
      scene.camera_position = glm::vec4(camera.position, -projection[1][1]);
      scene.viewport_near = glm::vec4(
        float(pending_width),
        float(pending_height),
        camera_near,
        float(lighting_mode));
      scene.light_direction = glm::vec4(light_direction, float(aa_mode));
      scene.shadow_params = glm::vec4(
        float(shadow_resolution),
        normal_bias_base,
        normal_bias_slope,
        0.0f);
      scene.filter_params = glm::vec4(
        aa_radius,
        pcss_enabled ? emitter_radius : 0.0f,
        contact_enabled ? contact_distance : 0.0f,
        contact_thickness);
      write_current_buffer(base, "scene_buffer", &scene, sizeof(scene));
      write_current_buffer(
        base,
        "directional_cascade_buffer",
        directional_cascades.data(),
        sizeof(directional_cascades));
      write_current_buffer(
        base,
        "directional_region_commands",
        &directional_regions,
        sizeof(directional_regions));

      const float seconds = std::chrono::duration<float>(now - start_time).count();
      auto spot_lights = make_spot_lights(seconds);
      for (auto& light : spot_lights) {
        light.shadow_params.y = map_shadows_enabled ? 1.0f : 0.0f;
        light.shadow_params.z = receiver_plane_scale;
      }
      write_current_buffer(base, "spot_light_buffer", &spot_lights, sizeof(spot_lights));
      caster_instances[0].x = -2.2f + std::sin(seconds * 0.72f) * 2.1f;
      caster_instances[0].z = 0.3f + std::cos(seconds * 0.46f) * 0.8f;
      caster_instances[3].y = -0.1f + std::sin(seconds * 0.9f) * 0.65f;
      const auto spot_regions = build_spot_regions(
        spot_lights,
        caster_instances,
        packed_caster_instances,
        stage_pair,
        caster_pair,
        raster_bias_constant,
        raster_bias_slope);
      const auto instance_frame = base.get_current_instance_resource_frame(caster_pair);
      std::memcpy(
        static_cast<uint8_t*>(instance_frame.mapped) + instance_frame.sub.offset,
        packed_caster_instances.data(),
        sizeof(packed_caster_instances));
      write_current_buffer(base, "spot_region_commands", &spot_regions, sizeof(spot_regions));

      context.prepare();

      const std::array<uint32_t, spot_count> region_casters{
        spot_regions.spans[1].instance_count,
        spot_regions.spans[3].instance_count,
        spot_regions.spans[5].instance_count,
        spot_regions.spans[7].instance_count};
      const uint32_t packed_count =
        region_casters[0] + region_casters[1] + region_casters[2] + region_casters[3];
      const std::string_view lighting_name =
        lighting_mode == 1 ? "directional only" : lighting_mode == 2 ? "spot only" : "all lights";
      const std::array<std::string_view, 3> aa_names{"hard", "3x3 PCF", "rotated Poisson"};
      std::array<std::string, 10> overlay_details{
        std::format(
          "CSM: 4 x {}^2   splits: {:.1f} / {:.1f} / {:.1f} / {:.1f}   tint [9]: {}",
          shadow_resolution / 2,
          directional_cascades[0].split_depths.y,
          directional_cascades[1].split_depths.y,
          directional_cascades[2].split_depths.y,
          directional_cascades[3].split_depths.y,
          cascade_debug ? "on" : "off"),
        std::format(
          "Spot atlas: 4 x {}^2   casters: [{}, {}, {}, {}]   packed: {}",
          shadow_resolution / 2,
          region_casters[0],
          region_casters[1],
          region_casters[2],
          region_casters[3],
          packed_count),
        std::format("Raster bias [B/N constant, M/, slope]: {:.2f}   {:.2f}", raster_bias_constant, raster_bias_slope),
        std::format(
          "World-texel normal bias [Z/X, C/V]: {:.2f} + {:.2f} slope   plane [G/H] {:.2f}",
          normal_bias_base,
          normal_bias_slope,
          receiver_plane_scale),
        "Bias presets: [0] zero   [1] defaults",
        std::format(
          "Lighting: {} [2-4]   map shadows [R]: {}",
          lighting_name,
          map_shadows_enabled ? "on" : "off"),
        std::format(
          "Edge AA: {}   radius {:.2f} texels [ / ]   [5-7] mode",
          aa_names[aa_mode],
          aa_radius),
        std::format(
          "Spot PCSS [8]: {}   emitter {:.3f}m [; / ']   contact [F]: {} {:.2f}m",
          pcss_enabled ? "on" : "off",
          emitter_radius,
          contact_enabled ? "on" : "off",
          contact_distance),
        "GPU passes: waiting for timestamp results...",
        "GPU graph: waiting for timestamp results..."};
      if (gpu_profiler.has_results() && gpu_profiler.passes().size() == 6) {
        const auto timings = gpu_profiler.passes();
        overlay_details[8] = std::format(
          "GPU: dir {:.3f}   spot {:.3f}   depth {:.3f}   contact {:.3f} ms",
          timings[0].milliseconds,
          timings[1].milliseconds,
          timings[2].milliseconds,
          timings[3].milliseconds);
        overlay_details[9] = std::format(
          "GPU: forward {:.3f} ms   blit {:.3f} ms   graph {:.3f} ms",
          timings[4].milliseconds,
          timings[5].milliseconds,
          gpu_profiler.frame_milliseconds());
      } else if (!gpu_profiler.available()) {
        overlay_details[8] = "GPU timestamps unavailable on the selected graphics queue";
        overlay_details[9].clear();
      }
      overlay.set_detail_lines(overlay_details);

      const uint64_t frame_delta_us = uint64_t(std::max(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<float>(dt)).count(),
        int64_t{1}));
      const uint64_t timestamp_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count());
      overlay.update(frame_delta_us, timestamp_us);
      write_overlay_buffers(base, overlay);

      context.draw();
      base.submit_frame();
      frame_pacer.wait();
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
