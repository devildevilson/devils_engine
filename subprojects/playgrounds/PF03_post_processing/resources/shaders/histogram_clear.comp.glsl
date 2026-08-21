#version 450

// Алгоритм: явное обнуление 256 корзин перед новым histogram build. Одна группа из 64 потоков пишет по четыре
// uint на поток, поэтому очищается весь буфер, а не только первая четверть. Отдельный shader/step важен для
// render graph: переход storage_write -> general создаёт настоящий барьер перед atomic build и не позволяет
// очистке пересечься с использованием данных прошлого кадра.

#include "pf03_frame.glsl"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0, std430) writeonly buffer HistogramBuffer {
  uint bins[];
} histogram;

// Обнуление отдельным шагом СПЕЦИАЛЬНО: юсадж записи отличается от юсаджа чтения в разборе, и только поэтому
// движок ставит между ними барьер. Обнули разбор сам — он бы читал и писал один юсадж, барьера бы не было, и
// обнуление могло бы обогнать чтение.
void main() {
  // Группа из 64 потоков на 256 корзин: каждый поток обнуляет четыре. Без цикла обнулялась бы только первая
  // четверть, остальные корзины копили бы значения между кадрами — а это тихо ломает перцентили, потому что
  // общая сумма распределения растёт от кадра к кадру.
  const uint per_thread = uint(PF03_HISTOGRAM_BINS) / 64u;
  const uint first = gl_GlobalInvocationID.x * per_thread;
  for (uint i = 0; i < per_thread; ++i) {
    histogram.bins[first + i] = 0u;
  }
}
