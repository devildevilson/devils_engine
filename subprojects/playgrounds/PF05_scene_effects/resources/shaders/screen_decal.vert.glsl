#version 450

// Алгоритм: каждый instance — ориентированный box, ограничивающий одну glyph-декаль. Мы рисуем только
// задние грани box (front faces отсекаются материалом), чтобы fragment shader запускался лишь внутри его
// экранной проекции. Сам box ничего не пишет: реальная точка поверхности позже восстанавливается из depth.

layout(location = 0) in vec3 in_position;
layout(location = 1) in mat4 in_decal_to_world;
layout(location = 5) in mat4 in_world_to_decal;
layout(location = 9) in vec4 in_uv_rect;
layout(location = 10) in vec4 in_fill_color;
layout(location = 11) in vec4 in_effect;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  mat4 inverse_view_projection;
  vec4 effect_params;
} camera_data;

layout(location = 0) flat out mat4 out_world_to_decal;
layout(location = 4) flat out vec4 out_uv_rect;
layout(location = 5) flat out vec4 out_fill_color;
layout(location = 6) flat out vec4 out_effect;
layout(location = 7) flat out vec3 out_projection_normal;

void main() {
  const vec4 world = in_decal_to_world * vec4(in_position, 1.0);
  gl_Position = camera_data.view_projection * world;
  out_world_to_decal = in_world_to_decal;
  out_uv_rect = in_uv_rect;
  out_fill_color = in_fill_color;
  out_effect = in_effect;
  out_projection_normal = normalize(in_decal_to_world[2].xyz);
}
