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

const uint PF10_NO_REGION = 0xffffffffu;
const float PF10_NO_BORDER = 1.0e12;

struct PoliticalSample {
  uint id;
  uint kind;
  uint state;
  uint neighbour_state;
  uint neighbour_kind;
  uint exact;
  float province_field;
  float coast_field;
  float polar_field;
  float coarse_edge;
};

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

vec3 cube_direction(const uint face, const vec2 uv) {
  if (face == 0u) return normalize(vec3(1.0, uv.y, -uv.x));
  if (face == 1u) return normalize(vec3(-1.0, uv.y, uv.x));
  if (face == 2u) return normalize(vec3(uv.x, 1.0, -uv.y));
  if (face == 3u) return normalize(vec3(uv.x, -1.0, uv.y));
  if (face == 4u) return normalize(vec3(uv.x, uv.y, 1.0));
  return normalize(vec3(-uv.x, uv.y, -1.0));
}

uint atlas_texel(const uint face, const uvec2 xy, const uint side) {
  const uint stride = side + 1u;
  return political_atlas.texels[face * stride * stride + xy.y * stride + xy.x];
}

// A clamped 3x3 stencil loses the province just across a cube seam. Remapping an out-of-face node through
// the sphere preserves the same tiny candidate budget while making the exact Voronoi pair seam-independent.
uint atlas_neighbour_texel(const uint face, const ivec2 xy, const uint side) {
  if (all(greaterThanEqual(xy, ivec2(0))) && all(lessThanEqual(xy, ivec2(side)))) {
    return atlas_texel(face, uvec2(xy), side);
  }
  const vec2 extended_uv = vec2(xy) / float(side) * 2.0 - 1.0;
  const CubeCoordinate mapped = cube_coordinate(cube_direction(face, extended_uv));
  const vec2 mapped_location = clamp((mapped.uv * 0.5 + 0.5) * float(side), vec2(0.0), vec2(float(side)));
  return atlas_texel(mapped.face, uvec2(mapped_location + 0.5), side);
}

float atlas_edge(const uint packed) {
  return float(packed >> 16u) * (0.04 / 65535.0);
}

PoliticalSample sample_baked_region(const vec3 direction) {
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
  const uint state = coarse_cell.metadata.z;
  const float edge = mix(edge0, edge1, blend.y);
  PoliticalSample coarse = PoliticalSample(stable_id, kind, state, PF10_NO_REGION, PF10_NO_REGION, 0u,
                                            PF10_NO_BORDER, PF10_NO_BORDER, PF10_NO_BORDER, edge);
  // Exact cells matter at the province-inspection LOD. At empire LOD the same boundary is sub-pixel, so the
  // compact distance field is both visually sufficient and substantially cheaper over the whole globe.
  if (edge > 0.0048 || length(camera_data.camera_position.xyz) >= 1.72) return coarse;

  // Water and poles are analytic. Evaluating this only in the atlas' conservative border band keeps the
  // common interior path at one atlas/table load while making coast and polar ownership sub-pixel smooth.
  const float polar_edge = abs(abs(direction.y) - 0.91);
  if (abs(direction.y) >= 0.91) {
    return PoliticalSample(0xc0000000u | (direction.y >= 0.0 ? 1u : 2u), 2u, PF10_NO_REGION,
                           PF10_NO_REGION, PF10_NO_REGION, 1u, PF10_NO_BORDER, PF10_NO_BORDER,
                           abs(direction.y) - 0.91, edge);
  }
  const vec4 oceans[4] = vec4[4](
    vec4(0.781, 0.120, 0.613, 0.675), vec4(-0.704, 0.151, 0.694, 0.715),
    vec4(0.050, -0.249, -0.967, 0.700), vec4(-0.252, 0.504, -0.826, 0.775));
  const float coast_noise = (pf10_value_noise(direction * 3.7 + vec3(17.0, 5.0, 29.0)) - 0.5) * 0.085;
  float best_ocean_score = -100.0;
  float second_ocean_score = -100.0;
  uint best_ocean = 0u;
  uint second_ocean = 0u;
  for (uint i = 0u; i < 4u; ++i) {
    const float score = dot(direction, oceans[i].xyz) - oceans[i].w + coast_noise;
    if (score > best_ocean_score) {
      second_ocean_score = best_ocean_score;
      second_ocean = best_ocean;
      best_ocean_score = score;
      best_ocean = i;
    } else if (score > second_ocean_score) {
      second_ocean_score = score;
      second_ocean = i;
    }
  }
  if (best_ocean_score > 0.0) {
    const float ocean_pair = best_ocean_score - second_ocean_score;
    const float signed_ocean_pair = best_ocean < second_ocean ? ocean_pair : -ocean_pair;
    return PoliticalSample(0x80000000u | (best_ocean + 1u), 1u, PF10_NO_REGION, PF10_NO_REGION, 1u, 1u,
                           signed_ocean_pair, best_ocean_score, abs(direction.y) - 0.91, edge);
  }

  // The 1024 atlas identifies a tiny candidate set. Distances to those exact query-space features recover
  // the continuous spherical Voronoi boundary; no per-fragment 27-cell procedural search is needed.
  float best_distance = 1e30;
  float second_distance = 1e30;
  uint best_owner = 0xffffffffu;
  uint second_owner = 0xffffffffu;
  uint best_kind = 0u;
  uint second_kind = PF10_NO_REGION;
  uint best_state = PF10_NO_REGION;
  uint second_state = PF10_NO_REGION;
  vec3 best_feature = vec3(0.0);
  vec3 second_feature = vec3(0.0);
  uint seen_indices[9];
  uint seen_count = 0u;
  const ivec2 centre = ivec2(floor(location));
  const ivec2 candidate_offsets[9] = ivec2[9](
    ivec2(0, 0), ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1),
    ivec2(-1, -1), ivec2(1, -1), ivec2(-1, 1), ivec2(1, 1));
  for (uint candidate = 0u; candidate < 9u; ++candidate) {
    const uint local_index = atlas_neighbour_texel(cube.face, centre + candidate_offsets[candidate], side) &
                             0xffffu;
    bool duplicate = false;
    for (uint previous = 0u; previous < seen_count; ++previous) {
      duplicate = duplicate || seen_indices[previous] == local_index;
    }
    if (duplicate) continue;
    seen_indices[seen_count++] = local_index;
    const PoliticalCell cell = political_regions.cells[local_index];
    if (cell.feature.w < 0.5) continue;
    const vec3 delta = direction * PF10_PROVINCE_FREQUENCY - cell.feature.xyz;
    const float distance_value = dot(delta, delta);
    const uint owner = cell.metadata.x;
    if (owner == best_owner) {
      if (distance_value < best_distance) {
        best_distance = distance_value;
        best_feature = cell.feature.xyz;
      }
    } else if (distance_value < best_distance) {
      if (best_owner != second_owner) {
        second_distance = best_distance;
        second_owner = best_owner;
        second_kind = best_kind;
        second_state = best_state;
        second_feature = best_feature;
      }
      best_distance = distance_value;
      best_owner = owner;
      best_kind = cell.metadata.y;
      best_state = cell.metadata.z;
      best_feature = cell.feature.xyz;
    } else if (owner == second_owner) {
      if (distance_value < second_distance) {
        second_distance = distance_value;
        second_feature = cell.feature.xyz;
      }
    } else if (distance_value < second_distance) {
      second_distance = distance_value;
      second_owner = owner;
      second_kind = cell.metadata.y;
      second_state = cell.metadata.z;
      second_feature = cell.feature.xyz;
    }
  }
  if (best_owner == 0xffffffffu) return coarse;
  // Squared-distance difference is an exact plane equation for a Voronoi bisector. Canonical owner order
  // supplies a continuous sign across the ownership switch; dividing by fwidth later converts it to pixels.
  const float distance_difference = second_owner == PF10_NO_REGION ? PF10_NO_BORDER :
                                      max(second_distance - best_distance, 0.0);
  const vec3 plane_gradient = 2.0 * PF10_PROVINCE_FREQUENCY * (best_feature - second_feature);
  const float pixel_gradient = abs(dot(plane_gradient, dFdx(direction))) +
                               abs(dot(plane_gradient, dFdy(direction)));
  const float province_pixel_distance = distance_difference / max(pixel_gradient, 1.0e-7);
  return PoliticalSample(best_owner, best_kind, best_state, second_state, second_kind, 1u,
                         province_pixel_distance, best_ocean_score, abs(direction.y) - 0.91, edge);
}

float field_distance_pixels(const float field) {
  return abs(field) / max(fwidth(field), 1.0e-7);
}

void main() {
  const vec3 direction = normalize(in_local_direction);
  const PoliticalSample region = sample_baked_region(direction);
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
    const bool exact_voronoi_metric = region.exact != 0u && (region.kind == 0u || region.kind == 3u);
    const float province_pixels = exact_voronoi_metric ? region.province_field :
                                                        field_distance_pixels(region.province_field);
    const float coast_pixels = field_distance_pixels(region.coast_field);
    const float polar_pixels = field_distance_pixels(region.polar_field);
    // The thin province contour is the coverage fallback for every playable adjacency. State ribbons are
    // drawn later and cover it; suppressing it here would leave a hole whenever a short state trail is culled.
    const bool ordinary_province = region.exact != 0u && region.neighbour_kind != PF10_NO_REGION;
    const float province_lod = 1.0 - smoothstep(1.42, 1.72, length(camera_data.camera_position.xyz));
    // Keep derivative AA well inside the conservative 0.0048-radian refinement band. At its outer edge the
    // exact implicit field is intentionally replaced by a coarse sentinel; letting a quad straddle that
    // implementation handover would turn the huge derivative into a false dotted inset contour.
    const float refinement_guard = 1.0 - smoothstep(0.0034, 0.0043, region.coarse_edge);
    const float province_border = ordinary_province ?
      (1.0 - smoothstep(0.68, 1.18, province_pixels)) * province_lod * refinement_guard : 0.0;
    const float analytic_pixels = min(coast_pixels, polar_pixels);
    const float analytic_border = region.exact != 0u ?
      (1.0 - smoothstep(0.92, 1.42, analytic_pixels)) * refinement_guard : 0.0;
    const float border = max(province_border, analytic_border);
    colour = mix(colour, camera_data.border_colour.rgb, border * camera_data.border_colour.a);

    const uint border_debug = (camera_data.params.w >> 8u) & 3u;
    if (border_debug == 1u) {
      const bool near_coarse_boundary = region.exact == 0u && region.coarse_edge < 0.0048;
      colour = region.exact != 0u ? mix(colour, vec3(0.05, 0.85, 0.20), 0.72) :
               (near_coarse_boundary ? vec3(0.95, 0.08, 0.16) : colour * 0.28);
    } else if (border_debug == 2u && region.exact != 0u) {
      const float pixels = min(province_pixels, analytic_pixels);
      colour = pixels < 0.5 ? vec3(0.95, 0.12, 0.08) :
               (pixels < 1.5 ? vec3(0.10, 0.88, 0.18) : vec3(0.08, 0.24, 0.90));
    } else if (border_debug == 3u && region.state != PF10_NO_REGION) {
      colour = province_colour(0x51a7e000u + region.state * 0x13579bdu);
    }
  }
  out_color = vec4(colour, 1.0);
}
