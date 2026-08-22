#version 450

// Алгоритм: fullscreen triangle растеризуется после сцены, но fixed-function compare пропускает fragment
// только при (stencil & 0x02) == 0x02. Alpha blending добавляет холодный tint уже готовому цвету — это
// локальный post-effect без sampled feedback из текущего color attachment.

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 debug_params;
} camera_data;

layout(location = 0) out vec4 out_color;

void main() {
  if (camera_data.debug_params.y < 0.5) discard;
  out_color = vec4(0.02, 0.68, 0.86, 0.30);
}
