#ifndef PF10_PLANET_GLSL
#define PF10_PLANET_GLSL

const float PF10_RADIUS = 1.0;
const float PF10_MIN_HEIGHT = -0.045;
const float PF10_MAX_HEIGHT = 0.085;
const float PF10_PROVINCE_FREQUENCY = 25.0;

uint pf10_mix32(uint value) {
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  value ^= value >> 16u;
  return value;
}

uint pf10_hash_cell(const ivec3 cell) {
  uint value = uint(cell.x) * 0x8da6b343u;
  value ^= uint(cell.y) * 0xd8163841u;
  value ^= uint(cell.z) * 0xcb1ab31fu;
  return pf10_mix32(value);
}

float pf10_hash01(const ivec3 cell, const uint salt) {
  return float((pf10_mix32(pf10_hash_cell(cell) ^ salt) >> 8u) & 0x00ffffffu) * (1.0 / 16777216.0);
}

float pf10_value_noise(const vec3 point) {
  const ivec3 base = ivec3(floor(point));
  vec3 blend = fract(point);
  blend = blend * blend * (3.0 - 2.0 * blend);
  float values[8];
  for (int z = 0; z < 2; ++z) {
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) {
        values[x + y * 2 + z * 4] = pf10_hash01(base + ivec3(x, y, z), 0x68bc21ebu);
      }
    }
  }
  const float x00 = mix(values[0], values[1], blend.x);
  const float x10 = mix(values[2], values[3], blend.x);
  const float x01 = mix(values[4], values[5], blend.x);
  const float x11 = mix(values[6], values[7], blend.x);
  return mix(mix(x00, x10, blend.y), mix(x01, x11, blend.y), blend.z);
}

float pf10_fractal_noise(vec3 point) {
  float value = 0.0;
  float weight = 0.5333333;
  for (uint octave = 0u; octave < 4u; ++octave) {
    value += pf10_value_noise(point) * weight;
    point = point * 2.031 + vec3(7.13, 3.71, 11.17);
    weight *= 0.5;
  }
  return value;
}

float pf10_surface_height(vec3 direction) {
  direction = normalize(direction);
  const float continent = pf10_fractal_noise(direction * 1.60 + vec3(3.0, 9.0, 1.0));
  const float folds = pf10_fractal_noise(direction * 4.20 + vec3(31.0, 7.0, 19.0));
  const float detail = pf10_value_noise(direction * 18.0 + vec3(13.0, 37.0, 5.0)) - 0.5;
  const float ridge = 1.0 - abs(folds * 2.0 - 1.0);
  const float mountain_mask = smoothstep(0.43, 0.61, continent);
  float height = (continent - 0.50) * 0.11 + pow(ridge, 4.0) * 0.032 * mountain_mask + detail * 0.008 - 0.006;

  const vec4 oceans[4] = vec4[4](
    vec4(0.781, 0.120, 0.613, 0.675), vec4(-0.704, 0.151, 0.694, 0.715),
    vec4(0.050, -0.249, -0.967, 0.700), vec4(-0.252, 0.504, -0.826, 0.775));
  const float coast_noise = (pf10_value_noise(direction * 3.7 + vec3(17.0, 5.0, 29.0)) - 0.5) * 0.085;
  float best_ocean = -100.0;
  for (uint i = 0u; i < 4u; ++i) {
    best_ocean = max(best_ocean, dot(direction, oceans[i].xyz) - oceans[i].w + coast_noise);
  }
  if (abs(direction.y) < 0.91) {
    const float coast_blend = smoothstep(-0.055, 0.025, best_ocean);
    height = mix(height, -0.018 + detail * 0.0015, coast_blend);
  }
  return clamp(height, PF10_MIN_HEIGHT, PF10_MAX_HEIGHT);
}

struct pf10_region_sample {
  uint id;
  uint kind; // 0 land, 1 water, 2 polar
  float edge;
};

pf10_region_sample pf10_sample_region(vec3 direction) {
  direction = normalize(direction);
  const float polar_edge = abs(abs(direction.y) - 0.91);
  if (abs(direction.y) >= 0.91) {
    return pf10_region_sample(0xc0000000u | (direction.y >= 0.0 ? 1u : 2u), 2u, polar_edge);
  }

  const vec4 oceans[4] = vec4[4](
    vec4(0.781, 0.120, 0.613, 0.675), vec4(-0.704, 0.151, 0.694, 0.715),
    vec4(0.050, -0.249, -0.967, 0.700), vec4(-0.252, 0.504, -0.826, 0.775));
  const float coast_noise = (pf10_value_noise(direction * 3.7 + vec3(17.0, 5.0, 29.0)) - 0.5) * 0.085;
  float best = -100.0;
  float second_water = -100.0;
  uint best_ocean = 0u;
  for (uint i = 0u; i < 4u; ++i) {
    const float score = dot(direction, oceans[i].xyz) - oceans[i].w + coast_noise;
    if (score > best) {
      second_water = best;
      best = score;
      best_ocean = i;
    } else if (score > second_water) {
      second_water = score;
    }
  }
  if (best > 0.0) {
    return pf10_region_sample(0x80000000u | (best_ocean + 1u), 1u, min(best, best - second_water));
  }

  const vec3 query = direction * PF10_PROVINCE_FREQUENCY;
  const ivec3 base = ivec3(floor(query));
  float nearest = 1e30;
  float second = 1e30;
  uint nearest_id = 0xffffffffu;
  for (int z = -1; z <= 1; ++z) {
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        const ivec3 cell = base + ivec3(x, y, z);
        const vec3 feature = vec3(cell) + vec3(pf10_hash01(cell, 0xa511e9b3u),
                                               pf10_hash01(cell, 0x63d83595u),
                                               pf10_hash01(cell, 0xb5297a4du));
        const vec3 delta = query - feature;
        const float distance_value = dot(delta, delta);
        if (distance_value < nearest) {
          second = nearest;
          nearest = distance_value;
          nearest_id = (pf10_hash_cell(cell) & 0x3fffffffu) | 1u;
        } else if (distance_value < second) {
          second = distance_value;
        }
      }
    }
  }
  const float edge = min(sqrt(second) - sqrt(nearest), min(-best, polar_edge) * PF10_PROVINCE_FREQUENCY);
  return pf10_region_sample(nearest_id, 0u, edge);
}

#endif
