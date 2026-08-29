#version 450

#include "pf08_precipitation.glsl"

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;
layout(set = 0, binding = 1, std140) uniform SkyBlock { pf08_sky_block sky; } sky_data;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec3 out_radiance;
layout(location = 2) out float out_alpha;
layout(location = 3) flat out uint out_kind;

const vec2 quad_positions[6] = vec2[](
  vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
  vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

uint pf08_mid_hash(uint value) {
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  return value ^ (value >> 16u);
}

float pf08_mid_random(const uint seed) {
  return float(pf08_mid_hash(seed) & 0x00ffffffu) / float(0x01000000u);
}

void pf08_hide_mid() {
  gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  out_uv = vec2(0.0);
  out_radiance = vec3(0.0);
  out_alpha = 0.0;
  out_kind = 0u;
}

void main() {
  const pf08_sky_block sky = sky_data.sky;
  const uint id = uint(gl_InstanceIndex);
  const float rain_fraction = max(sky.precipitation_params.x / 40.0, 0.0);
  const float snow_fraction = max(sky.snow_params.x / 10.0, 0.0);
  const float total_fraction = rain_fraction + snow_fraction;
  const float active_fraction = clamp(total_fraction, 0.0, 1.0);
  if (sky.precipitation_time.w < 0.5 || active_fraction <= 0.0 ||
      pf08_mid_random(id * 17u + 9u) >= active_fraction) {
    pf08_hide_mid();
    return;
  }

  const bool snow = pf08_mid_random(id * 29u + 13u) >=
                    rain_fraction / max(total_fraction, 1e-6);
  const float near_radius = max(snow ? sky.snow_params.w : sky.precipitation_params.w, 1.0);
  const float mid_radius = max(snow ? sky.precipitation_mist_lod.w :
                                      sky.precipitation_mist_lod.z, near_radius + 1.0);

  // 64x64 world-anchored columns, по два независимых слоя. При движении камеры меняется только
  // внешний ряд клеток; внутренние сохраняют world hash и не прилипают к экрану.
  const uint columns = 64u;
  const uint cells_per_layer = columns * columns;
  const uint layer = id / cells_per_layer;
  const uint cell_id = id % cells_per_layer;
  const ivec2 offset = ivec2(int(cell_id % columns), int(cell_id / columns)) - ivec2(32);
  const float cell_size = mid_radius * 2.0 / float(columns);
  const ivec2 camera_cell = ivec2(floor(camera_data.camera_position.xz / cell_size));
  const ivec2 world_cell = camera_cell + offset;
  const uint cell_seed = pf08_mid_hash(uint(world_cell.x) * 0x9e3779b9u ^
                                      uint(world_cell.y) * 0x85ebca6bu ^ layer * 0xc2b2ae35u);
  vec2 centre_xz = (vec2(world_cell) +
                    vec2(pf08_mid_random(cell_seed + 1u), pf08_mid_random(cell_seed + 2u))) * cell_size;

  const float fall_speed = max(snow ? sky.snow_params.y : sky.precipitation_params.y, 0.1);
  const float wind_speed = max(snow ? sky.snow_params.z : sky.precipitation_params.z, 0.0);
  const vec2 horizontal_velocity = sky.wind_params.xy * wind_speed;
  const float vertical_span = mix(42.0, 72.0, clamp(mid_radius / 160.0, 0.0, 1.0));
  const float phase = fract(pf08_mid_random(cell_seed + 3u) -
                            sky.wind_params.w * fall_speed / vertical_span);
  const float fall_age = (1.0 - phase) * vertical_span / fall_speed;
  centre_xz += horizontal_velocity * fall_age;
  if (snow) {
    const vec2 perpendicular = vec2(-sky.wind_params.y, sky.wind_params.x);
    centre_xz += perpendicular * sin(sky.wind_params.w * 1.7 +
                                     pf08_mid_random(cell_seed + 4u) * 6.2831853) * 0.65;
  }
  const vec3 centre = vec3(centre_xz, camera_data.camera_position.y - 10.0 + phase * vertical_span).xzy;
  const float horizontal_distance = length(centre.xz - camera_data.camera_position.xz);
  const float footprint = pf08_precipitation_field_density(sky, centre.xz);
  const float footprint_threshold = pf08_mid_random(cell_seed + 5u);
  if (horizontal_distance <= near_radius * 0.68 || horizontal_distance >= mid_radius ||
      footprint_threshold > footprint) {
    pf08_hide_mid();
    return;
  }

  const float near_fade = smoothstep(near_radius * 0.68, near_radius * 1.15, horizontal_distance);
  const float far_fade = 1.0 - smoothstep(mid_radius * 0.78, mid_radius, horizontal_distance);
  const float lod_scale = mix(1.0, snow ? 2.5 : 2.0,
                              clamp(horizontal_distance / mid_radius, 0.0, 1.0));
  const vec2 corner = quad_positions[gl_VertexIndex];
  const mat3 camera_world = transpose(mat3(camera_data.view));
  vec3 right;
  vec3 up;
  vec2 half_extent;
  if (!snow) {
    out_kind = 0u;
    up = normalize(vec3(horizontal_velocity.x, -fall_speed, horizontal_velocity.y));
    const vec3 around_axis = cross(up, camera_data.camera_position.xyz - centre);
    right = dot(around_axis, around_axis) > 1e-7 ? normalize(around_axis) : camera_world[0];
    half_extent = vec2(mix(0.007, 0.012, pf08_mid_random(cell_seed + 6u)),
                       sky.precipitation_shape.x * 0.48 * lod_scale);
    out_alpha = mix(0.10, 0.20, pf08_mid_random(cell_seed + 7u));
  } else {
    out_kind = 2u;
    const float angle = pf08_mid_random(cell_seed + 6u) * 6.2831853 + sky.wind_params.w * 0.65;
    right = normalize(camera_world[0] * cos(angle) + camera_world[1] * sin(angle));
    up = normalize(-camera_world[0] * sin(angle) + camera_world[1] * cos(angle));
    const float size = sky.snow_shape.x * mix(0.75, 1.20, pf08_mid_random(cell_seed + 7u)) * lod_scale;
    half_extent = vec2(size);
    out_alpha = mix(0.25, 0.48, pf08_mid_random(cell_seed + 8u));
  }
  out_alpha *= near_fade * far_fade;

  const vec3 world = centre + right * (corner.x * half_extent.x) + up * (corner.y * half_extent.y);
  gl_Position = camera_data.view_projection * vec4(world, 1.0);
  out_uv = corner * 0.5 + 0.5;

  vec3 weighted_light = vec3(0.0);
  float illuminance = 0.0;
  for (int s = 0; s < PF08_STAR_COUNT; ++s) {
    weighted_light += sky.star_color_illuminance[s].rgb * sky.star_color_illuminance[s].w;
    illuminance += sky.star_color_illuminance[s].w;
  }
  for (int m = 0; m < int(sky.march_params.w) && m < PF08_MOON_CAPACITY; ++m) {
    weighted_light += sky.moon_color_illuminance[m].rgb * sky.moon_color_illuminance[m].w;
    illuminance += sky.moon_color_illuminance[m].w;
  }
  const vec3 tint = illuminance > 0.0 ? weighted_light / illuminance : vec3(0.65, 0.75, 0.90);
  const vec3 particle_tint = snow ? mix(tint, vec3(0.92, 0.96, 1.0), 0.55) : tint;
  out_radiance = particle_tint * max(illuminance * (snow ? 0.065 : 0.030), snow ? 0.025 : 0.010);
}
