#ifndef PF03_FRAME_GLSL
#define PF03_FRAME_GLSL

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
  vec4 fog_color;

// viewport_near:    xy = размер кадра в пикселях, z = near, w = номер кадра с последнего сброса истории
// controls:         x = debug-режим, y = усиление motion при показе, z = усиление ошибки, w = кодировать sRGB
// light_direction:  xyz = направление НА свет, w = сила ambient
// tonemap:          x = оператор, y = ручная экспозиция (<= 0 => авто), z = скорость адаптации, w = dt в секундах
// exposure_limits:  x/y = границы log2 средней яркости, z = ключ (средний серый), w = яркость солнца
// fog_params:       x = плотность у опорной высоты, y = масштаб спада по высоте, z = опорная высота, w = анизотропия
// fog_color:        xyz = цвет рассеяния (уже с яркостью), w = вклад солнечного диска в рассеяние

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
