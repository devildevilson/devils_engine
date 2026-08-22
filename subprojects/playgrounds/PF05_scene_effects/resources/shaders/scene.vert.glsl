#version 450

// Алгоритм: одна локальная mesh используется множеством instance: xyz задаёт world offset, w выбирает fixture
// material. Position переводится в clip space, а world position/normal без искажения уходят в fragment shader.
// Плоские room/cube normals проверяют feature edges, гладкие normals отдельной сферы — cel lighting bands.

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
