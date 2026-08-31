#version 450

layout(location = 0) in vec3 in_coordinate;
layout(location = 1) flat in uvec3 in_states;
layout(location = 2) in vec3 in_boundary_direction;
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
layout(set = 0, binding = 2) uniform sampler2D scene_depth;

uint mix32(uint value) {
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  value ^= value >> 16u;
  return value;
}

void state_palette(const uint state, out vec3 primary, out vec3 secondary) {
  const uint index = state % 3u;
  if (index == 0u) {
    primary = vec3(0.92, 0.20, 0.12);
    secondary = vec3(1.00, 0.78, 0.24);
  } else if (index == 1u) {
    primary = vec3(0.12, 0.42, 0.92);
    secondary = vec3(0.76, 0.91, 1.00);
  } else {
    primary = vec3(0.16, 0.68, 0.30);
    secondary = vec3(0.91, 0.96, 0.72);
  }
}

void main() {
  const vec2 screen_uv = gl_FragCoord.xy / vec2(textureSize(scene_depth, 0));
  const float receiver_depth = texture(scene_depth, screen_uv).r;
  if (receiver_depth <= 0.0) discard;
  const float receiver_tolerance = max(0.0015, receiver_depth * 0.08);
  if (gl_FragCoord.z + receiver_tolerance < receiver_depth) discard;
  const vec4 world_h = camera_data.inverse_view_projection *
                       vec4(screen_uv * 2.0 - 1.0, receiver_depth, 1.0);
  const vec3 world = world_h.xyz / world_h.w;
  const vec3 receiver_direction = normalize((camera_data.world_to_planet * vec4(world, 1.0)).xyz);
  if (dot(receiver_direction, normalize(in_boundary_direction)) < 0.99980) discard;
  const float across = in_coordinate.y;
  const float half_width = in_coordinate.z;
  const float edge_aa = max(fwidth(across), 0.35);
  const float coverage = 1.0 - smoothstep(half_width - edge_aa, half_width + edge_aa, abs(across));
  if (coverage <= 1.0 / 255.0) discard;

  const uint state = across >= 0.0 ? in_states.x : in_states.y;
  vec3 primary;
  vec3 secondary;
  state_palette(state, primary, secondary);
  const float phase_offset = float(mix32(state * 0x9e3779b9u) & 65535u) / 65535.0;
  const float wave = sin((in_coordinate.x / 0.0135 + phase_offset) * 6.28318530718);
  const float pattern_aa = max(fwidth(wave), 0.035);
  vec3 colour = mix(primary, secondary, smoothstep(-pattern_aa, pattern_aa, wave));

  // Both states keep their own half-ribbon. A narrow neutral separator makes the two neighbouring political
  // identities readable even when their palette phases happen to choose similarly bright colours.
  const float separator = 1.0 - smoothstep(0.34, 0.78, abs(across));
  colour = mix(colour, vec3(0.025, 0.030, 0.038), separator);
  out_color = vec4(colour, coverage * 0.98);
}
