#version 450

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 planet_to_world;
  mat4 world_to_planet;
  vec4 camera_position;
  vec4 light_direction;
  vec4 border_colour;
  uvec4 params;
  vec4 viewport_near;
  mat4 inverse_view_projection;
} camera_data;

struct BorderSegment { vec4 a_direction_height; vec4 b_direction_height; uvec4 region_ids; };
layout(set = 0, binding = 1, std430) readonly buffer BorderSegments { BorderSegment segments[]; } borders;

layout(location = 0) flat out vec3 out_a_direction;
layout(location = 1) flat out vec3 out_b_direction;

void main() {
  const BorderSegment segment = borders.segments[gl_InstanceIndex];
  const vec2 corners[6] = vec2[6](vec2(0, -1), vec2(1, -1), vec2(1, 1),
                                   vec2(0, -1), vec2(1, 1), vec2(0, 1));
  const vec3 a = normalize(segment.a_direction_height.xyz);
  const vec3 b = normalize(segment.b_direction_height.xyz);
  const vec3 middle = normalize(a + b);
  const vec3 a_position = a * (1.0 + segment.a_direction_height.w);
  const vec3 b_position = b * (1.0 + segment.b_direction_height.w);
  const vec3 across = normalize(cross(middle, b - a));
  const vec3 local_position = mix(a_position, b_position, corners[gl_VertexIndex].x) +
                              across * corners[gl_VertexIndex].y * 0.0022;
  const vec3 world_middle = (camera_data.planet_to_world * vec4(middle, 1.0)).xyz;
  const vec3 world_radial = normalize((camera_data.planet_to_world * vec4(middle, 0.0)).xyz);
  if (dot(world_radial, camera_data.camera_position.xyz - world_middle) <= -0.015) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  } else {
    gl_Position = camera_data.view_projection * camera_data.planet_to_world * vec4(local_position, 1.0);
  }
  out_a_direction = a;
  out_b_direction = b;
}
