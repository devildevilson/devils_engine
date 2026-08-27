#version 450

#include "pf07_atmosphere.glsl"

// Небо считается честным маршем по атмосфере прямо здесь, без предрасчитанных таблиц. Это заведомо
// дорого и выбрано намеренно: следующий шаг среза заменит марш LUT-моделью, и тогда у выигрыша будет
// измеренная база, а не ссылка на литературу.
//
// Модель: рэлеевское рассеяние на молекулах, рассеяние Ми на аэрозоле и поглощение озоном. Двухзвёздность
// не требует ничего особенного, потому что однократное рассеяние ЛИНЕЙНО по источнику: вклады светил
// считаются независимо и складываются. Многократное рассеяние здесь опущено — из-за этого зенит в сумерках
// темнее реального, и это первое, что вернёт LUT-модель.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(set = 0, binding = 1, std140) uniform SkyBlock {
  pf07_sky_block sky;
} sky_data;

layout(set = 0, binding = 2) uniform sampler2D transmittance_lut;
layout(set = 0, binding = 3) uniform sampler2D multiscatter_lut;
layout(set = 0, binding = 4) uniform sampler2D sky_view_lut;
layout(set = 0, binding = 5) uniform sampler3D aerial_lut;

const float pi = pf07_pi;

// Звёздное поле. Направление переводится в инерциальную систему, поэтому звёзды закреплены за небом
// и вращаются вместе с планетой, а не едут за камерой. Сетка ячеек по направлению: в каждой ячейке
// одна звезда со своим смещением, размером и яркостью.
//
// Яркость здесь сознательно преувеличена. Настоящее звёздное небо даёт около 0.002 лк, и при любой
// экспозиции, на которой видна луна, отдельные звёзды физически неразличимы. Это первая явная
// художественная добавка в срезе, и она отделена от всего остального одним параметром.
float hash_cell(const vec3 cell) {
  return fract(sin(dot(cell, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
}

// Ячейки берутся по граням куба, а не по кубической решётке в пространстве направлений. Решётка
// давала кольцевые сгущения: соседние по индексу ячейки лежат на разном удалении от начала координат,
// и после нормировки их звёзды собираются в оболочку. Грани куба дают почти равновеликие ячейки.
vec3 star_cell_direction(const int face, const vec2 cell_uv) {
  if (face == 0) return normalize(vec3(1.0, cell_uv.x, cell_uv.y));
  if (face == 1) return normalize(vec3(-1.0, cell_uv.x, cell_uv.y));
  if (face == 2) return normalize(vec3(cell_uv.x, 1.0, cell_uv.y));
  if (face == 3) return normalize(vec3(cell_uv.x, -1.0, cell_uv.y));
  if (face == 4) return normalize(vec3(cell_uv.x, cell_uv.y, 1.0));
  return normalize(vec3(cell_uv.x, cell_uv.y, -1.0));
}

// Галактическая плоскость. Система живёт внутри галактики, и с планеты та видна как сгущение звёзд
// вдоль большого круга.
//
// Раньше здесь была РАЗМЫТАЯ ЛЕНТА СВЕТА, и у неё была видимая граница: гауссиана обрывалась порогом
// `band < 0.002`, а ровный градиент на почти чёрном небе показывает такой обрыв как чёткий край. Это
// та же болезнь, что у любой мягкой добавки поверх темноты — глаз находит край там, где математика
// считает его исчезающе слабым.
//
// Поэтому светимости здесь больше нет вовсе. Полоса выражена только ПЛОТНОСТЬЮ звёзд: их в этой
// области больше, и граница исчезает по построению — у россыпи точек края не бывает, сколько бы
// плавно ни менялась их частота. Рисунок звёздных облаков и тёмных прожилок отложен: он требует
// текстуры или шума, а не ещё одной формулы.
// Переход в звёздный базис: небо, закреплённое за системой, а не за горизонтом планеты. Одна функция
// на всех потребителей — россыпь звёзд и галактическую плоскость: разойдись они, полоса поехала бы
// относительно созвездий, которые сама же и населяет.
vec3 sky_inertial_direction(const vec3 direction) {
  const mat3 to_inertial = mat3(sky_data.sky.sky_basis_east.xyz, sky_data.sky.sky_basis_up.xyz,
                                -sky_data.sky.sky_basis_north.xyz);
  return normalize(to_inertial * direction);
}

float galactic_band(const vec3 inertial) {
  // Плоскость галактики наклонена к плоскости двойной: иначе лента совпала бы с путём светил и
  // читалась бы как ошибка, а не как отдельная структура.
  const vec3 galactic_pole = normalize(vec3(0.31, 0.82, -0.48));
  const float height = dot(inertial, galactic_pole);
  // Шире прежнего почти вшестеро: настоящая лента занимает десятки градусов, а прежние три с
  // половиной делали из неё шов. Сгущение вдоль ленты оставлено — оно даёт неоднородность россыпи.
  const float across = exp(-height * height / 0.045);
  const vec3 along = normalize(inertial - galactic_pole * height);
  const float clumping = 0.6 + 0.4 * sin(atan(along.z, along.x) * 3.0 + along.y * 4.0);
  return across * clumping;
}

vec3 star_field(const vec3 direction) {
  const float density = max(sky_data.sky.presentation_params.y, 1e-3);
  const float brightness = sky_data.sky.presentation_params.z;
  if (brightness <= 0.0) return vec3(0.0);

  const vec3 inertial = sky_inertial_direction(direction);

  const vec3 magnitude_axis = abs(inertial);
  int face;
  vec2 face_uv;
  if (magnitude_axis.x >= magnitude_axis.y && magnitude_axis.x >= magnitude_axis.z) {
    face = inertial.x > 0.0 ? 0 : 1;
    face_uv = inertial.yz / magnitude_axis.x;
  } else if (magnitude_axis.y >= magnitude_axis.z) {
    face = inertial.y > 0.0 ? 2 : 3;
    face_uv = inertial.xz / magnitude_axis.y;
  } else {
    face = inertial.z > 0.0 ? 4 : 5;
    face_uv = inertial.xy / magnitude_axis.z;
  }

  // 40 ячеек на ребро грани при заполнении 11% дают около 1050 звёзд на всё небо. Меньше, чем видит
  // невооружённый глаз, и это осознанно: звезда обязана быть крупнее пикселя (см. ниже), а поле из
  // трёх тысяч заметных зёрен читается шумом.
  const float grid = 40.0 * density;
  const vec2 scaled = face_uv * grid;
  const vec2 base = floor(scaled);

  // Занятость ячейки — та самая плотность, которой и выражена галактическая полоса. Множитель
  // считается один раз на пиксель по направлению взгляда: ячейки в окрестности отстоят от него на
  // доли градуса, и разница между ними на масштабе полосы в десятки градусов неразличима.
  const float occupancy = 0.11 * (1.0 + sky_data.sky.presentation_params.w * 2.5 * galactic_band(inertial));

  vec3 total = vec3(0.0);
  for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
      const vec2 cell = base + vec2(float(x), float(y));
      const vec3 key = vec3(cell, float(face) * 37.0);
      const float seed = hash_cell(key);
      if (seed > occupancy) continue;

      const vec2 jitter = vec2(hash_cell(key + 17.0), hash_cell(key + 43.0));
      const vec3 star_direction = star_cell_direction(face, (cell + jitter) / grid);
      const float angle = acos(clamp(dot(inertial, star_direction), -1.0, 1.0));

      const float magnitude = hash_cell(key + 91.0);
      // Звезда МЕНЬШЕ пикселя мерцает: при вращении неба она перескакивает между соседними пикселями,
      // и её яркость скачет вместе с попаданием в центр. Лечится это не сглаживанием, а размером —
      // радиус не даётся опуститься ниже полутора пикселей, а сколько это в радианах, зависит от поля
      // зрения и разрешения. Та же логика, что у экранно-осведомлённой ширины фильтра теней в PF02.
      const float pixel_angle = 2.0 * camera_data.viewport_near.w / max(camera_data.viewport_near.y, 1.0);
      const float radius = max(0.0007 + 0.0011 * magnitude * magnitude, 1.5 * pixel_angle);
      if (angle > radius) continue;

      const float falloff = 1.0 - smoothstep(0.0, radius, angle);
      // Холодные и тёплые звёзды: тот же спектр Планка, только грубее.
      const vec3 tint = mix(vec3(1.0, 0.86, 0.70), vec3(0.78, 0.86, 1.0), hash_cell(key + 13.0));
      // Яркость держится на постоянном ПОТОКЕ, а не на постоянной яркости поверхности: раздутая до
      // полутора пикселей звезда иначе стала бы во столько же раз ярче, во сколько выросла её площадь.
      const float area_correction = (0.0007 * 0.0007) / (radius * radius);
      total += tint * falloff * falloff * brightness * 4000.0 * (0.15 + magnitude * magnitude) *
               clamp(area_correction, 0.05, 1.0);
    }
  }
  return total;
}

void main() {
  const vec2 ndc = in_uv * 2.0 - 1.0;
  const float aspect = camera_data.viewport_near.x / max(camera_data.viewport_near.y, 1.0);
  // Направление луча восстанавливается из матрицы вида, а не из отдельного набора углов: так оно
  // остаётся согласованным с камерой при любом изменении FOV и разрешения.
  const mat3 camera_to_world = transpose(mat3(camera_data.view));
  const float tan_half_fov = camera_data.viewport_near.w;
  const vec3 view_direction =
    normalize(camera_to_world * vec3(ndc.x * aspect * tan_half_fov, -ndc.y * tan_half_fov, -1.0));

  const float planet_radius = sky_data.sky.atmosphere_geometry.x;
  const float atmosphere_top = sky_data.sky.atmosphere_geometry.y;
  const float camera_height = sky_data.sky.march_params.x;
  const int moon_count = int(sky_data.sky.march_params.w);

  // Начало луча берёт и ГОРИЗОНТАЛЬНОЕ положение камеры. Для самого неба оно безразлично — сдвиг на
  // десяток метров по шеститысячекилометровой сфере не меняет ничего, — но точка попадания в землю
  // обязана совпадать с тем местом, где стоит геометрия, иначе тени на земле уедут от предметов.
  const vec3 origin = vec3(camera_data.camera_position.x * 0.001, planet_radius + camera_height,
                           camera_data.camera_position.z * 0.001);
  const float ground_distance = pf07_ground_hit(origin, view_direction, planet_radius, camera_height);

  // Луч, уходящий в землю, небом больше не занимается вовсе: поверхность стала ГЕОМЕТРИЕЙ и рисуется
  // отдельным проходом, который перекроет эти пиксели по глубине. Здесь остаётся один тест — чтобы не
  // рисовать под горизонтом диски и звёзды.
  //
  // Убрано отсюда было не украшение, а весь расчёт освещения поверхности вместе с чтением карты теней.
  // Ветка эта исполнялась не всегда, но платили за неё всегда: код теней поднимал расход регистров
  // целого шейдера, и кадр без земли в поле зрения стоил 6.9 мс вместо 4.9.
  vec3 in_scattering = vec3(0.0);
  vec3 transmittance = vec3(0.0);
  if (ground_distance <= 0.0) {
    // Небо целиком приходит из таблицы: одна выборка вместо тридцати двух шагов марша на пиксель.
    const vec2 sky_view_size = vec2(textureSize(sky_view_lut, 0));
    const vec4 sampled = texture(sky_view_lut, pf07_sky_view_uv(view_direction, sky_view_size));
    in_scattering = sampled.rgb;
    transmittance = vec3(sampled.a);
  }

  vec3 color = in_scattering;

  if (ground_distance <= 0.0) {
    // Всё, что лежит ЗА атмосферой, собирается отдельно и лишь потом умножается на её прохождение.
    // Разделение принципиальное: луна обязана ЗАКРЫВАТЬ собой то, что за ней, а рассеянный свет
    // воздуха висит перед ней и никуда не девается.
    //
    // Прежде диски просто ПРИБАВЛЯЛИСЬ к цвету. Из-за этого тёмная луна прибавляла ноль и была
    // невидима вовсе, а солнечное затмение выражалось лишь в том, что диск светила равномерно тускнел
    // — и это падение яркости экспозиция послушно возвращала обратно. Затмение нельзя было разглядеть
    // в принципе, и по той же причине не темнели луны, стоящие против света.
    vec3 space = vec3(0.0);

    // Звёзды гаснут в светлом небе. Настоящая звезда занимает ничтожную долю пикселя, и рядом с
    // рассеянным светом её вклад теряется; здесь же звезда рисуется целым пикселем, поэтому днём
    // она пробивалась сквозь небо.
    const float sky_luminance = dot(in_scattering, vec3(0.2126, 0.7152, 0.0722));
    const float star_visibility = 1.0 / (1.0 + sky_luminance / 40.0);
    space += star_field(view_direction) * star_visibility;

    // Множитель размера дисков — осознанное преувеличение ради читаемости: 0.9° диска Selen при поле
    // зрения 65° занимают десяток пикселей. Яркость поверхности диска при этом НЕ меняется, потому
    // что телесный угол в знаменателе остаётся физическим: тело становится крупнее, а не ярче.
    const float disc_scale = max(sky_data.sky.presentation_params.x, 1.0);

    for (int s = 0; s < PF07_STAR_COUNT; ++s) {
      const vec3 light_direction = sky_data.sky.star_direction[s].xyz;
      const float angular_radius = sky_data.sky.star_direction[s].w;
      // Яркость диска берётся БЕЗ затмения: закрывать его будет сама луна, геометрически.
      const float illuminance = sky_data.sky.star_disc_illuminance[s];
      if (illuminance <= 0.0 || angular_radius <= 0.0) continue;

      const float drawn_radius = angular_radius * disc_scale;
      // Корона тянется на несколько радиусов от диска. Именно она делает затмение зрелищем: диск
      // закрыт, а вокруг него остаётся светящееся кольцо внешней атмосферы светила.
      const float corona_reach = drawn_radius * 5.0;
      const float angle = acos(clamp(dot(view_direction, light_direction), -1.0, 1.0));
      if (angle > corona_reach) continue;

      const float solid_angle = pf07_disc_solid_angle(angular_radius);
      const vec3 photosphere = sky_data.sky.star_color_illuminance[s].rgb * illuminance / solid_angle;

      if (angle <= drawn_radius) {
        // Потемнение к краю: диск не однороден, и без него светило читается как наклейка.
        const float edge = sqrt(max(0.0, 1.0 - pow(angle / drawn_radius, 2.0)));
        const float limb = 0.4 + 0.6 * pow(edge, 0.4);
        space += photosphere * limb;
        continue;
      }

      // Корона слабее фотосферы в МИЛЛИОН раз — оттого её и не видно, пока диск открыт. Спад примерно
      // как r^-2.5 от края диска. Множитель снаружи: в одиночной системе при полном затмении небо
      // темнеет в тысячу раз и физической яркости хватает с избытком, а здесь второе светило держит
      // небо дневным, и корона едва его превышает.
      const float corona_strength = max(sky_data.sky.grade_curve.z, 0.0);
      if (corona_strength <= 0.0) continue;

      // Корона проявляется по мере ЗАКРЫТИЯ диска, и это модель восприятия, а не физика: сама корона
      // никуда не девается, но рядом с открытым диском её топит блик — рассеяние в глазу и в оптике.
      // Блик здесь не моделируется вовсе, поэтому без этого множителя незатменное светило обрастало бы
      // ореолом, которого в жизни у него не видно.
      //
      // Доля закрытия выводится из двух уже имеющихся величин: освещённость с затмением и без него.
      const float open = sky_data.sky.star_color_illuminance[s].w;
      const float covered = clamp(1.0 - open / max(illuminance, 1e-6), 0.0, 1.0);
      const float reveal = smoothstep(0.15, 0.85, covered);
      if (reveal <= 0.0) continue;

      const float radii = angle / max(drawn_radius, 1e-6);
      // Ближе к диску корона теплее — там лежит хромосфера, и именно она даёт затмению золотой ободок.
      // Тёплая зона тянется до трёх радиусов не для красоты: луна крупнее светила и закрывает собой
      // всё до 1.76 радиуса, поэтому ободок, уложенный вплотную к диску, целиком уходит ей за спину.
      // Тон взят с сильным перекосом в тёплое не для красоты. Небо во время затмения здесь остаётся
      // ДНЕВНЫМ — компаньон никуда не делся, — поэтому кольцо неизбежно подходит к потолку яркости, а
      // в насыщенном пикселе цвета не остаётся вовсе. Чтобы золото пережило насыщение, синий канал
      // приходится уводить вниз втрое: тогда красный упирается в потолок раньше синего, и разница
      // между ними сохраняется.
      const vec3 tint = mix(vec3(1.55, 0.98, 0.42), vec3(1.0), smoothstep(1.0, 3.2, radii));
      space += photosphere * sky_data.sky.star_color_illuminance[s].rgb * tint * 1.0e-6 *
               corona_strength * reveal * pow(radii, -2.5);
    }

    // Луны. Ближайшая из перекрывшихся закрывает остальные, поэтому сначала выбирается победитель по
    // расстоянию, и лишь затем он заменяет собой всё, что было за ним.
    const int moon_count = int(sky_data.sky.march_params.w);
    float nearest_moon_distance = 1.0e30;
    vec3 nearest_moon_radiance = vec3(0.0);
    bool moon_hit = false;

    for (int m = 0; m < moon_count && m < PF07_MOON_CAPACITY; ++m) {
      const vec3 moon_direction = sky_data.sky.moon_direction[m].xyz;
      const float angular_radius = sky_data.sky.moon_direction[m].w;
      if (angular_radius <= 0.0) continue;

      // Увеличение дисков ломало бы геометрию затмений, и это была настоящая причина их невидимости:
      // радиусы раздуваются втрое, а угловое РАССТОЯНИЕ между телами оставалось прежним, и при
      // настоящем перекрытии в 46% нарисованная луна закрывала светило целиком. Вместо надкушенного
      // солнца зритель видел просто исчезнувшее солнце.
      //
      // Поэтому увеличивается и расстояние: при одинаковом множителе у радиусов и у разноса ДОЛЯ
      // перекрытия сохраняется в точности. Внутри касания разнос растёт вместе с дисками, снаружи
      // луна прижимается к точке касания — иначе нарисованные диски перекрывались бы там, где
      // настоящие уже разошлись.
      vec3 drawn_direction = moon_direction;
      for (int s = 0; s < PF07_STAR_COUNT; ++s) {
        const float star_radius = sky_data.sky.star_direction[s].w;
        if (star_radius <= 0.0 || sky_data.sky.star_disc_illuminance[s] <= 0.0) continue;

        const vec3 star_direction = sky_data.sky.star_direction[s].xyz;
        const float contact = star_radius + angular_radius;
        const float drawn_contact = contact * disc_scale;
        const float separation = acos(clamp(dot(star_direction, moon_direction), -1.0, 1.0));
        if (separation >= drawn_contact) continue;

        const float target = separation <= contact ? separation * disc_scale : drawn_contact;
        const vec3 axis = normalize(cross(star_direction, moon_direction) + vec3(1e-7, 0.0, 0.0));
        drawn_direction = star_direction * cos(target) + cross(axis, star_direction) * sin(target);
      }

      const float drawn_radius = angular_radius * disc_scale * max(sky_data.sky.moon_phase[m].w, 1.0);
      const float angle = acos(clamp(dot(view_direction, drawn_direction), -1.0, 1.0));
      if (angle > drawn_radius) continue;

      const float distance_km = sky_data.sky.moon_distance_km[m];
      if (distance_km >= nearest_moon_distance) continue;

      // Терминатор строится геометрически: нормаль точки диска восстанавливается из её положения на
      // видимой полусфере. Нормаль смотрит НА наблюдателя — с обратным знаком освещалась бы дальняя
      // сторона, и любой серп выглядел бы полным диском.
      //
      // Светил два, и терминатор у луны тоже двойной: солнца разнесены на небе до пятнадцати градусов,
      // поэтому вдоль края серпа идёт узкая полоса, освещённая только компаньоном.
      const vec3 local_x = normalize(cross(drawn_direction, vec3(0.0, 1.0, 0.0)) + vec3(1e-5, 0.0, 0.0));
      const vec3 local_y = cross(drawn_direction, local_x);
      const vec3 tangent = view_direction - drawn_direction * dot(view_direction, drawn_direction);
      const float sine_radius = max(sin(drawn_radius), 1e-6);
      const float offset_x = dot(tangent, local_x) / sine_radius;
      const float offset_y = dot(tangent, local_y) / sine_radius;
      const float radial = clamp(sqrt(offset_x * offset_x + offset_y * offset_y), 0.0, 1.0);
      const vec3 surface_normal = normalize(-drawn_direction * sqrt(max(0.0, 1.0 - radial * radial)) +
                                            local_x * offset_x + local_y * offset_y);

      // Яркость диска считается ФИЗИЧЕСКИ: ламбертова поверхность с известным альбедо под светом
      // звёзд. Прежде она бралась из освещённости, которую луна шлёт НАМ, а та обращается в ноль в
      // новолуние — вместе со всем диском, включая ту его часть, которую всё же видно.
      vec3 radiance = vec3(0.0);
      for (int s = 0; s < PF07_STAR_COUNT; ++s) {
        const float star_illuminance = sky_data.sky.star_disc_illuminance[s];
        if (star_illuminance <= 0.0) continue;
        const float lit = max(0.0, dot(surface_normal, sky_data.sky.star_direction[s].xyz));
        const float visibility = clamp(sky_data.sky.moon_star_visibility[m][s], 0.0, 1.0);
        radiance += sky_data.sky.star_color_illuminance[s].rgb * star_illuminance * lit * visibility;
      }
      const float albedo = max(sky_data.sky.moon_phase[m].x, 0.0);
      // Множитель заметности — осознанная неправда ради читаемости: физически Kolo и Iskra дают доли
      // процента общего света, и найти их на небе почти невозможно. Освещения сцены и затмений он не
      // касается, только нарисованного диска.
      const float boost = max(sky_data.sky.moon_phase[m].z, 1.0);

      nearest_moon_distance = distance_km;
      nearest_moon_radiance = radiance * (albedo / pi) * boost;
      moon_hit = true;
    }

    // Луна ЗАМЕЩАЕТ собой всё, что за ней: звёзды, диски светил, галактическую россыпь. Именно это и
    // делает затмение видимым — тёмный диск на месте светила, а не равномерно потускневшее светило.
    if (moon_hit) space = nearest_moon_radiance;

    color += transmittance * space;
  }

  // Отладочные режимы: 1 — направление луча, 2 — попадание в поверхность, 3 — длина марша.
  // Режим 4 выставляет цвет внутри цикла лун, поэтому он сюда не попадает: иначе разбор терминатора
  // затирался бы длиной марша, и отладка показывала бы одинаковые каналы вместо трёх разных величин.
  const float debug_mode = sky_data.sky.output_params.w;
  if (debug_mode > 6.5) {
    // Режим 7: галактическая плоскость как величина, а не как россыпь. Полоса выражена ПЛОТНОСТЬЮ
    // звёзд, и на глаз по готовому небу не понять, попала она в кадр или нет — а без этого подбор
    // ширины превращается в угадывание.
    out_color = vec4(vec3(galactic_band(sky_inertial_direction(view_direction))), 1.0);
    return;
  }
  if (debug_mode > 5.5) {
    // Режим 6: сама таблица прохождения, растянутая на экран.
    out_color = vec4(texture(transmittance_lut, in_uv).rgb, 1.0);
    return;
  }
  if (debug_mode > 0.5 && debug_mode < 3.5) {
    if (debug_mode < 1.5) color = view_direction * 0.5 + 0.5;
    else if (debug_mode < 2.5) color = ground_distance > 0.0 ? vec3(1.0, 0.2, 0.1) : vec3(0.1, 0.3, 1.0);
    else color = vec3(log2(max(ground_distance, 1e-3)) / 12.0);
  }

  // Потолок half-float. Яркость диска светила — освещённость, делённая на его телесный угол, то есть
  // около 2e9 нит: в sf4 это бесконечность, а бесконечность в кривой тонмаппинга даёт NaN и чёрный
  // диск вместо белого. Ограничение стоит именно здесь, на границе формата, а не в тонмаппинге.
  const float half_float_ceiling = 60000.0;
  out_color = vec4(min(color, vec3(half_float_ceiling)), 1.0);
}
