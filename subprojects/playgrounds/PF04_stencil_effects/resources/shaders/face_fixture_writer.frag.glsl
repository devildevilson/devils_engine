#version 450

// Алгоритм находится в fixed-function stencil state: front-facing fragments заменяют bits 0x30 на 0x10,
// back-facing — инвертируют нулевые bits в 0x30. При выключенном fixture discard предотвращает обе записи;
// color output отключён step mask, поэтому сами тестовые triangles невидимы.

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 debug_params;
} camera_data;

layout(location = 0) out vec4 out_color;

void main() {
  if (camera_data.debug_params.w < 0.5) discard;
  out_color = vec4(0.0);
}
