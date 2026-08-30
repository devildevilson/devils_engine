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

struct landmark_record {
  vec4 direction_height;
  vec4 colour_scale;
  uint region_id;
  uint kind;
  uint pad0;
  uint pad1;
};
layout(set = 0, binding = 1, std430) readonly buffer Landmarks { landmark_record records[]; } landmarks;
layout(location = 0) out vec3 out_colour;

void main() {
  const landmark_record marker = landmarks.records[gl_InstanceIndex];
  const vec3 direction = normalize(marker.direction_height.xyz);
  const vec3 axis = abs(direction.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  const vec3 tangent = normalize(cross(axis, direction));
  const vec3 bitangent = cross(direction, tangent);
  const float radius = 0.0105 * marker.colour_scale.w;
  const float height = 0.040 * marker.colour_scale.w;
  const vec3 anchor = direction * (PF10_RADIUS + marker.direction_height.w + 0.006);
  const uint side = uint(gl_VertexIndex) / 3u;
  const uint corner = uint(gl_VertexIndex) % 3u;
  const float a0 = float(side) * 1.57079632679 + 0.78539816339;
  const float a1 = float(side + 1u) * 1.57079632679 + 0.78539816339;
  vec3 local = direction * height;
  if (corner == 0u) local = tangent * (cos(a0) * radius) + bitangent * (sin(a0) * radius);
  else if (corner == 1u) local = tangent * (cos(a1) * radius) + bitangent * (sin(a1) * radius);
  const vec3 world = (camera_data.planet_to_world * vec4(anchor + local, 1.0)).xyz;
  gl_Position = camera_data.view_projection * vec4(world, 1.0);
  out_colour = marker.colour_scale.rgb;
}
