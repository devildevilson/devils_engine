#version 450
#include <utils/shared.h>

// Алгоритм настоящей screen-space decal: depth текущего opaque-кадра превращается через inverse VP в
// world position, затем world_to_decal переводит точку в локальный box. Точки вне box отбрасываются,
// а local XY задаёт UV Crimson MSDF. Scene normal ограничивает проекцию поверхностями, смотрящими в
// сторону decal, поэтому надпись не растягивается на соседние стены. Геометрия мира не перерисовывается,
// decal не меняет depth и смешивается с уже готовым scene color.

layout(location = 0) flat in mat4 in_world_to_decal;
layout(location = 4) flat in vec4 in_uv_rect;
layout(location = 5) flat in vec4 in_fill_color;
layout(location = 6) flat in vec4 in_effect; // atlas slot, boldness, softness, minimum normal cosine
layout(location = 7) flat in vec3 in_projection_normal;
layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  mat4 inverse_view_projection;
  vec4 effect_params;
} camera_data;

layout(set = 1, binding = 0) uniform sampler2D scene_depth_texture;
layout(set = 1, binding = 1) uniform sampler2D scene_normal_texture;
layout(set = 2, binding = 0) uniform texture2D text_images[16];
layout(set = 2, binding = 1) uniform sampler text_samplers[1];

const float atlas_px_range = 4.0;
#define DECAL_ATLAS sampler2D(text_images[atlas_index], text_samplers[0])

void main() {
  if (camera_data.effect_params.x < 0.5) discard;

  const vec2 screen_uv = gl_FragCoord.xy / camera_data.viewport_near.xy;
  const float depth = texture(scene_depth_texture, screen_uv).r;
  // Reverse-Z clear is zero: there is no finite scene surface to receive a decal.
  if (depth <= 0.0) discard;

  const vec4 world_h = camera_data.inverse_view_projection * vec4(screen_uv * 2.0 - 1.0, depth, 1.0);
  const vec3 world = world_h.xyz / world_h.w;
  const vec3 local = (in_world_to_decal * vec4(world, 1.0)).xyz;
  if (any(greaterThan(abs(local), vec3(0.5)))) discard;

  const vec3 scene_normal = normalize(texture(scene_normal_texture, screen_uv).xyz * 2.0 - 1.0);
  const float facing = dot(scene_normal, normalize(in_projection_normal));
  const float normal_fade = smoothstep(in_effect.w, min(in_effect.w + 0.12, 1.0), facing);
  if (normal_fade <= 0.0) discard;

  const vec2 decal_uv = local.xy + 0.5;
  const vec2 atlas_uv = mix(in_uv_rect.xy, in_uv_rect.zw, decal_uv);
  const uint atlas_index = clamp(uint(in_effect.x + 0.5), 0u, 15u);
  const vec4 sample_value = texture(DECAL_ATLAS, atlas_uv);
  const float signed_distance = median3(sample_value.r, sample_value.g, sample_value.b);
  const vec2 unit_range = vec2(atlas_px_range) / vec2(textureSize(DECAL_ATLAS, 0));
  const vec2 screen_tex_size = vec2(1.0) / max(fwidth(atlas_uv), vec2(1e-6));
  const float px = max(0.5 * dot(unit_range, screen_tex_size), 1.0) /
                   (1.0 + max(in_effect.z, 0.0) * 4.0);
  const float coverage = clamp(px * (signed_distance - (0.5 - in_effect.y)) + 0.5, 0.0, 1.0);
  const float alpha = in_fill_color.a * coverage * normal_fade;
  if (alpha <= 1.0 / 255.0) discard;
  frag_color = vec4(in_fill_color.rgb, alpha);
}
