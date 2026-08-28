// Чтение карты теней. Отделено от объявления структуры (`pf08_shadow.glsl`) потому, что проход
// ПОСТРОЕНИЯ карты держит буфер каскадов в собственном наборе и сэмплер ему не нужен вовсе, а два
// потребители поверхности обязаны читать карту ОДНИМ кодом.
//
// Набор здесь фиксирован (set = 1) и одинаков у обоих потребителей. Это не украшение: общий код
// невозможен, пока каждый шейдер объявляет ресурсы под своим индексом.

#ifndef PF08_SHADOW_SAMPLE_GLSL
#define PF08_SHADOW_SAMPLE_GLSL

#include "pf08_shadow.glsl"

layout(set = 1, binding = 0, std430) readonly buffer Pf07CascadeBuffer {
  pf08_cascade cascades[];
} pf08_shadow_data;

// Сравнивающий сэмплер: выборка возвращает билинейную ДОЛЮ текселей, прошедших сравнение, а не
// глубину. Одна выборка уже даёт сглаженный край, и девять дают радиус фильтра без девяти ветвлений.
layout(set = 1, binding = 1) uniform sampler2DShadow pf08_shadow_atlas;

float pf08_cascade_visibility(const int index, const vec3 world_position, const vec3 normal,
                              const float receiver_bias_scale) {
  const pf08_cascade cascade = pf08_shadow_data.cascades[index];

  // Смещение вдоль НОРМАЛИ, а не вдоль луча света. Приёмник и кастер здесь одна и та же поверхность,
  // и без смещения она затеняет сама себя полосами. Смещение по нормали на размер текселя убирает их
  // не трогая глубину, поэтому тень не отрывается от основания предмета — болезнь, которую даёт
  // одно лишь смещение глубины.
  const float texel_world = cascade.shadow_params.x;
  const vec3 offset_position = world_position + normal * texel_world * receiver_bias_scale;

  const vec4 light_clip = cascade.light_view_projection * vec4(offset_position, 1.0);
  const vec3 projected = light_clip.xyz / max(light_clip.w, 1e-6);
  if (any(lessThan(projected.xy, vec2(-1.0))) || any(greaterThan(projected.xy, vec2(1.0)))) return 1.0;
  // Глубина обратная и уже лежит в [0,1]: разворачивать её здесь не нужно, в отличие от xy.
  if (projected.z <= 0.0 || projected.z >= 1.0) return 1.0;

  const vec2 local_uv = projected.xy * 0.5 + 0.5;
  const vec2 atlas_uv = local_uv * cascade.uv_scale_offset.xy + cascade.uv_scale_offset.zw;
  const vec2 texel = 1.0 / vec2(textureSize(pf08_shadow_atlas, 0));

  float visible = 0.0;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      visible += texture(pf08_shadow_atlas, vec3(atlas_uv + vec2(x, y) * texel, projected.z));
    }
  }
  return visible / 9.0;
}

// Объём уже фильтруется трилинейной выборкой самой froxel-сетки, поэтому девятиточечный PCF здесь
// лишь умножал бы цену на девять и размывал тень второй раз. Точка воздуха не имеет поверхности и
// нормали; depth bias внесён при построении карты, а за пределами каскада источник считается видимым.
float pf08_cascade_visibility_single(const int index, const vec3 world_position) {
  const pf08_cascade cascade = pf08_shadow_data.cascades[index];
  const vec4 light_clip = cascade.light_view_projection * vec4(world_position, 1.0);
  const vec3 projected = light_clip.xyz / max(light_clip.w, 1e-6);
  if (any(lessThan(projected.xy, vec2(-1.0))) || any(greaterThan(projected.xy, vec2(1.0)))) return 1.0;
  if (projected.z <= 0.0 || projected.z >= 1.0) return 1.0;
  const vec2 local_uv = projected.xy * 0.5 + 0.5;
  const vec2 atlas_uv = local_uv * cascade.uv_scale_offset.xy + cascade.uv_scale_offset.zw;
  return texture(pf08_shadow_atlas, vec3(atlas_uv, projected.z));
}

// Видимость источника из точки. Каскад выбирается по расстоянию до камеры, а на стыке двух каскадов
// идёт смешивание: без него граница между ними видна как ступенька резкости, потому что мировой
// размер текселя у соседних каскадов отличается в разы.
float pf08_shadow_visibility(const int slot, const vec3 world_position, const vec3 normal,
                             const float view_distance, const float receiver_bias_scale) {
  const int first = slot * PF08_CASCADE_COUNT;
  const float strength = pf08_shadow_data.cascades[first].shadow_params.y;
  if (strength <= 0.0) return 1.0;

  int index = first + PF08_CASCADE_COUNT - 1;
  for (int i = 0; i < PF08_CASCADE_COUNT; ++i) {
    if (view_distance < pf08_shadow_data.cascades[first + i].split_depths.y) {
      index = first + i;
      break;
    }
  }

  float visibility = pf08_cascade_visibility(index, world_position, normal, receiver_bias_scale);

  const vec4 splits = pf08_shadow_data.cascades[index].split_depths;
  const bool has_next = index < first + PF08_CASCADE_COUNT - 1;
  if (has_next && view_distance > splits.z) {
    const float blend = clamp((view_distance - splits.z) / max(splits.y - splits.z, 1e-4), 0.0, 1.0);
    visibility = mix(visibility,
                     pf08_cascade_visibility(index + 1, world_position, normal, receiver_bias_scale), blend);
  }

  // Сила источника гасит тень у порога вхождения в двойку. Без этого тело, входящее в число двух
  // самых ярких, включало бы свою тень скачком.
  return mix(1.0, visibility, strength);
}

float pf08_volume_shadow_visibility(const int slot, const vec3 world_position, const float view_distance) {
  const int first = slot * PF08_CASCADE_COUNT;
  const float strength = pf08_shadow_data.cascades[first].shadow_params.y;
  if (strength <= 0.0) return 1.0;

  int index = first + PF08_CASCADE_COUNT - 1;
  for (int i = 0; i < PF08_CASCADE_COUNT; ++i) {
    if (view_distance < pf08_shadow_data.cascades[first + i].split_depths.y) {
      index = first + i;
      break;
    }
  }
  float visibility = pf08_cascade_visibility_single(index, world_position);
  const vec4 splits = pf08_shadow_data.cascades[index].split_depths;
  const bool has_next = index < first + PF08_CASCADE_COUNT - 1;
  if (has_next && view_distance > splits.z) {
    const float blend = clamp((view_distance - splits.z) / max(splits.y - splits.z, 1e-4), 0.0, 1.0);
    visibility = mix(visibility, pf08_cascade_visibility_single(index + 1, world_position), blend);
  }
  return mix(1.0, visibility, strength);
}

#endif
