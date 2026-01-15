#include "common.h"

#include <vector>
#include <gtl/phmap.hpp>
#include "vulkan_header.h"
#include "devils_engine/utils/core.h"
#include "graphics_base.h"

#include "auxiliary.h"

namespace devils_engine {
namespace painter {

namespace usage {
constexpr std::string_view names[] = {
#define X(name) #name,
  DEVILS_ENGINE_PAINTER_RESOURCE_USAGE_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_RESOURCE_USAGE_LIST
#undef X
};


std::string_view to_string(const enum values u) noexcept {
  if (static_cast<uint32_t>(u) >= count) return std::string_view();
  return names[u];
}

enum values from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return values::count;
  return itr->second;
}

bool is_attachment(const enum values u) noexcept {
  switch (u) {
    case values::color_attachment: return true;
    case values::depth_attachment: return true;
    case values::input_attachment: return true;
    case values::ignore_attachment: return true;
    case values::resolve_attachment: return true;
    default: break;
  }

  return false;
}

bool is_image(const enum values u) noexcept {
  switch (u) {
    case values::color_attachment: return true;
    case values::depth_attachment: return true;
    case values::input_attachment: return true;
    case values::ignore_attachment: return true;
    case values::resolve_attachment: return true;
    case values::sampled: return true;
    case values::depth_read: return true;
    case values::present: return true;
    case values::general: return true;
    case values::texel_read: return true;
    case values::texel_write: return true;
    default: break;
  }

  return false;
}

bool is_buffer(const enum values u) noexcept {
  switch (u) {
    case values::uniform: return true;
    case values::vertex: return true;
    case values::index: return true;
    case values::indirect_read: return true;
    case values::indirect_write: return true;
    case values::storage_read: return true;
    case values::storage_write: return true;
    case values::accel_struct_build_read: return true;
    case values::accel_struct_build_write: return true;
    case values::accel_struct_read: return true;
    case values::accel_struct_write: return true;
    default: break;
  }

  return false;
}

bool is_shader(const enum values u) noexcept {
  switch (u) {
    case values::input_attachment: return true;
    case values::sampled: return true;
    case values::depth_read: return true;
    case values::texel_read: return true;
    case values::texel_write: return true;
    case values::uniform: return true;
    case values::storage_read: return true;
    case values::storage_write: return true;
    default: break;
  }

  return false;
}

bool is_copy(const enum values u) noexcept {
  switch (u) {
    case values::transfer_dst: return true;
    case values::transfer_src: return true;
    case values::host_read: return true;
    case values::host_write: return true;
    default: break;
  }

  return false;
}

bool is_read(const enum values u) noexcept {
  switch (u) {
    case values::input_attachment: return true;
    case values::sampled: return true;
    case values::depth_read: return true;
    case values::present: return true;
    case values::texel_read: return true;
    case values::uniform: return true;
    case values::vertex: return true;
    case values::index: return true;
    case values::indirect_read: return true;
    case values::storage_read: return true;
    case values::accel_struct_build_read: return true;
    case values::accel_struct_read: return true;
    default: break;
  }

  return false;
}

bool is_write(const enum values u) noexcept {
  switch (u) {
    case values::color_attachment: return true;
    case values::depth_attachment: return true;
    case values::ignore_attachment: return true;
    case values::resolve_attachment: return true;
    case values::texel_write: return true;
    case values::indirect_write: return true;
    case values::storage_write: return true;
    case values::accel_struct_build_write: return true;
    case values::accel_struct_write: return true;
    default: break;
  }

  return false;
}

}

namespace role {
constexpr std::string_view names[] = {
#define X(name) #name,
  DEVILS_ENGINE_PAINTER_RESOURCE_ROLE_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_RESOURCE_ROLE_LIST
#undef X
};

std::string_view to_string(const enum values u) noexcept {
  if (static_cast<uint32_t>(u) >= count) return std::string_view();
  return names[u];
}

enum values from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return values::count;
  return itr->second;
}

bool is_attachment(const enum values u) noexcept {
  switch (u) {
    case values::gbuffer_albedo: return true;
    case values::gbuffer_normal: return true;
    case values::gbuffer_material: return true;
    case values::gbuffer_velocity: return true;
    case values::gbuffer_emissive: return true;
    case values::gbuffer_depth: return true;
    default: break;
  }

  return false;
}

bool is_image(const enum values u) noexcept {
  switch (u) {
    case values::present: return true;
    case values::shadow_map: return true;
    case values::shadow_map_array: return true;
    case values::depth: return true;
    case values::depth_stencil: return true;
    case values::hi_z: return true;
    case values::depth_history: return true;
    case values::gbuffer_albedo: return true;
    case values::gbuffer_normal: return true;
    case values::gbuffer_material: return true;
    case values::gbuffer_velocity: return true;
    case values::gbuffer_emissive: return true;
    case values::gbuffer_depth: return true;
    case values::light_accumulation: return true;
    case values::lighting_result: return true;
    case values::hdr_color: return true;
    case values::postprocess_input: return true;
    case values::postprocess_output: return true;
    case values::bloom: return true;
    case values::bloom_mip: return true;
    case values::exposure: return true;
    case values::color_lut: return true;
    case values::history_color: return true;
    case values::history_depth: return true;
    case values::history_velocity: return true;
    case values::history_exposure: return true;
    case values::ui_color: return true;
    case values::ui_depth: return true;
    case values::staging: return true;
    case values::screenshot: return true;
    default: break;
  }

  return false;
}

bool is_buffer(const enum values u) noexcept {
  switch (u) {
    case values::instance_input: return true;
    case values::instance_output: return true;
    case values::indirect_input: return true;
    case values::indirect_output: return true;
    case values::draw_commands: return true;
    case values::visibility: return true;
    case values::culling: return true;
    case values::animation_state: return true;
    case values::bone_buffer: return true;
    case values::bone_texture: return true;
    case values::skinning_output: return true;
    case values::material_data: return true;
    case values::material_table: return true;
    case values::texture_array: return true;
    case values::sampler_table: return true;
    case values::vertex_buffer: return true;
    case values::index_buffer: return true;
    case values::meshlet_buffer: return true;
    case values::meshlet_indices: return true;
    case values::acceleration_structure: return true;
    case values::ray_payload: return true;
    case values::ray_output: return true;
    case values::upload: return true;
    case values::readback: return true;
    case values::copy_src: return true;
    case values::copy_dst: return true;
    case values::global_uniform: return true;
    case values::frame_constants: return true;
    case values::view_constants: return true;
    default: break;
  }

  return false;
}

bool is_indirect(const enum values u) noexcept {
  switch (u) {
    case values::indirect_input: return true;
    case values::indirect_output: return true;
    default: break;
  }
  return false;
}

bool is_readback(const enum values u) noexcept {
  switch (u) {
    case values::readback: return true;
    case values::screenshot: return true;
    default: break;
  }
  return false;
}

bool is_staging(const enum values u) noexcept {
  switch (u) {
    case values::staging: return true;
    case values::upload: return true;
    default: break;
  }
  return false;
}

bool is_host_visible(const enum values u) noexcept {
  switch (u) {
    case values::instance_input: return true; // индирект?
    case values::indirect_input: return true;
    case values::readback: return true;
    case values::staging: return true;
    case values::upload: return true;
    default: break;
  }
  return false;
}

}

namespace size {
constexpr std::string_view names[] = {
#define X(name) #name,
  DEVILS_ENGINE_PAINTER_RESOURCE_SIZE_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_RESOURCE_SIZE_LIST
#undef X
};

std::string_view to_string(const enum values u) noexcept {
  if (static_cast<uint32_t>(u) >= count) return std::string_view();
  return names[u];
}

enum values from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return values::count;
  return itr->second;
}
}

namespace type {
constexpr std::string_view names[] = {
#define X(name) #name,
  DEVILS_ENGINE_PAINTER_RESOURCE_TYPE_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_RESOURCE_TYPE_LIST
#undef X
};

std::string_view to_string(const enum values u) noexcept {
  if (static_cast<uint32_t>(u) >= count) return std::string_view();
  return names[u];
}

enum values from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return values::count;
  return itr->second;
}

uint32_t compute_buffering(const graphics_base* base, const enum values u) noexcept {
  return compute_buffering(base->frames_in_flight(), base->swapchain_frames(), u);
}

uint32_t compute_buffering(const uint32_t frames_count, const uint32_t swapchain_count, const enum values u) noexcept {
  switch (u) {
    case values::singlebuffer: return 1;
    case values::doublebuffer: return 2;
    case values::triplebuffer: return 3;
    case values::quadbuffer: return 4;
    case values::rampagebuffer: return 5;
    case values::swapchain: return swapchain_count;
    case values::swapchain_plus_one: return swapchain_count+1;
    case values::frames_in_flight: return frames_count;
    case values::frames_in_flight_plus_one: return frames_count+1;
    default: break;
  }

  return 0;
}
}

namespace store_op {
constexpr std::string_view names[] = {
#define X(name) #name,
  DEVILS_ENGINE_PAINTER_STORE_OP_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_STORE_OP_LIST
#undef X
};

std::string_view to_string(const enum values u) noexcept {
  if (static_cast<uint32_t>(u) >= count) return std::string_view();
  return names[u];
}

enum values from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return values::count;
  return itr->second;
}
}

namespace step_type {
constexpr std::string_view names[] = {
#define X(name) #name,
  DEVILS_ENGINE_PAINTER_RENDER_STEP_TYPE_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_RENDER_STEP_TYPE_LIST
#undef X
};

std::string_view to_string(const enum values u) noexcept {
  if (static_cast<uint32_t>(u) >= count) return std::string_view();
  return names[u];
}

enum values from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return values::count;
  return itr->second;
}
}

namespace value_type {
constexpr std::string_view names[] = {
#define X(name) #name,
  DEVILS_ENGINE_PAINTER_VALUE_TYPES_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_VALUE_TYPES_LIST
#undef X
};

std::string_view to_string(const enum values u) noexcept {
  if (static_cast<uint32_t>(u) >= count) return std::string_view();
  return names[u];
}

enum values from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return values::count;
  return itr->second;
}
}

namespace preset {
constexpr std::string_view names[] = {
#define X(name) #name,
  DEVILS_ENGINE_PAINTER_PRESETS_NAME_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_PRESETS_NAME_LIST
#undef X
};

std::string_view to_string(const enum values u) noexcept {
  if (static_cast<uint32_t>(u) >= count) return std::string_view();
  return names[u];
}

enum values from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return values::count;
  return itr->second;
}
}

namespace command {
constexpr std::string_view command_names[] = {
#define X(name) #name,
  DEVILS_ENGINE_PAINTER_COMMAND_NAME_LIST
#undef X
};

constexpr std::string_view command_type_names[] = {
#define X(name) #name,
  DEVILS_ENGINE_PAINTER_COMMAND_TYPE_LIST
#undef X
};

//const gtl::flat_hash_map<std::string_view, name_values> name_map = {
//#define X(name) std::make_pair(command_names[name_values::name], name_values::name),
//  DEVILS_ENGINE_PAINTER_COMMAND_NAME_LIST
//#undef X
//};
//
const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name) std::make_pair(command_type_names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_COMMAND_TYPE_LIST
#undef X
};
//
//std::string_view name_to_string(const name_values u) noexcept {
//  if (u >= name_values::count) return std::string_view();
//  return command_names[u];
//}
//
//name_values name_from_string(const std::string_view& name) noexcept {
//  const auto itr = name_map.find(name);
//  if (itr == name_map.end()) return name_values::count;
//  return itr->second;
//}

std::string_view to_string(const values u) noexcept {
  if (u >= values::count) return std::string_view();
  return command_type_names[u];
}

values from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return values::count;
  return itr->second;
}

//name_values convert_to_name(const values u) noexcept {
//  switch (u) {
//    case values::draw: 
//    case values::draw_indexed:
//    case values::draw_indirect:
//    case values::draw_indexed_indirect: 
//    case values::draw_constant:
//    case values::draw_indexed_constant: return name_values::draw;
//    case values::dispatch:
//    case values::dispatch_indirect: 
//    case values::dispatch_constant: return name_values::dispatch;
//    case values::copy_buffer: 
//    case values::copy_image:
//    case values::copy_buffer_image:
//    case values::copy_image_buffer: return name_values::copy;
//    case values::blit_linear: 
//    case values::blit_nearest: return name_values::blit;
//    case values::clear_color: 
//    case values::clear_depth: return name_values::clear;
//  }
//
//  return name_values::count;
//}

bool is_graphics(const values u) noexcept {
  switch (u) {
    case values::draw: 
    case values::draw_indexed:
    case values::draw_indirect:
    case values::draw_indexed_indirect: 
    case values::draw_constant:
    case values::draw_indexed_constant: return true;
    default: break;
  }

  return false;
}

bool is_compute(const values u) noexcept {
  switch (u) {
    case values::dispatch:
    case values::dispatch_indirect: 
    case values::dispatch_constant: return true;
    default: break;
  }

  return false;
}

bool is_transfer(const values u) noexcept {
  switch (u) {
    case values::copy_buffer: 
    case values::copy_image:
    case values::copy_buffer_image:
    case values::copy_image_buffer:
    case values::blit_linear: 
    case values::blit_nearest:
    case values::clear_color: 
    case values::clear_depth: return true;
    default: break;
  }

  return false;
}

step_type::values convert(const values u) noexcept {
  if (is_graphics(u)) return step_type::graphics;
  if (is_compute(u))  return step_type::compute;
  if (is_transfer(u)) return step_type::transfer;
  return step_type::count;
}

}

namespace primitive_topology {
enum values : uint32_t {
#define X(name, vulkan_value) name = vulkan_value,
  DEVILS_ENGINE_PAINTER_PRIMITIVE_TOPOLOGY_LIST
#undef X
  count
};

constexpr std::string_view names[] = {
#define X(name, vulkan_value) #name,
  DEVILS_ENGINE_PAINTER_PRIMITIVE_TOPOLOGY_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name, vulkan_value) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_PRIMITIVE_TOPOLOGY_LIST
#undef X
};

std::string_view to_string(const uint32_t u) noexcept {
  if (u >= count) return std::string_view();
  return names[u];
}

uint32_t from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return UINT32_MAX;
  return itr->second;
}
}

namespace blend_factor {
enum values : uint32_t {
#define X(name, vulkan_value) name = vulkan_value,
  DEVILS_ENGINE_PAINTER_BLEND_FACTOR_LIST
#undef X
  count
};

constexpr std::string_view names[] = {
#define X(name, vulkan_value) #name,
  DEVILS_ENGINE_PAINTER_BLEND_FACTOR_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name, vulkan_value) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_BLEND_FACTOR_LIST
#undef X
};

std::string_view to_string(const uint32_t u) noexcept {
  if (u >= count) return std::string_view();
  return names[u];
}

uint32_t from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return UINT32_MAX;
  return itr->second;
}
}

namespace blend_op {
enum values : uint32_t {
#define X(name, vulkan_value) name = vulkan_value,
  DEVILS_ENGINE_PAINTER_BLEND_OP_LIST
#undef X
  count
};

constexpr std::string_view names[] = {
#define X(name, vulkan_value) #name,
  DEVILS_ENGINE_PAINTER_BLEND_OP_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name, vulkan_value) std::make_pair(#name, values::name),
  DEVILS_ENGINE_PAINTER_BLEND_OP_LIST
#undef X
  std::make_pair("+", values::add),
  std::make_pair("-", values::sub),
  std::make_pair("@-", values::reverse_subtract),
};

std::string_view to_string(const uint32_t u) noexcept {
  if (u >= count) return std::string_view();
  return names[u];
}

uint32_t from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return UINT32_MAX;
  return itr->second;
}
}

namespace compare_op {
enum values : uint32_t {
#define X(name, vulkan_value) name = vulkan_value,
  DEVILS_ENGINE_PAINTER_COMPARE_OP_LIST
#undef X
  count
};

constexpr std::string_view names[] = {
#define X(name, vulkan_value) #name,
  DEVILS_ENGINE_PAINTER_COMPARE_OP_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name, vulkan_value) std::make_pair(#name, values::name),
  DEVILS_ENGINE_PAINTER_COMPARE_OP_LIST
#undef X
};

std::string_view to_string(const uint32_t u) noexcept {
  if (u >= count) return std::string_view();
  return names[u];
}

uint32_t from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return UINT32_MAX;
  return itr->second;
}
}

namespace stencil_op {
enum values : uint32_t {
#define X(name, vulkan_value) name = vulkan_value,
  DEVILS_ENGINE_PAINTER_STENCIL_OP_LIST
#undef X
  count
};

constexpr std::string_view names[] = {
#define X(name, vulkan_value) #name,
  DEVILS_ENGINE_PAINTER_STENCIL_OP_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name, vulkan_value) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_STENCIL_OP_LIST
#undef X
};

std::string_view to_string(const uint32_t u) noexcept {
  if (u >= count) return std::string_view();
  return names[u];
}

uint32_t from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return UINT32_MAX;
  return itr->second;
}
}

namespace polygon_mode {
enum values : uint32_t {
#define X(name, vulkan_value) name = vulkan_value,
  DEVILS_ENGINE_PAINTER_POLYGON_MODE_LIST
#undef X
  count
};

constexpr std::string_view names[] = {
#define X(name, vulkan_value) #name,
  DEVILS_ENGINE_PAINTER_POLYGON_MODE_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name, vulkan_value) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_POLYGON_MODE_LIST
#undef X
};

std::string_view to_string(const uint32_t u) noexcept {
  if (u >= count) return std::string_view();
  return names[u];
}

uint32_t from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return UINT32_MAX;
  return itr->second;
}
}

namespace cull_mode {
enum values : uint32_t {
#define X(name, vulkan_value) name = vulkan_value,
  DEVILS_ENGINE_PAINTER_CULL_MODE_LIST
#undef X
  count
};

constexpr std::string_view names[] = {
#define X(name, vulkan_value) #name,
  DEVILS_ENGINE_PAINTER_CULL_MODE_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name, vulkan_value) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_CULL_MODE_LIST
#undef X
};

std::string_view to_string(const uint32_t u) noexcept {
  if (u >= count) return std::string_view();
  return names[u];
}

uint32_t from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return UINT32_MAX;
  return itr->second;
}
}

namespace front_face {
enum values : uint32_t {
#define X(name, vulkan_value) name = vulkan_value,
  DEVILS_ENGINE_PAINTER_FRONT_FACE_LIST
#undef X
  count
};

constexpr std::string_view names[] = {
#define X(name, vulkan_value) #name,
  DEVILS_ENGINE_PAINTER_FRONT_FACE_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name, vulkan_value) std::make_pair( #name , values::name),
  DEVILS_ENGINE_PAINTER_FRONT_FACE_LIST
#undef X
};

std::string_view to_string(const uint32_t u) noexcept {
  if (u >= count) return std::string_view();
  return names[u];
}

uint32_t from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return UINT32_MAX;
  return itr->second;
}
}

namespace format {
constexpr std::string_view names[] = {
#define X(name, aspect, vulkan_value, element_type) #name,
  DEVILS_ENGINE_PAINTER_FORMAT_NAME_LIST
#undef X
};

constexpr VkImageAspectFlags aspect_map[] = {
#define X(name, aspect, vulkan_value, element_type) aspect,
  DEVILS_ENGINE_PAINTER_FORMAT_NAME_LIST
#undef X
};

constexpr VkFormat format_map[] = {
#define X(name, aspect, vulkan_value, element_type) vulkan_value,
  DEVILS_ENGINE_PAINTER_FORMAT_NAME_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name, aspect, vulkan_value, element_type) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_FORMAT_NAME_LIST
#undef X
};

std::string_view to_string(const values u) noexcept {
  if (u >= count) return std::string_view();
  return names[u];
}

values from_string(const std::string_view& name) noexcept {
  const auto itr = map.find(name);
  if (itr == map.end()) return values::count;
  return itr->second;
}

uint32_t size(const values u) noexcept {
  if (u >= count) return 0;
  if (u == format::dispatch3) return sizeof(VkDispatchIndirectCommand);
  if (u == format::draw4) return sizeof(VkDrawIndirectCommand);
  if (u == format::indexed5) return sizeof(VkDrawIndexedIndirectCommand);
  if (u == format::swapchain4) return 0;
  return format_element_size(to_vulkan_format(u), to_vulkan_aspect(u));
}

uint32_t el_count(const values u) noexcept {
  if (u >= count) return 0;
  if (u == format::dispatch3) return sizeof(VkDispatchIndirectCommand) / sizeof(uint32_t);
  if (u == format::draw4) return sizeof(VkDrawIndirectCommand) / sizeof(uint32_t);
  if (u == format::indexed5) return sizeof(VkDrawIndexedIndirectCommand) / sizeof(uint32_t);
  if (u == format::swapchain4) return 0;
  return format_channel_count(to_vulkan_format(u));
}

format_element_type::values element_type(const values u) noexcept {
  switch (u) {
#define X(name, aspect, vulkan_value, element_type) case values::name : return format_element_type::element_type;
    DEVILS_ENGINE_PAINTER_FORMAT_NAME_LIST
#undef X
    default: break;
  }

  return format_element_type::INVALID;
}

uint32_t to_vulkan_format(const values u) noexcept {
  if (u >= count) return 0;
  return format_map[u];
}

uint32_t to_vulkan_aspect(const values u) noexcept {
  if (u >= count) return 0;
  return aspect_map[u];
}

bool is_depth_vk_format(const uint32_t fmt) noexcept {
  const auto vk_fmt = static_cast<vk::Format>(fmt);
  return vk_fmt == vk::Format::eD16Unorm ||
    vk_fmt == vk::Format::eD16UnormS8Uint ||
    vk_fmt == vk::Format::eD24UnormS8Uint ||
    vk_fmt == vk::Format::eD32Sfloat ||
    vk_fmt == vk::Format::eD32SfloatS8Uint ||
    vk_fmt == vk::Format::eX8D24UnormPack32;
}
}

uint32_t check(const uint32_t index, const std::string_view& type_hint, const std::string_view& hint, const std::string_view& name_hint) {
  if (index == INVALID_RESOURCE_SLOT) {
    utils::error{}("Could not find {} '{}', context: {}", type_hint, hint, name_hint);
  }
  return index;
}

role::values check(const role::values index, const std::string_view& hint, const std::string_view& name_hint) {
  if (index >= role::count) {
    utils::error{}("Could not find role '{}', context: {}", hint, name_hint);
  }
  return index;
}

size::values check(const size::values index, const std::string_view& hint, const std::string_view& name_hint) {
  if (index >= size::count) {
    utils::error{}("Could not find size '{}', context: {}", hint, name_hint);
  }
  return index;
  
}

type::values check(const type::values index, const std::string_view& hint, const std::string_view& name_hint) {
  if (index >= type::count) {
    utils::error{}("Could not find type '{}', context: {}", hint, name_hint);
  }
  return index;
}

usage::values check(const usage::values index, const std::string_view& hint, const std::string_view& name_hint) {
  if (index >= usage::count) {
    utils::error{}("Could not find usage '{}', context: {}", hint, name_hint);
  }
  return index;
}

store_op::values check(const store_op::values index, const std::string_view& hint, const std::string_view& name_hint) {
  if (index >= store_op::count) {
    utils::error{}("Could not find store_op '{}', context: {}", hint, name_hint);
  }
  return index;
}

value_type::values check(const value_type::values index, const std::string_view& hint, const std::string_view& name_hint) {
  if (index >= value_type::count) {
    utils::error{}("Could not find value_type '{}', context: {}", hint, name_hint);
  }
  return index;
}

preset::values check(const preset::values index, const std::string_view& hint, const std::string_view& name_hint) {
  if (index >= preset::count) {
    utils::error{}("Could not find preset '{}', context: {}", hint, name_hint);
  }
  return index;
}

command::values check(const command::values index, const std::string_view& hint, const std::string_view& name_hint) {
  if (index >= command::count) {
    utils::error{}("Could not find command '{}', context: {}", hint, name_hint);
  }
  return index;
}

//command::name_values check(const command::name_values index, const std::string_view& hint, const std::string_view& name_hint = {}) {
//  if (index >= command::name_values::count) {
//    utils::error{}("Could not find command name '{}', context: {}", hint, name_hint);
//  }
//  return index;
//}

}
}