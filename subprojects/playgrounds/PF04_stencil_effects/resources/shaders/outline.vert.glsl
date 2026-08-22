#version 450

// Алгоритм: этот fixture точно знает, что local mesh — box с half-extent 0.72. Каждая координата угла
// сдвигается по своему знаку, поэтому все шесть граней остаются соединённой увеличенной коробкой.
// Выталкивание flat-shaded вершин вдоль face normal дало бы шесть разорванных quad. Дальше back-face shell и
// stencil `!= selection` оставляют только кольцо вокруг видимого силуэта, а depth скрывает его за occluders.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_instance;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 debug_params;
} camera_data;

void main() {
  // Every authored box face has one axis-aligned unit normal, hence this is exactly 1 for all 36 vertices.
  const float valid_box_normal = dot(abs(in_normal), vec3(1.0));
  const vec3 expanded_box_position = in_position + sign(in_position) * (0.075 * valid_box_normal);
  const vec3 world_position = expanded_box_position + in_instance.xyz;
  gl_Position = camera_data.view_projection * vec4(world_position, 1.0);
}
