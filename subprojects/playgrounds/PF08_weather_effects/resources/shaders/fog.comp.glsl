#version 450

#include "pf08_atmosphere.glsl"
#include "pf08_local_medium.glsl"
#include "pf08_shadow_sample.glsl"

// Настоящая froxel-сетка: XY — редкая экранная сетка, Z — квадратично распределённое расстояние.
// Каждая нить проходит свой луч один раз и пишет НАРАСТАЮЩИЙ интеграл. Slice 0 хранит точное
// тождество (S=0, T=1), поэтому точки у самой камеры не получают туман первого ненулевого слоя.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(set = 0, binding = 1, std140) uniform SkyBlock {
  pf08_sky_block sky;
} sky_data;

layout(set = 0, binding = 2) uniform sampler2D transmittance_lut;
layout(set = 0, binding = 3) uniform sampler2D sky_view_lut;
layout(set = 0, binding = 4, rgba16f) uniform writeonly image3D fog_image;

int pf08_fog_shadow_slot(const float body_code) {
  if (sky_data.sky.shadow_bodies.x == body_code) return 0;
  if (sky_data.sky.shadow_bodies.y == body_code) return 1;
  return -1;
}

vec3 pf08_fog_ambient_radiance() {
  const vec2 size = vec2(textureSize(sky_view_lut, 0));
  const float tilt = 0.82;
  const float up = sqrt(max(0.0, 1.0 - tilt * tilt));
  vec3 total = texture(sky_view_lut, pf08_sky_view_uv(vec3(0.0, 1.0, 0.0), size)).rgb;
  total += texture(sky_view_lut, pf08_sky_view_uv(vec3( tilt, up, 0.0), size)).rgb;
  total += texture(sky_view_lut, pf08_sky_view_uv(vec3(-tilt, up, 0.0), size)).rgb;
  total += texture(sky_view_lut, pf08_sky_view_uv(vec3(0.0, up,  tilt), size)).rgb;
  total += texture(sky_view_lut, pf08_sky_view_uv(vec3(0.0, up, -tilt), size)).rgb;
  return total * 0.2;
}

vec3 pf08_fog_source(const vec3 direction, const vec3 world_position, const float view_distance,
                     const vec3 ambient, const float column_modulation) {
  const pf08_sky_block sky = sky_data.sky;
  const vec3 planet_point = vec3(world_position.x * 0.001,
                                 sky.atmosphere_geometry.x + world_position.y * 0.001,
                                 world_position.z * 0.001);
  vec3 source = ambient;
  const float g = sky.fog_params.z;

  for (int s = 0; s < PF08_STAR_COUNT; ++s) {
    const float illuminance = sky.star_color_illuminance[s].w;
    if (illuminance <= 0.0) continue;
    const vec3 light_direction = sky.star_direction[s].xyz;
    const int slot = pf08_fog_shadow_slot(float(s));
    const float visibility = slot < 0 ? 1.0 :
      pf08_volume_shadow_visibility(slot, world_position, view_distance);
    const vec3 atmosphere = pf08_transmittance_to_light(
      sky, transmittance_lut, planet_point, light_direction);
    const float local_medium = pf08_fog_light_transmittance(
      sky, world_position, light_direction, column_modulation);
    source += atmosphere * sky.star_color_illuminance[s].rgb * illuminance *
              pf08_mie_phase(dot(direction, light_direction), g) * visibility * local_medium;
  }

  const int moon_count = int(sky.march_params.w);
  for (int m = 0; m < moon_count && m < PF08_MOON_CAPACITY; ++m) {
    const float illuminance = sky.moon_color_illuminance[m].w;
    if (illuminance <= 0.0) continue;
    const vec3 light_direction = sky.moon_direction[m].xyz;
    if (light_direction.y <= 0.0) continue;
    const int slot = pf08_fog_shadow_slot(float(PF08_STAR_COUNT + m));
    const float visibility = slot < 0 ? 1.0 :
      pf08_volume_shadow_visibility(slot, world_position, view_distance);
    const float local_medium = pf08_fog_light_transmittance(
      sky, world_position, light_direction, column_modulation);
    source += sky.moon_color_illuminance[m].rgb * illuminance *
              pf08_mie_phase(dot(direction, light_direction), g) * visibility * local_medium;
  }
  return source;
}

void main() {
  const ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
  const ivec3 size = imageSize(fog_image);
  if (cell.x >= size.x || cell.y >= size.y) return;

  const float extinction = max(sky_data.sky.fog_params.x, 0.0);
  if (extinction <= 0.0) {
    for (int slice = 0; slice < size.z; ++slice) {
      imageStore(fog_image, ivec3(cell, slice), vec4(0.0, 0.0, 0.0, 1.0));
    }
    return;
  }

  const vec2 uv = (vec2(cell) + 0.5) / vec2(size.xy);
  const vec2 ndc = uv * 2.0 - 1.0;
  const float aspect = camera_data.viewport_near.x / max(camera_data.viewport_near.y, 1.0);
  const float tan_half_fov = camera_data.viewport_near.w;
  const mat3 camera_to_world = transpose(mat3(camera_data.view));
  const vec3 direction = normalize(camera_to_world *
    vec3(ndc.x * aspect * tan_half_fov, -ndc.y * tan_half_fov, -1.0));

  const float albedo = clamp(sky_data.sky.fog_params.y, 0.0, 1.0);
  const float max_range = max(sky_data.sky.fog_params.w, 1e-3);
  const vec3 ambient = pf08_fog_ambient_radiance();
  vec3 in_scattering = vec3(0.0);
  float transmittance = 1.0;
  float previous_distance = 0.0;
  imageStore(fog_image, ivec3(cell, 0), vec4(in_scattering, transmittance));

  for (int slice = 1; slice < size.z; ++slice) {
    const float normalized = float(slice) / float(max(size.z - 1, 1));
    const float slice_distance = max_range * normalized * normalized;
    const float segment = slice_distance - previous_distance;
    const float midpoint = previous_distance + segment * 0.5;
    previous_distance = slice_distance;

    const vec3 world_position = camera_data.camera_position.xyz + direction * midpoint;
    const float column_modulation = pf08_fog_column_modulation(sky_data.sky, world_position);
    const vec3 source = pf08_fog_source(
      direction, world_position, midpoint, ambient, column_modulation);
    const float local_extinction = extinction *
      pf08_fog_density(sky_data.sky, world_position.y) * column_modulation;
    const float segment_transmittance = exp(-local_extinction * segment);
    // Для постоянного source это точный аналитический интеграл на сегменте, а не sigma*ds.
    in_scattering += transmittance * source * albedo * (1.0 - segment_transmittance);
    transmittance *= segment_transmittance;
    imageStore(fog_image, ivec3(cell, slice), vec4(min(in_scattering, vec3(60000.0)), transmittance));
  }
}
