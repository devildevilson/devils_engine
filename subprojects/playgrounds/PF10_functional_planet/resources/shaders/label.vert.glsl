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
  mat4 inverse_view_projection;
} camera_data;

struct DecalGlyph {
  mat4 decal_to_planet;
  mat4 planet_to_decal;
  vec4 uv_rect;
  vec4 fill;
  vec4 effect;
};
layout(set = 0, binding = 1, std430) readonly buffer LabelGlyphs { DecalGlyph glyphs[]; } label_data;

layout(location = 0) flat out mat4 out_planet_to_decal;
layout(location = 4) flat out vec4 out_uv_rect;
layout(location = 5) flat out vec4 out_fill;
layout(location = 6) flat out vec4 out_effect;
layout(location = 7) flat out vec3 out_projection_normal;

void main() {
  const DecalGlyph glyph = label_data.glyphs[gl_InstanceIndex];
  // The cap is deliberately wider than the authoritative local [-.5,.5] clip: on a curved receiver the
  // outward cap and reconstructed surface do not have identical perspective bounds near a glyph edge.
  const vec2 corners[6] = vec2[6](vec2(-0.82, -0.82), vec2(0.82, -0.82), vec2(0.82, 0.82),
                                   vec2(-0.82, -0.82), vec2(0.82, 0.82), vec2(-0.82, 0.82));
  const vec3 local_anchor = glyph.decal_to_planet[3].xyz;
  const vec3 world_anchor = (camera_data.planet_to_world * vec4(local_anchor, 1.0)).xyz;
  const vec3 world_radial = normalize((camera_data.planet_to_world * vec4(glyph.decal_to_planet[2].xyz, 0.0)).xyz);
  const bool province_lod = length(camera_data.camera_position.xyz) < 1.72;
  const uint owner = floatBitsToUint(glyph.effect.w);
  // Land province IDs occupy class 00. State labels use the explicit 01 marker and belong to the far LOD;
  // treating every clipped label as provincial would cull all state glyphs as soon as clipping was enabled.
  const bool glyph_is_province = owner != 0xffffffffu && (owner & 0xc0000000u) == 0u;
  if (province_lod != glyph_is_province || dot(world_radial, camera_data.camera_position.xyz - world_anchor) <= 0.015) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  } else {
    const vec2 corner = corners[gl_VertexIndex];
    // This cap only bounds fragment work; with depth testing disabled it can sit through the receiver's
    // centre plane, avoiding near-camera parallax between a raised cap and the reconstructed surface.
    const vec3 planet_position = (glyph.decal_to_planet * vec4(corner, 0.0, 1.0)).xyz;
    gl_Position = camera_data.view_projection * camera_data.planet_to_world * vec4(planet_position, 1.0);
  }
  out_planet_to_decal = glyph.planet_to_decal;
  out_uv_rect = glyph.uv_rect;
  out_fill = glyph.fill;
  out_effect = glyph.effect;
  out_projection_normal = normalize(glyph.decal_to_planet[2].xyz);
}
