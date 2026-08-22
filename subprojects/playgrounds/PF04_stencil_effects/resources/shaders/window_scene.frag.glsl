#version 450

// Алгоритм: это тот же дешёвый directional lighting, что у основной сцены, но геометрия получает другую
// view-projection через window_camera_data, а fixed-function stencil пропускает fragments только внутри
// aperture bit 0x04. Холодная палитра намеренно отличает второй ракурс от основной сцены без post-effect.

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) flat in float material_id;
layout(location = 0) out vec4 out_color;

void main() {
  const vec3 n = normalize(world_normal);
  const vec3 light = normalize(vec3(0.35, 0.78, -0.45));
  const float diffuse = max(dot(n, light), 0.0);
  vec3 base = vec3(0.20, 0.34, 0.42);
  if (material_id > 1.5) base = vec3(0.52, 0.24, 0.58);
  else if (material_id > 0.5) base = vec3(0.18, 0.52, 0.42);
  const float floor_grid = world_normal.y > 0.9
    ? mix(0.68, 1.0, float((int(floor(world_position.x)) + int(floor(world_position.z))) & 1))
    : 1.0;
  out_color = vec4(base * floor_grid * (0.22 + 0.78 * diffuse), 1.0);
}
