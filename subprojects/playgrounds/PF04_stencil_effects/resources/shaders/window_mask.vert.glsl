#version 450

// Алгоритм: source aperture — настоящая oriented plane, поэтому instance несёт полную world matrix,
// ту же самую, что участвует в source→destination camera mapping. Это удерживает stencil-рамку и remote
// projection в одной системе при translation И rotation source window.

layout(location = 0) in vec3 in_position;
layout(location = 1) in mat4 in_instance;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 debug_params;
} camera_data;

void main() {
  gl_Position = camera_data.view_projection * in_instance * vec4(in_position, 1.0);
}
