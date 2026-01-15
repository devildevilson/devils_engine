#ifndef DEVILS_ENGINE_PAINTER_COMMON_H
#define DEVILS_ENGINE_PAINTER_COMMON_H

#include <cstdint>
#include <cstddef>
#include <bitset>
#include <string_view>

#define DEVILS_ENGINE_PAINTER_RESOURCE_USAGE_LIST \
  X(undefined) \
  X(color_attachment) \
  X(depth_attachment) \
  X(input_attachment) \
  X(ignore_attachment) \
  X(resolve_attachment) \
  X(sampled) \
  X(depth_read) \
  X(present) \
  X(general) \
  X(texel_read) \
  X(texel_write) \
  X(uniform) \
  X(vertex) \
  X(index) \
  X(indirect_read) \
  X(indirect_write) \
  X(storage_read) \
  X(storage_write) \
  X(transfer_dst) \
  X(transfer_src) \
  X(host_read) \
  X(host_write) \
  X(accel_struct_build_read) \
  X(accel_struct_build_write) \
  X(accel_struct_read) \
  X(accel_struct_write) \


// роли накладывать как флаги? вряд ли
// здесь почти все роли для гпу ресурсов
// и буквально парочка для хоста

#define DEVILS_ENGINE_PAINTER_RESOURCE_ROLE_LIST \
  X(undefined) \
  X(present) \
  X(shadow_map) \
  X(shadow_map_array) \
  X(depth) \
  X(depth_stencil) \
  X(hi_z) \
  X(depth_history) \
  X(gbuffer_albedo) \
  X(gbuffer_normal) \
  X(gbuffer_material) \
  X(gbuffer_velocity) \
  X(gbuffer_emissive) \
  X(gbuffer_depth) \
  X(light_accumulation) \
  X(lighting_result) \
  X(hdr_color) \
  X(postprocess_input) \
  X(postprocess_output) \
  X(bloom) \
  X(bloom_mip) \
  X(exposure) \
  X(color_lut) \
  X(history_color) \
  X(history_depth) \
  X(history_velocity) \
  X(history_exposure) \
  X(instance_input) \
  X(instance_output) \
  X(indirect_input) \
  X(indirect_output) \
  X(draw_commands) \
  X(visibility) \
  X(culling) \
  X(animation_state) \
  X(bone_buffer) \
  X(bone_texture) \
  X(skinning_output) \
  X(material_data) \
  X(material_table) \
  X(texture_array) \
  X(sampler_table) \
  X(vertex_buffer) \
  X(index_buffer) \
  X(meshlet_buffer) \
  X(meshlet_indices) \
  X(acceleration_structure) \
  X(ray_payload) \
  X(ray_output) \
  X(staging) \
  X(upload) \
  X(readback) \
  X(screenshot) \
  X(copy_src) \
  X(copy_dst) \
  X(ui_color) \
  X(ui_depth) \
  X(global_uniform) \
  X(frame_constants) \
  X(view_constants) \
  X(push_constants) \

#define DEVILS_ENGINE_PAINTER_RESOURCE_SIZE_LIST \
  X(viewport) \
  X(viewport_half) \
  X(viewport_quarter) \
  X(viewport_scaled) \
  X(shadow_map) \
  X(shadow_atlas) \
  X(environment_map) \
  X(from_resource) \

#define DEVILS_ENGINE_PAINTER_RESOURCE_TYPE_LIST \
  X(invalid) \
  X(singlebuffer) \
  X(doublebuffer) \
  X(triplebuffer) \
  X(quadbuffer) \
  X(rampagebuffer) \
  X(swapchain) \
  X(swapchain_plus_one) \
  X(frames_in_flight) \
  X(frames_in_flight_plus_one) \

#define DEVILS_ENGINE_PAINTER_STORE_OP_LIST \
  X(none) \
  X(store) \
  X(clear) \
  X(dont_care) \

#define DEVILS_ENGINE_PAINTER_RENDER_STEP_TYPE_LIST \
  X(graphics) \
  X(compute) \
  X(transfer) \


#define DEVILS_ENGINE_PAINTER_PRIMITIVE_TOPOLOGY_LIST \
  X(point_list, VK_PRIMITIVE_TOPOLOGY_POINT_LIST) \
  X(line_list,  VK_PRIMITIVE_TOPOLOGY_LINE_LIST) \
  X(line_strip, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) \
  X(triangle_list,  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) \
  X(triangle_strip, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) \
  X(triangle_fan,   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) \
  X(line_list_with_adjacency,  VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY) \
  X(line_strip_with_adjacency, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY) \
  X(triangle_list_with_adjacency,  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY) \
  X(triangle_strip_with_adjacency, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY) \
  X(patch_list, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) \

#define DEVILS_ENGINE_PAINTER_BLEND_FACTOR_LIST \
  X(zero, VK_BLEND_FACTOR_ZERO) \
  X(one, VK_BLEND_FACTOR_ONE) \
  X(src_color, VK_BLEND_FACTOR_SRC_COLOR) \
  X(one_minus_src_color, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR) \
  X(dst_color, VK_BLEND_FACTOR_DST_COLOR) \
  X(one_minus_dst_color, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR) \
  X(src_alpha, VK_BLEND_FACTOR_SRC_ALPHA) \
  X(one_minus_src_alpha, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA) \
  X(dst_alpha, VK_BLEND_FACTOR_DST_ALPHA) \
  X(one_minus_dst_alpha, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA) \
  X(constant_color, VK_BLEND_FACTOR_CONSTANT_COLOR) \
  X(one_minus_constant_color, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR) \
  X(constant_alpha, VK_BLEND_FACTOR_CONSTANT_ALPHA) \
  X(one_minus_constant_alpha, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA) \
  X(src_alpha_saturate, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE) \
  X(src1_color, VK_BLEND_FACTOR_SRC1_COLOR) \
  X(one_minus_src1_color, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR) \
  X(src1_alpha, VK_BLEND_FACTOR_SRC1_ALPHA) \
  X(one_minus_src1_alpha, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) \

#define DEVILS_ENGINE_PAINTER_BLEND_OP_LIST \
  X(add, VK_BLEND_OP_ADD) \
  X(subtract, VK_BLEND_OP_SUBTRACT) \
  X(reverse_subtract, VK_BLEND_OP_REVERSE_SUBTRACT) \
  X(min, VK_BLEND_OP_MIN) \
  X(max, VK_BLEND_OP_MAX) \
  X(sub, VK_BLEND_OP_SUBTRACT) \
  X(reverse_sub, VK_BLEND_OP_REVERSE_SUBTRACT) \

#define DEVILS_ENGINE_PAINTER_COMPARE_OP_LIST \
  X(never, VK_COMPARE_OP_NEVER) \
  X(less, VK_COMPARE_OP_LESS) \
  X(equal, VK_COMPARE_OP_EQUAL) \
  X(less_or_equal, VK_COMPARE_OP_LESS_OR_EQUAL) \
  X(greater, VK_COMPARE_OP_GREATER) \
  X(not_equal, VK_COMPARE_OP_NOT_EQUAL) \
  X(greater_or_equal, VK_COMPARE_OP_GREATER_OR_EQUAL) \
  X(always, VK_COMPARE_OP_ALWAYS) \
  X(lt, VK_COMPARE_OP_LESS) \
  X(eq, VK_COMPARE_OP_EQUAL) \
  X(le, VK_COMPARE_OP_LESS_OR_EQUAL) \
  X(gt, VK_COMPARE_OP_GREATER) \
  X(ne, VK_COMPARE_OP_NOT_EQUAL) \
  X(ge, VK_COMPARE_OP_GREATER_OR_EQUAL) \

#define DEVILS_ENGINE_PAINTER_STENCIL_OP_LIST \
  X(keep, VK_STENCIL_OP_KEEP) \
  X(zero, VK_STENCIL_OP_ZERO) \
  X(replace, VK_STENCIL_OP_REPLACE) \
  X(increment_and_clamp, VK_STENCIL_OP_INCREMENT_AND_CLAMP) \
  X(decrement_and_clamp, VK_STENCIL_OP_DECREMENT_AND_CLAMP) \
  X(invert, VK_STENCIL_OP_INVERT) \
  X(increment_and_wrap, VK_STENCIL_OP_INCREMENT_AND_WRAP) \
  X(decrement_and_wrap, VK_STENCIL_OP_DECREMENT_AND_WRAP) \

#define DEVILS_ENGINE_PAINTER_POLYGON_MODE_LIST \
  X(fill, VK_POLYGON_MODE_FILL) \
  X(line, VK_POLYGON_MODE_LINE) \
  X(point, VK_POLYGON_MODE_POINT) \

#define DEVILS_ENGINE_PAINTER_CULL_MODE_LIST \
  X(none, VK_CULL_MODE_NONE) \
  X(front, VK_CULL_MODE_FRONT_BIT) \
  X(back, VK_CULL_MODE_BACK_BIT) \
  X(front_and_back, VK_CULL_MODE_FRONT_AND_BACK) \

#define DEVILS_ENGINE_PAINTER_FRONT_FACE_LIST \
  X(counter_clockwise, VK_FRONT_FACE_COUNTER_CLOCKWISE) \
  X(clockwise, VK_FRONT_FACE_CLOCKWISE) \
  X(ccw, VK_FRONT_FACE_COUNTER_CLOCKWISE) \
  X(cw, VK_FRONT_FACE_CLOCKWISE) \

#define DEVILS_ENGINE_PAINTER_VALUE_TYPES_LIST \
  X(screensize) \
  X(fixed) \
  X(fixed_2d) \
  X(fixed_3d) \

#define DEVILS_ENGINE_PAINTER_PRESETS_NAME_LIST \
  X(low) \
  X(mid) \
  X(high) \

#define DEVILS_ENGINE_PAINTER_COMMAND_NAME_LIST \
  X(draw) \
  X(dispatch) \
  X(copy) \
  X(blit) \
  X(clear) \

#define DEVILS_ENGINE_PAINTER_COMMAND_TYPE_LIST \
  X(draw) \
  X(draw_indexed) \
  X(draw_indirect) \
  X(draw_indexed_indirect) \
  X(draw_constant) \
  X(draw_indexed_constant) \
  X(dispatch) \
  X(dispatch_indirect) \
  X(dispatch_constant) \
  X(copy_buffer) \
  X(copy_image) \
  X(copy_buffer_image) \
  X(copy_image_buffer) \
  X(blit_linear) \
  X(blit_nearest) \
  X(clear_color) \
  X(clear_depth) \

#define DEVILS_ENGINE_PAINTER_FORMAT_ELEMENT_TYPE_LIST \
  X(INVALID) \
  X(UNORM) \
  X(SNORM) \
  X(SFLOAT) \
  X(UINT) \
  X(SINT) \


// тут бы добавить помощь для составления std430
// больше всего раздражают форматы вроде "c3c1" - это 4 байта
// нет пусть это будут 8 байт
#define DEVILS_ENGINE_PAINTER_FORMAT_NAME_LIST \
  X(v4, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32A32_SFLOAT, SFLOAT) \
  X(p4, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32A32_SFLOAT, SFLOAT) \
  X(n4, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32A32_SFLOAT, SFLOAT) \
  X(uv4, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32A32_SFLOAT, SFLOAT) \
  X(ui4, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32A32_UINT, UINT) \
  X(i4, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32A32_SINT, SINT) \
  X(sf4, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R16G16B16A16_SFLOAT, SFLOAT) \
  X(bf4, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8G8B8A8_SNORM, SNORM) \
  X(c4, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8G8B8A8_UNORM, UNORM) \
  X(srgba8, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8G8B8A8_SRGB, UNORM) \
  X(rgba8, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8G8B8A8_UNORM, UNORM) \
  X(rgba16, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R16G16B16A16_UNORM, UNORM) \
  X(rgba32, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32A32_SFLOAT, SFLOAT) \
  X(v3, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32_SFLOAT, SFLOAT) \
  X(p3, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32_SFLOAT, SFLOAT) \
  X(n3, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32_SFLOAT, SFLOAT) \
  X(uv3, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32_SFLOAT, SFLOAT) \
  X(ui3, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32_UINT, UINT) \
  X(i3, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32B32_SINT, SINT) \
  X(sf3, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R16G16B16_SFLOAT, SFLOAT) \
  X(bf3, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8G8B8_SNORM, SNORM) \
  X(c3, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8G8B8_UNORM, UNORM) \
  X(rgb8, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8G8B8_UNORM, UNORM) \
  X(rgb16, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R16G16B16_UNORM, UNORM) \
  X(rgb32, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32_SFLOAT, SFLOAT) \
  X(v2, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32_SFLOAT, SFLOAT) \
  X(p2, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32_SFLOAT, SFLOAT) \
  X(n2, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32_SFLOAT, SFLOAT) \
  X(uv2, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32_SFLOAT, SFLOAT) \
  X(ui2, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32_UINT, UINT) \
  X(i2, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32_SINT, SINT) \
  X(sf2, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R16G16_SFLOAT, SFLOAT) \
  X(bf2, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8G8_SNORM, UNORM) \
  X(c2, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8G8_UNORM, UNORM) \
  X(rg8, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8G8_UNORM, UNORM)  \
  X(rg16, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R16G16_UNORM, UNORM) \
  X(rg32, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32G32_SFLOAT, SFLOAT) \
  X(v1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32_SFLOAT, SFLOAT) \
  X(p1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32_SFLOAT, SFLOAT) \
  X(n1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32_SFLOAT, SFLOAT) \
  X(uv1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32_SFLOAT, SFLOAT) \
  X(ui1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32_UINT, UINT) \
  X(i1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32_SINT, SINT) \
  X(sf1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R16_SFLOAT, SFLOAT) \
  X(bf1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8_SNORM, SNORM) \
  X(c1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8_UNORM, UNORM) \
  X(r8, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R8_UNORM, UNORM) \
  X(r16, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R16_UNORM, UNORM) \
  X(r32, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32_SFLOAT, SFLOAT) \
  X(d32, VK_IMAGE_ASPECT_DEPTH_BIT, VK_FORMAT_D32_SFLOAT, SFLOAT) \
  X(d16, VK_IMAGE_ASPECT_DEPTH_BIT, VK_FORMAT_D16_UNORM, UNORM) \
  X(ds32, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_FORMAT_D32_SFLOAT_S8_UINT, SFLOAT) \
  X(ds24, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_FORMAT_D24_UNORM_S8_UINT, SFLOAT) \
  X(ds16, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_FORMAT_D16_UNORM_S8_UINT, SFLOAT) \
  X(pad1, 0, VK_FORMAT_UNDEFINED, INVALID) \
  X(pad2, 0, VK_FORMAT_UNDEFINED, INVALID) \
  X(pad3, 0, VK_FORMAT_UNDEFINED, INVALID) \
  X(dispatch3, 0, VK_FORMAT_UNDEFINED, INVALID) \
  X(draw4, 0, VK_FORMAT_UNDEFINED, INVALID) \
  X(indexed5, 0, VK_FORMAT_UNDEFINED, INVALID) \
  X(swapchain4, 0, VK_FORMAT_UNDEFINED, INVALID) \


namespace devils_engine {
namespace painter {

struct graphics_base;

namespace usage {
enum values : uint32_t {
#define X(name) name,
  DEVILS_ENGINE_PAINTER_RESOURCE_USAGE_LIST
#undef X
  count
};

std::string_view to_string(const enum values u) noexcept;
enum values from_string(const std::string_view& name) noexcept;
bool is_attachment(const enum values u) noexcept;
bool is_image(const enum values u) noexcept;
bool is_buffer(const enum values u) noexcept;
bool is_shader(const enum values u) noexcept; // shader_read + shader_write + ...
bool is_copy(const enum values u) noexcept;
bool is_read(const enum values u) noexcept;
bool is_write(const enum values u) noexcept;
}

namespace role {
enum values {
#define X(name) name,
  DEVILS_ENGINE_PAINTER_RESOURCE_ROLE_LIST
#undef X
  count
};

std::string_view to_string(const enum values u) noexcept;
enum values from_string(const std::string_view& name) noexcept;
bool is_attachment(const enum values u) noexcept;
bool is_image(const enum values u) noexcept;
bool is_buffer(const enum values u) noexcept;
bool is_indirect(const enum values u) noexcept;
bool is_readback(const enum values u) noexcept;
bool is_staging(const enum values u) noexcept;
bool is_host_visible(const enum values u) noexcept;
}

namespace size {
  enum values {
#define X(name) name,
    DEVILS_ENGINE_PAINTER_RESOURCE_SIZE_LIST
#undef X
    count
  };

  std::string_view to_string(const enum values u) noexcept;
  enum values from_string(const std::string_view& name) noexcept;
}

namespace type {
  enum values {
#define X(name) name,
    DEVILS_ENGINE_PAINTER_RESOURCE_TYPE_LIST
#undef X
    count
  };

  std::string_view to_string(const enum values u) noexcept;
  enum values from_string(const std::string_view& name) noexcept;
  uint32_t compute_buffering(const graphics_base* base, const enum values u) noexcept;
  uint32_t compute_buffering(const uint32_t frames_count, const uint32_t swapchain_count, const enum values u) noexcept;
}

namespace store_op {
enum values : uint8_t {
#define X(name) name,
  DEVILS_ENGINE_PAINTER_STORE_OP_LIST
#undef X
  count
};

std::string_view to_string(const enum values u) noexcept;
enum values from_string(const std::string_view& name) noexcept;
}

namespace step_type {
enum values : uint8_t {
#define X(name) name,
  DEVILS_ENGINE_PAINTER_RENDER_STEP_TYPE_LIST
#undef X
  count
};

std::string_view to_string(const enum values u) noexcept;
enum values from_string(const std::string_view& name) noexcept;
}

namespace value_type {
enum values : uint8_t {
#define X(name) name,
  DEVILS_ENGINE_PAINTER_VALUE_TYPES_LIST
#undef X
  count
};

std::string_view to_string(const enum values u) noexcept;
enum values from_string(const std::string_view& name) noexcept;
}

namespace preset {
enum values : uint8_t {
#define X(name) name,
  DEVILS_ENGINE_PAINTER_PRESETS_NAME_LIST
#undef X
  count
};

std::string_view to_string(const enum values u) noexcept;
enum values from_string(const std::string_view& name) noexcept;
}

namespace command {
enum values {
#define X(name) name,
  DEVILS_ENGINE_PAINTER_COMMAND_TYPE_LIST
#undef X
  count
};

//enum name_values {
//#define X(name) name,
//  DEVILS_ENGINE_PAINTER_COMMAND_NAME_LIST
//#undef X
//  count
//};
//
//std::string_view name_to_string(const name_values u) noexcept;
//name_values name_from_string(const std::string_view& name) noexcept;

std::string_view to_string(const values u) noexcept;
values from_string(const std::string_view& name) noexcept;

//name_values convert_to_name(const values u) noexcept;

bool is_graphics(const values u) noexcept;
bool is_compute(const values u) noexcept;
bool is_transfer(const values u) noexcept;
step_type::values convert(const values u) noexcept;
}

namespace primitive_topology {
std::string_view to_string(const uint32_t u) noexcept;
uint32_t from_string(const std::string_view& name) noexcept;
}

namespace blend_factor {
std::string_view to_string(const uint32_t u) noexcept;
uint32_t from_string(const std::string_view& name) noexcept;
}

namespace blend_op {
std::string_view to_string(const uint32_t u) noexcept;
uint32_t from_string(const std::string_view& name) noexcept;
}

namespace compare_op {
std::string_view to_string(const uint32_t u) noexcept;
uint32_t from_string(const std::string_view& name) noexcept;
}

namespace stencil_op {
std::string_view to_string(const uint32_t u) noexcept;
uint32_t from_string(const std::string_view& name) noexcept;
}

namespace polygon_mode {
std::string_view to_string(const uint32_t u) noexcept;
uint32_t from_string(const std::string_view& name) noexcept;
}

namespace cull_mode {
std::string_view to_string(const uint32_t u) noexcept;
uint32_t from_string(const std::string_view& name) noexcept;
}

namespace front_face {
std::string_view to_string(const uint32_t u) noexcept;
uint32_t from_string(const std::string_view& name) noexcept;
}

namespace format_element_type {
enum values : uint32_t {
#define X(element_type) element_type,
  DEVILS_ENGINE_PAINTER_FORMAT_ELEMENT_TYPE_LIST
#undef X
  count
};
}

namespace format {
enum values : uint32_t {
#define X(name, aspect, vulkan_value, element_type) name,
  DEVILS_ENGINE_PAINTER_FORMAT_NAME_LIST
#undef X
  count
};

std::string_view to_string(const values u) noexcept;
values from_string(const std::string_view& name) noexcept;
uint32_t size(const values u) noexcept;
uint32_t el_count(const values u) noexcept;
format_element_type::values element_type(const values u) noexcept;
uint32_t to_vulkan_format(const values u) noexcept;
uint32_t to_vulkan_aspect(const values u) noexcept;
bool is_depth_vk_format(const uint32_t fmt) noexcept;
}

constexpr size_t MAXIMUM_RENDERING_RESOURCES_COUNT = 256;
constexpr size_t MAX_FRAMES_IN_FLIGHT = 8;
constexpr size_t MAX_FRAMEBUFFER_ATTACHMENTS = 8;
using resource_usage_t = std::bitset<MAXIMUM_RENDERING_RESOURCES_COUNT>;
constexpr uint32_t INVALID_RESOURCE_SLOT = UINT32_MAX;

uint32_t check(const uint32_t index, const std::string_view& type_hint, const std::string_view& hint, const std::string_view& name_hint = {});
role::values check(const role::values index, const std::string_view& hint, const std::string_view& name_hint = {});
size::values check(const size::values index, const std::string_view& hint, const std::string_view& name_hint = {});
type::values check(const type::values index, const std::string_view& hint, const std::string_view& name_hint = {});
usage::values check(const usage::values index, const std::string_view& hint, const std::string_view& name_hint = {});
store_op::values check(const store_op::values index, const std::string_view& hint, const std::string_view& name_hint = {});
value_type::values check(const value_type::values index, const std::string_view& hint, const std::string_view& name_hint = {});
preset::values check(const preset::values index, const std::string_view& hint, const std::string_view& name_hint = {});
command::values check(const command::values index, const std::string_view& hint, const std::string_view& name_hint = {});
//command::name_values check(const command::name_values index, const std::string_view& hint, const std::string_view& name_hint = {});

}
}

#endif