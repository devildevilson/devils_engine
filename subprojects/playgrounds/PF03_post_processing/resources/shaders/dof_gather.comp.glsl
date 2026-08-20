#version 450

#include "pf03_dof.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Число проб — тир качества, поэтому specialization-константа. Второй константой переключается использование
// пирамиды: она нужна не для скорости, а для покрытия пятна при том же числе проб, и это утверждение надо было
// проверить, а не принять на веру.
layout(constant_id = 0) const int dof_taps = 24;
layout(constant_id = 1) const int dof_use_pyramid = 1;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D scene_image;   // накопленный кадр в линейном HDR
layout(set = 1, binding = 1) uniform sampler2D coc_image;     // .r = CoC со знаком, .g = линейная глубина
layout(set = 1, binding = 2) uniform sampler2D pyramid_image; // .rgb = цвет, .a = средний |CoC|

layout(set = 2, binding = 0, rgba16f) uniform writeonly image2D result_image;

// Сбор устроен как РАССЕЯНИЕ, вывернутое наизнанку: свет точки размазывается по её собственному кругу
// нерезкости, поэтому центр получает вклад от тех проб, чьё пятно его накрывает — условие `r <= |CoC(проба)|`,
// а не `r <= |CoC(центра)|`. Именно из-за этой разницы передний план и умеет расплываться за свой силуэт.
//
// Аккумуляторов два, и это не оптимизация:
//   ЗАДНИЙ план (CoC > 0) обязан уважать силуэты: проба, которая БЛИЖЕ центра, физически им перекрыта, и без
//   этой проверки размытый фон протекает на резкий передний план — та же ошибка, что у bilinear в AO;
//   ПЕРЕДНИЙ план (CoC < 0) наоборот обязан протекать: его пятно накрывает то, что позади, и запрещать это
//   значит обрезать эффект по силуэту, чего в оптике не бывает.
void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(result_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const vec4 center_scene = texture(scene_image, uv);

  const float strength = frame.dof_params.x;
  if (strength <= 0.0) {
    imageStore(result_image, pixel, center_scene);
    return;
  }

  const vec2 center_coc = texture(coc_image, uv).rg;
  const float coc_center = center_coc.r;
  const float depth_center = center_coc.g;

  const int levels = int(frame.dof_params.w + 0.5);
  // Оценка размытия ОКРУГИ берётся из альфы верхнего уровня пирамиды: там лежит средний |CoC| по большому
  // блоку. Нужна она затем, что радиус сбора у переднего плана определяется не центром, а тем, есть ли рядом
  // размытая близкая поверхность — центр-то может быть идеально резким. Это приближение (среднее вместо
  // максимума), и оно занижает вылет пятна у тонких близких объектов.
  const float neighbourhood = texture(pyramid_image, uv).a;
  const float radius = min(max(abs(coc_center), neighbourhood), frame.dof_params.z);

  // Меньше половины пикселя — размывать нечего, и кадр обязан пройти насквозь без изменений
  if (radius < 0.5) {
    imageStore(result_image, pixel, center_scene);
    return;
  }

  const uint taps = uint(max(dof_taps, 1));
  // Уровень пирамиды выбирается так, чтобы РАССТОЯНИЕ МЕЖДУ ПРОБАМИ было порядка текселя уровня: тогда пробы
  // покрывают диск без дыр. Пирамида начинается с половинного разрешения, отсюда деление на два.
  float level = 0.0;
  if (dof_use_pyramid != 0) {
    const float spacing = radius / max(sqrt(float(taps)), 1.0);
    level = clamp(log2(max(spacing * 0.5, 1.0)), 0.0, float(levels - 1));
  }

  // Вращение диска своё у каждого пикселя и сдвинуто по кадрам: одинаковый набор направлений даёт не шум, а
  // жирные полосы, а временной сдвиг усредняет накопление TAA (тот же приём, что у выборки SSAO).
  const float rotation = fract(pf03_gradient_noise(vec2(pixel)) + frame.taa_params.w);

  vec3 far_color = vec3(0.0);
  float far_weight = 0.0;
  vec3 near_color = vec3(0.0);
  float near_weight = 0.0;

  for (uint i = 0u; i < taps; ++i) {
    const vec2 offset = pf03_bokeh_sample(i, taps, rotation) * radius;
    const vec2 tap_uv = uv + offset / vec2(size);
    if (any(lessThan(tap_uv, vec2(0.0))) || any(greaterThan(tap_uv, vec2(1.0)))) {
      continue;
    }

    const vec2 tap_coc = texture(coc_image, tap_uv).rg;
    const float tap_radius = abs(tap_coc.r);
    const float distance_px = length(offset);

    // Пятно пробы не достаёт до центра — вклада нет. Это и есть рассеяние наизнанку.
    if (distance_px > tap_radius) {
      continue;
    }

    const vec3 tap_color = dof_use_pyramid != 0
      ? textureLod(pyramid_image, tap_uv, level).rgb
      : texture(scene_image, tap_uv).rgb;

    // Вес обратно пропорционален площади пятна: свет точки распределяется по всему кругу, а не копируется в
    // каждый его пиксель. Без этого крупные пятна вытесняют мелкие и картинка «выцветает» к размытому.
    const float weight = 1.0 / max(tap_radius * tap_radius, 1.0);

    if (tap_coc.r < 0.0) {
      near_color += tap_color * weight;
      near_weight += weight;
    } else if (frame.dof_params.y < 0.5 || tap_coc.g >= depth_center - 0.05) {
      // Задний план: проба не ближе центра, значит она им не перекрыта. Проверку можно выключить ручкой —
      // ровно затем, чтобы измерить, сколько фона протекает на резкий передний план без неё.
      far_color += tap_color * weight;
      far_weight += weight;
    }
  }

  // Резкий центр входит в задний аккумулятор всегда: иначе у пикселя, вокруг которого нет ни одной подходящей
  // пробы, не осталось бы ничего.
  const float center_weight = 1.0 / max(abs(coc_center) * abs(coc_center), 1.0);
  far_color += center_scene.rgb * center_weight;
  far_weight += center_weight;

  const vec3 far_result = far_color / max(far_weight, 1.0e-6);
  const vec3 near_result = near_color / max(near_weight, 1.0e-6);

  // Доля переднего плана: сколько проб переднего плана накрыло центр. Нормируется числом проб, поэтому у
  // полностью накрытого центра получается единица, а у края пятна — плавный переход.
  const float near_coverage = clamp(near_weight / max(center_weight * 0.5 + near_weight, 1.0e-6), 0.0, 1.0);

  const vec3 blurred = mix(far_result, near_result, near_coverage);
  const vec3 result = mix(center_scene.rgb, blurred, clamp(strength, 0.0, 1.0));

  // Альфа несёт пропускание тумана дальше по цепочке — её размывать нельзя, иначе god rays поедут
  imageStore(result_image, pixel, vec4(result, center_scene.a));
}
