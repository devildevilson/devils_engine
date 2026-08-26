#version 450

#include "pf07_records.glsl"

// Вывод: физическая экспозиция, ночное зрение, цветовой сценарий, кривая, sRGB.
//
// Порядок здесь не произволен. Экспозиция идёт ПЕРВОЙ и работает с физическими нитами: она переводит
// сцену из восьми порядков освещённости в тот диапазон, где кривая имеет смысл. Ночное зрение решает
// по ИСХОДНОЙ яркости, а не по уже сжатой: колбочки отказывают при определённой яркости сцены, и после
// тонмаппинга эта величина уже потеряна. Сценарий работает по свету, а не по готовому изображению,
// поэтому стоит до кривой — иначе он менял бы не настроение, а форму самой кривой.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D sky_color;
layout(set = 0, binding = 1, std140) uniform SkyBlock {
  pf07_sky_block sky;
} sky_data;

const vec3 pf07_luma_weights = vec3(0.2126, 0.7152, 0.0722);

vec3 tonemap_aces(const vec3 value) {
  const float a = 2.51;
  const float b = 0.03;
  const float c = 2.43;
  const float d = 0.59;
  const float e = 0.14;
  return clamp((value * (a * value + b)) / (value * (c * value + d) + e), 0.0, 1.0);
}

// Эффект Пуркинье. Ниже примерно 0.005 нит цвет для глаза перестаёт существовать: работают только
// палочки, а их чувствительность смещена в синюю сторону относительно колбочек. Именно поэтому ночь
// выглядит не тёмной цветной фотографией, а голубовато-серой. Величина берётся ДО экспозиции: отказывают
// колбочки от яркости сцены, а не от того, как её потом вытянули.
vec3 apply_night_vision(const vec3 color, const float scene_luminance, const float strength) {
  if (strength <= 0.0) return color;

  const float rod_share = 1.0 - smoothstep(0.005, 3.0, scene_luminance);
  if (rod_share <= 0.0) return color;

  const vec3 rod_response = vec3(dot(color, pf07_luma_weights)) * vec3(0.82, 0.94, 1.24);
  return mix(color, rod_response, rod_share * strength);
}

void main() {
  const vec3 radiance = texture(sky_color, in_uv).rgb;
  if (sky_data.sky.output_params.w > 0.5) {
    out_color = vec4(clamp(radiance, 0.0, 1.0), 1.0);
    return;
  }

  const float exposure = sky_data.sky.output_params.x;
  const float scene_luminance = dot(radiance, pf07_luma_weights);
  vec3 color = radiance * exposure;

  color = apply_night_vision(color, scene_luminance, sky_data.sky.grade_curve.y);

  // Цветовой сценарий. Ограничен по построению: оттенок близок к единице, насыщенность и контраст
  // отходят от неё на проценты. Он подчёркивает то, что уже сделала физика, а не рисует настроение
  // заново — иначе закат перестал бы зависеть от мутности, а ночь от фазы луны.
  color *= sky_data.sky.grade_tint_saturation.rgb;
  const float grey = dot(color, pf07_luma_weights);
  color = mix(vec3(grey), color, sky_data.sky.grade_tint_saturation.w);

  vec3 mapped = tonemap_aces(color);

  // Контраст задаётся S-кривой, а не растяжкой вокруг средней точки. Растяжка `(x - 0.5) * k + 0.5`
  // при k > 1 ОБРУБАЕТ всё ниже 0.5 - 0.5/k в ноль: при k = 1.03 это уже 0.0146, и тёмные участки
  // теряли слабые каналы целиком. Земля на закате выходила чистым красным (22, 0, 0) — зелёный и синий
  // просто вырезались. S-кривая оставляет ноль нулём и единицу единицей по построению.
  const float contrast_amount = clamp(sky_data.sky.grade_curve.x - 1.0, -0.5, 0.5);
  const vec3 s_curve = mapped * mapped * (3.0 - 2.0 * mapped);
  mapped = clamp(mix(mapped, s_curve, contrast_amount * 2.0), 0.0, 1.0);
  // Вывод в sRGB. Здесь это единственное место, где линейный свет превращается в изображение.
  mapped = pow(max(mapped, vec3(0.0)), vec3(1.0 / 2.2));
  out_color = vec4(mapped, 1.0);
}
