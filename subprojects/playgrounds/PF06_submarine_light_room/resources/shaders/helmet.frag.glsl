#version 450

// Алгоритм: финальный LDR pass описывает стекло шлема экранной superellipse. Центр почти не меняется; ближе к краям
// появляются слабое radial refraction, холодный оттенок и Fresnel-like блик, за стеклом — тёмный мягкий обод.
// Мелкая неоднородность разрешена только около края, чтобы шлем ощущался отдельной поверхностью, но не превращал
// весь кадр в постоянно грязную камеру. strength=0 является точным passthrough для честного runtime A/B.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0) uniform sampler2D scene_ldr;
layout(set = 0, binding = 1, std140) uniform LightingBlock {
  vec4 state;
  vec4 presentation;
  vec4 weak_position_radius;
  vec4 weak_color_energy;
  vec4 safe_position_radius;
  vec4 safe_color_energy;
  vec4 flashlight_direction_cos;
  vec4 flashlight_color_energy;
  vec4 room_irradiance;
  vec4 source_reach;
  vec4 medium_params;
  vec4 medium_absorption;
  vec4 medium_scattering;
  vec4 tonemap_params;
  vec4 helmet_params;
  vec4 shadow_wall_params;
} lighting;

float hash21(const vec2 p) {
  vec3 q = fract(vec3(p.xyx) * 0.1031);
  q += dot(q, q.yzx + 33.33);
  return fract((q.x + q.y) * q.z);
}

void main() {
  const float strength = lighting.helmet_params.x;
  if (strength <= 0.0001) {
    frag_color = texture(scene_ldr, in_uv);
    return;
  }

  const vec2 p = in_uv * 2.0 - 1.0;
  const vec2 visor_p = vec2(p.x / 0.985, (p.y + 0.025) / 0.925);
  const float visor_shape = pow(abs(visor_p.x), 4.0) + pow(abs(visor_p.y), 4.0);
  const float edge = smoothstep(0.48, 1.0, visor_shape);
  const float outside = smoothstep(0.94, 1.04, visor_shape);
  const float rim = smoothstep(0.69, 0.98, visor_shape) * (1.0 - outside);
  const float inner_edge = smoothstep(0.70, 0.80, visor_shape) *
                           (1.0 - smoothstep(0.86, 0.96, visor_shape));

  const vec2 distorted_uv = clamp(0.5 + (in_uv - 0.5) * (1.0 + edge * edge * 0.012 * strength), 0.0, 1.0);
  vec3 color = texture(scene_ldr, distorted_uv).rgb;
  color *= mix(vec3(1.0), vec3(0.91, 0.965, 1.015), lighting.helmet_params.z * strength);
  color *= 1.0 - rim * lighting.helmet_params.y * strength;

  const float upper_region = 1.0 - smoothstep(-0.42, 0.08, p.y);
  const float left_region = 1.0 - smoothstep(0.05, 0.72, p.x);
  const float upper_arc = exp(-abs(visor_shape - 0.72) * 24.0) * upper_region * left_region;
  const float side_arc = exp(-abs(visor_shape - 0.78) * 31.0) *
                         smoothstep(0.42, 0.86, abs(p.x)) * (1.0 - smoothstep(0.28, 0.82, p.y));
  const float edge_noise = hash21(floor(in_uv * vec2(720.0, 260.0)));
  const float dust = smoothstep(0.992, 1.0, edge_noise) * rim * lighting.helmet_params.w;
  color += vec3(0.055, 0.080, 0.088) * inner_edge * 0.42 * strength;
  color += vec3(0.13, 0.19, 0.21) * (upper_arc * 0.34 + side_arc * 0.14 + dust * 0.12) * strength;

  const vec3 shell = vec3(0.006, 0.008, 0.009);
  color = mix(color, shell, outside * (0.92 * strength));
  frag_color = vec4(color, 1.0);
}
