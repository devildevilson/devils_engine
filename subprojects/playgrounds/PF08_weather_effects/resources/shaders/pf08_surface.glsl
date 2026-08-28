// Освещение поверхности PF08: единственное описание того, как свет неба и светил ложится на предмет.
//
// Существует по той же причине, что и общая модель среды рядом: землю и геометрию освещает ОДИН мир.
// Разойдись эти два места — и предмет будет выглядеть вырезанным из другой картинки, причём тем
// заметнее, чем вернее каждое из них по отдельности. Ошибка при этом не даст ни сбоя сборки, ни
// предупреждения: просто тихо неверный кадр.

#ifndef PF08_SURFACE_GLSL
#define PF08_SURFACE_GLSL

#include "pf08_atmosphere.glsl"
#include "pf08_clouds.glsl"
#include "pf08_local_medium.glsl"
#ifndef PF08_SURFACE_NO_SHADOWS
#include "pf08_shadow_sample.glsl"
#endif

// Две системы координат, и путать их нельзя.
//
// Атмосфера считается в КИЛОМЕТРАХ от центра планеты: иначе радиус в шесть тысяч и шаг марша в сотню
// метров не помещаются в один float с приемлемой точностью. Сцена живёт в МЕТРАХ около камеры: иначе
// коробка в полметра тонет в разряде мантиссы шеститысячного радиуса.
//
// Кривизной планеты внутри сцены пренебрегаем осознанно: на ста метрах поверхность отходит от
// касательной плоскости на 0.8 мм, то есть на три порядка меньше самого мелкого предмета.
vec3 pf08_scene_to_planet(const pf08_sky_block sky, const vec3 scene_position) {
  return vec3(scene_position.x * 0.001, sky.atmosphere_geometry.x + scene_position.y * 0.001,
              scene_position.z * 0.001);
}

// Свет НЕБА на площадку с заданной нормалью. Полный интеграл яркости по полусфере здесь не нужен:
// небо уже посчитано в таблице, и пяти выборок хватает на оценку его средней яркости, а освещённость
// площадки от полусферы такой яркости равна pi * L.
//
// Выборки берутся вокруг НОРМАЛИ, а не вокруг вертикали. Для земли это то же самое, а для боковой
// грани — принципиально: у неё половина полусферы смотрит в землю, таблица отдаёт там ноль, и грань
// честно получает вдвое меньше неба. Отражение от земли при этом не считается вовсе, и это видно как
// недостачу, а не как ошибку.
vec3 pf08_sky_illuminance(const sampler2D sky_view_lut, const vec3 normal) {
  const vec2 size = vec2(textureSize(sky_view_lut, 0));
  const vec3 helper = abs(normal.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  const vec3 tangent = normalize(cross(helper, normal));
  const vec3 bitangent = cross(normal, tangent);

  vec3 total = texture(sky_view_lut, pf08_sky_view_uv(normal, size)).rgb;
  const float tilt = 0.82; // около 55 градусов от нормали — середина полусферы по телесному углу
  const float straight = sqrt(max(0.0, 1.0 - tilt * tilt));
  total += texture(sky_view_lut, pf08_sky_view_uv(normal * straight + tangent * tilt, size)).rgb;
  total += texture(sky_view_lut, pf08_sky_view_uv(normal * straight - tangent * tilt, size)).rgb;
  total += texture(sky_view_lut, pf08_sky_view_uv(normal * straight + bitangent * tilt, size)).rgb;
  total += texture(sky_view_lut, pf08_sky_view_uv(normal * straight - bitangent * tilt, size)).rgb;
  return total * (pf08_pi / 5.0);
}

// Полная освещённость поверхности: прямой свет светил, прямой свет лун и рассеянный свет неба.
//
// Луны здесь не роскошь. Без них ночь без светил становится абсолютно чёрной, и граница неба и земли
// пропадает даже при полной луне над головой.
// Какой теневой слот достался телу с этим кодом. Кодировка: 0..1 — звезда, 2+m — луна m.
// Система теней берёт два самых ярких источника на небе, кем бы они ни были, поэтому обратное
// соответствие приходит с хоста данными — вывести его в шейдере нечем.
int pf08_shadow_slot(const pf08_sky_block sky, const float body_code) {
#ifdef PF08_SURFACE_NO_SHADOWS
  return -1;
#else
  if (sky.shadow_bodies.x == body_code) return 0;
  if (sky.shadow_bodies.y == body_code) return 1;
  return -1;
#endif
}

#ifdef PF08_SURFACE_NO_SHADOWS
float pf08_shadow_visibility(const int slot, const vec3 p, const vec3 n, const float d, const float b) { return 1.0; }
#endif

// Полная освещённость поверхности: прямой свет светил, прямой свет лун и рассеянный свет неба.
//
// Тени входят множителем к ПРЯМОМУ слагаемому и только к нему. Рассеянный свет неба тень не заслоняет
// по определению: он приходит со всей полусферы, и предмет, закрывающий одно направление, не закрывает
// остальные. Умножь мы на видимость всё вместе — тень стала бы чёрной дырой, а не тенью.
vec3 pf08_surface_illuminance(const pf08_sky_block sky, const sampler2D transmittance_lut,
                              const sampler2D sky_view_lut, const vec3 planet_point,
                              const vec3 scene_position, const vec3 normal, const float view_distance,
                              const float receiver_bias_scale, const float direct_visibility,
                              const float sky_visibility) {
  vec3 total = vec3(0.0);
  const float fog_column_modulation = sky.fog_params.x > 0.0
    ? pf08_fog_column_modulation(sky, scene_position) : 1.0;

  for (int s = 0; s < PF08_STAR_COUNT; ++s) {
    const vec3 light_direction = sky.star_direction[s].xyz;
    const float illuminance = sky.star_color_illuminance[s].w;
    if (illuminance <= 0.0) continue;

    const float cosine = max(dot(normal, light_direction), 0.0);
    if (cosine <= 0.0) continue;

    const int slot = pf08_shadow_slot(sky, float(s));
    const float visibility =
      slot < 0 ? 1.0 : pf08_shadow_visibility(slot, scene_position, normal, view_distance, receiver_bias_scale);
    if (visibility <= 0.0) continue;

    vec3 light_transmittance =
      pf08_transmittance_to_light(sky, transmittance_lut, planet_point, light_direction);
    // Shadow map отвечает только за геометрическую видимость. В тумане даже видимое светило доходит
    // до поверхности ослабленным; без этого множителя густой объём оставлял сухие резкие тени.
    if (sky.fog_params.x > 0.0) {
      light_transmittance *= pf08_fog_light_transmittance(
        sky, scene_position, light_direction, fog_column_modulation);
    }
    if (sky.cloud_params.x > 0.0) {
      light_transmittance *= pf08_cloud_light_transmittance(sky, scene_position, light_direction);
    }
    total += light_transmittance * sky.star_color_illuminance[s].rgb * illuminance * cosine * visibility;
  }

  const int moon_count = int(sky.march_params.w);
  for (int m = 0; m < moon_count && m < PF08_MOON_CAPACITY; ++m) {
    const vec3 light_direction = sky.moon_direction[m].xyz;
    const float illuminance = sky.moon_color_illuminance[m].w;
    if (illuminance <= 0.0) continue;

    const float cosine = max(dot(normal, light_direction), 0.0);
    if (cosine <= 0.0) continue;

    const int slot = pf08_shadow_slot(sky, float(PF08_STAR_COUNT + m));
    const float visibility =
      slot < 0 ? 1.0 : pf08_shadow_visibility(slot, scene_position, normal, view_distance, receiver_bias_scale);
    float local_medium = 1.0;
    if (sky.fog_params.x > 0.0) {
      local_medium = pf08_fog_light_transmittance(
        sky, scene_position, light_direction, fog_column_modulation);
    }
    if (sky.cloud_params.x > 0.0) {
      local_medium *= pf08_cloud_light_transmittance(sky, scene_position, light_direction);
    }
    total += sky.moon_color_illuminance[m].rgb * illuminance * cosine * visibility * local_medium;
  }

  return total * direct_visibility + pf08_sky_illuminance(sky_view_lut, normal) * sky_visibility;
}

#endif
