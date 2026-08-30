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

layout(location = 0) out vec3 out_local_direction;
layout(location = 1) out vec3 out_world_position;
layout(location = 2) out float out_height;

vec3 cube_direction(const uint face, const vec2 uv) {
  if (face == 0u) return normalize(vec3(1.0, uv.y, -uv.x));
  if (face == 1u) return normalize(vec3(-1.0, uv.y, uv.x));
  if (face == 2u) return normalize(vec3(uv.x, 1.0, -uv.y));
  if (face == 3u) return normalize(vec3(uv.x, -1.0, uv.y));
  if (face == 4u) return normalize(vec3(uv.x, uv.y, 1.0));
  return normalize(vec3(-uv.x, uv.y, -1.0));
}

void main() {
  const uint side = max(camera_data.params.z, 1u);
  const uint vertices_per_face = side * side * 6u;
  const uint face = uint(gl_VertexIndex) / vertices_per_face;
  const uint local_vertex = uint(gl_VertexIndex) % vertices_per_face;
  const uint cell = local_vertex / 6u;
  const uint corner_index = local_vertex % 6u;
  const uvec2 corners[6] = uvec2[6](uvec2(0, 0), uvec2(1, 0), uvec2(1, 1),
                                    uvec2(0, 0), uvec2(1, 1), uvec2(0, 1));
  const uvec2 cell_xy = uvec2(cell % side, cell / side) + corners[corner_index];
  const vec2 uv = vec2(cell_xy) / float(side) * 2.0 - 1.0;
  const vec3 direction = cube_direction(face, uv);
  const float height = pf10_surface_height(direction);
  const vec3 local_position = direction * (PF10_RADIUS + height);
  const vec3 world_position = (camera_data.planet_to_world * vec4(local_position, 1.0)).xyz;
  gl_Position = camera_data.view_projection * vec4(world_position, 1.0);
  out_local_direction = direction;
  out_world_position = world_position;
  out_height = height;
}
