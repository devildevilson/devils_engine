#version 450

// Алгоритм: свёртка 256-bin логарифмической гистограммы в одно состояние автоэкспозиции. Нижний и верхний
// перцентили отбрасывают соответственно тени и яркие выбросы, оставшийся диапазон усредняется в стопах.
// Результат экспоненциально адаптируется к истории с разными скоростями для света и темноты, после чего
// превращается в линейный множитель, ставящий выбранную яркость на middle grey. Весь pass — одна группа.

#include "pf03_frame.glsl"

// Одна группа: разбор сводит гистограмму в одно число
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0, std430) readonly buffer HistogramBuffer {
  uint bins[];
} histogram;

layout(set = 1, binding = 1, rgba16f) uniform writeonly image2D exposure_out;
// Прошлое состояние экспозиции ('history = 1'): из него берётся адаптация
layout(set = 1, binding = 2, rgba16f) uniform readonly image2D exposure_previous;

shared uint shared_bins[PF03_HISTOGRAM_BINS];
shared uint shared_total;

const uint bins_per_thread = PF03_HISTOGRAM_BINS / 64u;

void main() {
  const uint tid = gl_LocalInvocationIndex;

  uint local_sum = 0u;
  for (uint i = 0; i < bins_per_thread; ++i) {
    const uint bin = tid * bins_per_thread + i;
    const uint value = histogram.bins[bin];
    shared_bins[bin] = value;
    // Корзина 0 — «темнее нижней границы»; она исключается из статистики, иначе чёрный фон и небо за краем
    // сцены утягивали бы экспозицию вверх, и картинка выбеливалась.
    if (bin != 0u) {
      local_sum += value;
    }
  }

  if (tid == 0u) {
    shared_total = 0u;
  }
  barrier();
  atomicAdd(shared_total, local_sum);
  barrier();

  if (tid != 0u) {
    return;
  }

  const float total = float(shared_total);
  float measured_log = frame.exposure_limits.x;

  if (total > 0.0) {
    // Отсечение перцентилями — вся суть перехода от среднего к гистограмме. Среднее (даже логарифмическое)
    // не защищено от выбросов: маленький очень яркий источник поднимает его на стопы, и сцена вокруг тонет.
    // Здесь отбрасываются нижние и верхние проценты распределения, и в расчёт идёт только его середина.
    const float low_cut = total * clamp(frame.metering.x, 0.0, 0.9);
    const float high_cut = total * clamp(frame.metering.y, 0.1, 1.0);

    float seen = 0.0;
    float weighted_sum = 0.0;
    float weight_sum = 0.0;
    for (int bin = 1; bin < PF03_HISTOGRAM_BINS; ++bin) {
      const float count = float(shared_bins[bin]);
      if (count <= 0.0) {
        continue;
      }

      // Доля корзины, попавшая в окно перцентилей: границы почти никогда не совпадают с границами корзин,
      // и без частичного учёта экспозиция дёргалась бы ступеньками при плавном изменении сцены.
      const float bin_start = seen;
      const float bin_end = seen + count;
      seen = bin_end;

      const float inside = min(bin_end, high_cut) - max(bin_start, low_cut);
      if (inside <= 0.0) {
        continue;
      }

      weighted_sum += pf03_bin_to_log2(bin, frame.exposure_limits.x, frame.exposure_limits.y) * inside;
      weight_sum += inside;
    }

    if (weight_sum > 0.0) {
      measured_log = weighted_sum / weight_sum;
    }
  }

  const float clamped_log = clamp(measured_log, frame.exposure_limits.x, frame.exposure_limits.y);

  const bool history_valid = frame.viewport_near.w > 1.5;
  const float previous_log = history_valid ? imageLoad(exposure_previous, ivec2(0)).x : clamped_log;

  // Адаптация АСИММЕТРИЧНА, как у глаза: к яркому привыкание быстрое (секунды), к тёмному медленное (десятки
  // секунд). Один общий темп либо делает выход из тёмной комнаты неправдоподобно мягким, либо вход в неё
  // мгновенным — а именно эта асимметрия и создаёт ощущение ослепления.
  const bool getting_brighter = clamped_log > previous_log;
  const float rate = max(getting_brighter ? frame.tonemap.z : frame.metering.w, 0.0);
  const float blend = 1.0 - exp(-rate * max(frame.tonemap.w, 0.0));
  const float adapted_log = mix(previous_log, clamped_log, clamp(blend, 0.0, 1.0));

  const float key = max(frame.exposure_limits.z, 1.0e-4);
  const float automatic = exp2(log2(key) - adapted_log);
  const float exposure = frame.tonemap.y > 0.0 ? frame.tonemap.y : automatic;

  imageStore(exposure_out, ivec2(0), vec4(adapted_log, exposure, measured_log, total));
}
