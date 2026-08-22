#version 450

// Алгоритм: дешёвое directional Lambert shading делает глубину и ориентацию стен читаемыми. Material id
// выбирает несколько спокойных fixture-цветов, чтобы яркий MSDF fill/outline проверялся на разном фоне.

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) flat in float material_id;
layout(location = 0) out vec4 frag_color;

void main() {
  vec3 base = vec3(0.17, 0.20, 0.24);
  if (material_id > 0.5) base = vec3(0.28, 0.20, 0.14);
  if (material_id > 1.5) base = vec3(0.12, 0.25, 0.22);
  const vec3 light_dir = normalize(vec3(-0.4, 0.8, 0.5));
  const float diffuse = 0.28 + 0.72 * max(dot(normalize(world_normal), light_dir), 0.0);
  frag_color = vec4(base * diffuse, 1.0);
}
