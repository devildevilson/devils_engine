#version 450

// Алгоритм: минимальная opaque fixture получает world offset и material id из instance. Она нужна не как
// отдельный lighting proof, а как честная depth/occlusion среда для world-space и billboard MSDF текста.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_instance;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) flat out float material_id;

void main() {
  world_position = in_position + in_instance.xyz;
  world_normal = in_normal;
  material_id = in_instance.w;
  gl_Position = camera_data.view_projection * vec4(world_position, 1.0);
}
