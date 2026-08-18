#version 450

#ifndef PF01_MARKER_SIZE
#define PF01_MARKER_SIZE 14.0
#endif

layout(location = 0) in vec3 in_origin;
layout(location = 1) in vec4 in_position_radius;
layout(location = 2) in vec4 in_color_intensity;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(location = 0) out vec3 marker_color;

void main() {
  const vec3 world_position = in_position_radius.xyz + in_origin;
  marker_color = in_color_intensity.rgb;
  gl_Position = camera_data.view_projection * vec4(world_position, 1.0);
  gl_PointSize = PF01_MARKER_SIZE;
}
