#version 450

// Алгоритм: procedural six-vertex quad создаётся из gl_VertexIndex для каждого stable particle slot.
// Живой slot разворачивается camera right/up как spherical billboard, размер плавно возникает и исчезает
// по normalized lifetime. Мёртвый slot отправляется за clip volume; это сознательно простой fixed-pool draw,
// который не требует GPU compaction и indirect-count до появления измеримой причины их добавлять.

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

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec3 out_color;
layout(location = 2) out float out_life_fade;

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
  out_life_fade = appear * disappear;
  const float random_size = float(hash_u32(id) & 255u) / 255.0;
  const float size = mix(0.045, 0.105, random_size) * mix(0.65, 1.0, out_life_fade);
  const vec2 corner = quad_positions[gl_VertexIndex];
  const mat3 camera_world = transpose(mat3(camera_data.view));
  const vec3 world = particle.position_age.xyz +
    camera_world[0] * (corner.x * size) + camera_world[1] * (corner.y * size);
  gl_Position = camera_data.view_projection * vec4(world, 1.0);
  out_uv = corner * 0.5 + 0.5;
  const vec3 hot = vec3(1.0, 0.82, 0.28);
  const vec3 cool = vec3(1.0, 0.16, 0.035);
  out_color = mix(hot, cool, smoothstep(0.12, 0.9, life));
}
