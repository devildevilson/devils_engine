#version 450

// Алгоритм: вершины выбранной mesh немного выталкиваются вдоль нормали. Рисуются только back faces
// увеличенной оболочки, а stencil compare `!= 1` отбрасывает её часть поверх исходного объекта. В результате
// остаётся кольцо вокруг видимого силуэта; depth test по-прежнему скрывает outline за передними occluders.

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
  const vec3 world_position = in_position + in_normal * 0.075 + in_instance.xyz;
  gl_Position = camera_data.view_projection * vec4(world_position, 1.0);
}
