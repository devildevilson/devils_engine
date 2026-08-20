#ifndef PF03_SHADING_GLSL
#define PF03_SHADING_GLSL

#include "pf03_frame.glsl"

// Освещение сцены площадки, вынесенное из compute-пасса ради ВТОРОГО потребителя: forward-пасс с MSAA считает
// ровно то же самое, только не из G-buffer, а прямо при растеризации. Держать две копии этой математики значило
// бы получить две разные картинки и не понять, MSAA тому виной или расхождение формул.
//
// Шейдинг намеренно простой: площадка проверяет тракт, а не освещение. Комментарии к решениям — в срезе 1.

vec3 pf03_sky_color(const vec2 uv, const float sun) {
  // Небо ЗНАЧИТЕЛЬНО ярче освещённых поверхностей, и это не подгонка: в реальности оно измеряется тысячами
  // кандел против сотен у стены. Именно из-за него диапазон кадра не влезает в 8 бит.
  const float sun_scale = sun * 1.6;
  return mix(vec3(0.35, 0.50, 0.95), vec3(1.10, 1.40, 2.00), clamp(1.0 - uv.y, 0.0, 1.0)) * sun_scale;
}

// Полное затенение поверхности. ao приходит параметром, потому что у forward-ветви его просто нет: экранные
// эффекты с MSAA не согласуются (читают один сэмпл), и в простом проекте их не будет.
vec3 pf03_shade_surface(
  const vec3 world, const vec3 normal, const vec3 camera, const vec3 light_direction,
  const float sun, const float ambient_fraction, const float ao, const float lamp_intensity) {
  const float ndl = max(dot(normal, normalize(light_direction)), 0.0);
  // Ambient задан ДОЛЕЙ солнца, а не абсолютом: при смене яркости светила сцена не рассыпается
  const float ambient_light = ambient_fraction * sun;
  const float ambient = ambient_light * ao;

  // Альбедо берётся ИЗ НОРМАЛИ, а не из мировых координат: рисунок, привязанный к миру, «плывёт» по
  // движущейся поверхности, и тогда даже идеальные motion-векторы дают ошибку репроекции
  const vec3 albedo = vec3(0.58, 0.56, 0.54) + normal * vec3(0.16, 0.10, 0.18);

  // Исключение — статичный пол: на нём оставлен жёсткий шахматный стресс-паттерн для измерений
  const bool is_floor = normal.y > 0.99 && world.y < -2.9;
  const float checker = is_floor && (step(0.5, fract(world.x * 0.5)) != step(0.5, fract(world.z * 0.5))) ? 0.68 : 1.0;

  // Узкий яркий блик — это и есть настоящий свет, который в 8 бит не влезает
  const vec3 to_camera = normalize(camera - world);
  const vec3 half_vector = normalize(to_camera + normalize(light_direction));
  const float specular = pow(max(dot(normal, half_vector), 0.0), 24.0) * sun * 30.0;

  // Светящаяся панель: маленький источник на два порядка ярче среднего, который экспозиция вытянуть не может
  const bool lamp = world.z < -8.85 && abs(world.x - 2.5) < 2.2 && world.y > -1.2 && world.y < 0.9;
  const vec3 emissive = lamp ? vec3(1.0, 0.92, 0.78) * sun * lamp_intensity : vec3(0.0);

  return albedo * checker * (ndl * sun + ambient) + vec3(specular) + emissive;
}

#endif
