#version 450

// Алгоритм: native Nuklear выдаёт общий pixel-space stream для N окон, а CPU добавляет каждой вершине window_id.
// По id shader читает world anchor, pixel offset и distance policy. Anchor проецируется обычной camera VP; размер
// окна ведёт себя как perspective scale reference_distance/depth, но зажат min/max, чтобы вблизи окно не захватило
// экран, а вдали не стало нечитаемой точкой. После fade_start alpha плавно уходит в ноль к fade_end. Pixel offset
// прибавляется к clip.xy с множителем clip.w, поэтому anchor сохраняет честную world depth. Весь stream остаётся
// batched; отдельные draw commands нужны только там, где Nuklear меняет texture.

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in uint in_window_id;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

struct WindowTransform {
  vec4 anchor_fade_end;
  vec4 pixel_offset_size;
  vec4 distance_policy;
};

layout(set = 1, binding = 0, std430) readonly buffer WorldUiTransforms {
  WindowTransform windows[];
} world_ui;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) out float out_fade;

void main() {
  const WindowTransform window = world_ui.windows[in_window_id];
  const vec4 anchor_clip = camera_data.view_projection * vec4(window.anchor_fade_end.xyz, 1.0);
  const float view_depth = -(camera_data.view * vec4(window.anchor_fade_end.xyz, 1.0)).z;
  const float fade_end = window.anchor_fade_end.w;
  const float fade_start = window.distance_policy.w;
  const float scale = clamp(
    window.distance_policy.x / max(view_depth, 0.0001),
    window.distance_policy.y,
    window.distance_policy.z);
  out_fade = 1.0 - smoothstep(fade_start, fade_end, view_depth);
  if (fade_end <= 0.0 || anchor_clip.w <= 0.0 || out_fade <= 0.0) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  } else {
    const vec2 local_pixel = (in_position + window.pixel_offset_size.xy) * scale;
    const vec2 clip_offset = local_pixel / camera_data.viewport_near.xy * 2.0 * anchor_clip.w;
    gl_Position = vec4(anchor_clip.xy + clip_offset, anchor_clip.z, anchor_clip.w);
  }
  out_uv = in_uv;
  out_color = in_color;
}
