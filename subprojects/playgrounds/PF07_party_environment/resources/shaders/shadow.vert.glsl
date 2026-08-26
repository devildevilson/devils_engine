#version 450

#include "pf07_records.glsl"
#include "pf07_shadow.glsl"
#include "pf07_wind.glsl"

// Проход построения карты теней: только глубина. Индекс каскада приходит push-константой, потому что
// все каскады рисуются одним проходом с разными viewport, и вершина обязана знать, в какой именно.
//
// Поворот и ветер повторяются здесь ОДИН В ОДИН с основным проходом, и повторяются они не копией, а
// общим включаемым файлом. Разойдись эти два места — тень куста поедет относительно самого куста, и
// ни компилятор, ни слой проверки об этом не скажут.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_position_material;
layout(location = 3) in vec4 in_half_roughness;
layout(location = 4) in vec4 in_albedo_yaw;

layout(set = 0, binding = 0, std430) readonly buffer CascadeBuffer {
  pf07_cascade cascades[];
} shadow_data;

layout(set = 0, binding = 1, std140) uniform SkyBlock {
  pf07_sky_block sky;
} sky_data;

layout(push_constant) uniform RegionPush {
  uint data_index;
} region;

void main() {
  vec3 local = in_position * in_half_roughness.xyz * 2.0;
  if (in_position_material.w > 0.5) {
    const float yaw = in_albedo_yaw.w;
    const float cosine = cos(yaw);
    const float sine = sin(yaw);
    local = vec3(local.x * cosine - local.z * sine, local.y, local.x * sine + local.z * cosine);
  }

  vec3 world = local + in_position_material.xyz;
  if (in_position_material.w > 0.5) {
    world += pf07_wind_sway(world, in_position.y, in_half_roughness.w, sky_data.sky.wind_params);
  }
  // Нормаль в этом конвейере не нужна, но объявлена: раскладка вершин общая с основным проходом, и
  // убрать её отсюда значило бы завести вторую геометрию ради одного шейдера. Множитель — ноль из
  // данных, а не литерал: литерал свернулся бы до отражения вершинного входа.
  world += in_normal * (in_position_material.w * 0.0);

  gl_Position = shadow_data.cascades[region.data_index].light_view_projection * vec4(world, 1.0);
}
