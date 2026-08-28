#version 450

#include "pf08_precipitation.glsl"

// Persistent stable-slot rain pool. Один invocation владеет одной каплей: freelist и atomics не нужны.
// Falling state хранит position/velocity; после current-frame depth contact тот же slot на короткое время
// становится impact, затем переиспользуется.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct RainParticle {
  vec4 position_age;
  vec4 velocity_lifetime;
  vec4 impact_position_age;
  // x: 0 rain, 1 snow; y: постоянная фаза flutter.
  vec4 metadata;
};

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(set = 0, binding = 1, std140) uniform SkyBlock {
  pf08_sky_block sky;
} sky_data;

layout(set = 0, binding = 2, std430) writeonly buffer CurrentRain {
  RainParticle particles[];
} current_state;

layout(set = 0, binding = 3, std430) readonly buffer PreviousRain {
  RainParticle particles[];
} previous_state;

layout(set = 0, binding = 4) uniform sampler2D scene_depth_texture;

uint pf08_rain_hash(uint value) {
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  return value ^ (value >> 16u);
}

float pf08_rain_random(const uint seed) {
  return float(pf08_rain_hash(seed) & 0x00ffffffu) / float(0x01000000u);
}

RainParticle pf08_dead_rain() {
  RainParticle particle;
  particle.position_age = vec4(0.0, 0.0, 0.0, -1.0);
  particle.velocity_lifetime = vec4(0.0);
  particle.impact_position_age = vec4(0.0, 0.0, 0.0, -1.0);
  particle.metadata = vec4(-1.0);
  return particle;
}

RainParticle pf08_spawn_precipitation(const uint id, const bool snow, const bool scatter) {
  const pf08_sky_block sky = sky_data.sky;
  const uint frame = uint(max(sky.precipitation_time.y, 0.0));
  const uint seed = pf08_rain_hash(id * 0x9e3779b9u ^ frame);
  const float radius = max(snow ? sky.snow_params.w : sky.precipitation_params.w, 1.0);
  const float x = mix(-radius, radius, pf08_rain_random(seed + 1u));
  const float z = mix(-radius, radius, pf08_rain_random(seed + 2u));
  const float fall_speed = max(snow ? sky.snow_params.y : sky.precipitation_params.y, 0.1) *
                           mix(0.82, 1.18, pf08_rain_random(seed + 3u));
  const float wind_speed = max(snow ? sky.snow_params.z : sky.precipitation_params.z, 0.0);
  // wind_params.xy — это МИРОВЫЕ XZ. vec3(vec2, float) здесь незаметно превратил бы скорость
  // падения в Z и заставил капли лететь почти горизонтально.
  const vec3 velocity = vec3(sky.wind_params.x * wind_speed, -fall_speed,
                             sky.wind_params.y * wind_speed);
  const float vertical_span = radius * 1.35;
  const float lifetime = vertical_span / fall_speed + 0.35;
  const float age = scatter ? pf08_rain_random(seed + 4u) * lifetime * 0.92 : 0.0;

  RainParticle particle;
  particle.position_age = vec4(camera_data.camera_position.xyz + vec3(x, radius * 0.82, z) + velocity * age,
                               age);
  // Scatter может сразу положить медленный хлопок под крышу. Возвращаем такой slot к началу ЕГО ЖЕ
  // траектории над камерой: дальше он пересечёт реальную крышу segment test'ом. Простое поднятие по Y
  // изменило бы наклонную линию падения и могло перенести хлопок на неверную сторону края.
  if (pf08_shelter_blocks_precipitation(sky, particle.position_age.xyz, velocity)) {
    particle.position_age.xyz -= velocity * age;
    particle.position_age.w = 0.0;
  }
  particle.velocity_lifetime = vec4(velocity, lifetime);
  particle.impact_position_age = vec4(0.0, 0.0, 0.0, -1.0);
  particle.metadata = vec4(snow ? 1.0 : 0.0, pf08_rain_random(seed + 5u) * 6.2831853, 0.0, 0.0);
  return particle;
}

bool pf08_rain_scene_contact(const vec3 previous_position, const vec3 candidate_position,
                             const float thickness, out vec3 hit_position) {
  const vec4 clip = camera_data.view_projection * vec4(candidate_position, 1.0);
  if (clip.w <= 0.0) return false;
  const vec3 ndc = clip.xyz / clip.w;
  const vec2 uv = ndc.xy * 0.5 + 0.5;
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) return false;

  const float depth = texture(scene_depth_texture, uv).r;
  if (depth <= 0.0) return false;
  const float previous_depth = -(camera_data.view * vec4(previous_position, 1.0)).z;
  const float candidate_depth = -(camera_data.view * vec4(candidate_position, 1.0)).z;
  const float surface_depth = camera_data.viewport_near.z / depth;
  if (previous_depth > surface_depth + thickness * 0.35 ||
      candidate_depth < surface_depth - thickness || candidate_depth > surface_depth + thickness) return false;

  const float aspect = camera_data.viewport_near.x / max(camera_data.viewport_near.y, 1.0);
  const vec3 view_ray = normalize(vec3(ndc.x * aspect * camera_data.viewport_near.w,
                                        -ndc.y * camera_data.viewport_near.w, -1.0));
  const vec3 world_ray = normalize(transpose(mat3(camera_data.view)) * view_ray);
  const float ray_distance = surface_depth / max(-view_ray.z, 1e-6);
  hit_position = camera_data.camera_position.xyz + world_ray * ray_distance;
  return true;
}

void main() {
  const uint id = gl_GlobalInvocationID.x;
  if (id >= current_state.particles.length()) return;
  const pf08_sky_block sky = sky_data.sky;
  // Pool очищается и в clear: per-frame copies иначе сохранят старые drops, и следующий переход
  // clear -> rain на один кадр воскресит их в прежних местах. 4096 коротких записей дешевле скрытого
  // lifecycle-разрыва; vertex всё равно делает exact visual bypass по rate.
  if (sky.precipitation_time.w < 0.5 ||
      (sky.precipitation_params.x <= 0.0 && sky.snow_params.x <= 0.0)) {
    current_state.particles[id] = pf08_dead_rain();
    return;
  }
  const float rain_fraction = max(sky.precipitation_params.x / 40.0, 0.0);
  const float snow_fraction = max(sky.snow_params.x / 10.0, 0.0);
  const float total_fraction = rain_fraction + snow_fraction;
  const float active_fraction = clamp(total_fraction, 0.0, 1.0);
  if (active_fraction <= 0.0 || pf08_rain_random(id * 17u + 9u) >= active_fraction) {
    current_state.particles[id] = pf08_dead_rain();
    return;
  }
  const bool snow = pf08_rain_random(id * 29u + 13u) >=
                    rain_fraction / max(total_fraction, 1e-6);

  const bool startup = sky.precipitation_time.y < 4.0;
  RainParticle particle = startup ? pf08_spawn_precipitation(id, snow, true) : previous_state.particles[id];
  if (particle.metadata.x < -0.5 || (particle.metadata.x > 0.5) != snow) {
    particle = pf08_spawn_precipitation(id, snow, true);
  }
  const float dt = min(max(sky.precipitation_time.x, 0.0), 0.05);

  if (particle.impact_position_age.w >= 0.0) {
    particle.impact_position_age.w += dt;
    if (particle.impact_position_age.w >= max(sky.precipitation_time.z, 0.01)) {
      particle = pf08_spawn_precipitation(id, snow, false);
    }
    current_state.particles[id] = particle;
    return;
  }

  bool alive = particle.position_age.w >= 0.0 &&
               particle.position_age.w < particle.velocity_lifetime.w;
  if (alive) {
    const vec3 previous_position = particle.position_age.xyz;
    vec3 step_velocity = particle.velocity_lifetime.xyz;
    if (snow) {
      const vec2 perpendicular = vec2(-sky.wind_params.y, sky.wind_params.x);
      const float flutter = sin(sky.wind_params.w * 2.1 + particle.metadata.y) * 0.42;
      step_velocity.xz += perpendicular * flutter;
    }
    const vec3 candidate_position = previous_position + step_velocity * dt;
    vec3 hit_position;
    const float thickness = max(snow ? 0.08 : 0.20, -step_velocity.y * dt * 1.25);
    const bool shelter_contact = pf08_shelter_segment_contact(
      sky, previous_position, candidate_position, hit_position);
    const bool scene_contact = !shelter_contact && sky.precipitation_time.w < 1.5 &&
      pf08_rain_scene_contact(previous_position, candidate_position, thickness, hit_position);
    if (shelter_contact || scene_contact) {
      particle = snow ? pf08_spawn_precipitation(id, snow, false) : particle;
      if (!snow) {
        particle.position_age.w = -1.0;
        particle.impact_position_age = vec4(hit_position, 0.0);
      }
      current_state.particles[id] = particle;
      return;
    }
    particle.position_age.xyz = candidate_position;
    particle.position_age.w += dt;
    const vec3 relative = candidate_position - camera_data.camera_position.xyz;
    const float radius = max(snow ? sky.snow_params.w : sky.precipitation_params.w, 1.0);
    alive = particle.position_age.w < particle.velocity_lifetime.w &&
            abs(relative.x) <= radius * 1.15 && abs(relative.z) <= radius * 1.15 &&
            relative.y >= -radius * 0.55;
  }
  if (!alive) particle = pf08_spawn_precipitation(id, snow, false);
  current_state.particles[id] = particle;
}
