#version 450

#ifndef PF01_TILE_SIZE
#define PF01_TILE_SIZE 16
#endif
#ifndef PF01_MAX_LIGHTS_PER_TILE
#define PF01_MAX_LIGHTS_PER_TILE 64
#endif
#ifndef PF01_TILES_X
#define PF01_TILES_X 120
#endif
#ifndef PF01_TILES_Y
#define PF01_TILES_Y 68
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data[3];

layout(set = 0, binding = 1, std430) readonly buffer LightBuffer {
  vec4 words[];
} light_data[3];

layout(set = 1, binding = 0) uniform sampler2D depth_image[3];

layout(set = 2, binding = 0, std430) buffer TileBuffer {
  uint words[];
} tile_data[3];

shared uint nearest_depth_bits;
shared uint farthest_depth_bits;
shared uint geometry_count;
shared uint accepted_flags[128];

float view_depth(float reverse_depth) {
  return camera_data[0].viewport_near.z / max(reverse_depth, 0.000001);
}

bool sphere_intersects_plane(vec3 center, float radius, vec3 inward_normal) {
  return dot(inward_normal, center) >= -radius * length(inward_normal);
}

bool overlaps_tile(
  vec3 view_position,
  float radius,
  float center_depth,
  vec2 viewport,
  vec2 rect_min,
  vec2 rect_max) {
  // A sphere crossing the camera/near plane has no finite screen-space bound. Keeping it for every
  // tile is conservative; the depth interval still rejects surfaces outside its radius.
  if (center_depth <= radius + camera_data[0].viewport_near.z) {
    return true;
  }

  const vec2 ndc_min = rect_min / viewport * 2.0 - 1.0;
  const vec2 ndc_max = rect_max / viewport * 2.0 - 1.0;
  const float projection_y_magnitude = camera_data[0].viewport_near.w;
  const float projection_x = projection_y_magnitude * viewport.y / viewport.x;
  const float projection_y = -projection_y_magnitude;
  const vec3 left_plane = vec3(projection_x, 0.0, ndc_min.x);
  const vec3 right_plane = vec3(-projection_x, 0.0, -ndc_max.x);
  const vec3 lower_plane = vec3(0.0, projection_y, ndc_min.y);
  const vec3 upper_plane = vec3(0.0, -projection_y, -ndc_max.y);
  return sphere_intersects_plane(view_position, radius, left_plane) &&
         sphere_intersects_plane(view_position, radius, right_plane) &&
         sphere_intersects_plane(view_position, radius, lower_plane) &&
         sphere_intersects_plane(view_position, radius, upper_plane);
}

void main() {
  const uvec2 tile = gl_WorkGroupID.xy;
  if (tile.x >= PF01_TILES_X || tile.y >= PF01_TILES_Y) return;
  const uvec2 viewport = uvec2(camera_data[0].viewport_near.xy);
  const uvec2 tile_min = tile * PF01_TILE_SIZE;
  if (tile_min.x >= viewport.x || tile_min.y >= viewport.y) {
    return;
  }
  const uvec2 tile_max = min(tile_min + PF01_TILE_SIZE, viewport);
  const uint tile_index = tile.y * PF01_TILES_X + tile.x;
  const uint list_base = tile_index * (PF01_MAX_LIGHTS_PER_TILE + 1);

  const uint local_index = gl_LocalInvocationIndex;
  if (local_index == 0u) {
    nearest_depth_bits = floatBitsToUint(1e30);
    farthest_depth_bits = floatBitsToUint(0.0);
    geometry_count = 0u;
    tile_data[0].words[list_base] = 0u;
  }
  for (uint i = local_index; i < 128u; i += gl_WorkGroupSize.x * gl_WorkGroupSize.y) {
    accepted_flags[i] = 0u;
  }
  barrier();

  for (uint y = tile_min.y + gl_LocalInvocationID.y; y < tile_max.y; y += gl_WorkGroupSize.y) {
    for (uint x = tile_min.x + gl_LocalInvocationID.x; x < tile_max.x; x += gl_WorkGroupSize.x) {
      const float reverse_depth = texelFetch(depth_image[0], ivec2(x, y), 0).r;
      if (reverse_depth > 0.0) {
        const uint bits = floatBitsToUint(view_depth(reverse_depth));
        atomicMin(nearest_depth_bits, bits);
        atomicMax(farthest_depth_bits, bits);
        atomicAdd(geometry_count, 1u);
      }
    }
  }
  barrier();

  if (geometry_count == 0u) {
    return;
  }

  const float nearest_depth = uintBitsToFloat(nearest_depth_bits);
  const float farthest_depth = uintBitsToFloat(farthest_depth_bits);
  const uint light_count = min(floatBitsToUint(light_data[0].words[0].x), 128u);
  const vec2 viewport_f = vec2(viewport);
  const vec2 rect_min = vec2(tile_min);
  const vec2 rect_max = vec2(tile_max);

  for (uint i = local_index; i < light_count; i += gl_WorkGroupSize.x * gl_WorkGroupSize.y) {
    const vec4 position_radius = light_data[0].words[1u + i * 2u];
    const vec3 view_position = (camera_data[0].view * vec4(position_radius.xyz, 1.0)).xyz;
    const float radius = position_radius.w;
    const float center_depth = -view_position.z;
    if (center_depth + radius < nearest_depth || center_depth - radius > farthest_depth) {
      continue;
    }
    if (!overlaps_tile(view_position, radius, center_depth, viewport_f, rect_min, rect_max)) {
      continue;
    }
    accepted_flags[i] = 1u;
  }
  barrier();

  if (local_index == 0u) {
    uint accepted = 0u;
    for (uint i = 0u; i < light_count && accepted < PF01_MAX_LIGHTS_PER_TILE; ++i) {
      if (accepted_flags[i] != 0u) {
        tile_data[0].words[list_base + 1u + accepted] = i;
        ++accepted;
      }
    }
    tile_data[0].words[list_base] = accepted;
  }
}
