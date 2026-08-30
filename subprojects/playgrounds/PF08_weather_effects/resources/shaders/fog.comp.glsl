#version 450

#include "pf08_atmosphere.glsl"
#include "pf08_clouds.glsl"
#include "pf08_local_medium.glsl"
#include "pf08_lightning.glsl"
#include "pf08_precipitation.glsl"
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

void pf08_weather_sources(const vec3 direction, const vec3 world_position, const float view_distance,
                          const vec3 ambient, const float column_modulation,
                          out vec3 fog_source, out vec3 cloud_source, out vec3 rain_source,
                          out vec3 snow_source) {
  const pf08_sky_block sky = sky_data.sky;
  const vec3 planet_point = vec3(world_position.x * 0.001,
                                 sky.atmosphere_geometry.x + world_position.y * 0.001,
                                 world_position.z * 0.001);
  fog_source = ambient;
  cloud_source = ambient;
  rain_source = ambient;
  snow_source = ambient;

  for (int s = 0; s < PF08_STAR_COUNT; ++s) {
    const float illuminance = sky.star_color_illuminance[s].w;
    if (illuminance <= 0.0) continue;
    const vec3 light_direction = sky.star_direction[s].xyz;
    const int slot = pf08_fog_shadow_slot(float(s));
    const float visibility = slot < 0 ? 1.0 :
      pf08_volume_shadow_visibility(slot, world_position, view_distance);
    const vec3 atmosphere = pf08_transmittance_to_light(
      sky, transmittance_lut, planet_point, light_direction);
    float local_medium = 1.0;
    if (sky.fog_params.x > 0.0) {
      local_medium *= pf08_fog_light_transmittance(
        sky, world_position, light_direction, column_modulation);
    }
    if (sky.cloud_params.x > 0.0) {
      local_medium *= pf08_cloud_light_transmittance(sky, world_position, light_direction);
    }
    if ((pf08_rain_active(sky) && sky.precipitation_shape.y > 0.0) ||
        (pf08_snow_active(sky) && sky.snow_shape.y > 0.0)) {
      local_medium *= pf08_precipitation_light_transmittance(sky, world_position, light_direction);
    }
    const vec3 source_radiance = atmosphere * sky.star_color_illuminance[s].rgb * illuminance *
                                 visibility * local_medium;
    const float cosine = dot(direction, light_direction);
    fog_source += source_radiance * pf08_mie_phase(cosine, sky.fog_params.z);
    cloud_source += source_radiance * pf08_mie_phase(cosine, sky.cloud_params.w);
    rain_source += source_radiance * pf08_mie_phase(cosine, 0.60);
    snow_source += source_radiance * pf08_mie_phase(cosine, 0.45);
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
    float local_medium = 1.0;
    if (sky.fog_params.x > 0.0) {
      local_medium *= pf08_fog_light_transmittance(
        sky, world_position, light_direction, column_modulation);
    }
    if (sky.cloud_params.x > 0.0) {
      local_medium *= pf08_cloud_light_transmittance(sky, world_position, light_direction);
    }
    if ((pf08_rain_active(sky) && sky.precipitation_shape.y > 0.0) ||
        (pf08_snow_active(sky) && sky.snow_shape.y > 0.0)) {
      local_medium *= pf08_precipitation_light_transmittance(sky, world_position, light_direction);
    }
    const vec3 source_radiance =
      sky.moon_color_illuminance[m].rgb * illuminance * visibility * local_medium;
    const float cosine = dot(direction, light_direction);
    fog_source += source_radiance * pf08_mie_phase(cosine, sky.fog_params.z);
    cloud_source += source_radiance * pf08_mie_phase(cosine, sky.cloud_params.w);
    rain_source += source_radiance * pf08_mie_phase(cosine, 0.60);
    snow_source += source_radiance * pf08_mie_phase(cosine, 0.45);
  }

  // Дальний channel закономерно исчезает раньше, чем его свет. Объём поэтому читает не толщину
  // нарисованной линии, а ту же физическую геометрию события как короткий локальный источник.
  vec3 lightning_direction;
  const vec3 lightning = pf08_lightning_illuminance(sky, world_position, lightning_direction);
  if (dot(lightning, lightning) > 0.0) {
    const float cosine = dot(direction, lightning_direction);
    fog_source += lightning * pf08_mie_phase(cosine, sky.fog_params.z);
    cloud_source += lightning * pf08_mie_phase(cosine, sky.cloud_params.w);
    rain_source += lightning * pf08_mie_phase(cosine, 0.60);
    snow_source += lightning * pf08_mie_phase(cosine, 0.45);
  }
}

void main() {
  const ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
  const ivec3 size = imageSize(fog_image);
  if (cell.x >= size.x || cell.y >= size.y) return;

  const float fog_extinction = max(sky_data.sky.fog_params.x, 0.0);
  const float cloud_extinction = max(sky_data.sky.cloud_params.y, 0.0);
  const bool fog_active = fog_extinction > 0.0;
  const bool cloud_active = sky_data.sky.cloud_params.x > 0.0 && cloud_extinction > 0.0;
  const bool rain_active = pf08_rain_active(sky_data.sky) && sky_data.sky.precipitation_shape.y > 0.0;
  const bool snow_active = pf08_snow_active(sky_data.sky) && sky_data.sky.snow_shape.y > 0.0;
  const bool splash_mist_active = sky_data.sky.precipitation_mist_lod.x > 0.0 &&
                                  pf08_rain_active(sky_data.sky);
  if (!fog_active && !cloud_active && !rain_active && !snow_active && !splash_mist_active) {
    // Apply-pass имеет такой же exact-copy bypass и образ не читает. Не записывать 96 бесполезных
    // слоёв — часть clear-контракта: выключенная погода не должна платить за объём.
    return;
  }

  const vec2 uv = (vec2(cell) + 0.5) / vec2(size.xy);
  const vec2 ndc = uv * 2.0 - 1.0;
  const float aspect = camera_data.viewport_near.x / max(camera_data.viewport_near.y, 1.0);
  const float tan_half_fov = camera_data.viewport_near.w;
  const mat3 camera_to_world = transpose(mat3(camera_data.view));
  const vec3 direction = normalize(camera_to_world *
    vec3(ndc.x * aspect * tan_half_fov, -ndc.y * tan_half_fov, -1.0));

  const float fog_albedo = clamp(sky_data.sky.fog_params.y, 0.0, 1.0);
  const float cloud_albedo = clamp(sky_data.sky.cloud_params.z, 0.0, 1.0);
  const float max_range = max(pf08_weather_volume_range(sky_data.sky), 1e-3);
  const vec3 ambient = pf08_fog_ambient_radiance();
  vec3 in_scattering = vec3(0.0);
  float transmittance = 1.0;
  float previous_distance = 0.0;
  vec3 cached_fog_source = ambient;
  vec3 cached_cloud_source = ambient;
  vec3 cached_rain_source = ambient;
  vec3 cached_snow_source = ambient;
  bool source_cache_valid = false;
  const bool precipitation_active = rain_active || snow_active || splash_mist_active;
  const int precipitation_light_stride = clamp(int(sky_data.sky.fog_noise.w + 0.5), 1, 4);
  imageStore(fog_image, ivec3(cell, 0), vec4(in_scattering, transmittance));

  for (int slice = 1; slice < size.z; ++slice) {
    const float normalized = float(slice) / float(max(size.z - 1, 1));
    const float slice_distance = max_range * normalized * normalized;
    const float segment = slice_distance - previous_distance;
    const float midpoint = previous_distance + segment * 0.5;
    previous_distance = slice_distance;

    const vec3 world_position = camera_data.camera_position.xyz + direction * midpoint;
    const float column_modulation = fog_active
      ? pf08_fog_column_modulation(sky_data.sky, world_position) : 1.0;
    const float local_fog_extinction = fog_active
      ? fog_extinction * pf08_fog_density(sky_data.sky, world_position.y) * column_modulation : 0.0;
    const float local_cloud_extinction = cloud_active
      ? cloud_extinction * pf08_cloud_density(sky_data.sky, world_position) : 0.0;
    const float precipitation_footprint = (rain_active || snow_active || splash_mist_active)
      ? pf08_precipitation_field_density(sky_data.sky, world_position.xz) : 0.0;
    const float local_rain_extinction = rain_active
      ? sky_data.sky.precipitation_shape.y * pf08_rain_far_weight(sky_data.sky, midpoint) *
        pf08_precipitation_vertical_density(sky_data.sky, world_position.y) *
        precipitation_footprint *
        (pf08_shelter_blocks_precipitation(sky_data.sky, world_position,
                                           pf08_precipitation_velocity(sky_data.sky, false)) ? 0.0 : 1.0)
        : 0.0;
    const float local_snow_extinction = snow_active
      ? sky_data.sky.snow_shape.y * pf08_snow_far_weight(sky_data.sky, midpoint) *
        pf08_precipitation_vertical_density(sky_data.sky, world_position.y) *
        precipitation_footprint *
        (pf08_shelter_blocks_precipitation(sky_data.sky, world_position,
                                           pf08_precipitation_velocity(sky_data.sky, true)) ? 0.0 : 1.0)
        : 0.0;
    const float splash_height = max(sky_data.sky.precipitation_mist_lod.y, 0.05);
    const float local_splash_extinction = splash_mist_active
      ? sky_data.sky.precipitation_mist_lod.x * precipitation_footprint *
        exp(-max(world_position.y, 0.0) / splash_height) : 0.0;
    const float local_extinction = local_fog_extinction + local_cloud_extinction +
                                   local_rain_extinction + local_snow_extinction +
                                   local_splash_extinction;
    if (local_extinction <= 1e-8) {
      imageStore(fog_image, ivec3(cell, slice), vec4(min(in_scattering, vec3(60000.0)), transmittance));
      continue;
    }

    // Density still has all 96 slices: that is what removed the kilometre-distance cloud stairs.
    // During precipitation the much costlier lighting term is sampled once per adjacent pair.
    // Trilinear froxel lookup already filters the result, while source visibility, atmosphere and
    // three light-path media no longer run twice for almost the same point. Clear/fog/cloud-only
    // weather deliberately keeps the frozen pre-precipitation path byte-for-byte untouched.
    if (!source_cache_valid || !precipitation_active ||
        (slice - 1) % precipitation_light_stride == 0) {
      pf08_weather_sources(direction, world_position, midpoint, ambient, column_modulation,
                           cached_fog_source, cached_cloud_source,
                           cached_rain_source, cached_snow_source);
      source_cache_valid = true;
    }
    const vec3 scattering_source =
      (cached_fog_source * local_fog_extinction * fog_albedo +
       cached_cloud_source * local_cloud_extinction * cloud_albedo +
       cached_rain_source * local_rain_extinction * 0.90 +
       cached_snow_source * local_snow_extinction * 0.98 +
       cached_rain_source * local_splash_extinction * 0.96) / local_extinction;
    const float segment_transmittance = exp(-local_extinction * segment);
    // Для постоянного source это точный аналитический интеграл на сегменте, а не sigma*ds.
    in_scattering += transmittance * scattering_source * (1.0 - segment_transmittance);
    transmittance *= segment_transmittance;
    imageStore(fog_image, ivec3(cell, slice), vec4(min(in_scattering, vec3(60000.0)), transmittance));
  }
}
