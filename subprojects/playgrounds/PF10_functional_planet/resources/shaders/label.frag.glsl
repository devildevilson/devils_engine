#version 450
#include <utils/shared.h>

layout(location = 0) flat in mat4 in_planet_to_decal;
layout(location = 4) flat in vec4 in_uv_rect;
layout(location = 5) flat in vec4 in_fill;
layout(location = 6) flat in vec4 in_effect;
layout(location = 7) flat in vec3 in_projection_normal;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 planet_to_world;
  mat4 world_to_planet;
  vec4 camera_position;
  vec4 light_direction;
  vec4 border_colour;
  uvec4 params;
  vec4 viewport_near;
  mat4 inverse_view_projection;
} camera_data;
layout(set = 0, binding = 2) uniform sampler2D scene_depth_texture;
struct PoliticalTexel { uint region_id; float edge_distance; };
layout(set = 0, binding = 3, std430) readonly buffer PoliticalAtlas { PoliticalTexel texels[]; } political_atlas;
layout(set = 1, binding = 0) uniform texture2D textures[16];
layout(set = 1, binding = 1) uniform sampler samplers[1];
#define LABEL_SAMPLER sampler2D(textures[clamp(uint(in_effect.x + 0.5), 0u, 15u)], samplers[0])

uint region_at(const vec3 direction) {
  const vec3 magnitude = abs(direction);
  uint face;
  vec2 uv;
  if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
    if (direction.x >= 0.0) { face = 0u; uv = vec2(-direction.z, direction.y) / magnitude.x; }
    else { face = 1u; uv = vec2(direction.z, direction.y) / magnitude.x; }
  } else if (magnitude.y >= magnitude.z) {
    if (direction.y >= 0.0) { face = 2u; uv = vec2(direction.x, -direction.z) / magnitude.y; }
    else { face = 3u; uv = vec2(direction.x, direction.z) / magnitude.y; }
  } else if (direction.z >= 0.0) { face = 4u; uv = vec2(direction.x, direction.y) / magnitude.z; }
  else { face = 5u; uv = vec2(-direction.x, direction.y) / magnitude.z; }
  const uint side = uint(camera_data.viewport_near.w + 0.5);
  const uvec2 xy = uvec2(clamp((uv * 0.5 + 0.5) * float(side) + 0.5, vec2(0.0), vec2(float(side))));
  const uint stride = side + 1u;
  return political_atlas.texels[face * stride * stride + xy.y * stride + xy.x].region_id;
}

void main() {
  const vec2 screen_uv = gl_FragCoord.xy / camera_data.viewport_near.xy;
  const float depth = texture(scene_depth_texture, screen_uv).r;
  if (depth <= 0.0) discard;
  const vec4 world_h = camera_data.inverse_view_projection * vec4(screen_uv * 2.0 - 1.0, depth, 1.0);
  const vec3 world = world_h.xyz / world_h.w;
  const vec3 planet = (camera_data.world_to_planet * vec4(world, 1.0)).xyz;
  const uint owner_region = floatBitsToUint(in_effect.w);
  if (owner_region != 0xffffffffu && region_at(normalize(planet)) != owner_region) discard;
  const vec3 local = (in_planet_to_decal * vec4(planet, 1.0)).xyz;
  if (any(greaterThan(abs(local), vec3(0.5)))) discard;

  const float facing = dot(normalize(planet), normalize(in_projection_normal));
  const float normal_fade = smoothstep(0.72, 0.90, facing);
  if (normal_fade <= 0.0) discard;

  const vec2 atlas_uv = mix(in_uv_rect.xy, in_uv_rect.zw, local.xy + 0.5);
  const vec4 sample_value = texture(LABEL_SAMPLER, atlas_uv);
  const float signed_distance = median3(sample_value.r, sample_value.g, sample_value.b);
  const vec2 unit_range = vec2(4.0) / vec2(textureSize(LABEL_SAMPLER, 0));
  const vec2 screen_tex_size = vec2(1.0) / max(fwidth(atlas_uv), vec2(1e-6));
  const float px = max(0.5 * dot(unit_range, screen_tex_size), 1.0) /
                   (1.0 + max(in_effect.z, 0.0) * 4.0);
  const float coverage = clamp(px * (signed_distance - (0.5 - in_effect.y)) + 0.5, 0.0, 1.0);
  const float alpha = coverage * normal_fade * in_fill.a;
  if (alpha <= 1.0 / 255.0) discard;
  out_color = vec4(in_fill.rgb, alpha);
}
