#version 450

#include "pf08_records.glsl"

struct RainParticle {
  vec4 position_age;
  vec4 velocity_lifetime;
  vec4 impact_position_age;
  vec4 metadata;
};

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;
layout(set = 0, binding = 1, std140) uniform SkyBlock { pf08_sky_block sky; } sky_data;
layout(set = 0, binding = 2, std430) readonly buffer RainState { RainParticle particles[]; } rain_state;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec3 out_radiance;
layout(location = 2) out float out_alpha;
layout(location = 3) flat out uint out_kind;

const vec2 quad_positions[6] = vec2[](
  vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
  vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

uint pf08_rain_hash(uint value) {
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  return value ^ (value >> 16u);
}

void main() {
  const uint id = uint(gl_InstanceIndex);
  const RainParticle particle = rain_state.particles[id];
  const bool impact = particle.impact_position_age.w >= 0.0;
  const bool falling = particle.position_age.w >= 0.0 && particle.velocity_lifetime.w > 0.0;
  const bool snow = particle.metadata.x > 0.5;
  if (sky_data.sky.precipitation_time.w < 0.5 ||
      (sky_data.sky.precipitation_params.x <= 0.0 && sky_data.sky.snow_params.x <= 0.0) ||
      (!impact && !falling)) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    out_uv = vec2(0.0);
    out_radiance = vec3(0.0);
    out_alpha = 0.0;
    out_kind = 0u;
    return;
  }

  const vec2 corner = quad_positions[gl_VertexIndex];
  const mat3 camera_world = transpose(mat3(camera_data.view));
  vec3 centre;
  vec3 right;
  vec3 up;
  vec2 half_extent;
  if (impact) {
    out_kind = 1u;
    const float life = clamp(particle.impact_position_age.w / max(sky_data.sky.precipitation_time.z, 0.01),
                             0.0, 1.0);
    centre = particle.impact_position_age.xyz;
    right = camera_world[0];
    up = camera_world[1];
    half_extent = vec2(mix(0.025, 0.16, life));
    out_alpha = (1.0 - life) * 0.42;
  } else if (!snow) {
    out_kind = 0u;
    centre = particle.position_age.xyz;
    up = normalize(particle.velocity_lifetime.xyz);
    const vec3 to_camera = camera_data.camera_position.xyz - centre;
    const vec3 around_axis = cross(up, to_camera);
    right = dot(around_axis, around_axis) > 1e-7 ? normalize(around_axis) : camera_world[0];
    const float random_size = float(pf08_rain_hash(id) & 255u) / 255.0;
    half_extent = vec2(mix(0.004, 0.009, random_size),
                       sky_data.sky.precipitation_shape.x * mix(0.36, 0.62, random_size));
    out_alpha = mix(0.12, 0.28, random_size);
  } else {
    out_kind = 2u;
    centre = particle.position_age.xyz;
    const float angle = particle.metadata.y + sky_data.sky.wind_params.w * 0.85;
    right = normalize(camera_world[0] * cos(angle) + camera_world[1] * sin(angle));
    up = normalize(-camera_world[0] * sin(angle) + camera_world[1] * cos(angle));
    const float random_size = float(pf08_rain_hash(id) & 255u) / 255.0;
    const float size = sky_data.sky.snow_shape.x * mix(0.62, 1.35, random_size);
    half_extent = vec2(size);
    out_alpha = mix(0.42, 0.78, random_size);
  }

  if (!impact) {
    const float near_radius = max(snow ? sky_data.sky.snow_params.w :
                                       sky_data.sky.precipitation_params.w, 1.0);
    const float horizontal_distance = length(centre.xz - camera_data.camera_position.xz);
    out_alpha *= 1.0 - smoothstep(near_radius * 0.80, near_radius * 1.12,
                                 horizontal_distance);
  }

  const vec3 world = centre + right * (corner.x * half_extent.x) + up * (corner.y * half_extent.y);
  gl_Position = camera_data.view_projection * vec4(world, 1.0);
  out_uv = corner * 0.5 + 0.5;

  vec3 weighted_light = vec3(0.0);
  float illuminance = 0.0;
  for (int s = 0; s < PF08_STAR_COUNT; ++s) {
    weighted_light += sky_data.sky.star_color_illuminance[s].rgb * sky_data.sky.star_color_illuminance[s].w;
    illuminance += sky_data.sky.star_color_illuminance[s].w;
  }
  for (int m = 0; m < int(sky_data.sky.march_params.w) && m < PF08_MOON_CAPACITY; ++m) {
    weighted_light += sky_data.sky.moon_color_illuminance[m].rgb * sky_data.sky.moon_color_illuminance[m].w;
    illuminance += sky_data.sky.moon_color_illuminance[m].w;
  }
  const vec3 tint = illuminance > 0.0 ? weighted_light / illuminance : vec3(0.65, 0.75, 0.90);
  const float response = snow ? 0.075 : (impact ? 0.055 : 0.035);
  const vec3 particle_tint = snow ? mix(tint, vec3(0.92, 0.96, 1.0), 0.55) : tint;
  out_radiance = particle_tint * max(illuminance * response, snow ? 0.035 : 0.015);
}
