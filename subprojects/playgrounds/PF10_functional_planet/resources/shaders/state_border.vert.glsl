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

struct StateBorderSegment { vec4 a_position_s; vec4 b_position_s; uvec4 states; };
layout(set = 0, binding = 1, std430) readonly buffer StateBorders {
  StateBorderSegment segments[];
} state_borders;

layout(location = 0) out vec3 out_coordinate; // cumulative s, signed across pixels, half width pixels
layout(location = 1) flat out uvec3 out_states; // +across, -across, component
layout(location = 2) out vec3 out_boundary_direction;

void main() {
  const StateBorderSegment segment = state_borders.segments[gl_InstanceIndex];
  const vec2 corners[6] = vec2[6](vec2(0, -1), vec2(1, -1), vec2(1, 1),
                                   vec2(0, -1), vec2(1, 1), vec2(0, 1));
  const vec2 corner = corners[gl_VertexIndex];
  const vec3 local_a = segment.a_position_s.xyz + normalize(segment.a_position_s.xyz) * 0.00082;
  const vec3 local_b = segment.b_position_s.xyz + normalize(segment.b_position_s.xyz) * 0.00082;
  const vec4 clip_a = camera_data.view_projection * camera_data.planet_to_world * vec4(local_a, 1.0);
  const vec4 clip_b = camera_data.view_projection * camera_data.planet_to_world * vec4(local_b, 1.0);
  const vec2 viewport = max(camera_data.viewport_near.xy, vec2(1.0));
  const vec2 screen_a = (clip_a.xy / clip_a.w * 0.5 + 0.5) * viewport;
  const vec2 screen_b = (clip_b.xy / clip_b.w * 0.5 + 0.5) * viewport;
  vec2 along = screen_b - screen_a;
  if (dot(along, along) < 1.0e-8) along = vec2(1.0, 0.0);
  along = normalize(along);
  const vec2 across = vec2(-along.y, along.x);
  const float eye_distance = length(camera_data.camera_position.xyz);
  const float half_width = mix(3.65, 2.55, smoothstep(1.16, 4.5, eye_distance));
  const float bound = half_width + 1.1;
  const vec2 screen_position = mix(screen_a, screen_b, corner.x) +
                               along * ((corner.x * 2.0 - 1.0) * 0.85) + across * (corner.y * bound);
  vec4 clip = mix(clip_a, clip_b, corner.x);
  clip.xy = (screen_position / viewport * 2.0 - 1.0) * clip.w;

  const vec3 middle = normalize(local_a + local_b);
  const vec3 local_eye = (camera_data.world_to_planet * vec4(camera_data.camera_position.xyz, 1.0)).xyz;
  const float conservative_horizon = 0.995 / length(local_eye) - 0.002;
  gl_Position = dot(middle, normalize(local_eye)) < conservative_horizon ?
                  vec4(2.0, 2.0, 2.0, 1.0) : clip;
  out_coordinate = vec3(mix(segment.a_position_s.w, segment.b_position_s.w, corner.x),
                        corner.y * bound, half_width);
  out_states = segment.states.xyz;
  out_boundary_direction = normalize(mix(local_a, local_b, corner.x));
}
