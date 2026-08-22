#version 450

// Алгоритм: CPU выполняет text layout/word wrap только в локальных координатах billboard, anchor.w выбирает
// размещение. Spherical берёт camera right/up; cylindrical сохраняет world Y и поворачивает только вокруг неё;
// screen-size сначала проецирует world anchor, затем добавляет local offset в пикселях прямо в clip XY.
// Во всех режимах anchor depth сохраняется, поэтому billboard остаётся depth-tested объектом сцены.

layout(location = 0) in vec2 in_position;
layout(location = 1) in mat4 in_glyph_local;
layout(location = 5) in vec4 in_uv_rect;
layout(location = 6) in vec4 in_fill_color;
layout(location = 7) in vec4 in_outline_color;
layout(location = 8) in vec4 in_effect;
layout(location = 9) in vec4 in_anchor;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_fill_color;
layout(location = 2) out vec4 out_outline_color;
layout(location = 3) out vec3 out_effect;
layout(location = 4) flat out uint out_atlas_index;
layout(location = 5) out vec2 out_detail_uv;
layout(location = 6) flat out uint out_detail_index;
layout(location = 7) flat out float out_detail_mix;

const float billboard_spherical = 0.0;
const float billboard_cylindrical_y = 1.0;
const float billboard_screen_size = 2.0;

void main() {
  const vec3 local = (in_glyph_local * vec4(in_position, 0.0, 1.0)).xyz;
  const mat3 camera_world = transpose(mat3(camera_data.view));
  if (in_anchor.w < billboard_cylindrical_y - 0.5) {
    const vec3 world = in_anchor.xyz + camera_world[0] * local.x + camera_world[1] * local.y;
    gl_Position = camera_data.view_projection * vec4(world, 1.0);
  } else if (in_anchor.w < billboard_screen_size - 0.5) {
    const vec3 world_up = vec3(0.0, 1.0, 0.0);
    const vec3 flat_to_camera = vec3(
      camera_data.camera_position.x - in_anchor.x,
      0.0,
      camera_data.camera_position.z - in_anchor.z);
    const vec3 right = dot(flat_to_camera, flat_to_camera) > 1e-8
      ? normalize(cross(world_up, flat_to_camera))
      : camera_world[0];
    const vec3 world = in_anchor.xyz + right * local.x + world_up * local.y;
    gl_Position = camera_data.view_projection * vec4(world, 1.0);
  } else {
    gl_Position = camera_data.view_projection * vec4(in_anchor.xyz, 1.0);
    const vec2 clip_per_pixel = vec2(2.0) / camera_data.viewport_near.xy;
    // PF05 projection flips world Y for Vulkan; local billboard coordinates remain conventional Y-up.
    gl_Position.xy += vec2(local.x, -local.y) * clip_per_pixel * gl_Position.w;
  }
  out_uv = mix(in_uv_rect.xy, in_uv_rect.zw, in_position);
  out_fill_color = in_fill_color;
  out_outline_color = in_outline_color;
  out_effect = in_effect.xyz;
  const uint packed_textures = floatBitsToUint(in_effect.w);
  out_atlas_index = packed_textures & 0xffu;
  out_detail_uv = in_position;
  out_detail_index = (packed_textures >> 8u) & 0xffu;
  out_detail_mix = float((packed_textures >> 16u) & 0xffu) / 255.0;
}
