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
layout(set = 0, binding = 1, std430) readonly buffer SurfaceVertices { vec4 positions[]; } surface;
layout(set = 0, binding = 4, std430) readonly buffer SurfacePatches { uvec4 patches[]; } patch_data;

layout(location = 0) out vec3 out_local_direction;
layout(location = 1) out vec3 out_world_position;
layout(location = 2) out float out_height;
layout(location = 3) out vec3 out_world_normal;

vec3 cube_direction(const uint face, const vec2 uv) {
  if (face == 0u) return normalize(vec3(1.0, uv.y, -uv.x));
  if (face == 1u) return normalize(vec3(-1.0, uv.y, uv.x));
  if (face == 2u) return normalize(vec3(uv.x, 1.0, -uv.y));
  if (face == 3u) return normalize(vec3(uv.x, -1.0, uv.y));
  if (face == 4u) return normalize(vec3(uv.x, uv.y, 1.0));
  return normalize(vec3(-uv.x, uv.y, -1.0));
}

vec3 unpack_octahedral_normal(const float packed) {
  const vec2 encoded = unpackSnorm2x16(floatBitsToUint(packed));
  vec3 normal = vec3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
  const float fold = max(-normal.z, 0.0);
  normal.xy += vec2(normal.x >= 0.0 ? -fold : fold, normal.y >= 0.0 ? -fold : fold);
  return normalize(normal);
}

vec4 baked_vertex(const uint face, const uvec2 node, const uint side) {
  const uint stride = side + 1u;
  return surface.positions[face * stride * stride + node.y * stride + node.x];
}

void main() {
  const uint side = max(camera_data.params.z, 1u);
  const uint patch_side = 16u;
  const uint factor = 4u;
  const uint fine_side = patch_side * factor;
  const uint patch_index = uint(gl_InstanceIndex) / fine_side;
  const uint row = uint(gl_InstanceIndex) % fine_side;
  const uvec4 patch_record = patch_data.patches[patch_index];
  const uint strip_vertex = uint(gl_VertexIndex);
  const uvec2 fine_node = uvec2(strip_vertex / 2u, row + (strip_vertex & 1u));
  const vec2 coarse_offset = vec2(fine_node) / float(factor);
  const uvec2 coarse_low_offset = uvec2(floor(coarse_offset));
  const uvec2 coarse_high_offset = min(coarse_low_offset + 1u, uvec2(patch_side));
  const vec2 blend = fract(coarse_offset);
  const uvec2 origin = patch_record.yz;
  const vec4 v00 = baked_vertex(patch_record.x, origin + uvec2(coarse_low_offset.x, coarse_low_offset.y), side);
  const vec4 v10 = baked_vertex(patch_record.x, origin + uvec2(coarse_high_offset.x, coarse_low_offset.y), side);
  const vec4 v01 = baked_vertex(patch_record.x, origin + uvec2(coarse_low_offset.x, coarse_high_offset.y), side);
  const vec4 v11 = baked_vertex(patch_record.x, origin + uvec2(coarse_high_offset.x, coarse_high_offset.y), side);
  const vec3 coarse_position = mix(mix(v00.xyz, v10.xyz, blend.x), mix(v01.xyz, v11.xyz, blend.x), blend.y);

  const vec2 global_node = vec2(origin) + coarse_offset;
  const vec2 uv = global_node / float(side) * 2.0 - 1.0;
  const vec3 direction = cube_direction(patch_record.x, uv);
  // Refine spherical direction while interpolating the authoritative CPU-baked radius. This preserves
  // authored mountain ridges exactly and avoids re-running eleven noise samples for every transient vertex.
  const float detailed_radius = mix(mix(length(v00.xyz), length(v10.xyz), blend.x),
                                    mix(length(v01.xyz), length(v11.xyz), blend.x), blend.y);
  const vec3 detailed_position = direction * detailed_radius;
  const float edge_nodes = float(min(min(fine_node.x, fine_node.y),
                                     min(fine_side - fine_node.x, fine_side - fine_node.y)));
  // The outer coarse-cell ring converges to the original piecewise-linear boundary. Consequently the 4x
  // patch meets an unrefined neighbour exactly, without skirts, cracks or a second global vertex bake.
  const float detail_weight = smoothstep(0.0, float(factor), edge_nodes);
  const vec3 local_position = mix(coarse_position, detailed_position, detail_weight);

  const vec3 n00 = unpack_octahedral_normal(v00.w);
  const vec3 n10 = unpack_octahedral_normal(v10.w);
  const vec3 n01 = unpack_octahedral_normal(v01.w);
  const vec3 n11 = unpack_octahedral_normal(v11.w);
  const vec3 local_normal = normalize(mix(mix(n00, n10, blend.x), mix(n01, n11, blend.x), blend.y));
  const vec3 world_position = (camera_data.planet_to_world * vec4(local_position, 1.0)).xyz;
  gl_Position = camera_data.view_projection * vec4(world_position, 1.0);
  out_local_direction = normalize(local_position);
  out_world_position = world_position;
  out_height = length(local_position) - PF10_RADIUS;
  out_world_normal = normalize((camera_data.planet_to_world * vec4(local_normal, 0.0)).xyz);
}
