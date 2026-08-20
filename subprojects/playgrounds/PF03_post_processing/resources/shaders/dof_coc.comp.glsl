#version 450

#include "pf03_dof.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D depth_image;
layout(set = 1, binding = 1) uniform sampler2D normal_image;
layout(set = 1, binding = 2) uniform sampler2D motion_image;

layout(set = 2, binding = 0, rg16f) uniform writeonly image2D coc_image;

// Круг нерезкости отдельным пассом, а не внутри сбора: его читают и пирамида, и сбор, и отладочный вид, а
// считать одно и то же в трёх местах — верный способ получить «размытие не совпадает с маской».
// .r = CoC в пикселях со знаком (минус — передний план), .g = линейная глубина для depth-aware выборки.
void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(coc_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const float depth = texture(depth_image, uv).r;

  // Небо: бесконечно далеко, то есть ровно предел размытия заднего плана. Считать его через формулу нельзя —
  // глубина 0 в reverse-Z это бесконечность, и деление даст мусор.
  if (depth <= 0.0) {
    const float limit = pf03_coc_far_limit(
      frame.dof_lens.x, frame.dof_lens.y, frame.dof_lens.z, frame.dof_lens.w, frame.viewport_near.y);
    imageStore(coc_image, pixel, vec4(min(limit, frame.dof_params.z), 1.0e6, 0.0, 0.0));
    return;
  }

  const float linear_depth = pf03_linear_depth(depth, frame.viewport_near.z);
  const float coc = pf03_coc_pixels(
    linear_depth, frame.dof_lens.x, frame.dof_lens.y, frame.dof_lens.z, frame.dof_lens.w,
    frame.viewport_near.y);

  // Ограничение сверху — это бюджет сбора, а не физика: пятно шире предела собрать нечем, и молчаливое
  // расширение радиуса означало бы, что проб на пиксель не хватает и боке распадается на точки.
  const float limited = clamp(coc, -frame.dof_params.z, frame.dof_params.z);
  imageStore(coc_image, pixel, vec4(limited, linear_depth, 0.0, 0.0));
}
