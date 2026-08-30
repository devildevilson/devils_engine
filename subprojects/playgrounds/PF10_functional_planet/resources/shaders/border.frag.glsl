#version 450

layout(location = 0) flat in vec3 in_a_direction;
layout(location = 1) flat in vec3 in_b_direction;
layout(location = 0) out vec4 out_color;

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
layout(set = 0, binding = 2) uniform sampler2D scene_depth_texture;

void main() {
  const vec2 screen_uv = gl_FragCoord.xy / camera_data.viewport_near.xy;
  const float depth = texture(scene_depth_texture, screen_uv).r;
  if (depth <= 0.0) discard;
  const vec4 world_h = camera_data.inverse_view_projection * vec4(screen_uv * 2.0 - 1.0, depth, 1.0);
  const vec3 world = world_h.xyz / world_h.w;
  const vec3 direction = normalize((camera_data.world_to_planet * vec4(world, 1.0)).xyz);
  const vec3 segment = in_b_direction - in_a_direction;
  const float t = clamp(dot(direction - in_a_direction, segment) / max(dot(segment, segment), 1e-10), 0.0, 1.0);
  const float distance_to_curve = length(direction - normalize(mix(in_a_direction, in_b_direction, t)));
  const float half_width = 0.00034;
  const float aa = max(fwidth(distance_to_curve), 1e-6);
  const float coverage = 1.0 - smoothstep(half_width - aa, half_width + aa, distance_to_curve);
  const float alpha = coverage * camera_data.border_colour.a;
  if (alpha <= 1.0 / 255.0) discard;
  out_color = vec4(camera_data.border_colour.rgb, alpha);
}
