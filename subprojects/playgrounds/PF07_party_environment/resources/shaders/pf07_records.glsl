// Единственное объявление раскладки буферов PF07. Дублировать эти блоки по шейдерам нельзя: в PF02
// расхождение stride между двумя вершинными шейдерами однажды уже стоило регрессии, при которой все
// элементы буферов кроме нулевого читались со сдвигом.
//
// Система координат мира: X — восток, Y — вверх, Z — на юг. Небесный модуль отдаёт ENU (восток,
// север, зенит), перевод в мир делает CPU при упаковке, чтобы шейдер не знал про две системы сразу.
//
// Единицы физические. Освещённость светил в люксах, яркость неба в нитах (лм/м²/ср). Экспозиция
// применяется только на выводе, поэтому все промежуточные величины сравнимы между состояниями.

#ifndef PF07_RECORDS_GLSL
#define PF07_RECORDS_GLSL

#define PF07_STAR_COUNT 2
#define PF07_MOON_CAPACITY 4

struct pf07_sky_block {
  // xyz — направление на светило в мире, w — угловой радиус диска в радианах.
  vec4 star_direction[PF07_STAR_COUNT];
  // rgb — цвет светила, нормированный по максимальному каналу; w — перпендикулярная освещённость
  // вне атмосферы, люксы. Разделение сделано намеренно: цвет приходит из спектра Планка и не
  // зависит от расстояния, а освещённость меняется каждый кадр.
  vec4 star_color_illuminance[PF07_STAR_COUNT];

  vec4 moon_direction[PF07_MOON_CAPACITY];
  // rgb — цвет отражённого света, w — освещённость от луны в люксах.
  vec4 moon_color_illuminance[PF07_MOON_CAPACITY];
  // x — освещённая доля диска, y — глубина лунного затмения, z — множитель яркости рисуемого
  // диска, w — множитель его размера. Последние два — чистая презентация, физики не касаются.
  vec4 moon_phase[PF07_MOON_CAPACITY];

  // x — радиус планеты, км; y — верх атмосферы, км; z — шкала Рэлея, км; w — шкала Ми, км.
  vec4 atmosphere_geometry;
  // x — параметр анизотропии Ми; y — центр озонового слоя, км; z — полуширина озона, км;
  // w — мутность, множитель рассеяния Ми.
  vec4 atmosphere_medium;
  // x — высота камеры над поверхностью, км; y — число шагов основного марша; z — дальность таблицы
  // воздушной перспективы, км; w — число активных лун.
  vec4 march_params;
  // x — экспозиция, y — игровое время в сутках, z — альбедо поверхности, w — режим отладки.
  vec4 output_params;

  // Базис горизонта в инерциальной системе. Звёздное поле закреплено за небом, а не за наблюдателем,
  // поэтому направление взгляда переводится сюда перед выборкой звёзд.
  vec4 sky_basis_east;
  vec4 sky_basis_north;
  vec4 sky_basis_up;
  // x — множитель размера рисуемых дисков, y — плотность звёзд, z — яркость звёзд, w — яркость
  // галактической полосы.
  vec4 presentation_params;
  // Цветовой сценарий: rgb — оттенок, w — насыщенность.
  vec4 grade_tint_saturation;
  // x — контраст, y — сила ночного зрения, zw — резерв.
  vec4 grade_curve;
  // Какие ТЕЛА получили каскады теней, кодом: 0..1 — звезда с этим индексом, 2+m — луна m,
  // отрицательное — слот пуст. Без этого поля освещение и тени не нашли бы друг друга: система теней
  // берёт два самых ярких источника на небе, кем бы они ни были, и в цикле по звёздам и лунам иначе
  // нечем понять, чья это тень. x — первый слот, y — второй.
  vec4 shadow_bodies;
  // Ветер: xy — направление в горизонтальной плоскости, z — сила в метрах отклонения верхушки,
  // w — время в секундах. Поле одно на весь мир, потому что ветер — свойство погоды.
  vec4 wind_params;
};

// --- параметризация таблицы прохождения ---
//
// Таблица хранит долю света, доходящую от точки на радиусе r до верхней границы атмосферы вдоль луча
// с косинусом зенитного угла mu. Наивная сетка по (высота, mu) бесполезна: почти вся интересная часть
// функции сидит у горизонта, где mu меняется на тысячные доли. Поэтому по одной оси откладывается не
// сам mu, а ДЛИНА луча до края атмосферы, нормированная своим диапазоном, — тогда отсчёты сгущаются
// у горизонта сами собой. Параметризация Bruneton, и она обязана быть одна на запись и на выборку,
// иначе таблица читается со сдвигом.

// Отступ на половину текселя: без него крайние отсчёты размазываются линейной фильтрацией за границу.
float pf07_unit_to_texture(const float value, const float texel_count) {
  return 0.5 / texel_count + value * (1.0 - 1.0 / texel_count);
}

float pf07_texture_to_unit(const float coordinate, const float texel_count) {
  return (coordinate - 0.5 / texel_count) / (1.0 - 1.0 / texel_count);
}

// Пересекает ли луч поверхность планеты. Луч, направленный вниз и проходящий ближе центра, чем
// радиус планеты, до верхней границы не доходит.
bool pf07_ray_hits_ground(const float radius, const float mu, const float ground_radius) {
  return mu < 0.0 && radius * radius * (mu * mu - 1.0) + ground_radius * ground_radius >= 0.0;
}

vec2 pf07_transmittance_uv(const float radius, const float mu, const float ground_radius,
                           const float top_radius, const vec2 size) {
  const float horizon = sqrt(max(0.0, top_radius * top_radius - ground_radius * ground_radius));
  const float rho = sqrt(max(0.0, radius * radius - ground_radius * ground_radius));
  const float distance_to_top =
    max(0.0, -radius * mu + sqrt(max(0.0, radius * radius * (mu * mu - 1.0) + top_radius * top_radius)));
  const float shortest = top_radius - radius;
  const float longest = rho + horizon;
  const float x_mu = longest > shortest ? (distance_to_top - shortest) / (longest - shortest) : 0.0;
  const float x_radius = horizon > 0.0 ? rho / horizon : 0.0;
  return vec2(pf07_unit_to_texture(clamp(x_mu, 0.0, 1.0), size.x),
              pf07_unit_to_texture(clamp(x_radius, 0.0, 1.0), size.y));
}

void pf07_transmittance_from_uv(const vec2 uv, const float ground_radius, const float top_radius,
                                const vec2 size, out float radius, out float mu) {
  const float x_mu = pf07_texture_to_unit(uv.x, size.x);
  const float x_radius = pf07_texture_to_unit(uv.y, size.y);
  const float horizon = sqrt(max(0.0, top_radius * top_radius - ground_radius * ground_radius));
  const float rho = horizon * x_radius;
  radius = sqrt(rho * rho + ground_radius * ground_radius);
  const float shortest = top_radius - radius;
  const float longest = rho + horizon;
  const float distance_to_top = shortest + x_mu * (longest - shortest);
  mu = distance_to_top == 0.0
         ? 1.0
         : clamp((horizon * horizon - rho * rho - distance_to_top * distance_to_top) /
                   (2.0 * radius * distance_to_top), -1.0, 1.0);
}

// --- параметризация таблицы sky-view ---
//
// Небо, посчитанное для одной высоты наблюдателя, зависит только от направления взгляда. Значит его
// можно посчитать один раз в небольшую таблицу и потом просто читать — и цена перестанет зависеть от
// разрешения экрана. По горизонтали откладывается азимут в мировой системе (а не относительно солнца:
// светил здесь два, единого опорного азимута не существует), по вертикали — высота над горизонтом с
// корневым сгущением, потому что вся интересная часть неба сидит у самого горизонта.

vec2 pf07_sky_view_uv(const vec3 direction, const vec2 size) {
  const float azimuth = atan(direction.x, -direction.z);
  const float u = azimuth / (2.0 * 3.14159265358979323846) + 0.5;

  const float sine = clamp(direction.y, -1.0, 1.0);
  const float elevation = asin(sine);
  const float shaped = sqrt(abs(elevation) / (0.5 * 3.14159265358979323846));
  const float v = 0.5 + 0.5 * shaped * sign(elevation);
  return vec2(u, pf07_unit_to_texture(clamp(v, 0.0, 1.0), size.y));
}

vec3 pf07_sky_view_direction(const vec2 uv, const vec2 size) {
  const float azimuth = (uv.x - 0.5) * 2.0 * 3.14159265358979323846;

  const float v = pf07_texture_to_unit(uv.y, size.y);
  const float shaped = (v - 0.5) * 2.0;
  const float elevation = sign(shaped) * shaped * shaped * (0.5 * 3.14159265358979323846);
  const float cosine = cos(elevation);
  return vec3(sin(azimuth) * cosine, sin(elevation), -cos(azimuth) * cosine);
}

// --- параметризация таблицы многократного рассеяния ---
//
// Двух осей достаточно: свет высоких порядков считается изотропным, поэтому направление ВЗГЛЯДА на него
// не влияет, и остаются только высота точки и зенитный угол светила.

vec2 pf07_multiscatter_uv(const float radius, const float mu_sun, const float ground_radius,
                          const float top_radius, const vec2 size) {
  const float u = pf07_unit_to_texture(clamp(mu_sun * 0.5 + 0.5, 0.0, 1.0), size.x);
  const float height = clamp((radius - ground_radius) / max(top_radius - ground_radius, 1e-6), 0.0, 1.0);
  return vec2(u, pf07_unit_to_texture(height, size.y));
}

void pf07_multiscatter_from_uv(const vec2 uv, const float ground_radius, const float top_radius,
                               const vec2 size, out float radius, out float mu_sun) {
  mu_sun = clamp(pf07_texture_to_unit(uv.x, size.x) * 2.0 - 1.0, -1.0, 1.0);
  radius = ground_radius + pf07_texture_to_unit(uv.y, size.y) * (top_radius - ground_radius);
}

#endif
