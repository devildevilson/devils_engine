#version 450

// Алгоритм: тот же instanced transform, что у выбранного объекта в scene pass, но vertex shader выводит
// только clip-position. Отдельный минимальный интерфейс важен не для картинки, а для честного pipeline:
// mask fragment не нужны normal/material varying, и validation не должна сообщать о потерянных outputs.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_instance;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 debug_params;
} camera_data;

void main() {
  const vec3 world_position = in_position + in_instance.xyz;
  gl_Position = camera_data.view_projection * vec4(world_position, 1.0);
  // Маска переиспользует p3n3 geometry. Чтение normal оставляет vertex-interface полным; malformed NaN
  // безопасно выбрасывает вершину за clip volume и не влияет на корректные mesh.
  if (isnan(in_normal.x)) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
}
