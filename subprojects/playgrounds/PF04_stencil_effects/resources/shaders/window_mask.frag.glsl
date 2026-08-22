#version 450

// Алгоритм: world-space aperture проходит обычный depth test основной камеры и заменяет только stencil
// bit 0x04. При выключенном window fragment отбрасывается до fixed-function stencil update, поэтому маска
// действительно исчезает, а последующие fullscreen/scene consumers автоматически не проходят compare.

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 debug_params;
} camera_data;

layout(location = 0) out vec4 out_color;

void main() {
  if (camera_data.debug_params.z < 0.5) discard;
  out_color = vec4(0.0);
}
