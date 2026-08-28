#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec3 in_radiance;
layout(location = 2) in float in_alpha;
layout(location = 3) flat in uint in_kind;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 3) uniform sampler2D scene_depth;

void main() {
  const vec2 screen_uv = gl_FragCoord.xy / vec2(textureSize(scene_depth, 0));
  const float opaque_depth = texture(scene_depth, screen_uv).r;
  // Reverse-Z: большее значение ближе. Particle за opaque receiver не должен просвечивать.
  if (opaque_depth > 0.0 && gl_FragCoord.z <= opaque_depth) discard;

  float alpha;
  if (in_kind == 0u) {
    const float across = abs(in_uv.x - 0.5) * 2.0;
    const float width = 1.0 - smoothstep(0.18, 1.0, across);
    const float ends = smoothstep(0.0, 0.14, in_uv.y) * (1.0 - smoothstep(0.82, 1.0, in_uv.y));
    alpha = width * ends * in_alpha;
  } else {
    const float radius = length(in_uv - 0.5) * 2.0;
    const float ring = smoothstep(0.35, 0.58, radius) * (1.0 - smoothstep(0.64, 0.92, radius));
    alpha = ring * in_alpha;
  }
  if (alpha <= 1.0 / 255.0) discard;
  out_color = vec4(in_radiance, alpha);
}
