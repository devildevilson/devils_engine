#version 450

// Алгоритм: target mesh повторно рисуется после всех depth writers. При reverse-Z равная или большая
// глубина означает, что fragment цели виден, и stencil pass-op оставляет bit 0x40 нулевым. Более дальний
// fragment проваливает depth test; fixed-function depth-fail-op записывает 0x40. Color writes отключены.

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 debug_params;
  vec4 effect_params;
} camera_data;

layout(location = 2) flat in float material_id;
layout(location = 0) out vec4 out_color;

void main() {
  if (camera_data.effect_params.x < 0.5 || material_id < 2.5) discard;
  out_color = vec4(0.0);
}
