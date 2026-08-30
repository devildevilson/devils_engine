#version 450
layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 planet_to_world;
  mat4 world_to_planet;
  vec4 camera_position;
  vec4 light_direction;
  vec4 border_colour;
  uvec4 params;
  vec4 viewport_near;
} camera_data;
struct label_glyph {
  vec4 direction_height;
  vec4 pixel_rect;
  vec4 uv_rect;
  uvec4 params;
};
layout(set = 0, binding = 1, std430) readonly buffer LabelGlyphs { label_glyph glyphs[]; } label_data;
layout(location = 0) out vec2 out_uv;
layout(location = 1) flat out uint out_texture;

void main() {
  const label_glyph glyph = label_data.glyphs[gl_InstanceIndex];
  const vec2 corners[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(1, 1),
                                   vec2(0, 0), vec2(1, 1), vec2(0, 1));
  const vec2 corner = corners[gl_VertexIndex];
  const vec3 local_anchor = glyph.direction_height.xyz * (1.0 + glyph.direction_height.w);
  const vec3 world_anchor = (camera_data.planet_to_world * vec4(local_anchor, 1.0)).xyz;
  const vec3 world_radial = normalize((camera_data.planet_to_world * vec4(glyph.direction_height.xyz, 0.0)).xyz);
  if (dot(world_radial, camera_data.camera_position.xyz - world_anchor) <= 0.04) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  } else {
    gl_Position = camera_data.view_projection * vec4(world_anchor, 1.0);
    const vec2 pixel = glyph.pixel_rect.xy + corner * glyph.pixel_rect.zw;
    gl_Position.xy += vec2(pixel.x, -pixel.y) * (2.0 / camera_data.viewport_near.xy) * gl_Position.w;
  }
  out_uv = mix(glyph.uv_rect.xy, glyph.uv_rect.zw, corner);
  out_texture = glyph.params.x;
}
