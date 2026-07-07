#version 450

layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 uv;
layout(location = 2) flat in int tex_index;
layout(location = 0) out vec4 frag_color;

// asset-текстуры (bindless v2): sampled-image массив (binding0) + sampler-пул (binding1). Тайлы — linear.
layout(set = 1, binding = 0) uniform texture2D tex[16];
layout(set = 1, binding = 1) uniform sampler   samp[2]; // 0=linear, 1=nearest

void main() {
  frag_color = texture(sampler2D(tex[clamp(tex_index, 0, 15)], samp[0]), uv) * vertex_color;
}
