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
} g;

out gl_PerVertex {
  vec4 gl_Position;
};

const float tile_texture_span = 4.0;

void main() {
  const vec2 world_pos = in_tile_center + in_vertex_pos.xy;
  gl_Position = g.view_proj * vec4(world_pos, 0.0, 1.0);
  vertex_color = in_color;
  // Текстура лежит в МИРОВЫХ координатах и повторяется раз в tile_texture_span тайлов: соседние
  // тайлы одного рельефа продолжают друг друга, а не повторяют одну картинку с сеткой швов.
  out_uv = world_pos / tile_texture_span;
  out_tex = int(in_tex);
}
