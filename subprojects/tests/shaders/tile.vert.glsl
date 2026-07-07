#version 450

layout(location = 0) in vec3 in_vertex_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_tile_center;
layout(location = 3) in uint in_tex;

layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 out_uv;
layout(location = 2) flat out int out_tex;

layout(set = 0, binding = 0) uniform global_ubo {
  mat4 view_proj;
  mat4 ui_proj;
  vec4 misc;
  vec4 reserved[3];
} g[2];

out gl_PerVertex {
  vec4 gl_Position;
};

void main() {
  const vec2 world_pos = in_tile_center + in_vertex_pos.xy;
  gl_Position = g[0].view_proj * vec4(world_pos, 0.0, 1.0);
  vertex_color = in_color;
  out_uv = in_vertex_pos.xy + vec2(0.5);
  out_tex = int(in_tex);
}
