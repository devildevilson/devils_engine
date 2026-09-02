#version 450

#include "camera_block.glsl"

layout(location = 0) in vec2 in_ndc;

layout(location = 0) out vec4 out_colour;

vec2 hash22(const vec3 p) {
  const vec2 q = vec2(dot(p, vec3(127.1, 311.7, 74.7)), dot(p, vec3(269.5, 183.3, 246.1)));
  return fract(sin(q) * 43758.5453123);
}

float hash13(const vec3 p) {
  return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453123);
}

// Небо разбито на клетки КУБИЧЕСКОЙ РАЗВЁРТКИ, и звезда берётся только из той клетки, в которую
// попал луч. Клетка одна, а не двадцать семь соседей: звезда мельче клетки на два порядка.
//
// Развёртка, а не кубическая решётка в объёме, и это не украшательство. Решётка в объёме НЕ
// биективна направлениям: клетка ищется по floor(direction * density), то есть по точке ровно на
// сфере радиуса density, а центр клетки на этой сфере не лежит — он отстоит от неё вдоль радиуса до
// 0.87 клетки. Нормируя центр, звезда уезжала в соседнюю клетку и не рисовалась НИКЕМ. Измерено:
// расчёт обещал полторы тысячи звёзд на экране, вышло около сорока — разница в тридцать раз, и вся
// она из этой несогласованности. Развёртка задаёт взаимно однозначное соответствие «направление —
// клетка», поэтому звезда, положенная в клетку, гарантированно этой же клеткой и находится.
//
// Плотность слоя — величина ИЗМЕРИМАЯ. Клеток всего 6 * (2*density)^2 на полный угол 4*pi, значит
// звёзд на стерадиан — 1.91 * share * density^2. Экран при поле зрения 48 градусов занимает около
// 1.3 стерадиана. На настоящем небе невооружённым глазом видно порядка 3 000 звёзд на всю сферу,
// то есть 240 на стерадиан; здесь взято втрое-вчетверо больше, потому что рисуется космос без
// атмосферы, но порядок именно этот.
vec3 star_layer(const vec3 direction, const float density, const float share, const float radius, const float weight) {
  const vec3 magnitudes = abs(direction);

  // Грань развёртки и координаты на ней. Знак грани входит в ключ клетки: без него звёзды северного
  // и южного полушарий совпали бы.
  vec2 face_uv;
  float face;
  if (magnitudes.x >= magnitudes.y && magnitudes.x >= magnitudes.z) {
    face_uv = direction.yz / magnitudes.x;
    face = direction.x > 0.0 ? 0.0 : 1.0;
  } else if (magnitudes.y >= magnitudes.z) {
    face_uv = direction.xz / magnitudes.y;
    face = direction.y > 0.0 ? 2.0 : 3.0;
  } else {
    face_uv = direction.xy / magnitudes.z;
    face = direction.z > 0.0 ? 4.0 : 5.0;
  }

  const vec2 cell = floor(face_uv * density);
  const vec3 key = vec3(cell, face);

  const float existence = hash13(key + 19.19);
  if (existence > share) {
    return vec3(0.0);
  }

  // Звезда сидит в случайной точке своей клетки, но не у самого края: у края её частично срезало бы
  // соседней клеткой, которая о ней не знает.
  const vec2 jitter = hash22(key);
  const vec2 star_uv = (cell + 0.15 + 0.70 * jitter) / density;

  vec3 star;
  if (face < 0.5) {
    star = vec3(1.0, star_uv.x, star_uv.y);
  } else if (face < 1.5) {
    star = vec3(-1.0, star_uv.x, star_uv.y);
  } else if (face < 2.5) {
    star = vec3(star_uv.x, 1.0, star_uv.y);
  } else if (face < 3.5) {
    star = vec3(star_uv.x, -1.0, star_uv.y);
  } else if (face < 4.5) {
    star = vec3(star_uv.x, star_uv.y, 1.0);
  } else {
    star = vec3(star_uv.x, star_uv.y, -1.0);
  }
  star = normalize(star);

  // Хорда вместо угла: на таких малых углах они совпадают, а арккосинус здесь стоил бы дороже всего
  // остального вместе взятого.
  const float distance_to_star = length(direction - star);
  // Яркость по степенному закону: ярких звёзд должно быть заметно меньше, чем тусклых, иначе небо
  // выглядит равномерной сыпью. Степень ВТОРАЯ, а не третья, и у яркости есть пол. С кубом медианная
  // звезда светила на 20 из 255 — звёзды были на месте (315 штук на половине экрана при расчётных
  // 950 на весь), но не читались. Считать надо было не число, а яркость: правильное число не той
  // величины остаётся не той величиной.
  const float brightness = hash13(key + 41.31);
  const float magnitude = brightness * brightness;
  const float size = radius * (0.60 + 0.40 * magnitude);
  const float core = smoothstep(size, 0.0, distance_to_star);
  // Ореол втрое шире ядра и на порядок слабее: без него звезда размером в пиксель мерцает при
  // повороте камеры, попадая то в центр пикселя, то между ними.
  const float halo = smoothstep(size * 3.0, 0.0, distance_to_star) * 0.06;

  // Цвет звезды: голубоватые горячие и желтоватые холодные. Разброс небольшой — на реальном небе
  // цвет звезды заметен только у самых ярких.
  const vec3 warm = vec3(1.00, 0.93, 0.80);
  const vec3 cool = vec3(0.82, 0.89, 1.00);
  const vec3 tint = mix(cool, warm, hash13(key + 3.7));

  // Пол в 0.40: звезда размером в полтора пикселя почти никогда не попадает центром в центр
  // пикселя, поэтому на экране она вдвое слабее собственной яркости.
  return tint * (core + halo) * (0.40 + 0.60 * magnitude) * weight;
}

void main() {
  // Направление луча: точка ближней плоскости минус камера. Ближняя, а не дальняя, потому что
  // проекция здесь обратная и бесконечная — у дальней плоскости w обращается в ноль.
  const vec4 near_point = camera.inverse_view_projection * vec4(in_ndc, 1.0, 1.0);
  const vec3 direction = normalize(near_point.xyz / near_point.w - camera.camera_position.xyz);

  // Фон не чёрный, а очень тёмный сине-фиолетовый с лёгким градиентом: абсолютно чёрный фон
  // читается как дырка в кадре, а не как пространство.
  const float gradient = 0.5 + 0.5 * direction.y;
  vec3 colour = mix(vec3(0.010, 0.012, 0.024), vec3(0.017, 0.019, 0.038), gradient);

  // Три слоя. Частые и мелкие дают «пыль», редкие и крупные — те немногие звёзды, по которым глаз и
  // опознаёт небо. В сумме около 970 звёзд на стерадиан, то есть примерно 1 250 на экран.
  colour += star_layer(direction, 26.0, 0.55, 0.0020, 0.75);
  colour += star_layer(direction, 14.0, 0.55, 0.0028, 1.00);
  colour += star_layer(direction, 8.0, 0.45, 0.0040, 1.60);

  out_colour = vec4(colour, 1.0);
}
