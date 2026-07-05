#version 450

layout(location = 0) in  vec4 vertex_color;
layout(location = 1) in  vec2 uv;
layout(location = 2) flat in int tex_index;
layout(location = 0) out vec4 frag_color;

// asset-текстуры (bindless v2): раздельные sampled-image массив (binding0) + sampler-пул (binding1,
// immutable). Пара собирается на месте: sampler2D(tex[i], samp[s]). Тест-триангл — всегда linear (samp[0]).
layout(set = 0, binding = 0) uniform texture2D tex[16];
layout(set = 0, binding = 1) uniform sampler   samp[2]; // 0=linear, 1=nearest

void main() {
  frag_color = texture(sampler2D(tex[clamp(tex_index, 0, 15)], samp[0]), uv);
}
