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
  vec4 filter_params;
  vec4 contact_params;
} scene_data[3];

layout(set = 1, binding = 0) uniform sampler2D directional_shadow_image[3];
layout(set = 1, binding = 1) uniform sampler2D spot_shadow_atlas[3];
layout(set = 2, binding = 0) uniform sampler2D directional_contact_image[3];
layout(set = 2, binding = 1) uniform sampler2D spot_contact_image[3];

void main() {
  const vec2 viewport = scene_data[0].viewport_near.xy;
  const float side = min(224.0, min(viewport.x * 0.22, viewport.y * 0.32));
  const vec2 directional_origin = vec2(viewport.x - side * 2.0 - 24.0, 12.0);
  const vec2 atlas_origin = vec2(viewport.x - side - 12.0, 12.0);
  const vec2 directional_contact_origin = directional_origin + vec2(0.0, side + 10.0);
  const vec2 spot_contact_origin = atlas_origin + vec2(0.0, side + 10.0);
  const vec2 directional_uv = (gl_FragCoord.xy - directional_origin) / side;
  const vec2 atlas_uv = (gl_FragCoord.xy - atlas_origin) / side;
  const vec2 directional_contact_uv = (gl_FragCoord.xy - directional_contact_origin) / side;
  const vec2 spot_contact_uv = (gl_FragCoord.xy - spot_contact_origin) / side;
  const bool in_directional = all(greaterThanEqual(directional_uv, vec2(0.0))) && all(lessThanEqual(directional_uv, vec2(1.0)));
  const bool in_atlas = all(greaterThanEqual(atlas_uv, vec2(0.0))) && all(lessThanEqual(atlas_uv, vec2(1.0)));
  const bool in_directional_contact = all(greaterThanEqual(directional_contact_uv, vec2(0.0))) && all(lessThanEqual(directional_contact_uv, vec2(1.0)));
  const bool in_spot_contact = all(greaterThanEqual(spot_contact_uv, vec2(0.0))) && all(lessThanEqual(spot_contact_uv, vec2(1.0)));
  if (!in_directional && !in_atlas && !in_directional_contact && !in_spot_contact) discard;

  if (in_directional_contact || in_spot_contact) {
    const vec2 contact_uv = in_directional_contact ? directional_contact_uv : spot_contact_uv;
    const bool contact_border = any(lessThan(contact_uv, vec2(0.012))) || any(greaterThan(contact_uv, vec2(0.988)));
    const vec4 contact_value = in_directional_contact
      ? vec4(vec3(texture(directional_contact_image[0], contact_uv).r), 1.0)
      : vec4(texture(spot_contact_image[0], contact_uv).rgb, 1.0);
    out_color = contact_border ? vec4(0.18, 0.86, 1.0, 1.0) : contact_value;
    return;
  }

  const vec2 sample_uv = in_directional ? directional_uv : atlas_uv;
  const bool outer_border = any(lessThan(sample_uv, vec2(0.012))) || any(greaterThan(sample_uv, vec2(0.988)));
  const bool tile_border = abs(sample_uv.x - 0.5) < 0.006 || abs(sample_uv.y - 0.5) < 0.006;
  const float depth = in_directional
    ? texture(directional_shadow_image[0], sample_uv).r
    : texture(spot_shadow_atlas[0], sample_uv).r;
  const float visible_depth = pow(clamp(depth, 0.0, 1.0), 0.35);
  out_color = (outer_border || tile_border) ? vec4(1.0, 0.78, 0.18, 1.0) : vec4(vec3(visible_depth), 1.0);
}
