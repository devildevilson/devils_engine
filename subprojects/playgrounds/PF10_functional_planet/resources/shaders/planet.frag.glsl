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
layout(set = 0, binding = 3, std430) readonly buffer PoliticalRegionTable { uint ids[]; } political_regions;

layout(location = 0) in vec3 in_local_direction;
layout(location = 1) in vec3 in_world_position;
layout(location = 2) in float in_height;
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
  const uint stable_id = political_regions.ids[nearest & 0xffffu];
  const uint high_bits = stable_id & 0xe0000000u;
  const uint kind = high_bits == 0xc0000000u ? 2u :
                    (high_bits == 0xa0000000u ? 3u : (high_bits == 0x80000000u ? 1u : 0u));
  const float edge = mix(edge0, edge1, blend.y);
  return pf10_region_sample(stable_id, kind, edge);
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

  vec3 normal = normalize(cross(dFdx(in_world_position), dFdy(in_world_position)));
  const vec3 radial = normalize((camera_data.planet_to_world * vec4(direction, 0.0)).xyz);
  if (dot(normal, radial) < 0.0) normal = -normal;
  const float direct = max(dot(normal, -camera_data.light_direction.xyz), 0.0);
  const float rim = pow(1.0 - max(dot(normal, normalize(camera_data.camera_position.xyz - in_world_position)), 0.0), 3.0);
  vec3 colour = albedo * (0.19 + direct * 0.86) + vec3(0.055, 0.085, 0.12) * rim;

  // The packed distance remains available for diagnostics and tiled consumers.  Authored boundary colour is
  // drawn by the shared-curve decal pass, avoiding atlas stair steps at close range.
  out_color = vec4(colour, 1.0);
}
