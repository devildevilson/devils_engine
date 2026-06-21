#version 450

layout(location = 0) in  vec4 vertex_color;
layout(location = 1) in  vec2 uv;
layout(location = 2) flat in int tex_index;
layout(location = 0) out vec4 frag_color;

// asset-текстурный дескриптор 'textures' (set=0, binding=0): массив combinedImageSampler.
// Индекс выбирается per-instance (dynamic indexing, shaderSampledImageArrayDynamicIndexing).
layout(set = 0, binding = 0) uniform sampler2D tex[8];

void main() {
  frag_color = texture(tex[tex_index], uv);
}
