#version 450

// Алгоритм: фиксированный GPU pool разбит на независимые stable slots — один invocation читает прошлое
// состояние своего slot и пишет текущее, поэтому update не требует freelist/atomics. CPU emitter передаёт
// только непрерывный диапазон spawn serials; serial modulo capacity выбирает свободные slots и одновременно
// служит seed. Semi-implicit Euler применяет acceleration и drag, затем аналитически отражает частицу от
// пола и стен комнаты. Age >= lifetime освобождает slot; выключение emitter прекращает spawn, но живые
// частицы продолжают физику и естественно переводят emitter из draining в stopped.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Particle {
  vec4 position_age;
  vec4 velocity_lifetime;
};

layout(set = 0, binding = 0, std140) uniform EmitterBlock {
  vec4 origin_rate;          // xyz origin, particles/second
  vec4 direction_speed;      // xyz central direction, base speed
  vec4 acceleration_dt;      // xyz acceleration, timestep
  vec4 bounds_restitution;   // floor Y, half X, min Z, max Z
  vec4 lifetime_drag;        // min life, max life, drag, restitution
  uvec4 lifecycle;           // first spawn serial, count, capacity, reset
} emitter;

layout(set = 0, binding = 1, std430) writeonly buffer CurrentParticles {
  Particle particles[];
} current_state;

layout(set = 0, binding = 2, std430) readonly buffer PreviousParticles {
  Particle particles[];
} previous_state;

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

Particle spawn_particle(const uint serial) {
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

void main() {
  const uint id = gl_GlobalInvocationID.x;
  const uint capacity = emitter.lifecycle.z;
  if (id >= capacity) return;

  Particle particle;
  if (emitter.lifecycle.w != 0u) {
    particle.position_age = vec4(0.0, 0.0, 0.0, -1.0);
    particle.velocity_lifetime = vec4(0.0);
  } else {
    particle = previous_state.particles[id];
  }

  bool alive = particle.position_age.w >= 0.0 &&
               particle.position_age.w < particle.velocity_lifetime.w;
  if (alive) {
    const float dt = min(emitter.acceleration_dt.w, 0.05);
    vec3 position = particle.position_age.xyz;
    vec3 velocity = particle.velocity_lifetime.xyz;
    velocity += emitter.acceleration_dt.xyz * dt;
    velocity *= exp(-max(emitter.lifetime_drag.z, 0.0) * dt);
    position += velocity * dt;

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

    particle.position_age = vec4(position, particle.position_age.w + dt);
    particle.velocity_lifetime.xyz = velocity;
    alive = particle.position_age.w < particle.velocity_lifetime.w;
    if (!alive) particle.position_age.w = -1.0;
  }

  const uint first_slot = emitter.lifecycle.x % capacity;
  const uint distance_from_first = (id + capacity - first_slot) % capacity;
  const bool spawn_requested = distance_from_first < emitter.lifecycle.y;
  if (!alive && spawn_requested) {
    particle = spawn_particle(emitter.lifecycle.x + distance_from_first);
  }
  current_state.particles[id] = particle;
}
