#ifndef PF03_GRADE_GLSL
#define PF03_GRADE_GLSL

#include "pf03_frame.glsl"

// Единственный источник математики грейда: и пасс запекания LUT, и аналитический путь в компоновке считают
// ОДНУ И ТУ ЖЕ функцию. Это не удобство, а условие измеримости: разница между табличным и аналитическим
// путём тогда равна ровно ошибке табулирования, и её можно померить, а не оценить.

struct pf03_grade_params {
  vec3 slope;         // ASC CDL slope (усиление)
  vec3 offset;        // ASC CDL offset (подъём)
  vec3 power;         // ASC CDL power (гамма)
  vec3 filter_color;  // цветной фильтр перед объективом
  float filter_strength;
  float temperature;  // объявленная температура освещения сцены, K (6500 — нейтрально)
  float tint;         // сдвиг по оси зелёный–пурпурный, [-1, 1]
  float contrast;     // контраст вокруг опорной точки (1 — нейтрально)
  float pivot;        // опорная точка контраста (средний серый)
  float saturation;   // насыщенность относительно яркости (1 — нейтрально)
  float naive_balance; // > 0.5 — наивный баланс белого усилением каналов (для A/B)
};

pf03_grade_params pf03_make_grade_params(
  const vec4 balance, const vec4 tone, const vec4 slope, const vec4 offset, const vec4 power, const vec4 filt) {
  pf03_grade_params p;
  p.temperature = balance.x;
  p.tint = balance.y;
  p.naive_balance = balance.z;
  p.contrast = balance.w;
  p.saturation = tone.x;
  p.pivot = tone.y;
  p.slope = slope.xyz;
  p.offset = offset.xyz;
  p.power = power.xyz;
  p.filter_color = filt.xyz;
  p.filter_strength = filt.w;
  return p;
}

// Rec.709/sRGB (D65) <-> CIE XYZ. Столбцы, а не строки: GLSL конструирует матрицу по столбцам.
const mat3 pf03_rgb_to_xyz = mat3(
  0.4124564, 0.2126729, 0.0193339,
  0.3575761, 0.7151522, 0.1191920,
  0.1804375, 0.0721750, 0.9503041);
const mat3 pf03_xyz_to_rgb = mat3(
   3.2404542, -0.9692660,  0.0556434,
  -1.5371385,  1.8760108, -0.2040259,
  -0.4985314,  0.0415560,  1.0572252);

// Bradford: пространство хроматической адаптации. Баланс белого — это адаптация зрителя к освещению, и
// делать её умножением RGB нельзя: RGB-каналы не соответствуют конусным откликам, поэтому «потепление»
// заодно сдвигает яркость. Здесь ровно та ловушка, которую площадка и меряет (см. naive_balance).
const mat3 pf03_bradford = mat3(
   0.8951, -0.7502,  0.0389,
   0.2664,  1.7135, -0.0685,
  -0.1614,  0.0367,  1.0296);
const mat3 pf03_bradford_inv = mat3(
   0.9869929, 0.4323053, -0.0085287,
  -0.1470543, 0.5183603,  0.0400428,
   0.1599627, 0.0492912,  0.9684867);

// Планковский локус в CIE xy (кусочное приближение Kim et al.): цветность излучателя заданной температуры.
// Нужен именно локус, а не «побольше красного»: температура — физическая величина, и по ней должна получаться
// цветность реального источника, иначе 3200 K в площадке и 3200 K в референсе — разные цвета.
vec2 pf03_planckian_xy(const float kelvin) {
  const float t = clamp(kelvin, 1667.0, 25000.0);
  const float u = 1000.0 / t; // обратные килокельвины: коэффициенты приближения даны именно в них
  const float u2 = u * u;
  const float u3 = u2 * u;

  const float x = t <= 4000.0
    ? (-0.2661239 * u3 - 0.2343589 * u2 + 0.8776956 * u + 0.179910)
    : (-3.0258469 * u3 + 2.1070379 * u2 + 0.2226347 * u + 0.240390);

  const float x2 = x * x;
  const float x3 = x2 * x;
  float y;
  if (t <= 2222.0) {
    y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
  } else if (t <= 4000.0) {
    y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
  } else {
    y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
  }
  return vec2(x, y);
}

vec3 pf03_xy_to_xyz(const vec2 xy) {
  return vec3(xy.x / max(xy.y, 1.0e-4), 1.0, (1.0 - xy.x - xy.y) / max(xy.y, 1.0e-4));
}

// Оттенок (tint) двигает белую точку ПЕРПЕНДИКУЛЯРНО локусу: вдоль локуса лежит температура, и смешивать эти
// две ручки значит сделать их неразделимыми. Касательная берётся численно — локус задан приближением, и его
// аналитическая производная точнее не будет.
vec2 pf03_white_point_xy(const float kelvin, const float tint) {
  const vec2 center = pf03_planckian_xy(kelvin);
  if (abs(tint) < 1.0e-5) {
    return center;
  }
  const vec2 tangent = normalize(pf03_planckian_xy(kelvin * 1.02) - pf03_planckian_xy(kelvin * 0.98));
  const vec2 normal = vec2(-tangent.y, tangent.x);
  return center + normal * tint * 0.05;
}

// Баланс белого по фон Крису: объявленная температура освещения приводится к D65 (пространство рендера).
// Матрица нормируется так, чтобы ЯРКОСТЬ нейтрали не менялась — иначе ручка баланса белого начинает спорить
// с автоэкспозицией, и замер отматывает обратно то, что грейд только что сделал.
vec3 pf03_white_balance(const vec3 color, const pf03_grade_params p) {
  if (p.naive_balance > 0.5) {
    // Наивная реализация «на глаз»: температура как усиление R и B напрямую. Направление то же, что у
    // честной, поэтому сравнение идёт именно про яркость, а не про цвет.
    const float k = clamp((6500.0 - p.temperature) / 6500.0, -1.0, 1.0);
    return color * vec3(1.0 - k, 1.0 + p.tint * 0.3, 1.0 + k);
  }

  const vec3 source_lms = pf03_bradford * pf03_xy_to_xyz(pf03_white_point_xy(p.temperature, p.tint));
  const vec3 target_lms = pf03_bradford * pf03_xy_to_xyz(pf03_planckian_xy(6500.0));
  const vec3 gain = target_lms / max(source_lms, vec3(1.0e-5));

  const mat3 adapt = pf03_xyz_to_rgb * pf03_bradford_inv * mat3(
    gain.x, 0.0, 0.0,
    0.0, gain.y, 0.0,
    0.0, 0.0, gain.z) * pf03_bradford * pf03_rgb_to_xyz;

  const vec3 neutral = adapt * vec3(1.0);
  return (adapt * color) / max(pf03_luminance(neutral), 1.0e-4);
}

// Контраст вокруг опорной точки. Записан в виде степени от отношения к опорной точке НЕ случайно: это ровно
// линейное растяжение в логарифмическом пространстве, то есть контраст в стопах, а не «умножить и обрезать».
// Поэтому он безопасен в неограниченном HDR: яркая часть кадра не выезжает в бесконечность скачком.
vec3 pf03_grade_contrast(const vec3 color, const float contrast, const float pivot) {
  const float safe_pivot = max(pivot, 1.0e-4);
  return safe_pivot * pow(max(color / safe_pivot, vec3(0.0)), vec3(contrast));
}

// Полный грейд. Порядок не произволен:
//   1. баланс белого — свойство ОСВЕЩЕНИЯ, поэтому идёт первым: всё дальнейшее считает свет уже нейтральным;
//   2. цветной фильтр — это стекло перед объективом, то есть тоже часть съёмки, а не обработки;
//   3. контраст вокруг среднего серого — определяет, что считать средними тонами;
//   4. slope/offset/power (ASC CDL) — стандартный набор колориста, действует на уже расставленные тона;
//   5. насыщенность — ПОСЛЕДНЕЙ, потому что она определена относительно яркости, а каждый предыдущий шаг
//      яркость меняет; поставь её раньше — и её действие зависело бы от остальных ручек.
// Функция сознательно НЕ обрезает отрицательные значения: выход за гамму — измеряемое свойство грейда, и вид
// PF03_DEBUG_GAMUT показывает именно его. Обрезка делается в точке применения.
vec3 pf03_apply_grade(const vec3 color, const pf03_grade_params p) {
  vec3 c = pf03_white_balance(color, p);
  c *= mix(vec3(1.0), p.filter_color, p.filter_strength);
  c = pf03_grade_contrast(c, p.contrast, p.pivot);
  c = pow(max(c * p.slope + p.offset, vec3(0.0)), p.power);
  return mix(vec3(pf03_luminance(c)), c, p.saturation);
}

// ---------------------------------------------------------------------------------------------------------
// Табулирование грейда: shaper и LUT-полоса
// ---------------------------------------------------------------------------------------------------------

// Shaper — это КОДИРОВКА ОБЛАСТИ ОПРЕДЕЛЕНИЯ таблицы, а не пространство грейда. Грейд считается в линейном
// scene-referred всегда; вопрос лишь в том, как разложить сетку таблицы по неограниченному диапазону.
// log2-кодировка кладёт узлы равномерно по СТОПАМ, линейная — равномерно по значению, из-за чего почти вся
// сетка уходит в верхний стоп, а тени остаются с одним-двумя узлами (это измерено).
#define PF03_SHAPER_LOG2   0
#define PF03_SHAPER_LINEAR 1

vec3 pf03_shaper_encode(const vec3 color, const float min_stop, const float max_stop, const int mode) {
  if (mode == PF03_SHAPER_LINEAR) {
    return clamp(color / exp2(max_stop), vec3(0.0), vec3(1.0));
  }
  const vec3 stops = log2(max(color, vec3(exp2(min_stop))));
  return clamp((stops - min_stop) / max(max_stop - min_stop, 1.0e-4), vec3(0.0), vec3(1.0));
}

vec3 pf03_shaper_decode(const vec3 encoded, const float min_stop, const float max_stop, const int mode) {
  if (mode == PF03_SHAPER_LINEAR) {
    return encoded * exp2(max_stop);
  }
  return exp2(encoded * (max_stop - min_stop) + min_stop);
}

// Выборка 3D-таблицы, разложенной ПОЛОСОЙ: ширина = N*N (плитка на каждый синий срез), высота = N. Полоса, а
// не 3D-картинка, потому что ресурсы render graph пока только двумерные (записано как пробел движка).
//
// Ключевая деталь — координата внутри плитки обязана лежать в ЦЕНТРАХ текселей: r отображается в
// [0.5, N-0.5] текселя своей плитки, поэтому билинейная фильтрация никогда не заходит в соседний СИНИЙ срез.
// Без этого таблица тихо смешивала бы несмежные цвета по краям красной оси, а выглядело бы это «почти
// правильно». По синей оси интерполяция делается руками — двумя выборками и mix, что и даёт ровно трилинейную.
vec3 pf03_sample_lut(const sampler2D lut, const vec3 encoded, const float size) {
  const vec3 c = clamp(encoded, vec3(0.0), vec3(1.0));
  const float last = size - 1.0;

  const float blue = c.b * last;
  const float slice_low = floor(blue);
  const float slice_high = min(slice_low + 1.0, last);
  const float blend = blue - slice_low;

  // смещение внутри плитки; полная ширина полосы = size * size
  const float u = (c.r * last + 0.5) / (size * size);
  const float v = (c.g * last + 0.5) / size;

  const vec3 low = texture(lut, vec2(u + slice_low / size, v)).rgb;
  const vec3 high = texture(lut, vec2(u + slice_high / size, v)).rgb;
  return mix(low, high, blend);
}

#endif
