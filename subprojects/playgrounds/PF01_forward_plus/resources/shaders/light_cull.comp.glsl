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

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

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

float view_depth(float reverse_depth) {
  return camera_data[0].viewport_near.z / max(reverse_depth, 0.000001);
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
  tile_data[0].words[list_base] = 0u;

  float nearest_depth = 1e30;
  float farthest_depth = 0.0;
  bool has_geometry = false;
  for (uint y = tile_min.y; y < tile_max.y; ++y) {
    for (uint x = tile_min.x; x < tile_max.x; ++x) {
      const float d = texelFetch(depth_image[0], ivec2(x, y), 0).r;
      if (d > 0.0) {
        const float z = view_depth(d);
        nearest_depth = min(nearest_depth, z);
        farthest_depth = max(farthest_depth, z);
        has_geometry = true;
      }
    }
  }
  if (!has_geometry) {
    return;
  }

  const uint light_count = min(floatBitsToUint(light_data[0].words[0].x), 128u);
  const vec2 viewport_f = vec2(viewport);
  const vec2 rect_min = vec2(tile_min);
  const vec2 rect_max = vec2(tile_max);
  uint accepted = 0u;
  for (uint i = 0u; i < light_count; ++i) {
    const vec4 position_radius = light_data[0].words[1u + i * 2u];
    const vec3 view_position = (camera_data[0].view * vec4(position_radius.xyz, 1.0)).xyz;
    const float radius = position_radius.w;
    const float center_depth = -view_position.z;
    if (center_depth + radius < nearest_depth || center_depth - radius > farthest_depth || center_depth <= 0.0) {
      continue;
    }

    const vec4 clip = camera_data[0].view_projection * vec4(position_radius.xyz, 1.0);
    if (clip.w <= 0.0) {
      continue;
    }
    const vec2 screen = (clip.xy / clip.w * 0.5 + 0.5) * viewport_f;
    const float radius_pixels = radius * abs(camera_data[0].view_projection[1][1]) * viewport_f.y * 0.5 / center_depth;
    const vec2 closest = clamp(screen, rect_min, rect_max);
    if (dot(screen - closest, screen - closest) > radius_pixels * radius_pixels) {
      continue;
    }

    if (accepted < PF01_MAX_LIGHTS_PER_TILE) {
      tile_data[0].words[list_base + 1u + accepted] = i;
      ++accepted;
    }
  }
  tile_data[0].words[list_base] = accepted;
}
