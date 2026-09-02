#version 450

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 inverse_view_projection;
  mat4 planet_to_world;
  vec4 camera_position;
  vec4 light_direction;
  vec4 params;
  vec4 viewport_near;
} camera;

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec3 in_colour;
layout(location = 2) flat in vec3 in_flat_colour;
layout(location = 3) in float in_height;

layout(location = 0) out vec4 out_colour;

void main() {
  // Категориальные поля (плиты, провинции, культуры) рисуются ПЛОСКО: интерполяция между двумя
  // случайными цветами меток даёт третий цвет, которого нет ни у одной области, и карта врёт.
  // Непрерывные поля наоборот интерполируются, иначе рельеф и температура становятся ступенчатыми.
  const vec3 base = camera.params.y > 0.5 ? in_flat_colour : in_colour;

  const vec3 normal = normalize(in_normal);
  const float diffuse = max(dot(normal, -camera.light_direction.xyz), 0.0);
  // Терминатор смягчён: у глобуса он занимает половину видимого диска, и жёсткая граница читается
  // как дефект отрисовки, а не как ночь.
  const float shading = 0.42 + 0.70 * smoothstep(0.0, 0.35, diffuse) * (0.35 + 0.65 * diffuse);

  const vec3 view_direction = normalize(camera.camera_position.xyz);
  const float rim = pow(1.0 - max(dot(normal, view_direction), 0.0), 3.0);
  out_colour = vec4(base * shading + vec3(0.10, 0.16, 0.26) * rim * 0.55, 1.0);
}
