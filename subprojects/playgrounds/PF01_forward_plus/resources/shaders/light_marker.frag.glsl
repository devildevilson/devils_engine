#version 450

layout(location = 0) in vec3 marker_color;
layout(location = 0) out vec4 out_color;

void main() {
  const vec2 centered = gl_PointCoord * 2.0 - 1.0;
  const float radius_squared = dot(centered, centered);
  if (radius_squared > 1.0) discard;
  const float core = 1.0 - smoothstep(0.15, 1.0, radius_squared);
  out_color = vec4(marker_color * (0.55 + core * 0.45), 1.0);
}
