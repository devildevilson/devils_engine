#version 450

#include "pf03_grade.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D lut_image;

// Запекание грейда в таблицу. Раскладка — полоса: ширина = N*N, высота = N, плитка на каждый синий срез.
// Размер N берётся из самой картинки (высота), поэтому смена размера таблицы — это пересоздание РЕСУРСА и
// ничего больше: ни константы шага, ни правки шейдера.
void main() {
  const ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(lut_image);
  if (texel.x >= size.x || texel.y >= size.y) {
    return;
  }

  const float grid = float(size.y);
  const float last = grid - 1.0;
  const int slice = texel.x / size.y;      // индекс синего среза = номер плитки
  const int red = texel.x - slice * size.y;

  // Координата узла сетки в кодированном пространстве. Именно узлы, а не центры текселей: выборка в
  // pf03_sample_lut отображает вход в [0.5, N-0.5] текселя, то есть узел k соответствует значению k/(N-1).
  const vec3 encoded = vec3(float(red), float(texel.y), float(slice)) / last;

  const int shaper = int(frame.lut_params.y + 0.5);
  const float min_stop = frame.lut_params.z;
  const float max_stop = frame.lut_params.w;
  const bool display_referred = frame.grade_tone.w > 0.5;

  const pf03_grade_params params = pf03_make_grade_params(
    frame.grade_balance, frame.grade_tone, frame.grade_slope, frame.grade_offset, frame.grade_power,
    frame.grade_filter);

  vec3 result;
  if (display_referred) {
    // display-referred таблица: область определения уже ограничена кривой, поэтому shaper не нужен —
    // вход и выход это [0,1]. Ровно так работали «LUT'ы цветокоррекции» в движках до HDR-тракта.
    result = clamp(pf03_apply_grade(encoded, params), vec3(0.0), vec3(1.0));
  } else {
    // scene-referred: узел сетки декодируется в линейный HDR, грейдится и кодируется обратно ТЕМ ЖЕ
    // shaper'ом. Кодировка выхода здесь принципиальна: при нейтральном грейде таблица становится тождеством
    // В КОДИРОВАННЫХ координатах, а трилинейная интерполяция воспроизводит линейную функцию точно — значит
    // тождество проходит через таблицу без ошибки, и любая измеренная ошибка это баг, а не «так и должно».
    // Если бы выход хранился линейным, интерполяция экспоненты между узлами давала бы систематический
    // перекос вверх даже у тождества.
    const vec3 linear = pf03_shaper_decode(encoded, min_stop, max_stop, shaper);
    const vec3 graded = max(pf03_apply_grade(linear, params), vec3(0.0));
    result = pf03_shaper_encode(graded, min_stop, max_stop, shaper);
  }

  imageStore(lut_image, texel, vec4(result, 1.0));
}
