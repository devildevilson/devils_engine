#version 450

layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 uv;
layout(location = 2) flat in int tex_index;
layout(location = 0) out vec4 frag_color;

layout(set = 1, binding = 0) uniform sampler2D tex[8];

void main() {
  frag_color = texture(tex[clamp(tex_index, 0, 7)], uv) * vertex_color;
}
