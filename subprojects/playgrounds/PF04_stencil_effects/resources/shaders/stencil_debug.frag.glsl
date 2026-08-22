#version 450

// Алгоритм: shader ничего не знает о stencil image. Material сравнивает stencil с reference=1 до запуска
// этого fragment, поэтому полупрозрачный magenta tint попадает ровно в записанную область. debug_params.x
// позволяет скрыть tint, не меняя graph и сохраняя один и тот же stencil consumer для проверки.

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 debug_params;
} camera_data;

layout(location = 0) out vec4 out_color;

void main() {
  if (camera_data.debug_params.x < 0.5) discard;
  out_color = vec4(0.95, 0.08, 0.82, 0.38);
}
