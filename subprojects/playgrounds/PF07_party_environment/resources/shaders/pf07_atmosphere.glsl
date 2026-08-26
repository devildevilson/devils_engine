// Модель среды PF07: единственное описание рассеяния и поглощения.
//
// Раньше эти константы и профили жили отдельно в марше неба и в вычислителе таблицы прохождения. Так
// делать нельзя: расхождение между тем, чем считается таблица, и тем, чем пользуется небо, даёт не
// ошибку компиляции, а тихо неверную картинку. Тот же урок PF02 про раскладку буферов, только про среду.

#ifndef PF07_ATMOSPHERE_GLSL
#define PF07_ATMOSPHERE_GLSL

#include "pf07_records.glsl"

const float pf07_pi = 3.14159265358979323846;

// Коэффициенты на километр, RGB-приближение земной атмосферы. Рэлей растёт к синему как обратная
// четвёртая степень длины волны, озон поглощает в оранжевом и делает сумеречный зенит синим.
const vec3 pf07_rayleigh_scattering = vec3(5.802e-3, 13.558e-3, 33.100e-3);
const float pf07_mie_scattering = 3.996e-3;
const float pf07_mie_extinction = 4.440e-3;
const vec3 pf07_ozone_absorption = vec3(0.650e-3, 1.881e-3, 0.085e-3);

struct pf07_medium {
  vec3 scattering_rayleigh;
  float scattering_mie;
  vec3 extinction;
};

pf07_medium pf07_sample_medium(const pf07_sky_block sky, const float height_km) {
  const float rayleigh_scale = sky.atmosphere_geometry.z;
  const float mie_scale = sky.atmosphere_geometry.w;
  const float ozone_center = sky.atmosphere_medium.y;
  const float ozone_width = sky.atmosphere_medium.z;
  const float turbidity = sky.atmosphere_medium.w;

  const float rayleigh_density = exp(-max(height_km, 0.0) / rayleigh_scale);
  const float mie_density = exp(-max(height_km, 0.0) / mie_scale) * turbidity;
  // Озон живёт слоем, а не убывает экспоненциально: треугольный профиль вокруг своей высоты.
  const float ozone_density = max(0.0, 1.0 - abs(height_km - ozone_center) / ozone_width);

  pf07_medium result;
  result.scattering_rayleigh = pf07_rayleigh_scattering * rayleigh_density;
  result.scattering_mie = pf07_mie_scattering * mie_density;
  result.extinction = pf07_rayleigh_scattering * rayleigh_density + vec3(pf07_mie_extinction * mie_density) +
                      pf07_ozone_absorption * ozone_density;
  return result;
}

float pf07_rayleigh_phase(const float cosine) {
  return 3.0 / (16.0 * pf07_pi) * (1.0 + cosine * cosine);
}

float pf07_mie_phase(const float cosine, const float g) {
  const float gg = g * g;
  const float denominator = 1.0 + gg - 2.0 * g * cosine;
  return (1.0 - gg) / (4.0 * pf07_pi * max(denominator * sqrt(max(denominator, 1e-6)), 1e-6));
}

// Телесный угол диска с угловым радиусом theta. Наивная запись 2*pi*(1 - cos(theta)) при малых углах
// катастрофически теряет значимость: у диска светила theta около 0.004 рад, и на этой видеокарте
// cos возвращает для него ровно 1.0. Разность обращается в НОЛЬ, деление освещённости на неё даёт
// бесконечность, та обрезается потолком half-float, и диск выходит равномерно белым.
float pf07_disc_solid_angle(const float angular_radius) {
  const float half_sine = sin(angular_radius * 0.5);
  return 4.0 * pf07_pi * half_sine * half_sine;
}

// Ближайшее положительное пересечение луча со сферой радиуса radius с центром в начале координат.
float pf07_sphere_hit(const vec3 origin, const vec3 direction, const float radius) {
  const float b = dot(origin, direction);
  const float c = dot(origin, origin) - radius * radius;
  const float discriminant = b * b - c;
  if (discriminant < 0.0) return -1.0;

  const float root = sqrt(discriminant);
  const float near_hit = -b - root;
  const float far_hit = -b + root;
  if (near_hit >= 0.0) return near_hit;
  return far_hit >= 0.0 ? far_hit : -1.0;
}

// Доля света, доходящая от точки до светила: одна выборка вместо вложенного марша.
vec3 pf07_transmittance_to_light(const pf07_sky_block sky, const sampler2D table, const vec3 position,
                                 const vec3 light_direction) {
  const float ground_radius = sky.atmosphere_geometry.x;
  const float top_radius = sky.atmosphere_geometry.y;

  const float length_to_centre = max(length(position), 1e-6);
  const float radius = clamp(length_to_centre, ground_radius, top_radius);
  const float mu = dot(position / length_to_centre, light_direction);
  if (pf07_ray_hits_ground(radius, mu, ground_radius)) return vec3(0.0);

  const vec2 size = vec2(textureSize(table, 0));
  return texture(table, pf07_transmittance_uv(radius, mu, ground_radius, top_radius, size)).rgb;
}

// Вклад многократного рассеяния в точке: изотропная добавка, пропорциональная местному коэффициенту
// рассеяния. Именно её отсутствие делало небо вдали от светила слишком тёмным и серым.
vec3 pf07_multiscatter(const pf07_sky_block sky, const sampler2D table, const vec3 position,
                       const vec3 light_direction) {
  const float ground_radius = sky.atmosphere_geometry.x;
  const float top_radius = sky.atmosphere_geometry.y;
  const float length_to_centre = max(length(position), 1e-6);
  const float radius = clamp(length_to_centre, ground_radius, top_radius);
  const float mu_sun = dot(position / length_to_centre, light_direction);

  const vec2 size = vec2(textureSize(table, 0));
  return texture(table, pf07_multiscatter_uv(radius, mu_sun, ground_radius, top_radius, size)).rgb;
}

// Однократное рассеяние вдоль луча плюс изотропная добавка высоких порядков. Вынесено сюда, потому что этим же кодом считается и таблица
// sky-view, и короткий марш до поверхности во фрагментном шейдере: разойтись им нельзя.
vec3 pf07_march_scattering(const pf07_sky_block sky, const sampler2D transmittance_table,
                           const sampler2D multiscatter_table, const vec3 origin, const vec3 direction,
                           const float march_length, const int steps, out vec3 view_transmittance) {
  const float ground_radius = sky.atmosphere_geometry.x;
  const float mie_g = sky.atmosphere_medium.x;

  vec3 in_scattering = vec3(0.0);
  view_transmittance = vec3(1.0);
  if (march_length <= 0.0) return in_scattering;

  const float step_length = march_length / float(steps);
  for (int i = 0; i < steps; ++i) {
    const vec3 point = origin + direction * ((float(i) + 0.5) * step_length);
    const pf07_medium medium = pf07_sample_medium(sky, length(point) - ground_radius);
    const vec3 step_transmittance = exp(-medium.extinction * step_length);

    // Однократное рассеяние линейно по источнику, поэтому светила просто складываются. Это и есть
    // причина, по которой вторая звезда почти ничего не стоит: та же геометрия, второй вклад.
    vec3 source = vec3(0.0);
    for (int s = 0; s < PF07_STAR_COUNT; ++s) {
      const vec3 light_direction = sky.star_direction[s].xyz;
      const float illuminance = sky.star_color_illuminance[s].w;
      if (illuminance <= 0.0) continue;

      const vec3 light_transmittance =
        pf07_transmittance_to_light(sky, transmittance_table, point, light_direction);
      const float cosine = dot(direction, light_direction);
      const vec3 phase_weighted = medium.scattering_rayleigh * pf07_rayleigh_phase(cosine) +
                                  vec3(medium.scattering_mie * pf07_mie_phase(cosine, mie_g));

      // Многократное рассеяние приходит со всех сторон, поэтому фаза для него изотропна и в множитель
      // не входит: остаётся сам коэффициент рассеяния, помноженный на светимость из таблицы.
      const vec3 higher_orders = pf07_multiscatter(sky, multiscatter_table, point, light_direction) *
                                 (medium.scattering_rayleigh + vec3(medium.scattering_mie));

      source += (light_transmittance * phase_weighted + higher_orders) *
                sky.star_color_illuminance[s].rgb * illuminance;
    }

    // Луны освещают воздух наравне со светилами. Без этого ночное небо оставалось абсолютно чёрным
    // при освещённой луной земле — сочетание, которого в природе не бывает: свет, дошедший до земли,
    // обязан был пройти сквозь тот же воздух и часть его рассеять. Полнолуние даёт около 0.03 нит
    // рассеянного света, и на ночной экспозиции это отчётливо видимое синеватое небо.
    //
    // Многократное рассеяние для лун не считается сознательно: их вклад и так на пять порядков ниже
    // солнечного, а вторая выборка из таблицы удвоила бы цену марша ради невидимой добавки.
    const int moon_count = int(sky.march_params.w);
    for (int m = 0; m < moon_count && m < PF07_MOON_CAPACITY; ++m) {
      const float illuminance = sky.moon_color_illuminance[m].w;
      if (illuminance <= 0.0) continue;

      const vec3 light_direction = sky.moon_direction[m].xyz;
      const vec3 light_transmittance =
        pf07_transmittance_to_light(sky, transmittance_table, point, light_direction);
      const float cosine = dot(direction, light_direction);
      const vec3 phase_weighted = medium.scattering_rayleigh * pf07_rayleigh_phase(cosine) +
                                  vec3(medium.scattering_mie * pf07_mie_phase(cosine, mie_g));
      source += light_transmittance * phase_weighted * sky.moon_color_illuminance[m].rgb * illuminance;
    }

    // Аналитическое интегрирование вклада на шаге вместо умножения на длину: на длинных шагах у
    // горизонта это заметно точнее и не даёт полос.
    const vec3 safe_extinction = max(medium.extinction, vec3(1e-9));
    in_scattering += view_transmittance * (source - source * step_transmittance) / safe_extinction;
    view_transmittance *= step_transmittance;
  }
  return in_scattering;
}

#endif
