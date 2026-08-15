#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_instance;
layout(location = 0) out vec2 screen_uv;

layout(set = 0, binding = 0, std140) uniform SceneBlock {
  mat4 view_projection;
  mat4 view;
  mat4 light_view_projection;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 light_direction;
  vec4 shadow_params;
  vec4 filter_params;
} scene_data[3];

void main() {
  gl_Position = vec4(in_position + in_instance.xyz * scene_data[0].shadow_params.w, 1.0);
  screen_uv = in_position.xy * 0.5 + 0.5;
}
