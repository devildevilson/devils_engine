#version 450

// Алгоритм: fixed GPU pool разделён на stable spark slots и camera-local weather slots. Каждый invocation
// читает прошлое состояние своего slot и пишет текущее, поэтому update не требует freelist/atomics. После
// semi-implicit Euler отрезок previous→candidate проецируется в текущий opaque depth; переход с camera-side
// на surface-side означает столкновение. World hit и normal восстанавливаются из depth/normal: spark отражает
// velocity, rain/snow завершаются и переиспользуют slot. Явный shelter AABB отбрасывает precipitation даже когда
// крыша вне экрана: это дешёвый gameplay volume, дополняющий screen collision, которое знает только первую видимую
// поверхность и намеренно ничего не обещает вне viewport или за occluder.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Particle {
  vec4 position_age;
  vec4 velocity_lifetime;
};

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  mat4 inverse_view_projection;
  vec4 effect_params;
} camera_data;

layout(set = 1, binding = 0, std140) uniform EmitterBlock {
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

layout(set = 1, binding = 1, std430) writeonly buffer CurrentParticles {
  Particle particles[];
} current_state;

layout(set = 1, binding = 2, std430) readonly buffer PreviousParticles {
  Particle particles[];
} previous_state;

layout(set = 1, binding = 3) uniform sampler2D scene_depth_texture;
layout(set = 1, binding = 4) uniform sampler2D scene_normal_texture;

const uint weather_clear = 0u;
const uint weather_rain = 1u;
const uint weather_snow = 2u;

uint hash_u32(uint value) {
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  return value ^ (value >> 16u);
}

float random01(const uint seed) {
  return float(hash_u32(seed) & 0x00ffffffu) / float(0x01000000u);
}

Particle dead_particle() {
  Particle particle;
  particle.position_age = vec4(0.0, 0.0, 0.0, -1.0);
  particle.velocity_lifetime = vec4(0.0);
  return particle;
}

bool inside_weather_shelter(const vec3 position) {
  if (emitter.shelter_min_enabled.w < 0.5) return false;
  return all(greaterThanEqual(position, emitter.shelter_min_enabled.xyz)) &&
         all(lessThanEqual(position, emitter.shelter_max.xyz));
}

Particle spawn_spark(const uint serial) {
  const float angle = random01(serial * 4u + 0u) * 6.28318530718;
  const float radial = sqrt(random01(serial * 4u + 1u)) * 0.52;
  const float speed = emitter.direction_speed.w * mix(0.72, 1.28, random01(serial * 4u + 2u));
  const vec3 direction = normalize(emitter.direction_speed.xyz);
  const vec3 helper = abs(direction.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  const vec3 tangent = normalize(cross(helper, direction));
  const vec3 bitangent = cross(direction, tangent);
  const vec3 cone_direction = normalize(
    direction + radial * (cos(angle) * tangent + sin(angle) * bitangent));
  const float lifetime = mix(
    emitter.lifetime_drag.x, emitter.lifetime_drag.y, random01(serial * 4u + 3u));
  Particle particle;
  particle.position_age = vec4(emitter.origin_rate.xyz, 0.0);
  particle.velocity_lifetime = vec4(cone_direction * speed, lifetime);
  return particle;
}

Particle spawn_weather(const uint local_id, const bool scatter_initially) {
  const uint seed = hash_u32(local_id ^ (emitter.weather.w * 0x9e3779b9u));
  const float x = mix(-5.6, 5.6, random01(seed + 0u));
  const float z = mix(-5.6, 5.6, random01(seed + 1u));
  const float top = camera_data.camera_position.y + 4.8;
  Particle particle;
  if (emitter.weather.x == weather_rain) {
    const float speed = mix(8.5, 12.0, random01(seed + 2u));
    const float life = 1.25;
    const float age = scatter_initially ? random01(seed + 3u) * 0.92 : 0.0;
    const vec3 velocity = vec3(0.72, -speed, 0.22);
    particle.position_age = vec4(
      camera_data.camera_position.xyz + vec3(x, 4.8, z) + velocity * age,
      age);
    particle.velocity_lifetime = vec4(velocity, life);
  } else {
    const float life = 9.0;
    const float age = scatter_initially ? random01(seed + 3u) * 6.2 : 0.0;
    const vec3 velocity = vec3(
      mix(-0.22, 0.22, random01(seed + 4u)),
      -mix(0.58, 1.05, random01(seed + 2u)),
      mix(-0.16, 0.28, random01(seed + 5u)));
    particle.position_age = vec4(
      vec3(camera_data.camera_position.x + x, top, camera_data.camera_position.z + z) + velocity * age,
      age);
    particle.velocity_lifetime = vec4(velocity, life);
  }
  return particle;
}

bool screen_collision(
  const vec3 previous_position,
  const vec3 candidate_position,
  const vec3 velocity,
  const float thickness,
  out vec3 hit_position,
  out vec3 hit_normal) {
  if (camera_data.effect_params.y < 0.5) return false;
  const vec4 candidate_clip = camera_data.view_projection * vec4(candidate_position, 1.0);
  if (candidate_clip.w <= 0.0) return false;
  const vec3 candidate_ndc = candidate_clip.xyz / candidate_clip.w;
  const vec2 uv = candidate_ndc.xy * 0.5 + 0.5;
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) return false;

  const float scene_depth = texture(scene_depth_texture, uv).r;
  if (scene_depth <= 0.0) return false;
  const vec4 surface_h = camera_data.inverse_view_projection * vec4(uv * 2.0 - 1.0, scene_depth, 1.0);
  hit_position = surface_h.xyz / surface_h.w;
  hit_normal = normalize(texture(scene_normal_texture, uv).xyz * 2.0 - 1.0);
  if (dot(velocity, hit_normal) >= -0.01) return false;

  const float previous_view_depth = -(camera_data.view * vec4(previous_position, 1.0)).z;
  const float candidate_view_depth = -(camera_data.view * vec4(candidate_position, 1.0)).z;
  const float surface_view_depth = -(camera_data.view * vec4(hit_position, 1.0)).z;
  const bool started_on_camera_side = previous_view_depth <= surface_view_depth + thickness * 0.35;
  const bool reached_surface = candidate_view_depth >= surface_view_depth - thickness;
  const bool stayed_near_surface = candidate_view_depth <= surface_view_depth + thickness;
  return started_on_camera_side && reached_surface && stayed_near_surface;
}

void main() {
  const uint id = gl_GlobalInvocationID.x;
  if (id >= emitter.weather.z) return;
  const bool is_weather = id >= emitter.lifecycle.z;
  const bool reset = is_weather ? emitter.weather.y != 0u : emitter.lifecycle.w != 0u;
  Particle particle = reset ? dead_particle() : previous_state.particles[id];
  bool alive = particle.position_age.w >= 0.0 &&
               particle.position_age.w < particle.velocity_lifetime.w;

  if (is_weather && emitter.weather.x == weather_clear) {
    current_state.particles[id] = dead_particle();
    return;
  }

  if (alive) {
    const float dt = min(emitter.acceleration_dt.w, 0.05);
    const vec3 previous_position = particle.position_age.xyz;
    vec3 position = previous_position;
    vec3 velocity = particle.velocity_lifetime.xyz;
    if (!is_weather) {
      velocity += emitter.acceleration_dt.xyz * dt;
      velocity *= exp(-max(emitter.lifetime_drag.z, 0.0) * dt);
    } else if (emitter.weather.x == weather_rain) {
      velocity += vec3(0.18, -0.85, 0.06) * dt;
    } else {
      const float phase = float(emitter.weather.w) * 0.035 + float(id) * 0.71;
      velocity.xz += vec2(sin(phase), cos(phase * 0.83)) * (0.32 * dt);
      velocity *= exp(-0.12 * dt);
    }
    position += velocity * dt;

    if (is_weather && inside_weather_shelter(position)) alive = false;

    if (!is_weather) {
      const float restitution = clamp(emitter.lifetime_drag.w, 0.0, 1.0);
      if (position.y < emitter.bounds_restitution.x) {
        position.y = emitter.bounds_restitution.x;
        if (velocity.y < 0.0) velocity.y = -velocity.y * restitution;
        velocity.xz *= 0.82;
      }
      if (abs(position.x) > emitter.bounds_restitution.y) {
        position.x = clamp(position.x, -emitter.bounds_restitution.y, emitter.bounds_restitution.y);
        velocity.x = -velocity.x * restitution;
      }
      if (position.z < emitter.bounds_restitution.z || position.z > emitter.bounds_restitution.w) {
        position.z = clamp(position.z, emitter.bounds_restitution.z, emitter.bounds_restitution.w);
        velocity.z = -velocity.z * restitution;
      }
    }

    vec3 hit_position;
    vec3 hit_normal;
    const float collision_thickness = is_weather && emitter.weather.x == weather_rain ? 0.30 : 0.12;
    if (alive && screen_collision(
          previous_position, position, velocity, collision_thickness, hit_position, hit_normal)) {
      if (is_weather) {
        alive = false;
      } else {
        position = hit_position + hit_normal * 0.055;
        velocity -= (1.0 + emitter.lifetime_drag.w) * dot(velocity, hit_normal) * hit_normal;
      }
    }

    particle.position_age = vec4(position, particle.position_age.w + dt);
    particle.velocity_lifetime.xyz = velocity;
    alive = alive && particle.position_age.w < particle.velocity_lifetime.w;
    if (is_weather) {
      const vec3 relative = position - camera_data.camera_position.xyz;
      alive = alive && abs(relative.x) <= 6.4 && abs(relative.z) <= 6.4 && relative.y >= -3.5;
    }
    if (!alive) particle.position_age.w = -1.0;
  }

  if (is_weather) {
    if (!alive) {
      particle = spawn_weather(id - emitter.lifecycle.z, reset);
      // A reset may scatter a new particle through its lifetime. Reject it before the render pass too,
      // otherwise an interior spawn would flash for one frame before the next simulation update kills it.
      if (inside_weather_shelter(particle.position_age.xyz)) particle = dead_particle();
    }
  } else {
    const uint first_slot = emitter.lifecycle.x % emitter.lifecycle.z;
    const uint distance_from_first = (id + emitter.lifecycle.z - first_slot) % emitter.lifecycle.z;
    const bool spawn_requested = distance_from_first < emitter.lifecycle.y;
    if (!alive && spawn_requested) {
      particle = spawn_spark(emitter.lifecycle.x + distance_from_first);
    }
  }
  current_state.particles[id] = particle;
}
