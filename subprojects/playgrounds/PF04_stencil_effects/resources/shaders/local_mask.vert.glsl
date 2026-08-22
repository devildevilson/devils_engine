#version 450

// Алгоритм: world-space прямоугольник проецируется обычной камерой и задаёт область локального эффекта.
// Материал пишет только stencil bit 0x02 и отключает color writes, поэтому сама proxy geometry невидима;
// depth test не даёт маске проходить сквозь более близкую геометрию.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_instance;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 debug_params;
} camera_data;

void main() {
  gl_Position = camera_data.view_projection * vec4(in_position + in_instance.xyz, 1.0);
}
