#version 450

#include "pf03_frame.glsl"

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(location = 0) in vec3 world_normal;
layout(location = 1) in vec4 previous_clip;

// Порядок location = порядок ресурсов в render target 'gbuffer_target'
layout(location = 0) out vec2 out_normal;
layout(location = 1) out vec2 out_motion;

void main() {
  out_normal = pf03_encode_normal(normalize(world_normal));

  // Текущий uv берём из gl_FragCoord, а не из интерполированного клипа: это точный центр пикселя, и
  // ошибка репроекции тогда меряет motion, а не погрешность интерполяции.
  const vec2 current_uv = gl_FragCoord.xy / frame.viewport_near.xy;
  const vec2 previous_uv = (previous_clip.xy / previous_clip.w) * 0.5 + 0.5;

  // Договор: motion — это СМЕЩЕНИЕ К прошлому кадру, поэтому читатель делает uv + motion
  out_motion = previous_uv - current_uv;
}
