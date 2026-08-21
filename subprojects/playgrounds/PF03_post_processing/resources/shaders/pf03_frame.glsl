#ifndef PF03_FRAME_GLSL
#define PF03_FRAME_GLSL

// Общий математический и wire-контракт PF03. Макрос ниже задаёт единственную std140 раскладку FrameBlock,
// далее собраны функции, которые обязаны означать одно и то же во всех эффектах: логарифмическая яркость,
// tone mapping, аналитический fog, low-discrepancy sampling, temporal range compression/Catmull-Rom,
// dithering, octahedral normals и reverse-Z linearization. Это не отдельный pass, а словарь всей цепочки.

// Единственный источник раскладки FrameBlock: все шейдеры площадки включают этот файл, поэтому раскладка
// не может разъехаться между ними (в PF02 именно дублирование записи стоило регрессии).
#define PF03_FRAME_BLOCK_BODY       \
  mat4 view_projection;             \
  mat4 previous_view_projection;    \
  mat4 inverse_view_projection;     \
  vec4 camera_position;             \
  vec4 viewport_near;               \
  vec4 controls;                    \
  vec4 light_direction;             \
  vec4 tonemap;                     \
  vec4 exposure_limits;             \
  vec4 fog_params;                  \
  vec4 fog_color;                   \
  vec4 ao_params;                   \
  vec4 taa_params;                  \
  vec4 taa_jitter;                  \
  vec4 bloom_params;                \
  vec4 shaft_params;                \
  vec4 lens_params;                 \
  vec4 output_params;               \
  vec4 metering;                    \
  vec4 grade_balance;               \
  vec4 grade_tone;                  \
  vec4 grade_slope;                 \
  vec4 grade_offset;                \
  vec4 grade_power;                 \
  vec4 grade_filter;                \
  vec4 lut_params;                  \
  vec4 hiz_params;                  \
  vec4 ssr_params;                  \
  vec4 dof_params;                  \
  vec4 dof_lens;                    \
  vec4 blur_params;

// viewport_near:    xy = размер кадра в пикселях, z = near, w = номер кадра с последнего сброса истории
// controls:         x = debug-режим, y = усиление motion при показе, z = усиление ошибки, w = кодировать sRGB
// light_direction:  xyz = направление НА свет, w = сила ambient
// tonemap:          x = оператор, y = ручная экспозиция (<= 0 => авто), z = скорость адаптации, w = dt в секундах
// exposure_limits:  x/y = границы log2 средней яркости, z = ключ (средний серый), w = яркость солнца
// fog_params:       x = плотность у опорной высоты, y = масштаб спада по высоте, z = опорная высота, w = анизотропия
// fog_color:        xyz = цвет рассеяния (уже с яркостью), w = вклад солнечного диска в рассеяние
// ao_params:        x = радиус в метрах, y = сила, z = порог по касательной плоскости,
//                   w = показатель контраста (0 => AO выключен целиком)
// taa_params:       x = вес истории, y = режим отбраковки (0 нет, 1 min/max, 2 дисперсия, 3 по скорости),
//                   z = 0 выключен / 1 bilinear-история / 2 Catmull-Rom история,
//                   w = временной сдвиг выборки AO
// taa_jitter:       xy = джиттер текущего кадра в UV, zw = джиттер предыдущего кадра в UV
// bloom_params:     x = сила, y = порог яркого прохода, z = мягкость колена порога, w = вес шага подъёма
// shaft_params:     xy = положение солнца на экране в UV, z = сила лучей, w = затухание вдоль луча
//                   (z <= 0 => солнце вне кадра либо лучи выключены)
// lens_params:      x = сила резкости, y = виньетка, z = хроматическая аберрация, w = зерно
// output_params:    x = дизеринг вкл, y = семя зерна (меняется по кадрам), z = яркость панели,
//                   w = вес перехода между поколениями таблицы грейда (1 — только текущее)
// metering:         x = нижний перцентиль, y = верхний перцентиль, z = сила центровзвешенности,
//                   w = скорость адаптации К ТЁМНОМУ (к яркому лежит в tonemap.z)
// grade_balance:    x = температура освещения в K, y = оттенок (зелёный–пурпурный), z = наивный баланс белого,
//                   w = контраст
// grade_tone:       x = насыщенность, y = опорная точка контраста, z = грейд включён,
//                   w = пространство грейда (0 scene-referred до кривой, 1 display-referred после кривой)
// grade_slope:      xyz = ASC CDL slope
// grade_offset:     xyz = ASC CDL offset
// grade_power:      xyz = ASC CDL power
// grade_filter:     xyz = цвет фильтра, w = его сила
// lut_params:       x = путь (0 аналитический, 1 таблица), y = shaper (0 log2, 1 линейный),
//                   z/w = границы shaper'а в стопах
// hiz_params:       x = уровень пирамиды для отладочного вида, y = число уровней, zw = резерв
// ssr_params:       x = сила отражений (0 — выключены), y = шероховатость, z = толщина поверхности в метрах,
//                   w = предел шагов марша
// dof_params:       x = сила глубины резкости (0 — выключена), y = уважать силуэты у заднего плана,
//                   z = предел CoC в пикселях (бюджет сбора), w = число уровней пирамиды боке
// dof_lens:         x = дистанция фокусировки в метрах, y = диафрагменное число, z = фокусное расстояние в мм,
//                   w = высота сенсора в мм
// blur_params:      x = доля открытой шторки (0 — motion blur выключен), yz = размер рендера в пикселях,
//                   w = размер плитки максимума motion в пикселях дисплея

#define PF03_DEBUG_SHADED         0
#define PF03_DEBUG_DEPTH          1
#define PF03_DEBUG_NORMAL         2
#define PF03_DEBUG_MOTION         3
#define PF03_DEBUG_REPROJECTED    4
#define PF03_DEBUG_ERROR_MOTION   5
#define PF03_DEBUG_ERROR_NAIVE    6
#define PF03_DEBUG_CLIPPING       7
#define PF03_DEBUG_CALIBRATION    8
#define PF03_DEBUG_EXPOSURE       9
#define PF03_DEBUG_TRANSMITTANCE  10
#define PF03_DEBUG_AO             11
#define PF03_DEBUG_AO_RAW         12
#define PF03_DEBUG_TAA_WEIGHT     13
#define PF03_DEBUG_BLOOM          14
#define PF03_DEBUG_SHAFTS         15
#define PF03_DEBUG_SHARPEN        16
#define PF03_DEBUG_HISTOGRAM      17
#define PF03_DEBUG_HISTOGRAM_PLOT 18
#define PF03_DEBUG_LUMINANCE      19
#define PF03_DEBUG_GRADE          20
#define PF03_DEBUG_LUT_ERROR      21
#define PF03_DEBUG_GAMUT          22
#define PF03_DEBUG_LUT_STRIP      23
#define PF03_DEBUG_HIZ            24
#define PF03_DEBUG_HIZ_CHECK      25
#define PF03_DEBUG_SSR_STEPS      26
#define PF03_DEBUG_SSR_FATE       27
#define PF03_DEBUG_DOF_COC        28
#define PF03_DEBUG_BLUR_TILES     29

// Число корзин обязано совпадать с declare_value 'histogram_bins'. Корзина 0 зарезервирована под «темнее
// нижней границы»: такие пиксели исключаются из статистики, иначе чёрный фон утягивает экспозицию вверх.
#define PF03_HISTOGRAM_BINS 256

// Отображение яркости в номер корзины и обратно. Логарифмическое: экспозиция живёт в стопах, и линейная
// сетка тратила бы почти все корзины на света, которых мало.
int pf03_luminance_to_bin(const float luminance, const float min_log2, const float max_log2) {
  if (luminance < 1.0e-5) {
    return 0;
  }
  const float value = (log2(luminance) - min_log2) / max(max_log2 - min_log2, 1.0e-4);
  if (value <= 0.0) {
    return 0;
  }
  return clamp(int(value * float(PF03_HISTOGRAM_BINS - 1)) + 1, 1, PF03_HISTOGRAM_BINS - 1);
}

float pf03_bin_to_log2(const int bin, const float min_log2, const float max_log2) {
  const float value = (float(bin) - 0.5) / float(PF03_HISTOGRAM_BINS - 1);
  return min_log2 + value * (max_log2 - min_log2);
}

#define PF03_TONEMAP_NONE     0
#define PF03_TONEMAP_REINHARD 1
#define PF03_TONEMAP_HABLE    2
#define PF03_TONEMAP_ACES     3

// Яркость по Rec.709: свёртка спектральной чувствительности глаза в одно число. Экспозиция и tone mapping
// работают именно по ней, а не по среднему каналов, иначе синий весит столько же, сколько зелёный.
float pf03_luminance(const vec3 color) {
  return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// Reinhard: простейшее сжатие диапазона. x/(1+x) отображает [0, inf) в [0, 1), но гасит контраст в средних
// тонах — историческая база, а не хороший вид.
vec3 pf03_tonemap_reinhard(const vec3 c) {
  return c / (1.0 + c);
}

// Кривая Uncharted 2 (Hable): плечо в светах, «нога» в тенях — то, что делает плёнка. Нормируется на белую
// точку, иначе картинка выходит тусклой.
vec3 pf03_hable_curve(const vec3 x) {
  const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
  return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 pf03_tonemap_hable(const vec3 c) {
  // Белая точка 11.2 и bias 2.0 — из оригинального доклада Hable. Без bias кривая систематически
  // недоэкспонирует: нормировка на белую точку опускает средние тона, и картинка выходит темнее остальных
  // операторов при той же экспозиции (проверено измерением, средняя яркость падала втрое).
  const float white = 11.2;
  const float exposure_bias = 2.0;
  return pf03_hable_curve(c * exposure_bias) / pf03_hable_curve(vec3(white));
}

// Аналитическая аппроксимация ACES (Narkowicz): та же идея, но с характерным «уводом в тепло» ярких
// участков, поэтому светлые области не становятся плоско-белыми.
vec3 pf03_tonemap_aces(const vec3 c) {
  const float a = 2.51, b = 0.03, cc = 2.43, d = 0.59, e = 0.14;
  return clamp((c * (a * c + b)) / (c * (cc * c + d) + e), 0.0, 1.0);
}

vec3 pf03_apply_tonemap(const vec3 color, const int op) {
  if (op == PF03_TONEMAP_REINHARD) return pf03_tonemap_reinhard(color);
  if (op == PF03_TONEMAP_HABLE) return pf03_tonemap_hable(color);
  if (op == PF03_TONEMAP_ACES) return pf03_tonemap_aces(color);
  return clamp(color, 0.0, 1.0); // без оператора остаётся только обрезка — ровно то, что и надо увидеть в A/B
}

// Оптическая глубина тумана с ЭКСПОНЕНЦИАЛЬНЫМ спадом плотности по высоте, взятая аналитически вдоль луча.
// Численное интегрирование тут не нужно: интеграл экспоненты по прямой берётся в закрытой форме, и это ровно
// то, что делает туман дешёвым — одна exp на пиксель вместо марша.
//   density(h) = density0 * exp(-(h - h0) / H)
// Вдоль луча из cam в направлении dir на расстояние dist подстановка h = cam.y + t*dir.y даёт:
//   tau = density0 * exp(-(cam.y - h0)/H) * H/dir.y * (1 - exp(-dist*dir.y/H))
// При dir.y -> 0 выражение переходит в density * dist, поэтому горизонтальный луч считается отдельной ветвью.
float pf03_fog_optical_depth(
  const vec3 camera, const vec3 direction, const float distance_along_ray,
  const float density, const float falloff, const float reference_height) {
  const float base = density * exp(-(camera.y - reference_height) / max(falloff, 1.0e-3));
  const float vertical = direction.y;
  if (abs(vertical) < 1.0e-4) {
    return base * distance_along_ray;
  }
  const float scale = max(falloff, 1.0e-3) / vertical;
  return base * scale * (1.0 - exp(-distance_along_ray * vertical / max(falloff, 1.0e-3)));
}

// Фазовая функция Henyey-Greenstein: доля света, рассеянного под углом. При g > 0 рассеяние вперёд, поэтому
// туман светится в сторону солнца — это и есть та самая узнаваемая черта аэроперспективы, без неё туман
// выглядит равномерной серой пеленой.
float pf03_phase_hg(const float cos_theta, const float g) {
  const float gg = g * g;
  const float denom = 1.0 + gg - 2.0 * g * cos_theta;
  return (1.0 - gg) / (4.0 * 3.14159265 * pow(max(denom, 1.0e-4), 1.5));
}

// Interleaved gradient noise (Jorge Jimenez): дешёвый детерминированный шум БЕЗ текстуры-ассета. Нужен,
// чтобы у каждого пикселя было своё вращение выборки — иначе весь кадр использует один и тот же набор
// направлений, и вместо шума получаются жирные полосы, которые никакой блюр уже не уберёт.
float pf03_gradient_noise(const vec2 pixel) {
  return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Точка последовательности Хаммерсли: низкодискрепансный набор вместо случайного. При 16 пробах разница
// видна прямо глазом — случайные точки сбиваются в кучки и дают пятна.
vec2 pf03_hammersley(const uint index, const uint count) {
  uint bits = index;
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return vec2(float(index) / float(count), float(bits) * 2.3283064365386963e-10);
}

// Точка в единичном диске из низкодискрепансной пары: SSAO смотрит вокруг пикселя на ЭКРАНЕ, поэтому нужен
// диск, а не полушарие. Радиус берётся как sqrt, иначе точки сгущаются к центру.
vec2 pf03_disc_sample(const vec2 xi, const float rotation) {
  const float angle = 6.28318531 * fract(xi.x + rotation);
  const float radius = sqrt(clamp(xi.y, 0.0, 1.0));
  return vec2(cos(angle), sin(angle)) * radius;
}

// Обратимое сжатие диапазона для СМЕШИВАНИЯ истории. Копить надо не в линейном HDR: один яркий пиксель
// (светящаяся панель, блик) в линейном пространстве перетягивает среднее на себя, и вокруг него остаётся
// светящийся шлейф. Приём из доклада Karis: смешиваем в сжатом пространстве и разжимаем обратно.
vec3 pf03_range_compress(const vec3 color) {
  return color / (1.0 + pf03_luminance(color));
}

vec3 pf03_range_expand(const vec3 color) {
  return color / max(1.0 - pf03_luminance(color), 1.0e-4);
}

// YCoCg: яркость отделена от двух цветовых осей. Для отбраковки истории это принципиально — коробка,
// построенная в RGB, растягивается по всем трём каналам сразу и отбраковывает историю там, где менялась
// только цветность; в YCoCg ограничение по яркости работает отдельно от цвета и получается заметно плотнее.
// Преобразование дешёвое и точно обратимое.
vec3 pf03_rgb_to_ycocg(const vec3 c) {
  return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 pf03_ycocg_to_rgb(const vec3 c) {
  return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Выборка истории фильтром Catmull-Rom через 5 билинейных обращений (оптимизация Karis). Зачем: под движением
// история переинтерполируется КАЖДЫЙ кадр, и обычный bilinear на каждом шаге чуть размывает картинку — за
// десяток кадров накапливается заметное мыло, и именно оно, а не отбраковка, даёт основную ошибку при
// движении камеры (измерено). Catmull-Rom имеет отрицательные лепестки, то есть слегка подчёркивает детали и
// компенсирует это размытие.
vec3 pf03_sample_catmull_rom(const sampler2D tex, const vec2 uv, const vec2 size) {
  const vec2 sample_position = uv * size;
  const vec2 texel_center = floor(sample_position - 0.5) + 0.5;
  const vec2 f = sample_position - texel_center;

  const vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  const vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  const vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  const vec2 w3 = f * f * (-0.5 + 0.5 * f);

  const vec2 w12 = w1 + w2;
  const vec2 offset12 = w2 / max(w12, vec2(1.0e-5));

  const vec2 tc0 = (texel_center - 1.0) / size;
  const vec2 tc3 = (texel_center + 2.0) / size;
  const vec2 tc12 = (texel_center + offset12) / size;

  vec3 result = vec3(0.0);
  result += texture(tex, vec2(tc12.x, tc0.y)).rgb * (w12.x * w0.y);
  result += texture(tex, vec2(tc0.x, tc12.y)).rgb * (w0.x * w12.y);
  result += texture(tex, vec2(tc12.x, tc12.y)).rgb * (w12.x * w12.y);
  result += texture(tex, vec2(tc3.x, tc12.y)).rgb * (w3.x * w12.y);
  result += texture(tex, vec2(tc12.x, tc3.y)).rgb * (w12.x * w3.y);

  return max(result, vec3(0.0));
}

// Треугольный дизер (TPDF): сумма двух независимых равномерных величин. Ровный шум амплитудой в один шаг
// квантования оставляет заметную зернистость и не убирает бандинг полностью, а треугольный распределяет
// ошибку так, что полосы исчезают при меньшей видимой шумности — стандартный приём из обработки звука,
// работающий здесь по той же причине.
float pf03_triangular_dither(const vec2 pixel, const float seed) {
  const float a = pf03_gradient_noise(pixel + vec2(seed, 0.0));
  const float b = pf03_gradient_noise(pixel + vec2(0.0, seed + 11.0));
  return (a + b - 1.0);
}

// Кодирование в sRGB. Нужно только если конечная запись НЕ в sRGB-формат: иначе преобразование сделает
// железо, и второй раз его применять нельзя.
vec3 pf03_encode_srgb(const vec3 c) {
  const vec3 lo = c * 12.92;
  const vec3 hi = 1.055 * pow(max(c, vec3(1e-5)), vec3(1.0 / 2.4)) - 0.055;
  return mix(lo, hi, greaterThan(c, vec3(0.0031308)));
}

// Октаэдральная упаковка нормали: единичный вектор → две компоненты без потери качества на глаз.
vec2 pf03_encode_normal(const vec3 n) {
  const vec3 v = n / (abs(n.x) + abs(n.y) + abs(n.z));
  vec2 e = v.xy;
  if (v.z < 0.0) {
    e = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
  }
  return e;
}

vec3 pf03_decode_normal(const vec2 e) {
  vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
  if (v.z < 0.0) {
    v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
  }
  return normalize(v);
}

// reverse-Z с бесконечной дальней плоскостью: глубина 1 у near, 0 на бесконечности
float pf03_linear_depth(const float reverse_depth, const float near_plane) {
  return near_plane / max(reverse_depth, 1e-6);
}

#endif
