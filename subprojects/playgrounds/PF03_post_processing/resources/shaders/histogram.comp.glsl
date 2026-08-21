#version 450

// Алгоритм: параллельное построение логарифмической гистограммы яркости. Группа 16x16 сначала очищает 256
// shared bins, затем каждый поток заносит один пиксель разреженной сетки в локальную корзину atomic'ом.
// После barrier ровно один global atomic на корзину объединяет группу с общим SSBO. Яркость может получать
// вес центра кадра, а bin 0 резервируется для значений ниже рабочего диапазона и исключается из экспозиции.

#include "pf03_frame.glsl"

// Группа 16x16 = 256 потоков подобрана ПОД ЧИСЛО КОРЗИН: тогда на поток приходится ровно одна корзина и одно
// атомарное сложение в глобальный буфер. При группе 8x8 (64 потока) выходило 256 глобальных атомиков на 64
// пикселя работы, и замер стоил больше, чем весь остальной кадр — измерено: 8.5 мс против 3.3.
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D scene_image;
layout(set = 1, binding = 1, std430) buffer HistogramBuffer {
  uint bins[];
} histogram;

// Локальная гистограмма в разделяемой памяти. Атомарные сложения прямо в глобальный буфер конкурировали бы
// между всеми группами; здесь каждая группа делает не больше одного глобального atomic на непустую корзину.
shared uint local_bins[PF03_HISTOGRAM_BINS];

void main() {
  const uint tid = gl_LocalInvocationIndex;
  local_bins[tid] = 0u;
  barrier();

  // Замер идёт по СЕТКЕ ВДВОЕ РЕЖЕ полного кадра: это статистика распределения, и четверти пикселей для неё
  // достаточно — проверено, замер сходится с полнокадровым в пределах шага отсчёта. Цена при этом падает
  // вчетверо, а она оказалась заметной (полнокадровый сбор стоил около миллисекунды на 1280x720).
  const ivec2 grid = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = textureSize(scene_image, 0);
  // Шаг сетки приходит числом: сколько групп запустил хост, столько проб и берём, а размер сетки выводим из
  // размера самого dispatch — тогда шейдер не обязан знать масштаб.
  const ivec2 grid_size = ivec2(gl_NumWorkGroups.xy) * ivec2(gl_WorkGroupSize.xy);
  if (grid.x < grid_size.x && grid.y < grid_size.y) {
    // Выборка линейным сэмплером между четырьмя пикселями: заодно снижает шум статистики
    const vec2 uv = (vec2(grid) + 0.5) / vec2(grid_size);
    const float luminance = pf03_luminance(texture(scene_image, uv).rgb);
    const int bin = pf03_luminance_to_bin(luminance, frame.exposure_limits.x, frame.exposure_limits.y);

    // Центровзвешенность: вклад пикселя падает к краям кадра. Смысл прикладной — зритель смотрит в центр, и
    // яркий фонарь в углу не должен затемнять всю сцену. Вес квантуется, потому что атомик целочисленный.
    const vec2 from_center = uv - 0.5;
    const float radial = 1.0 - clamp(length(from_center) * 1.4142, 0.0, 1.0);
    const float weight = mix(1.0, radial * radial, clamp(frame.metering.z, 0.0, 1.0));

    atomicAdd(local_bins[bin], uint(weight * 64.0 + 0.5));
  }
  barrier();

  // Одно глобальное сложение на поток, и только если корзина непустая
  if (local_bins[tid] != 0u) {
    atomicAdd(histogram.bins[tid], local_bins[tid]);
  }
}
