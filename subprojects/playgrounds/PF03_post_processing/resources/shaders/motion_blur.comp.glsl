#version 450

// Алгоритм: gather-аппроксимация интеграла света за время открытой шторки. Максимальный motion соседней
// tile задаёт направление и возможную длину шлейфа, затем вдоль симметричного отрезка собираются пробы.
// Depth-aware веса различают близкий движущийся объект, который должен наползать на фон, и дальний фон,
// который нельзя затягивать поверх неподвижного центра. Temporal jitter распределяет недобор проб по кадрам.

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Число проб — тир качества (specialization-константа). Второй константой отключается расширение по плиткам:
// ровно затем, чтобы измерить, во что превращается шлейф без него.
layout(constant_id = 0) const int blur_taps = 12;
layout(constant_id = 1) const int blur_use_tiles = 1;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D depth_image;
layout(set = 1, binding = 1) uniform sampler2D normal_image;
layout(set = 1, binding = 2) uniform sampler2D motion_image;

layout(set = 2, binding = 0) uniform sampler2D scene_image;
layout(set = 2, binding = 1) uniform sampler2D tile_image; // расширенный максимум motion по плиткам

layout(set = 3, binding = 0, rgba16f) uniform writeonly image2D result_image;

// Линейная глубина, у неба — «бесконечно далеко». Нужна именно линейная: сравнения «ближе/дальше» в reverse-Z
// работают, но пороги в ней бессмысленны, а глубина неба (ровно 0) вообще ломает деление.
float depth_image_linear(const vec2 uv) {
  const float d = texture(depth_image, uv).r;
  return d > 0.0 ? pf03_linear_depth(d, frame.viewport_near.z) : 1.0e6;
}

// Motion blur — это интеграл яркости за время открытой шторки, то есть та же задача, что у глубины резкости,
// только пятно вырождено в ОТРЕЗОК вдоль вектора движения. Отсюда и устройство сбора: он такой же «рассеяние
// наизнанку», и веса решают тот же вопрос — какая проба физически может накрыть центр.
//
// Длина отрезка = motion-вектор кадра, умноженный на долю открытой шторки: при 180 градусах и 60 кадрах в
// секунду это половина смещения за кадр. Экспозиция и длина шторки в реальной камере — ОДНА ручка (дольше
// открыта — больше света и больше смаз), здесь они разведены сознательно: автоэкспозиция площадки живёт своей
// жизнью, и связывать их значило бы менять яркость кадра ручкой размытия.
void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(result_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const vec4 center = texture(scene_image, uv);

  const float shutter = frame.blur_params.x;
  if (shutter <= 0.0) {
    imageStore(result_image, pixel, center);
    return;
  }

  // motion в UV; в пиксели переводим размером кадра. Знак: вектор указывает, КУДА сместиться, чтобы попасть в
  // прошлый кадр, то есть противоположен движению — для симметричного отрезка это безразлично.
  const vec2 center_motion = texture(motion_image, uv).rg * vec2(size) * shutter;
  const vec2 tile_motion = blur_use_tiles != 0
    ? texture(tile_image, uv).rg * vec2(size) * shutter
    : center_motion;

  // Направление сбора берётся от самого быстрого в округе: у неподвижного фона позади летящего объекта
  // собственный вектор нулевой, и без этого он остался бы резким, а шлейф оборвался бы по силуэту.
  const vec2 direction = length(tile_motion) > length(center_motion) ? tile_motion : center_motion;
  const float span = length(direction);
  if (span < 0.5) {
    imageStore(result_image, pixel, center);
    return;
  }

  const float center_depth = depth_image_linear(uv);
  const float center_span = length(center_motion);

  // Джиттер по кадру: пробы стоят в разных местах отрезка каждый кадр, поэтому недобор проб усредняется
  // накоплением, а не мерцает (тот же приём, что у SSAO и боке).
  const float jitter = fract(pf03_gradient_noise(vec2(pixel)) + frame.taa_params.w);

  const uint taps = uint(max(blur_taps, 1));
  vec3 sum = center.rgb;
  float weight = 1.0;

  for (uint i = 0u; i < taps; ++i) {
    // Отрезок симметричен относительно центра: шторка открыта и до, и после момента кадра
    const float t = ((float(i) + jitter) / float(taps) - 0.5);
    const vec2 offset = direction * t;
    const vec2 tap_uv = uv + offset / vec2(size);
    if (any(lessThan(tap_uv, vec2(0.0))) || any(greaterThan(tap_uv, vec2(1.0)))) {
      continue;
    }

    const float distance_px = length(offset);
    const float tap_depth = depth_image_linear(tap_uv);
    const vec2 tap_motion = texture(motion_image, tap_uv).rg * vec2(size) * shutter;

    // Два вклада, и они про разное:
    //   ПЕРЕДНИЙ: проба ближе центра, и её собственный смаз достаёт до центра — тогда быстрый объект наползает
    //   на то, что позади него (без этого шлейф обрывается по силуэту);
    //   ЗАДНИЙ: проба дальше центра, и тогда её вклад разрешён ровно настолько, насколько смазан САМ центр —
    //   иначе неподвижный фон затягивало бы в размытие движущегося объекта, которого перед ним нет.
    const float foreground = tap_depth < center_depth
      ? clamp(length(tap_motion) - distance_px + 1.0, 0.0, 1.0)
      : 0.0;
    const float background = tap_depth >= center_depth
      ? clamp(center_span - distance_px + 1.0, 0.0, 1.0)
      : 0.0;

    const float tap_weight = max(foreground, background);
    if (tap_weight <= 0.0) {
      continue;
    }

    sum += texture(scene_image, tap_uv).rgb * tap_weight;
    weight += tap_weight;
  }

  const vec3 result = sum / max(weight, 1.0e-6);
  // Альфа несёт пропускание тумана дальше по цепочке — её размывать нельзя
  imageStore(result_image, pixel, vec4(result, center.a));
}
