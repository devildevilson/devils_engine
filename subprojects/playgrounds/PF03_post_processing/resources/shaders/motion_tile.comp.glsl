#version 450

// Алгоритм: свёртка full/render-resolution motion field в сетку tile-max. Каждая output tile получает вектор
// с наибольшей длиной из своей точной прямоугольной доли источника; границы считаются рациональными долями,
// поэтому при дробном TAAU масштабе покрываются и остаточные строки/столбцы. Максимум, а не среднее, сохраняет
// настоящий размах самого быстрого объекта для следующего расширения и blur gather.

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
  // Границы выводятся как две целочисленные доли, а не как `block = source_size / tiles`. При дробном
  // render scale размеры не обязаны делиться на сетку плиток: один усечённый block оставлял правый и нижний
  // хвосты motion buffer вообще непрочитанными (при 0.67 — десятки пикселей). Эта раскладка покрывает каждый
  // исходный тексель ровно одной плиткой при любом отношении размеров.
  const ivec2 begin = tile * source_size / tiles;
  const ivec2 end = (tile + 1) * source_size / tiles;

  vec2 best = vec2(0.0);
  float best_length = 0.0;
  for (int y = begin.y; y < end.y; ++y) {
    for (int x = begin.x; x < end.x; ++x) {
      const ivec2 tap = ivec2(x, y);
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
