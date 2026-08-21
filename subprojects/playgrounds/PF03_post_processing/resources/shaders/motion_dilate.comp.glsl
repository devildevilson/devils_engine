#version 450

// Алгоритм: расширение tile-max velocity на окрестность 3x3. В каждой выходной плитке сохраняется самый
// длинный вектор среди неё и соседей, чтобы пиксели неподвижного фона заранее знали о быстром объекте рядом.
// Без этого следующий motion-blur gather обрывает шлейф строго по границе 16x16 tile — технической сетке,
// которая не имеет никакого отношения к движению сцены.

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D tile_image;
layout(set = 1, binding = 1, rg16f) uniform writeonly image2D dilated_image;

// Расширение максимума на соседние плитки. Без этого шлейф обрывается на границе плитки, в которой объект
// находится: пиксель соседней плитки не знает, что рядом кто-то летит, и остаётся резким — получается ступенька
// ровно по сетке плиток, то есть артефакт РЕАЛИЗАЦИИ, а не оптики.
void main() {
  const ivec2 tile = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 tiles = imageSize(dilated_image);
  if (tile.x >= tiles.x || tile.y >= tiles.y) {
    return;
  }

  vec2 best = vec2(0.0);
  float best_length = 0.0;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      const ivec2 tap = clamp(tile + ivec2(x, y), ivec2(0), tiles - 1);
      const vec2 motion = texelFetch(tile_image, tap, 0).rg;
      const float len = length(motion);
      if (len > best_length) {
        best_length = len;
        best = motion;
      }
    }
  }

  imageStore(dilated_image, tile, vec4(best, 0.0, 0.0));
}
