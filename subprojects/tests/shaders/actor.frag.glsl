#version 450

layout(location = 0) in vec4 in_color;
layout(location = 1) flat in uint in_tex;
layout(location = 0) out vec4 frag_color;

void main() {
  frag_color = in_tex == 0xffffffffu ? vec4(0.0) : in_color;
}
