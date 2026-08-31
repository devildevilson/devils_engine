#version 450
#include "pf10_planet.glsl"

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 planet_to_world;
  mat4 world_to_planet;
  vec4 camera_position;
  vec4 light_direction;
  vec4 border_colour;
  uvec4 params;
  vec4 viewport_near;
} camera_data;
layout(set = 0, binding = 2, std430) readonly buffer PoliticalAtlas { uint texels[]; } political_atlas;
struct PoliticalCell { vec4 feature; uvec4 metadata; };
layout(set = 0, binding = 3, std430) readonly buffer PoliticalRegionTable { PoliticalCell cells[]; } political_regions;

layout(location = 0) in vec3 in_local_direction;
layout(location = 1) in vec3 in_world_position;
layout(location = 2) in float in_height;
layout(location = 3) in vec3 in_world_normal;
layout(location = 0) out vec4 out_color;

vec3 province_colour(const uint id) {
  const uint value = pf10_mix32(id);
  const vec3 random = vec3(float((value >> 0u) & 255u), float((value >> 8u) & 255u),
                           float((value >> 16u) & 255u)) / 255.0;
  return mix(vec3(0.24, 0.31, 0.18), vec3(0.64, 0.52, 0.31), random * 0.72);
}

struct CubeCoordinate { uint face; vec2 uv; };

CubeCoordinate cube_coordinate(const vec3 direction) {
  const vec3 magnitude = abs(direction);
  if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
    if (direction.x >= 0.0) return CubeCoordinate(0u, vec2(-direction.z, direction.y) / magnitude.x);
    return CubeCoordinate(1u, vec2(direction.z, direction.y) / magnitude.x);
  }
  if (magnitude.y >= magnitude.z) {
    if (direction.y >= 0.0) return CubeCoordinate(2u, vec2(direction.x, -direction.z) / magnitude.y);
    return CubeCoordinate(3u, vec2(direction.x, direction.z) / magnitude.y);
  }
  if (direction.z >= 0.0) return CubeCoordinate(4u, vec2(direction.x, direction.y) / magnitude.z);
  return CubeCoordinate(5u, vec2(-direction.x, direction.y) / magnitude.z);
}

uint atlas_texel(const uint face, const uvec2 xy, const uint side) {
  const uint stride = side + 1u;
  return political_atlas.texels[face * stride * stride + xy.y * stride + xy.x];
}

float atlas_edge(const uint packed) {
  return float(packed >> 16u) * (0.04 / 65535.0);
}

pf10_region_sample sample_baked_region(const vec3 direction) {
  const uint side = uint(camera_data.viewport_near.w + 0.5);
  const CubeCoordinate cube = cube_coordinate(direction);
  const vec2 location = clamp((cube.uv * 0.5 + 0.5) * float(side), vec2(0.0), vec2(float(side)));
  const uvec2 nearest_xy = uvec2(location + 0.5);
  const uint nearest = atlas_texel(cube.face, nearest_xy, side);
  const uvec2 low = uvec2(floor(location));
  const uvec2 high = min(low + 1u, uvec2(side));
  const vec2 blend = fract(location);
  const float edge0 = mix(atlas_edge(atlas_texel(cube.face, uvec2(low.x, low.y), side)),
                          atlas_edge(atlas_texel(cube.face, uvec2(high.x, low.y), side)), blend.x);
  const float edge1 = mix(atlas_edge(atlas_texel(cube.face, uvec2(low.x, high.y), side)),
                          atlas_edge(atlas_texel(cube.face, uvec2(high.x, high.y), side)), blend.x);
  const PoliticalCell coarse_cell = political_regions.cells[nearest & 0xffffu];
  const uint stable_id = coarse_cell.metadata.x;
  const uint kind = coarse_cell.metadata.y;
  const float edge = mix(edge0, edge1, blend.y);
  pf10_region_sample coarse = pf10_region_sample(stable_id, kind, edge);
  // Exact cells matter at the province-inspection LOD. At empire LOD the same boundary is sub-pixel, so the
  // compact distance field is both visually sufficient and substantially cheaper over the whole globe.
  if (edge > 0.0075 || length(camera_data.camera_position.xyz) >= 1.72) return coarse;

  // Water and poles are analytic. Evaluating this only in the atlas' conservative border band keeps the
  // common interior path at one atlas/table load while making coast and polar ownership sub-pixel smooth.
  const float polar_edge = abs(abs(direction.y) - 0.91);
  if (abs(direction.y) >= 0.91) {
    return pf10_region_sample(0xc0000000u | (direction.y >= 0.0 ? 1u : 2u), 2u, polar_edge);
  }
  const vec4 oceans[4] = vec4[4](
    vec4(0.781, 0.120, 0.613, 0.675), vec4(-0.704, 0.151, 0.694, 0.715),
    vec4(0.050, -0.249, -0.967, 0.700), vec4(-0.252, 0.504, -0.826, 0.775));
  const float coast_noise = (pf10_value_noise(direction * 3.7 + vec3(17.0, 5.0, 29.0)) - 0.5) * 0.085;
  float best_ocean_score = -100.0;
  float second_ocean_score = -100.0;
  uint best_ocean = 0u;
  for (uint i = 0u; i < 4u; ++i) {
    const float score = dot(direction, oceans[i].xyz) - oceans[i].w + coast_noise;
    if (score > best_ocean_score) {
      second_ocean_score = best_ocean_score;
      best_ocean_score = score;
      best_ocean = i;
    } else if (score > second_ocean_score) second_ocean_score = score;
  }
  if (best_ocean_score > 0.0) {
    return pf10_region_sample(0x80000000u | (best_ocean + 1u), 1u,
                              min(best_ocean_score, best_ocean_score - second_ocean_score));
  }

  // The 1024 atlas identifies a tiny candidate set. Distances to those exact query-space features recover
  // the continuous spherical Voronoi boundary; no per-fragment 27-cell procedural search is needed.
  float best_distance = 1e30;
  float second_distance = 1e30;
  uint best_owner = 0xffffffffu;
  uint second_owner = 0xffffffffu;
  uint best_kind = 0u;
  uint seen_indices[9];
  uint seen_count = 0u;
  const ivec2 centre = ivec2(floor(location));
  for (int oy = -1; oy <= 1; ++oy) {
    for (int ox = -1; ox <= 1; ++ox) {
      const uvec2 xy = uvec2(clamp(centre + ivec2(ox, oy), ivec2(0), ivec2(side)));
      const uint local_index = atlas_texel(cube.face, xy, side) & 0xffffu;
      bool duplicate = false;
      for (uint previous = 0u; previous < seen_count; ++previous) duplicate = duplicate || seen_indices[previous] == local_index;
      if (duplicate) continue;
      seen_indices[seen_count++] = local_index;
      const PoliticalCell cell = political_regions.cells[local_index];
      if (cell.feature.w < 0.5) continue;
      const vec3 delta = direction * PF10_PROVINCE_FREQUENCY - cell.feature.xyz;
      const float distance_value = dot(delta, delta);
      const uint owner = cell.metadata.x;
      if (owner == best_owner) {
        best_distance = min(best_distance, distance_value);
      } else if (distance_value < best_distance) {
        if (best_owner != second_owner) {
          second_distance = best_distance;
          second_owner = best_owner;
        }
        best_distance = distance_value;
        best_owner = owner;
        best_kind = cell.metadata.y;
      } else if (owner == second_owner) {
        second_distance = min(second_distance, distance_value);
      } else if (distance_value < second_distance) {
        second_distance = distance_value;
        second_owner = owner;
      }
    }
  }
  if (best_owner == 0xffffffffu) return coarse;
  const float land_edge = min(-best_ocean_score, polar_edge);
  const float voronoi_edge = second_owner == 0xffffffffu ? 0.04 :
                              max(sqrt(second_distance) - sqrt(best_distance), 0.0) /
                                PF10_PROVINCE_FREQUENCY;
  return pf10_region_sample(best_owner, best_kind, min(voronoi_edge, land_edge));
}

void main() {
  const vec3 direction = normalize(in_local_direction);
  const pf10_region_sample region = sample_baked_region(direction);
  const bool political = (camera_data.params.w & 1u) != 0u;
  vec3 albedo;
  if (region.kind == 2u) {
    albedo = direction.y > 0.0 ? vec3(0.76, 0.82, 0.86) : vec3(0.66, 0.73, 0.80);
  } else if (region.kind == 1u) {
    const float ocean_variant = float(pf10_mix32(region.id) & 255u) / 255.0;
    albedo = mix(vec3(0.035, 0.15, 0.25), vec3(0.055, 0.31, 0.42), ocean_variant);
  } else if (region.kind == 3u) {
    const float crag = pf10_value_noise(direction * 64.0 + vec3(19.0, 7.0, 43.0));
    albedo = mix(vec3(0.20, 0.19, 0.17), vec3(0.48, 0.46, 0.41), crag);
  } else if (political) {
    albedo = province_colour(region.id);
  } else {
    const float elevation = clamp((in_height - PF10_MIN_HEIGHT) / (PF10_MAX_HEIGHT - PF10_MIN_HEIGHT), 0.0, 1.0);
    albedo = mix(vec3(0.16, 0.26, 0.12), vec3(0.54, 0.45, 0.31), elevation);
  }

  if (region.id == camera_data.params.x) albedo = mix(albedo, vec3(1.0, 0.68, 0.12), 0.62);
  else if (region.id == camera_data.params.y) albedo = mix(albedo, vec3(0.95), 0.30);

  vec3 normal = normalize(in_world_normal);
  const vec3 radial = normalize((camera_data.planet_to_world * vec4(direction, 0.0)).xyz);
  if (dot(normal, radial) < 0.0) normal = -normal;
  const float direct = max(dot(normal, -camera_data.light_direction.xyz), 0.0);
  const float rim = pow(1.0 - max(dot(normal, normalize(camera_data.camera_position.xyz - in_world_position)), 0.0), 3.0);
  vec3 colour = albedo * (0.19 + direct * 0.86) + vec3(0.055, 0.085, 0.12) * rim;

  if (political) {
    const float line_half_width = 0.00042;
    // Cap the derivative: the exact/coarse handover intentionally changes the auxiliary distance value,
    // and an unbounded fwidth would reveal that invisible handover as a faint dotted contour.
    const float aa = clamp(fwidth(region.edge), 0.000035, 0.00030);
    const float border = 1.0 - smoothstep(line_half_width - aa, line_half_width + aa, region.edge);
    colour = mix(colour, camera_data.border_colour.rgb, border * camera_data.border_colour.a);
  }
  out_color = vec4(colour, 1.0);
}
