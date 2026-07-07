#version 450

layout(location = 0) in  vec3 in_vertex_pos;
layout(location = 1) in  vec4 in_color;
layout(location = 2) in  vec4 in_pos;   // xyz = позиция инстанса, w = индекс текстуры
layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 out_uv;
layout(location = 2) flat out int out_tex;

out gl_PerVertex {
  vec4 gl_Position;
};

void main() {
  const vec4 pos = vec4(in_vertex_pos * 0.5, 1.0) + vec4(in_pos.xyz, 0.0);
  gl_Position = pos;
  vertex_color = in_color;
  out_uv = in_vertex_pos.xy * 0.5 + 0.5;
  out_tex = int(in_pos.w + 0.5); // индекс текстуры из w инстанса
}
