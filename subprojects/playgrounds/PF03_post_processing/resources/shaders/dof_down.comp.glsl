#version 450

// Алгоритм: построение цветовой пирамиды для depth of field. Первый уровень совместно уменьшает temporal
// HDR кадр и карту CoC, следующие уровни усредняют предыдущий; RGB несёт свет, alpha — средний модуль CoC.
// Пирамида нужна для равномерного покрытия большого боке небольшим числом проб: gather выбирает LOD так,
// чтобы расстояние между пробами соответствовало размеру текселя, а не оставляло дырявый диск.

#include "pf03_dof.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Нулевой уровень берётся из кадра и маски CoC, остальные — из предыдущего уровня пирамиды. Различает их
// specialization-константа: биндинги у обоих вариантов одни и те же, поэтому шейдер один (в отличие от
// пирамиды глубины, где источники были разными ресурсами).
layout(constant_id = 0) const int dof_from_frame = 0;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D source_image; // кадр либо предыдущий уровень
layout(set = 1, binding = 1) uniform sampler2D coc_image;
layout(set = 1, binding = 2, rgba16f) uniform writeonly image2D target_image;

// Пирамида цвета с весом по CoC. Смысл не в экономии: при одинаковом числе проб сбор с УРОВНЯ покрывает всю
// площадь пятна, а сбор с нулевого уровня той же горсткой проб покрывает её дырками — боке распадается на
// отдельные точки. То есть пирамида здесь покупает КАЧЕСТВО при фиксированной цене, а не скорость.
//
// В альфе несётся |CoC| в пикселях: усреднять его надо вместе с цветом, иначе на уровне выше маска и цвет
// разъезжаются, и размытие вылезает за пределы своей области.
void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(target_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);

  if (dof_from_frame != 0) {
    // Половинное разрешение: четыре отсчёта кадра и четыре отсчёта маски, усреднённые вместе
    const ivec2 source_size = textureSize(source_image, 0);
    const ivec2 base = pixel * 2;
    vec3 color = vec3(0.0);
    float coc = 0.0;
    for (int y = 0; y <= 1; ++y) {
      for (int x = 0; x <= 1; ++x) {
        const ivec2 tap = clamp(base + ivec2(x, y), ivec2(0), source_size - 1);
        color += texelFetch(source_image, tap, 0).rgb;
        coc += abs(texelFetch(coc_image, tap, 0).r);
      }
    }
    imageStore(target_image, pixel, vec4(color * 0.25, coc * 0.25));
    return;
  }

  // Дальше обычное понижение вдвое; фильтр — четыре билинейных отсчёта по углам, то есть эффективно 2x2 блока
  const vec2 texel = 1.0 / vec2(size);
  vec4 sum = vec4(0.0);
  sum += texture(source_image, uv + vec2(-0.25, -0.25) * texel * 2.0);
  sum += texture(source_image, uv + vec2( 0.25, -0.25) * texel * 2.0);
  sum += texture(source_image, uv + vec2(-0.25,  0.25) * texel * 2.0);
  sum += texture(source_image, uv + vec2( 0.25,  0.25) * texel * 2.0);
  imageStore(target_image, pixel, sum * 0.25);
}
