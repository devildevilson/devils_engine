#version 450

layout(location = 0) in vec2 screen_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform SceneBlock {
  mat4 view_projection;
  mat4 view;
  mat4 light_view_projection;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 light_direction;
  vec4 shadow_params;
} scene_data[3];

layout(set = 1, binding = 0) uniform sampler2D directional_shadow_image[3];
layout(set = 1, binding = 1) uniform sampler2D spot_shadow_atlas[3];

void main() {
  const vec2 viewport = scene_data[0].viewport_near.xy;
  const float side = min(224.0, min(viewport.x * 0.22, viewport.y * 0.32));
  const vec2 directional_origin = vec2(viewport.x - side * 2.0 - 24.0, 12.0);
  const vec2 atlas_origin = vec2(viewport.x - side - 12.0, 12.0);
  const vec2 directional_uv = (gl_FragCoord.xy - directional_origin) / side;
  const vec2 atlas_uv = (gl_FragCoord.xy - atlas_origin) / side;
  const bool in_directional = all(greaterThanEqual(directional_uv, vec2(0.0))) && all(lessThanEqual(directional_uv, vec2(1.0)));
  const bool in_atlas = all(greaterThanEqual(atlas_uv, vec2(0.0))) && all(lessThanEqual(atlas_uv, vec2(1.0)));
  if (!in_directional && !in_atlas) discard;

  const vec2 sample_uv = in_directional ? directional_uv : atlas_uv;
  const bool outer_border = any(lessThan(sample_uv, vec2(0.012))) || any(greaterThan(sample_uv, vec2(0.988)));
  const bool tile_border = in_atlas && (abs(sample_uv.x - 0.5) < 0.006 || abs(sample_uv.y - 0.5) < 0.006);
  const float depth = in_directional
    ? texture(directional_shadow_image[0], sample_uv).r
    : texture(spot_shadow_atlas[0], sample_uv).r;
  const float visible_depth = pow(clamp(depth, 0.0, 1.0), 0.35);
  out_color = (outer_border || tile_border) ? vec4(1.0, 0.78, 0.18, 1.0) : vec4(vec3(visible_depth), 1.0);
}
