#version 450

// Алгоритм: дешёвое направленное освещение делает объём proxy geometry читаемым, не привнося lighting
// pipeline из PF01. Stencil здесь намеренно не вычисляется: материал selected_material записывает reference
// fixed-function стадией только для действительно прошедших depth test фрагментов.

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) flat in float material_id;
layout(location = 0) out vec4 out_color;

void main() {
  const vec3 n = normalize(world_normal);
  const vec3 light = normalize(vec3(-0.45, 0.8, 0.35));
  const float diffuse = max(dot(n, light), 0.0);
  vec3 base = vec3(0.31, 0.35, 0.40);
  if (material_id > 1.5) base = vec3(0.18, 0.46, 0.72);
  else if (material_id > 0.5) base = vec3(0.50, 0.29, 0.18);
  const float floor_grid = world_normal.y > 0.9
    ? mix(0.72, 1.0, float((int(floor(world_position.x)) + int(floor(world_position.z))) & 1))
    : 1.0;
  out_color = vec4(base * floor_grid * (0.24 + 0.76 * diffuse), 1.0);
}
