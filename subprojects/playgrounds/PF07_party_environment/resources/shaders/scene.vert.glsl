#version 450

// Геометрия сцены: один единичный куб, инстансированный в столбы и плиты. Масштаб оси-выровненный,
// поэтому нормаль грани остаётся точной без обратной транспонированной матрицы. Освещение целиком
// попиксельное, вершина отдаёт только положение в МЕТРАХ и нормаль.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_position_material;
layout(location = 3) in vec4 in_half_roughness;
layout(location = 4) in vec4 in_albedo_unused;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(location = 0) out vec3 out_world_position;
layout(location = 1) out vec3 out_world_normal;
layout(location = 2) out vec3 out_albedo;

void main() {
  out_world_position = in_position * in_half_roughness.xyz * 2.0 + in_position_material.xyz;
  out_world_normal = in_normal;
  out_albedo = in_albedo_unused.rgb;
  gl_Position = camera_data.view_projection * vec4(out_world_position, 1.0);
}
