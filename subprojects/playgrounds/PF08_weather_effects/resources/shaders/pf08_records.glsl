// Единственное объявление раскладки буферов PF08. Дублировать эти блоки по шейдерам нельзя: в PF02
// расхождение stride между двумя вершинными шейдерами однажды уже стоило регрессии, при которой все
// элементы буферов кроме нулевого читались со сдвигом.
//
// Система координат мира: X — восток, Y — вверх, Z — на юг. Небесный модуль отдаёт ENU (восток,
// север, зенит), перевод в мир делает CPU при упаковке, чтобы шейдер не знал про две системы сразу.
//
// Единицы физические. Освещённость светил в люксах, яркость неба в нитах (лм/м²/ср). Экспозиция
// применяется только на выводе, поэтому все промежуточные величины сравнимы между состояниями.

#ifndef PF08_RECORDS_GLSL
#define PF08_RECORDS_GLSL

#define PF08_STAR_COUNT 2
#define PF08_MOON_CAPACITY 4

struct pf08_sky_block {
  // xyz — направление на светило в мире, w — угловой радиус диска в радианах.
  vec4 star_direction[PF08_STAR_COUNT];
  // rgb — цвет светила, нормированный по максимальному каналу; w — перпендикулярная освещённость
  // вне атмосферы, люксы. Разделение сделано намеренно: цвет приходит из спектра Планка и не
  // зависит от расстояния, а освещённость меняется каждый кадр.
  vec4 star_color_illuminance[PF08_STAR_COUNT];

  vec4 moon_direction[PF08_MOON_CAPACITY];
  // rgb — цвет отражённого света, w — освещённость от луны в люксах.
  vec4 moon_color_illuminance[PF08_MOON_CAPACITY];
  // x — альбедо, y — глубина лунного затмения, z — множитель яркости рисуемого
  // диска, w — множитель его размера. Последние два — чистая презентация, физики не касаются.
  vec4 moon_phase[PF08_MOON_CAPACITY];
  // x/y — видимая с луны доля диска каждого светила. Одно светило может быть закрыто планетой,
  // пока второе продолжает освещать и окрашивать луну.
  vec4 moon_star_visibility[PF08_MOON_CAPACITY];

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
  // x — контраст, y — сила ночного зрения, z — множитель яркости короны, w — резерв.
  vec4 grade_curve;
  // Какие ТЕЛА получили каскады теней, кодом: 0..1 — звезда с этим индексом, 2+m — луна m,
  // отрицательное — слот пуст. Без этого поля освещение и тени не нашли бы друг друга: система теней
  // берёт два самых ярких источника на небе, кем бы они ни были, и в цикле по звёздам и лунам иначе
  // нечем понять, чья это тень. x — первый слот, y — второй.
  vec4 shadow_bodies;
  // Ветер: xy — направление в горизонтальной плоскости, z — сила в метрах отклонения верхушки,
  // w — время в секундах. Поле одно на весь мир, а не отдельное на каждый куст.
  vec4 wind_params;
  // Локальная froxel-среда: x — extinction, 1/м; y — scattering albedo; z — HG anisotropy;
  // w — дальность объёма, м. Нулевой extinction включает точный clear-bypass композиции.
  vec4 fog_params;
  // x — высота начала экспоненциального спада, м; y — scale height, м.
  vec4 fog_shape;
  // x — амплитуда неоднородности; y — размер ячейки, м; z — скорость advection, м/с.
  // w — stride lighting вдоль Z только при активных осадках; density остаётся полночастотной.
  vec4 fog_noise;
  // Облака: coverage, extinction 1/м, scattering albedo и HG anisotropy.
  vec4 cloud_params;
  // Нижняя/верхняя граница слоя и дальность froxel-интеграла в метрах.
  vec4 cloud_shape;
  // x — размер world-space ячейки, м; y — скорость advection, м/с.
  vec4 cloud_motion;
  // Дождь: rate мм/ч, скорость падения, горизонтальная скорость ветра и near-radius в метрах.
  vec4 precipitation_params;
  // Длина капли, дальний extinction 1/м, начало far LOD и дальность volume в метрах.
  vec4 precipitation_shape;
  // x — dt реального кадра, y — номер кадра, z — lifetime impact, секунды.
  vec4 precipitation_time;
  // Снег: rate водного эквивалента, скорость падения, горизонтальный перенос и near-radius.
  vec4 snow_params;
  // Размер хлопья, дальний extinction, начало far LOD и дальность volume.
  vec4 snow_shape;
  // AABB ВИДИМОЙ крыши. w > 0 включает один и тот же shelter-test в particle и froxel consumers.
  vec4 shelter_minimum;
  vec4 shelter_maximum;
  // Инициализация world-map: rain mm, snow-water mm, dry half-life h и visual-response enable.
  vec4 surface_weather;
  // World-map: half extent m, cell size m, world seconds/real second и snow melt mm/h.
  vec4 surface_weather_shape;
  // Общий footprint near/mid/far/surface: coverage, cell metres, advection m/s, edge softness.
  vec4 precipitation_field;
  // Приповерхностная splash-взвесь и границы среднего particle LOD.
  vec4 precipitation_mist_lod;
  // Освещённость светила БЕЗ затмения и без горизонта, по одной на звезду в x и y. Нужна ровно диску:
  // затмение теперь показывается геометрически — луна закрывает часть диска собой, — и дополнительно
  // гасить сам диск значило бы посчитать затмение дважды.
  vec4 star_disc_illuminance;
  // Расстояния до лун, км. Нужны, чтобы при наложении дисков ближняя луна закрывала дальнюю. Порядок
  // лун в массиве при этом НЕ переставляется: по индексу луны его читает система теней, и сортировка
  // здесь развела бы освещение с затенением без единого предупреждения.
  vec4 moon_distance_km;
  // Художественные параметры радуги. Физическая геометрия отделена от её заметности.
  // appearance: intensity, saturation, band width, edge sharpness.
  vec4 rainbow_appearance;
  // context: veil, local background contrast, memory persistence, current-rain cutoff mm/h.
  vec4 rainbow_context;
  // sources: primary/brightest/all mode, secondary bow, source balance, separation scale.
  vec4 rainbow_sources;
  // Художественный снег: intensity, доля активных микрограней, sharpness и баланс светил.
  vec4 snow_sparkle;
  // Молния: начало/channel envelope; конец/flash envelope; цвет/intensity cd;
  // физический радиус канала, яркость канала, радиус локального glow и deterministic path seed.
  vec4 lightning_start_channel;
  vec4 lightning_end_flash;
  vec4 lightning_colour_intensity;
  vec4 lightning_shape;
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
float pf08_unit_to_texture(const float value, const float texel_count) {
  return 0.5 / texel_count + value * (1.0 - 1.0 / texel_count);
}

float pf08_texture_to_unit(const float coordinate, const float texel_count) {
  return (coordinate - 0.5 / texel_count) / (1.0 - 1.0 / texel_count);
}

// Пересекает ли луч поверхность планеты. Луч, направленный вниз и проходящий ближе центра, чем
// радиус планеты, до верхней границы не доходит.
bool pf08_ray_hits_ground(const float radius, const float mu, const float ground_radius) {
  return mu < 0.0 && radius * radius * (mu * mu - 1.0) + ground_radius * ground_radius >= 0.0;
}

vec2 pf08_transmittance_uv(const float radius, const float mu, const float ground_radius,
                           const float top_radius, const vec2 size) {
  const float horizon = sqrt(max(0.0, top_radius * top_radius - ground_radius * ground_radius));
  const float rho = sqrt(max(0.0, radius * radius - ground_radius * ground_radius));
  const float distance_to_top =
    max(0.0, -radius * mu + sqrt(max(0.0, radius * radius * (mu * mu - 1.0) + top_radius * top_radius)));
  const float shortest = top_radius - radius;
  const float longest = rho + horizon;
  const float x_mu = longest > shortest ? (distance_to_top - shortest) / (longest - shortest) : 0.0;
  const float x_radius = horizon > 0.0 ? rho / horizon : 0.0;
  return vec2(pf08_unit_to_texture(clamp(x_mu, 0.0, 1.0), size.x),
              pf08_unit_to_texture(clamp(x_radius, 0.0, 1.0), size.y));
}

void pf08_transmittance_from_uv(const vec2 uv, const float ground_radius, const float top_radius,
                                const vec2 size, out float radius, out float mu) {
  const float x_mu = pf08_texture_to_unit(uv.x, size.x);
  const float x_radius = pf08_texture_to_unit(uv.y, size.y);
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

vec2 pf08_sky_view_uv(const vec3 direction, const vec2 size) {
  const float azimuth = atan(direction.x, -direction.z);
  const float u = azimuth / (2.0 * 3.14159265358979323846) + 0.5;

  const float sine = clamp(direction.y, -1.0, 1.0);
  const float elevation = asin(sine);
  const float shaped = sqrt(abs(elevation) / (0.5 * 3.14159265358979323846));
  const float v = 0.5 + 0.5 * shaped * sign(elevation);
  return vec2(u, pf08_unit_to_texture(clamp(v, 0.0, 1.0), size.y));
}

vec3 pf08_sky_view_direction(const vec2 uv, const vec2 size) {
  const float azimuth = (uv.x - 0.5) * 2.0 * 3.14159265358979323846;

  const float v = pf08_texture_to_unit(uv.y, size.y);
  const float shaped = (v - 0.5) * 2.0;
  const float elevation = sign(shaped) * shaped * shaped * (0.5 * 3.14159265358979323846);
  const float cosine = cos(elevation);
  return vec3(sin(azimuth) * cosine, sin(elevation), -cos(azimuth) * cosine);
}

// --- параметризация таблицы многократного рассеяния ---
//
// Двух осей достаточно: свет высоких порядков считается изотропным, поэтому направление ВЗГЛЯДА на него
// не влияет, и остаются только высота точки и зенитный угол светила.

vec2 pf08_multiscatter_uv(const float radius, const float mu_sun, const float ground_radius,
                          const float top_radius, const vec2 size) {
  const float u = pf08_unit_to_texture(clamp(mu_sun * 0.5 + 0.5, 0.0, 1.0), size.x);
  const float height = clamp((radius - ground_radius) / max(top_radius - ground_radius, 1e-6), 0.0, 1.0);
  return vec2(u, pf08_unit_to_texture(height, size.y));
}

void pf08_multiscatter_from_uv(const vec2 uv, const float ground_radius, const float top_radius,
                               const vec2 size, out float radius, out float mu_sun) {
  mu_sun = clamp(pf08_texture_to_unit(uv.x, size.x) * 2.0 - 1.0, -1.0, 1.0);
  radius = ground_radius + pf08_texture_to_unit(uv.y, size.y) * (top_radius - ground_radius);
}

#endif
