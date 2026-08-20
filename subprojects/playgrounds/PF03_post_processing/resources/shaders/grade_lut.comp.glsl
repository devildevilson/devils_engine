#version 450

#include "pf03_grade.glsl"

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0, rgba16f) uniform writeonly image3D lut_image;

// Запекание грейда в таблицу. Ресурс — трёхмерная картинка, поэтому узел сетки адресуется напрямую тремя
// координатами: ни раскладки плитками, ни арифметики «номер плитки = синий срез» больше нет. Размер N берётся
// из самой картинки, поэтому смена размера таблицы — это пересоздание РЕСУРСА и ничего больше: ни константы
// шага, ни правки шейдера.
void main() {
  const ivec3 texel = ivec3(gl_GlobalInvocationID);
  const ivec3 size = imageSize(lut_image);
  if (any(greaterThanEqual(texel, size))) {
    return;
  }

  // Координата узла сетки в кодированном пространстве. Именно узлы, а не центры текселей: выборка в
  // pf03_sample_lut отображает вход в [0.5, N-0.5] текселя, то есть узел k соответствует значению k/(N-1).
  const vec3 encoded = vec3(texel) / vec3(size - 1);

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
