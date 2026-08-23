#version 450

// Алгоритм: 1536 стабильных instance ids процедурно задают точки внутри комнаты; CPU не передаёт particle stream.
// Медленный wrap по Y и небольшой world-space drift дают параллакс настоящих пространственных частиц. Каждый mote
// разворачивается spherical billboard через camera right/up, сохраняет world depth и поэтому исчезает за opaque.

layout(location = 0) out vec2 out_local;
layout(location = 1) out float out_visibility;
layout(location = 2) out float out_scatter_tint;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;
layout(set = 1, binding = 0, std140) uniform LightingBlock {
  vec4 state;
  vec4 presentation;
  vec4 weak_position_radius;
  vec4 weak_color_energy;
  vec4 safe_position_radius;
  vec4 safe_color_energy;
  vec4 flashlight_direction_cos;
  vec4 flashlight_color_energy;
  vec4 room_irradiance;
  vec4 source_reach;
  vec4 medium_params;
  vec4 medium_absorption;
  vec4 medium_scattering;
} lighting;

float hash11(float p) {
  p = fract(p * 0.1031);
  p *= p + 33.33;
  p *= p + p;
  return fract(p);
}

void main() {
  const vec2 corners[6] = vec2[6](
    vec2(-1,-1), vec2(1,-1), vec2(1,1),
    vec2(-1,-1), vec2(1,1), vec2(-1,1));
  const float id = float(gl_InstanceIndex) + 1.0;
  vec3 position = vec3(
    mix(-4.8, 4.8, hash11(id * 1.17)),
    mix(-1.35, 2.85, hash11(id * 2.31)),
    mix(-4.8, 3.8, hash11(id * 4.73)));
  const float speed = mix(0.018, 0.055, hash11(id * 7.11));
  position.y = -1.35 + mod(position.y + 1.35 + lighting.presentation.y * speed, 4.20);
  position.xz += vec2(
    sin(lighting.presentation.y * 0.13 + id),
    cos(lighting.presentation.y * 0.09 + id * 1.7)) * 0.055;

  const mat3 camera_to_world = transpose(mat3(camera_data.view));
  const vec3 camera_right = camera_to_world * vec3(1, 0, 0);
  const vec3 camera_up = camera_to_world * vec3(0, 1, 0);
  const float distance_to_camera = length(position - camera_data.camera_position.xyz);
  const float world_radius = mix(0.0025, 0.0085, hash11(id * 9.37));
  const float two_pixel_radius = 2.2 * distance_to_camera * 2.0 * tan(radians(65.0) * 0.5) /
                                 max(camera_data.viewport_near.y, 1.0);
  const float radius = min(world_radius, two_pixel_radius);
  const vec2 corner = corners[gl_VertexIndex];
  const vec3 world = position + (camera_right * corner.x + camera_up * corner.y) * radius;
  gl_Position = camera_data.view_projection * vec4(world, 1.0);
  out_local = corner;
  out_visibility = lighting.source_reach.w * step(0.0001, lighting.medium_params.x) *
                   lighting.medium_params.w *
                   mix(1.0, 0.32, smoothstep(0.05, 0.6, lighting.state.y));
  // На почти чёрном фоне чисто чёрная пыль математически не может быть заметна. Поэтому часть взвеси — маленькие
  // ненасыщенные крупинки, которые читаются как слабое рассеяние доступного света, а не как emissive sparks.
  const float scattering_mote = step(0.58, hash11(id * 13.91));
  out_scatter_tint = scattering_mote * mix(0.12, 1.0, hash11(id * 17.43));
}
