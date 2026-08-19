#version 450

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D depth_image;
layout(set = 1, binding = 1) uniform sampler2D normal_image;
layout(set = 1, binding = 2) uniform sampler2D motion_image;

// Здесь это уже ЗАТУМАНЕННЫЙ кадр (см. граф): в альфе лежит пропускание тумана
layout(set = 2, binding = 0) uniform sampler2D scene_image;
// Прошлый кадр приходит ОТДЕЛЬНЫМ биндингом ('history = 1' в конфиге), а не элементом массива копий:
// номер кадра шейдеру не нужен ни данными, ни индексом.
layout(set = 2, binding = 1) uniform sampler2D history_image;
// Экспозиция посчитана отдельным пассом этого же кадра: .y — множитель
layout(set = 2, binding = 2, rgba16f) uniform readonly image2D exposure_state;

layout(set = 2, binding = 3) uniform sampler2D ao_image;
layout(set = 2, binding = 4) uniform sampler2D ao_raw_image;

layout(set = 3, binding = 0, rgba16f) uniform writeonly image2D composed_image;

vec3 motion_to_color(const vec2 motion, const vec2 size) {
  // motion в UV крошечный, поэтому показываем его в ПИКСЕЛЯХ и с усилением: иначе всё чёрное
  const vec2 pixels = motion * size * frame.controls.y;
  return vec3(0.5 + pixels.x * 0.5, 0.5 + pixels.y * 0.5, 0.5);
}

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(composed_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const float depth = texture(depth_image, uv).r;
  const vec2 motion = texture(motion_image, uv).rg;
  const vec3 current = texture(scene_image, uv).rgb;

  // История на первом кадре после сброса пустая (движок чистит копии в нули), поэтому сравнивать с ней
  // нечего: показываем текущий кадр, а не «ошибку» размером во весь экран.
  const bool history_valid = frame.viewport_near.w > 1.5;
  const vec2 reprojected_uv = uv + motion;
  const bool inside = all(greaterThanEqual(reprojected_uv, vec2(0.0))) && all(lessThanEqual(reprojected_uv, vec2(1.0)));

  const vec3 reprojected = (history_valid && inside) ? texture(history_image, reprojected_uv).rgb : current;
  const vec3 naive = history_valid ? texture(history_image, uv).rgb : current;

  const int mode = int(frame.controls.x + 0.5);
  const float exposure = imageLoad(exposure_state, ivec2(0)).y;
  const int tonemap_op = int(frame.tonemap.x + 0.5);

  // Порядок операций не произволен: экспозиция — это МНОЖИТЕЛЬ в линейном HDR, tone mapping — КРИВАЯ,
  // сжимающая уже отэкспонированный диапазон в [0,1]. Поменять их местами значит растянуть обратно то, что
  // кривая только что сжала.
  vec3 result = pf03_apply_tonemap(current * exposure, tonemap_op);

  if (mode == PF03_DEBUG_DEPTH) {
    const float linear_depth = depth > 0.0 ? pf03_linear_depth(depth, frame.viewport_near.z) : 0.0;
    result = vec3(clamp(linear_depth / 40.0, 0.0, 1.0));
  } else if (mode == PF03_DEBUG_NORMAL) {
    result = depth > 0.0 ? pf03_decode_normal(texture(normal_image, uv).rg) * 0.5 + 0.5 : vec3(0.0);
  } else if (mode == PF03_DEBUG_MOTION) {
    result = depth > 0.0 ? motion_to_color(motion, vec2(size)) : vec3(0.0);
  } else if (mode == PF03_DEBUG_REPROJECTED) {
    result = pf03_apply_tonemap(reprojected * exposure, tonemap_op);
  } else if (mode == PF03_DEBUG_CLIPPING) {
    // Где кадр выходит за диапазон ПОСЛЕ экспозиции: красное — светы за единицей, синее — почти чёрное.
    // Это то, что кривая обязана вытянуть, и то, чего простая обрезка не умеет.
    const vec3 exposed = current * exposure;
    const float peak = max(max(exposed.r, exposed.g), exposed.b);
    result = pf03_apply_tonemap(exposed, tonemap_op) * 0.25;
    if (peak > 1.0) result = vec3(1.0, 0.1, 0.0);
    if (peak < 0.02) result = vec3(0.0, 0.15, 0.8);
  } else if (mode == PF03_DEBUG_AO) {
    result = vec3(texture(ao_image, uv).r);
  } else if (mode == PF03_DEBUG_AO_RAW) {
    result = vec3(texture(ao_raw_image, uv).r);
  } else if (mode == PF03_DEBUG_TRANSMITTANCE) {
    // Пропускание тумана: белое — поверхность видна как есть, чёрное — от неё не дошло ничего
    result = vec3(texture(scene_image, uv).a);
  } else if (mode == PF03_DEBUG_EXPOSURE) {
    // Числа состояния экспозиции прямо в пиксели: дамп читается кодом, поэтому диагностика не требует
    // ни оверлея, ни угадывания по яркости. r = множитель/16, g = adapted log2, b = measured log2 (сдвинуты).
    const vec4 state = imageLoad(exposure_state, ivec2(0));
    // Все три величины в log2 и с одинаковым сдвигом: множитель экспозиции бывает сильно меньше единицы, и
    // в линейной кодировке 8-битный шаг оказывался грубее самого значения.
    result = vec3(log2(max(state.y, 1e-6)) / 32.0 + 0.5, state.x / 32.0 + 0.5, state.z / 32.0 + 0.5);
  } else if (mode == PF03_DEBUG_CALIBRATION) {
    // Проверка передаточной функции дисплея: слева половина кадра — ровно 0.5 в линейном свете. Если тракт
    // кодирует sRGB, на скриншоте будет ~188/255, если нет — ~128/255. Правая половина — линейный клин.
    result = uv.x < 0.5 ? vec3(0.5) : vec3(clamp((uv.x - 0.5) * 2.0, 0.0, 1.0));
  } else if (mode == PF03_DEBUG_ERROR_MOTION) {
    result = abs(reprojected - current) * frame.controls.z;
  } else if (mode == PF03_DEBUG_ERROR_NAIVE) {
    // Та же ошибка, но БЕЗ motion-векторов: контрольная величина, относительно которой видно, что
    // векторы действительно что-то исправляют, а не просто «выглядят правдоподобно».
    result = abs(naive - current) * frame.controls.z;
  }

  // Кодирование в sRGB делается ТОЛЬКО если конечная запись идёт в линейный формат. Тумблер оставлен
  // данными, потому что это свойство тракта презентации, а не картинки, и его надо было измерить.
  if (frame.controls.w > 0.5) {
    result = pf03_encode_srgb(result);
  }

  imageStore(composed_image, pixel, vec4(result, 1.0));
}
