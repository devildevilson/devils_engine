#version 450

// Алгоритм: круглая coverage превращает procedural quad в маленькую тёмную взвесь. Частицы не пишут depth и
// обычным alpha blend либо отнимают немного света, либо возвращают слабый scattering с вариацией от холодного
// серо-зелёного до редкого грязно-янтарного. В blackout весь pass гасится source presence, а в exploration светлая
// подгруппа остаётся читаемой даже вне яркого луча.

layout(location = 0) in vec2 in_local;
layout(location = 1) in float in_visibility;
layout(location = 2) in float in_scatter_tint;
layout(location = 0) out vec4 frag_color;

void main() {
  const float radius = length(in_local);
  const float coverage = 1.0 - smoothstep(0.52, 1.0, radius);
  if (coverage <= 0.001) discard;
  const float scattering_mote = step(0.001, in_scatter_tint);
  const vec3 scattering_color = mix(
    vec3(0.090, 0.125, 0.112),
    vec3(0.155, 0.120, 0.078),
    smoothstep(0.22, 1.0, in_scatter_tint));
  const vec3 color = mix(vec3(0.004, 0.010, 0.011), scattering_color, scattering_mote);
  const float alpha = mix(0.62, 0.56, scattering_mote);
  frag_color = vec4(color, coverage * in_visibility * alpha);
}
