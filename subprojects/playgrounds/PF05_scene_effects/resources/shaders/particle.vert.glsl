#version 450

// Алгоритм: procedural six-vertex quad создаётся из gl_VertexIndex для каждого stable slot. Spark и snow
// становятся spherical billboards разных размеров. Rain — cylindrical ribbon: длинная ось зафиксирована вдоль
// world velocity, а вокруг неё к камере поворачивается только тонкая поперечная ось. Поэтому наклон камеры даёт
// честное перспективное сокращение капли, а не сохраняет её вертикальной в screen space. Диапазон instance id
// определяет spark/weather, а uniform mode различает rain/snow без перекладки particle buffer.

struct Particle {
  vec4 position_age;
  vec4 velocity_lifetime;
};

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(set = 1, binding = 0, std430) readonly buffer CurrentParticles {
  Particle particles[];
} particle_state;

layout(set = 1, binding = 1, std140) uniform EmitterBlock {
  vec4 origin_rate;
  vec4 direction_speed;
  vec4 acceleration_dt;
  vec4 bounds_restitution;
  vec4 lifetime_drag;
  uvec4 lifecycle;
  uvec4 weather;
  vec4 shelter_min_enabled;
  vec4 shelter_max;
} emitter;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec3 out_color;
layout(location = 2) out float out_life_fade;
layout(location = 3) flat out uint out_particle_kind;

const vec2 quad_positions[6] = vec2[](
  vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
  vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

uint hash_u32(uint value) {
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  return value ^ (value >> 16u);
}

void main() {
  const uint id = uint(gl_InstanceIndex);
  const bool is_weather = id >= emitter.lifecycle.z;
  out_particle_kind = is_weather ? emitter.weather.x : 0u;
  const Particle particle = particle_state.particles[id];
  if (particle.position_age.w < 0.0 || particle.velocity_lifetime.w <= 0.0) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    out_uv = vec2(0.0);
    out_color = vec3(0.0);
    out_life_fade = 0.0;
    return;
  }

  const float life = clamp(particle.position_age.w / particle.velocity_lifetime.w, 0.0, 1.0);
  const float appear = smoothstep(0.0, 0.07, life);
  const float disappear = 1.0 - smoothstep(0.72, 1.0, life);
  out_life_fade = is_weather ? appear : appear * disappear;
  const float random_size = float(hash_u32(id) & 255u) / 255.0;
  const vec2 corner = quad_positions[gl_VertexIndex];
  const mat3 camera_world = transpose(mat3(camera_data.view));
  vec2 half_extent;
  if (out_particle_kind == 1u) {
    half_extent = vec2(mix(0.0025, 0.0050, random_size), mix(0.075, 0.15, random_size));
  } else if (out_particle_kind == 2u) {
    const float size = mix(0.030, 0.070, random_size);
    half_extent = vec2(size);
  } else {
    const float size = mix(0.045, 0.105, random_size) * mix(0.65, 1.0, out_life_fade);
    half_extent = vec2(size);
  }
  vec3 billboard_right = camera_world[0];
  vec3 billboard_up = camera_world[1];
  if (out_particle_kind == 1u) {
    billboard_up = normalize(particle.velocity_lifetime.xyz);
    const vec3 to_camera = camera_data.camera_position.xyz - particle.position_age.xyz;
    const vec3 around_axis = cross(billboard_up, to_camera);
    if (dot(around_axis, around_axis) > 1e-6) billboard_right = normalize(around_axis);
  }
  const vec3 world = particle.position_age.xyz + billboard_right * (corner.x * half_extent.x) +
                     billboard_up * (corner.y * half_extent.y);
  gl_Position = camera_data.view_projection * vec4(world, 1.0);
  out_uv = corner * 0.5 + 0.5;
  if (out_particle_kind == 1u) {
    out_color = vec3(0.50, 0.72, 0.95);
  } else if (out_particle_kind == 2u) {
    out_color = vec3(0.90, 0.95, 1.0);
  } else {
    const vec3 hot = vec3(1.0, 0.82, 0.28);
    const vec3 cool = vec3(1.0, 0.16, 0.035);
    out_color = mix(hot, cool, smoothstep(0.12, 0.9, life));
  }
}
