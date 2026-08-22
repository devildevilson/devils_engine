#version 450

// Алгоритм: общая Visage geometry приходит уже в пикселях viewport. Vertex stage переводит её в clip space
// и передаёт MSDF UV/color; UI рисуется последним и не участвует в stencil proof.

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

void main() {
  const vec2 clip = in_position / camera_data.viewport_near.xy * 2.0 - 1.0;
  gl_Position = vec4(clip, 0.0, 1.0);
  out_uv = in_uv;
  out_color = in_color;
}
