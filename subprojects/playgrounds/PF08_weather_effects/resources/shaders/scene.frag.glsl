#version 450

#include "pf08_surface.glsl"
#include "pf08_surface_weather.glsl"

// Затенение предметов сцены. Физически это ровно то же, что и земля: ламбертова поверхность в том же
// свете, поэтому весь расчёт освещённости живёт в общем `pf08_surface.glsl`, а здесь остаётся только
// альбедо и воздух между предметом и глазом.
//
// Воздушная перспектива обязательна и не является украшением. Небо у нас физическое и на километре
// заметно синеет; предмет, нарисованный без этого слагаемого, окажется контрастнее и теплее фона на
// том же расстоянии и будет читаться наклеенным поверх кадра — та самая болезнь, из-за которой земля
// когда-то выглядела вырезанной из другой картинки.

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_world_normal;
layout(location = 2) in vec3 in_albedo;
layout(location = 3) in vec2 in_foliage_height;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(set = 2, binding = 0, std140) uniform SkyBlock {
  pf08_sky_block sky;
} sky_data;

layout(set = 2, binding = 1) uniform sampler2D transmittance_lut;
layout(set = 2, binding = 2) uniform sampler2D sky_view_lut;
layout(set = 2, binding = 3) uniform sampler3D aerial_lut;

void main() {
  const vec3 normal = normalize(in_world_normal);
  const vec3 planet_point = pf08_scene_to_planet(sky_data.sky, in_world_position);

  const float view_distance = length(in_world_position - camera_data.camera_position.xyz);
  const float foliage = step(0.5, in_foliage_height.x);
  const float open_height = smoothstep(0.0, 0.75, in_foliage_height.y);
  // Единый bias в 1.5 текселя был шире десятисантиметрового лезвия и выталкивал его из собственной
  // тени. У растения приёмник смещается лишь на четверть текселя. Оставшаяся окклюзия описывает сам
  // куст: у основания свет закрывают соседние лезвия и земля, у верхушки растение открыто.
  const float receiver_bias_scale = mix(1.5, 0.25, foliage);
  const float direct_visibility = mix(1.0, mix(0.45, 1.0, open_height), foliage);
  const float sky_visibility = mix(1.0, mix(0.25, 0.80, open_height), foliage);
  vec3 surface_albedo;
  float roughness;
  vec3 f0;
  float snow_mask;
  float wet_mask;
  pf08_surface_weather_material(sky_data.sky, in_world_position, normal, foliage, in_albedo,
                                surface_albedo, roughness, f0, snow_mask, wet_mask);
  vec3 color;
  // При roughness 0.86 снежный GGX незаметен рядом с его светлым diffuse, но всё равно удваивал
  // обход светил и shadow-map samples на каждом покрытом пикселе. Зеркальная ветка нужна именно
  // жидкой плёнке; снег выражается альбедо, пятнами и настоящей толщиной.
  if (wet_mask > 1e-4) {
    const vec3 view_direction = normalize(camera_data.camera_position.xyz - in_world_position);
    color = pf08_surface_wet_radiance(
      sky_data.sky, transmittance_lut, sky_view_lut, planet_point, in_world_position, normal,
      view_direction, view_distance, receiver_bias_scale, direct_visibility, sky_visibility,
      surface_albedo, roughness, f0);
  } else {
    const vec3 illuminance =
      pf08_surface_illuminance(sky_data.sky, transmittance_lut, sky_view_lut, planet_point,
                               in_world_position, normal, view_distance, receiver_bias_scale,
                               direct_visibility, sky_visibility);
    color = illuminance * surface_albedo / pf08_pi;
  }

  // Воздух между предметом и глазом. Ось расстояния таблицы квадратичная, поэтому и выборка идёт по
  // корню: у камеры срезы густые, вдали редкие.
  const float max_range = sky_data.sky.march_params.z;
  const float distance_km = view_distance * 0.001;
  const float slice = sqrt(clamp(distance_km / max(max_range, 1e-6), 0.0, 1.0));
  const vec2 screen_uv = gl_FragCoord.xy / max(camera_data.viewport_near.xy, vec2(1.0));
  const vec4 aerial = texture(aerial_lut, vec3(screen_uv, slice));
  color = color * aerial.a + aerial.rgb;

  const float half_float_ceiling = 60000.0;
  out_color = vec4(min(color, vec3(half_float_ceiling)), 1.0);
}
