#version 450

// Алгоритм: один shader даёт трём particle families разный coverage: мягкое emissive ядро spark, тонкий
// вытянутый rain streak и холодный круглый snow flake. Геометрия rain уже является тонким velocity-aligned
// cylindrical ribbon, здесь coverage только смягчает субпиксельную ширину и концы. Spark step использует additive
// blending, weather step — обычный alpha; оба проходят depth test opaque-сцены и сами глубину не пишут.

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec3 in_color;
layout(location = 2) in float in_life_fade;
layout(location = 3) flat in uint in_particle_kind;
layout(location = 0) out vec4 frag_color;

void main() {
  const float radius = length(in_uv - 0.5) * 2.0;
  if (in_particle_kind == 1u) {
    const float across = abs(in_uv.x - 0.5) * 2.0;
    const float width = 1.0 - smoothstep(0.30, 1.0, across);
    const float ends = smoothstep(0.0, 0.12, in_uv.y) * (1.0 - smoothstep(0.82, 1.0, in_uv.y));
    const float alpha = width * ends * in_life_fade * 0.34;
    if (alpha <= 1.0 / 255.0) discard;
    frag_color = vec4(in_color, alpha);
    return;
  }
  if (in_particle_kind == 2u) {
    const float coverage = 1.0 - smoothstep(0.48, 1.0, radius);
    const float alpha = coverage * in_life_fade * 0.72;
    if (alpha <= 1.0 / 255.0) discard;
    frag_color = vec4(in_color, alpha);
    return;
  }
  const float coverage = 1.0 - smoothstep(0.55, 1.0, radius);
  const float core = 1.0 - smoothstep(0.0, 0.48, radius);
  const float alpha = coverage * in_life_fade * 0.72;
  if (alpha <= 1.0 / 255.0) discard;
  frag_color = vec4(in_color * mix(1.4, 3.2, core), alpha);
}
