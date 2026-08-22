#version 450

// Алгоритм: квадратный billboard превращается в мягкий круг по расстоянию до центра. Яркое ядро и lifetime
// fade формируют emissive spark; additive blending делает порядок частиц несущественным. Depth test остаётся,
// поэтому opaque мир закрывает частицы, но сами полупрозрачные sparks глубину не пишут.

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec3 in_color;
layout(location = 2) in float in_life_fade;
layout(location = 0) out vec4 frag_color;

void main() {
  const float radius = length(in_uv - 0.5) * 2.0;
  const float coverage = 1.0 - smoothstep(0.55, 1.0, radius);
  const float core = 1.0 - smoothstep(0.0, 0.48, radius);
  const float alpha = coverage * in_life_fade * 0.72;
  if (alpha <= 1.0 / 255.0) discard;
  frag_color = vec4(in_color * mix(1.4, 3.2, core), alpha);
}
