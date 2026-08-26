#version 450

#include "pf07_shadow.glsl"

// Проход построения карты теней: только глубина. Индекс каскада приходит push-константой, потому что
// все каскады рисуются одним проходом с разными viewport, и вершина обязана знать, в какой именно.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_position_material;
layout(location = 3) in vec4 in_half_roughness;
layout(location = 4) in vec4 in_albedo_unused;

layout(set = 0, binding = 0, std430) readonly buffer CascadeBuffer {
  pf07_cascade cascades[];
} shadow_data;

layout(push_constant) uniform RegionPush {
  uint data_index;
} region;

void main() {
  // Нормаль и альбедо в этом конвейере не нужны, но объявлены: раскладка вершин и инстансов общая с
  // основным проходом, и убрать их отсюда означало бы завести вторую геометрию ради одного шейдера.
  // Множитель — ноль из данных, а не литерал: литерал свернулся бы до отражения вершинного входа.
  const vec3 unused = (in_normal + in_albedo_unused.rgb) * in_position_material.w;
  const vec3 world_position = in_position * in_half_roughness.xyz * 2.0 + in_position_material.xyz + unused;
  gl_Position = shadow_data.cascades[region.data_index].light_view_projection * vec4(world_position, 1.0);
}
