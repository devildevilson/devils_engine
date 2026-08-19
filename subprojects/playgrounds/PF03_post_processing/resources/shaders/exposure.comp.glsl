#version 450

#include "pf03_frame.glsl"

// Одна рабочая группа: замер сводит весь кадр в одно число, поэтому вторая группа была бы уже
// межгрупповой редукцией без всякой пользы на этом масштабе.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D scene_image;

layout(set = 2, binding = 0, rgba16f) uniform writeonly image2D exposure_out;
// Прошлое состояние экспозиции ('history = 1'): именно из него берётся адаптация
layout(set = 2, binding = 1, rgba16f) uniform readonly image2D exposure_previous;

const uint taps_per_thread = 64u;
const uint grid = 64u; // 64x64 = 4096 проб по кадру

shared float partial_sums[64];

void main() {
  const uint tid = gl_LocalInvocationIndex;

  // Средняя яркость берётся в ЛОГАРИФМЕ, то есть считается геометрическое среднее: иначе одно яркое солнце
  // в кадре перетягивает арифметическое среднее на себя и вся сцена уходит в темноту.
  float sum = 0.0;
  for (uint i = 0; i < taps_per_thread; ++i) {
    const uint index = tid * taps_per_thread + i;
    const vec2 uv = (vec2(float(index % grid), float(index / grid)) + 0.5) / float(grid);
    const float luminance = pf03_luminance(texture(scene_image, uv).rgb);
    sum += log2(max(luminance, 1.0e-4));
  }

  partial_sums[tid] = sum;
  barrier();

  for (uint stride = 32u; stride > 0u; stride >>= 1u) {
    if (tid < stride) {
      partial_sums[tid] += partial_sums[tid + stride];
    }
    barrier();
  }

  if (tid != 0u) {
    return;
  }

  const float measured_log = partial_sums[0] / float(taps_per_thread * 64u);
  const float clamped_log = clamp(measured_log, frame.exposure_limits.x, frame.exposure_limits.y);

  // Адаптация: на первом кадре истории нет, поэтому берём замер как есть — иначе яркость поехала бы от нуля.
  const bool history_valid = frame.viewport_near.w > 1.5;
  const float previous_log = history_valid ? imageLoad(exposure_previous, ivec2(0)).x : clamped_log;

  // Экспоненциальное сглаживание по РЕАЛЬНОМУ dt, а не по кадрам: иначе скорость привыкания зависела бы
  // от частоты кадров.
  const float rate = max(frame.tonemap.z, 0.0);
  const float blend = 1.0 - exp(-rate * max(frame.tonemap.w, 0.0));
  const float adapted_log = mix(previous_log, clamped_log, clamp(blend, 0.0, 1.0));

  // Экспозиция — просто множитель, который переносит среднюю яркость сцены в «средний серый» (ключ).
  const float key = max(frame.exposure_limits.z, 1.0e-4);
  const float automatic = exp2(log2(key) - adapted_log);
  const float exposure = frame.tonemap.y > 0.0 ? frame.tonemap.y : automatic;

  imageStore(exposure_out, ivec2(0), vec4(adapted_log, exposure, measured_log, 0.0));
}
