#ifndef PF03_DOF_GLSL
#define PF03_DOF_GLSL

// Общая часть depth-of-field алгоритма. Здесь физическая формула тонкой линзы переводит метры сцены и
// параметры объектива в signed circle of confusion в пикселях, отдельно задаёт конечный предел дальнего
// плана и строит равномерные Hammersley-точки диска боке. CoC pass, pyramid и gather включают этот файл,
// поэтому знак, единицы и распределение проб не могут незаметно разойтись между стадиями.

#include "pf03_frame.glsl"

// Единственный источник математики глубины резкости: и пасс CoC, и пирамида, и сбор считают одну и ту же
// функцию. Иначе «размытие не совпадает с маской» становится неотличимо от «маска посчитана неверно».

// Диаметр круга нерезкости на СЕНСОРЕ по формуле тонкой линзы:
//   A = f / N — диаметр зрачка, f — фокусное расстояние, N — диафрагменное число,
//   S — дистанция фокусировки, D — дистанция до точки.
//   coc = A * f * |D - S| / (D * (S - f))
// Формула физическая, а не «|глубина − фокус| × сила», и это не педантизм: у физической есть ЕДИНИЦЫ, поэтому
// её можно проверить (у неё есть предел на бесконечности и обратная пропорциональность диафрагме), а у ручки
// «сила размытия» проверять нечего.
//
// Знак несёт смысл: отрицательный CoC — точка БЛИЖЕ плоскости фокуса (передний план), положительный — дальше.
// Разделять их придётся, потому что ведут они себя принципиально по-разному (см. ниже).
float pf03_coc_pixels(const float distance_m, const float focus_m, const float aperture_n,
                      const float focal_mm, const float sensor_mm, const float height_px) {
  const float f = focal_mm;
  const float s = max(focus_m * 1000.0, f + 1.0e-3); // дистанция фокусировки в мм, дальше фокусного
  const float d = max(distance_m * 1000.0, 1.0);
  const float aperture = f / max(aperture_n, 1.0e-3);

  const float coc_mm = aperture * f * abs(d - s) / max(d * (s - f), 1.0e-6);
  const float coc_px = coc_mm / max(sensor_mm, 1.0e-3) * height_px;
  return d < s ? -coc_px : coc_px;
}

// Асимметрия переднего и заднего плана — не деталь реализации, а свойство формулы, и из него следует всё
// устройство эффекта:
//   при D -> бесконечность coc стремится к A*f/(S-f), то есть у ЗАДНЕГО плана размытие ОГРАНИЧЕНО;
//   при D -> f coc растёт неограниченно, то есть у ПЕРЕДНЕГО плана предела нет.
// Поэтому задний план можно собирать «на месте» ограниченным радиусом, а передний обязан РАСПЛЫВАТЬСЯ за свой
// силуэт: его пятно накрывает то, что позади, а не наоборот.
float pf03_coc_far_limit(const float focus_m, const float aperture_n, const float focal_mm,
                         const float sensor_mm, const float height_px) {
  const float f = focal_mm;
  const float s = max(focus_m * 1000.0, f + 1.0e-3);
  const float aperture = f / max(aperture_n, 1.0e-3);
  return aperture * f / max(s - f, 1.0e-6) / max(sensor_mm, 1.0e-3) * height_px;
}

// Точка на диске по низкодискрепансной паре: у боке важна РАВНОМЕРНОСТЬ покрытия, иначе при малом числе проб
// пятно распадается на отдельные точки. sqrt по радиусу — иначе пробы сгущаются к центру.
vec2 pf03_bokeh_sample(const uint index, const uint count, const float rotation) {
  const vec2 xi = pf03_hammersley(index, count);
  const float angle = 6.28318531 * fract(xi.x + rotation);
  const float radius = sqrt(clamp(xi.y, 0.0, 1.0));
  return vec2(cos(angle), sin(angle)) * radius;
}

#endif
