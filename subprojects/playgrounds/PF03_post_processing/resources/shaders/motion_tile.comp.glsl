#version 450

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D motion_image;
layout(set = 1, binding = 1, rg16f) uniform writeonly image2D tile_image;

// Максимальный по модулю motion-вектор плитки. Нужен затем, что размазывать пиксель надо по НАПРАВЛЕНИЮ САМОГО
// БЫСТРОГО в окрестности, а не по своему: у неподвижного фона позади летящего объекта собственный вектор нулевой,
// и по нему собирать нечего — а размытие объекта обязано на этот фон наползать.
//
// Берётся максимум, а не среднее: среднее размывало бы быстрый объект по заниженной длине, то есть обрезало бы
// шлейф ровно там, где он должен быть длиннее всего.
void main() {
  const ivec2 tile = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 tiles = imageSize(tile_image);
  if (tile.x >= tiles.x || tile.y >= tiles.y) {
    return;
  }

  const ivec2 source_size = textureSize(motion_image, 0);
  // Размер блока выводится из ОТНОШЕНИЯ размеров, а не берётся из UBO: при TAAU motion живёт в разрешении
  // рендера, а сетка плиток — в разрешении дисплея, и константа из UBO покрывала бы не то поле.
  const ivec2 block = max(source_size / tiles, ivec2(1));
  const ivec2 base = tile * block;

  vec2 best = vec2(0.0);
  float best_length = 0.0;
  for (int y = 0; y < block.y; ++y) {
    for (int x = 0; x < block.x; ++x) {
      const ivec2 tap = clamp(base + ivec2(x, y), ivec2(0), source_size - 1);
      const vec2 motion = texelFetch(motion_image, tap, 0).rg;
      const float len = length(motion);
      if (len > best_length) {
        best_length = len;
        best = motion;
      }
    }
  }

  imageStore(tile_image, tile, vec4(best, 0.0, 0.0));
}
