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

void main() {
  const uint side = max(camera_data.params.z, 1u);
  const uint patch_side = 16u;
  const uint patch_index = uint(gl_InstanceIndex) / patch_side;
  const uint row = uint(gl_InstanceIndex) % patch_side;
  const uvec4 patch_record = patch_data.patches[patch_index];
  const uint face = patch_record.x;
  const uint strip_vertex = uint(gl_VertexIndex);
  const uvec2 cell_xy = uvec2(patch_record.y + strip_vertex / 2u,
                              patch_record.z + row + (strip_vertex & 1u));
  const uint nodes_per_face = (side + 1u) * (side + 1u);
  const uint node = face * nodes_per_face + cell_xy.y * (side + 1u) + cell_xy.x;
  const vec3 local_position = surface.positions[node].xyz;
  const vec3 direction = normalize(local_position);
  const float height = length(local_position) - PF10_RADIUS;
  const vec3 world_position = (camera_data.planet_to_world * vec4(local_position, 1.0)).xyz;
  gl_Position = camera_data.view_projection * vec4(world_position, 1.0);
  out_local_direction = direction;
  out_world_position = world_position;
  out_height = height;
}
