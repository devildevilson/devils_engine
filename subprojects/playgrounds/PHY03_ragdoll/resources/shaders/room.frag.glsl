#version 450

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec4 vertex_color;
layout(location = 0) out vec4 out_color;

void main() {
  vec3 color = vertex_color.rgb;
  if (vertex_color.a < 0.0) {
    out_color = vec4(color, -vertex_color.a);
    return;
  }
  if (vertex_color.a > 0.5) {
    vec2 grid = abs(fract(world_position.xz + 0.5) - 0.5);
    float line = 1.0 - smoothstep(0.012, 0.028, min(grid.x, grid.y));
    color *= mix(1.0, 0.63, line);
  }
  out_color = vec4(color, 1.0);
}
