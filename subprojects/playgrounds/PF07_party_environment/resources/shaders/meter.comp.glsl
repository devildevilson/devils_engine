#version 450

// Замер экспозиции по тому, что РЕАЛЬНО нарисовано.
//
// До этого прохода экспозиция считалась от горизонтальной освещённости — то есть от падающего света,
// как у фотографа с полусферой на объективе. Для сцены, которую свет ОСВЕЩАЕТ, это верно. Но небо
// светом не освещается: оно само и есть источник, и в сумерках оно занимает весь кадр. Падающий замер
// такую сцену измеряет мимо: на рассвете при светиле на -6° он давал 17 лк, экспозиция открывалась,
// и небо выходило белым при формально верных числах. Никакая поправка это не лечит, потому что
// ошибочна сама измеряемая величина.
//
// Здесь измеряется яркость кадра, как в отражённом замере любого экспонометра. Величина
// геометрическая, а не арифметическая: усредняется ЛОГАРИФМ яркости. Это не косметика — диск светила
// имеет яркость порядка двух миллиардов нит, и в арифметическом среднем несколько его пикселей
// перевесили бы всё небо целиком. В логарифме тот же диск сдвигает итог на сотые доли ступени.
//
// Кадр читается разреженной сеткой 64x64: экспозиция — величина медленная, сглаженная адаптацией по
// секундам, и различать её на большем числе выборок нечем.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1, std430) buffer MeterBlock {
  vec4 value;
} meter;

const int pf07_meter_samples_per_thread = 4;
const uint pf07_meter_threads = 256;

shared float partial_sum[pf07_meter_threads];

void main() {
  const uint index = gl_LocalInvocationIndex;
  const vec2 thread_uv = (vec2(gl_LocalInvocationID.xy) + 0.5) / 16.0;

  float sum = 0.0;
  for (int y = 0; y < pf07_meter_samples_per_thread; ++y) {
    for (int x = 0; x < pf07_meter_samples_per_thread; ++x) {
      const vec2 offset =
        ((vec2(float(x), float(y)) + 0.5) / float(pf07_meter_samples_per_thread) - 0.5) / 16.0;
      const vec2 uv = clamp(thread_uv + offset, vec2(0.0), vec2(1.0));
      const vec3 radiance = texture(scene_color, uv).rgb;
      const float luminance = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
      // Пол в одну стотысячную нита — примерно яркость безлунной ночи. Он не даёт логарифму уйти в
      // минус бесконечность на пикселях чистой черноты и заодно задаёт нижнюю границу адаптации.
      sum += log(max(luminance, 1e-5));
    }
  }

  partial_sum[index] = sum / float(pf07_meter_samples_per_thread * pf07_meter_samples_per_thread);
  barrier();

  for (uint stride = pf07_meter_threads / 2; stride > 0; stride >>= 1) {
    if (index < stride) partial_sum[index] += partial_sum[index + stride];
    barrier();
  }

  if (index == 0) {
    const float mean_log = partial_sum[0] / float(pf07_meter_threads);
    meter.value = vec4(exp(mean_log), 0.0, 0.0, 1.0);
  }
}
