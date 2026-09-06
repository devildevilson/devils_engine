#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "devils_engine/originator/tools.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/shared.h"

// СОБСТВЕННЫЙ ШУМ ДВИЖКА, который умеет и CPU, и устройство.
//
// ЭТО НЕ FastNoise2 И НЕ ОБЯЗАН С НИМ СОВПАДАТЬ. Совпадения тут не добиться и не нужно: у FastNoise2
// дерево узлов приезжает ЗАКОДИРОВАННОЙ СТРОКОЙ и вычисляется его собственной библиотекой, то есть
// перенести его на устройство значило бы перенести туда интерпретатор графа. Здесь вместо этого взяты
// ИЗВЕСТНЫЕ алгоритмы, написанные дважды — на C++ и на GLSL, — и объявлено вслух, что это своя
// реализация: поле, посчитанное этим шумом, не равно полю FastNoise2 ни при каком зерне.
//
// ЧТО ЭТО ПОКУПАЕТ: замер §5 п.0 назвал шум главным оставшимся `no_body` у всех трёх генераторов, то
// есть тем, что рвёт цепочки и держит очередь на CPU. Свой шум эту цепочку не рвёт.
//
// ОДИН ХЕШ НА ОБА ПУТИ — `utils::shared::prng2`, тот же, которым пользуется весь движок и который уже
// выписан в преамбуле шейдера. Два хеша означали бы два разных мира под одним зерном, и разница
// вылезла бы не сразу.
//
// ОБА ПУТИ СЧИТАЮТ ВО `float32`. На CPU это осознанно: канонический тип поля — double, но считать шум
// в double значило бы получить ДРУГИЕ числа, чем на устройстве, там где их можно получить одинаковыми.
// Побитового равенства всё равно никто не обещает (§4.2), но расхождение остаётся на уровне последнего
// бита, а не на уровне алгоритма — и это измерено тестом.
//
// РЕШЁТКА ТРЁХМЕРНА ВСЕГДА, а поле позиций с двумя компонентами означает СРЕЗ `z = 0`. Так у всех трёх
// алгоритмов одна реализация вместо двух, которые однажды разъехались бы. Цена названа: плоскому полю
// достаётся вдвое больше хешей, чем нужно двумерной решётке, и у ячеистого шума — втрое больше клеток.
// Если замер покажет, что это дорого, отдельный двумерный путь добавляется, а пока его нет.
//
// РАЗМЕР ФОРМЫ ЗДЕСЬ ТОЧНЫЙ, в отличие от FastNoise2: период решётки равен единице по построению,
// поэтому `frequency = 1 / feature` — не измерение, а тождество. Мерить период дерева, как это делает
// обвязка FastNoise2, здесь незачем.

namespace devils_engine {
namespace originator {

namespace {
namespace shared = utils::shared;

// Хеш узла решётки. Три раунда общего хеша движка: по одному на ось, с разными константами, чтобы
// перестановка координат давала разное значение.
uint32_t lattice_hash(const uint32_t seed, const int32_t x, const int32_t y, const int32_t z) noexcept {
  uint32_t h = shared::prng2(seed + 0x9E3779B9u, uint32_t(x) * 0x85EBCA6Bu + 0x27D4EB2Fu);
  h = shared::prng2(h + 0xC2B2AE35u, uint32_t(y) * 0x165667B1u + 0x9E3779B9u);
  return shared::prng2(h + 0x27D4EB2Fu, uint32_t(z) * 0x85EBCA6Bu + 0xC2B2AE35u);
}

float lattice_value(const uint32_t seed, const int32_t x, const int32_t y, const int32_t z) noexcept {
  return shared::prng_normalize(lattice_hash(seed, x, y, z));
}

// Сглаживание Эрмита у значения решётки и квинтика у градиентного: у первого важна дешевизна, у
// второго — непрерывность ВТОРОЙ производной, иначе на границах клеток видны складки.
float smooth_step(const float t) noexcept {
  return t * t * (3.0f - 2.0f * t);
}

float quintic(const float t) noexcept {
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// ФОРМУЛА ТА ЖЕ, ЧТО У GLSL `mix`, а не привычная `a + (b - a) * t`. Математически они равны, а
// численно нет — и на четырёх октавах разница копится до ~3e-06, то есть до величины, которая уже
// видна в сравнении путей. Совпадение здесь держится на том, что обе стороны считают ОДНИ И ТЕ ЖЕ
// операции в одном порядке; вольность в записи это ломает.
float lerp_f(const float a, const float b, const float t) noexcept {
  return a * (1.0f - t) + b * t;
}

// Значение решётки, интерполированное трилинейно. В [-1, 1].
float value_noise(const uint32_t seed, const float x, const float y, const float z) noexcept {
  const float fx = std::floor(x);
  const float fy = std::floor(y);
  const float fz = std::floor(z);
  const auto ix = int32_t(fx);
  const auto iy = int32_t(fy);
  const auto iz = int32_t(fz);

  const float tx = smooth_step(x - fx);
  const float ty = smooth_step(y - fy);
  const float tz = smooth_step(z - fz);

  const float v000 = lattice_value(seed, ix, iy, iz);
  const float v100 = lattice_value(seed, ix + 1, iy, iz);
  const float v010 = lattice_value(seed, ix, iy + 1, iz);
  const float v110 = lattice_value(seed, ix + 1, iy + 1, iz);
  const float v001 = lattice_value(seed, ix, iy, iz + 1);
  const float v101 = lattice_value(seed, ix + 1, iy, iz + 1);
  const float v011 = lattice_value(seed, ix, iy + 1, iz + 1);
  const float v111 = lattice_value(seed, ix + 1, iy + 1, iz + 1);

  const float x00 = lerp_f(v000, v100, tx);
  const float x10 = lerp_f(v010, v110, tx);
  const float x01 = lerp_f(v001, v101, tx);
  const float x11 = lerp_f(v011, v111, tx);
  const float y0 = lerp_f(x00, x10, ty);
  const float y1 = lerp_f(x01, x11, ty);
  return lerp_f(y0, y1, tz) * 2.0f - 1.0f;
}

// Градиент узла: двенадцать рёбер куба — классический набор Перлина. Выбор по битам хеша, поэтому у
// двух путей он один и тот же.
void lattice_gradient(const uint32_t hash, float& gx, float& gy, float& gz) noexcept {
  const uint32_t pick = hash & 15u;
  const float u = pick < 8u ? 1.0f : 0.0f;
  const float v = pick < 4u ? 1.0f : 0.0f;
  gx = u != 0.0f ? 1.0f : 0.0f;
  gy = v != 0.0f ? 1.0f : (pick == 12u || pick == 14u ? 1.0f : 0.0f);
  gz = u != 0.0f ? 0.0f : (v != 0.0f ? 0.0f : 1.0f);
  gx = (hash & 1u) != 0u ? -gx : gx;
  gy = (hash & 2u) != 0u ? -gy : gy;
  gz = (hash & 4u) != 0u ? -gz : gz;
}

float gradient_dot(const uint32_t seed, const int32_t ix, const int32_t iy, const int32_t iz,
                   const float dx, const float dy, const float dz) noexcept {
  float gx = 0.0f;
  float gy = 0.0f;
  float gz = 0.0f;
  lattice_gradient(lattice_hash(seed, ix, iy, iz), gx, gy, gz);
  return gx * dx + gy * dy + gz * dz;
}

// Градиентный (перлиновский) шум. В [-1, 1] с запасом: у трёхмерного максимум около 0.87, поэтому
// значение домножается на нормировку, а не зажимается.
float perlin_noise(const uint32_t seed, const float x, const float y, const float z) noexcept {
  const float fx = std::floor(x);
  const float fy = std::floor(y);
  const float fz = std::floor(z);
  const auto ix = int32_t(fx);
  const auto iy = int32_t(fy);
  const auto iz = int32_t(fz);

  const float dx = x - fx;
  const float dy = y - fy;
  const float dz = z - fz;
  const float tx = quintic(dx);
  const float ty = quintic(dy);
  const float tz = quintic(dz);

  const float n000 = gradient_dot(seed, ix, iy, iz, dx, dy, dz);
  const float n100 = gradient_dot(seed, ix + 1, iy, iz, dx - 1.0f, dy, dz);
  const float n010 = gradient_dot(seed, ix, iy + 1, iz, dx, dy - 1.0f, dz);
  const float n110 = gradient_dot(seed, ix + 1, iy + 1, iz, dx - 1.0f, dy - 1.0f, dz);
  const float n001 = gradient_dot(seed, ix, iy, iz + 1, dx, dy, dz - 1.0f);
  const float n101 = gradient_dot(seed, ix + 1, iy, iz + 1, dx - 1.0f, dy, dz - 1.0f);
  const float n011 = gradient_dot(seed, ix, iy + 1, iz + 1, dx, dy - 1.0f, dz - 1.0f);
  const float n111 = gradient_dot(seed, ix + 1, iy + 1, iz + 1, dx - 1.0f, dy - 1.0f, dz - 1.0f);

  const float x00 = lerp_f(n000, n100, tx);
  const float x10 = lerp_f(n010, n110, tx);
  const float x01 = lerp_f(n001, n101, tx);
  const float x11 = lerp_f(n011, n111, tx);
  const float y0 = lerp_f(x00, x10, ty);
  const float y1 = lerp_f(x01, x11, ty);
  return lerp_f(y0, y1, tz) * 1.1547005f;
}

// Ячеистый шум (Ворли): расстояние до ближайшей точки, по одной на клетку решётки. Возвращает
// то же [-1, 1], что и остальные, чтобы `amplitude`/`offset` вели себя одинаково у всех трёх.
float cellular_noise(const uint32_t seed, const float x, const float y, const float z) noexcept {
  const auto bx = int32_t(std::floor(x));
  const auto by = int32_t(std::floor(y));
  const auto bz = int32_t(std::floor(z));

  float nearest = 3.0f;
  for (int32_t dz = -1; dz <= 1; ++dz) {
    for (int32_t dy = -1; dy <= 1; ++dy) {
      for (int32_t dx = -1; dx <= 1; ++dx) {
        const int32_t cx = bx + dx;
        const int32_t cy = by + dy;
        const int32_t cz = bz + dz;
        const uint32_t h = lattice_hash(seed, cx, cy, cz);
        const float px = float(cx) + shared::prng_normalize(h);
        const float py = float(cy) + shared::prng_normalize(shared::prng2(h + 0x9E3779B9u, 0x85EBCA6Bu));
        const float pz = float(cz) + shared::prng_normalize(shared::prng2(h + 0xC2B2AE35u, 0x27D4EB2Fu));
        const float ex = px - x;
        const float ey = py - y;
        const float ez = pz - z;
        nearest = std::min(nearest, ex * ex + ey * ey + ez * ez);
      }
    }
  }

  return std::min(std::sqrt(nearest), 1.0f) * 2.0f - 1.0f;
}

// Октавы. Одна форма на все три алгоритма: складывать их по-разному не за что, а разные формы
// однажды разъехались бы.
struct field_settings {
  float frequency = 1.0f;
  float lacunarity = 2.0f;
  float gain = 0.5f;
  float amplitude = 1.0f;
  float offset = 0.0f;
  float ridged = 0.0f;
  uint32_t octaves = 1;
  uint32_t seed = 0;
};

template <typename noise_t>
float octaves_of(const field_settings& s, const noise_t& noise, float x, float y, float z) noexcept {
  float value = 0.0f;
  float amplitude = 1.0f;
  float normalization = 0.0f;
  float frequency = s.frequency;

  for (uint32_t octave = 0; octave < s.octaves; ++octave) {
    // Зерно ОКТАВЫ отличается от зерна прохода: иначе все октавы легли бы одна на другую в узлах
    // решётки, и это видно глазом как сетка.
    const uint32_t octave_seed = shared::prng2(s.seed + 0x9E3779B9u, octave + 0x85EBCA6Bu);
    float sample = noise(octave_seed, x * frequency, y * frequency, z * frequency);
    // Складчатая форма: `1 - |n|` даёт ИЗЛОМ там, где значение проходит ноль, то есть хребет вместо
    // круглого гребня. Тот же приём, что `absolute` у `remap`, но по октавам.
    sample = s.ridged != 0.0f ? 1.0f - std::abs(sample) * 2.0f : sample;
    value += amplitude * sample;
    normalization += amplitude;
    amplitude *= s.gain;
    frequency *= s.lacunarity;
  }

  return s.offset + s.amplitude * (value / normalization);
}

field_settings read_field_settings(const tool_call& call) {
  const auto& p = *call.params;
  field_settings s;

  // РАЗМЕР ФОРМЫ ИЛИ ЧАСТОТА, но не оба сразу: это два способа назвать одно число, и разошедшись они
  // дали бы поле не того масштаба. Период решётки равен единице, поэтому перевод точен.
  const bool has_feature = p.has("feature");
  const bool has_frequency = p.has("frequency");
  if (has_feature && has_frequency) {
    utils::error{}("originator step '{}': noise got both 'feature' and 'frequency' — one is the size of the "
                   "shape in world units, the other its inverse, and declaring both means one of them is wrong",
                   call.step_name);
  }
  if (has_feature) {
    const double feature = p.number("feature", 1.0);
    if (feature <= 0.0) {
      utils::error{}("originator step '{}': noise needs a positive feature size, got {}", call.step_name, feature);
    }
    s.frequency = float(1.0 / feature);
  } else {
    s.frequency = float(p.number("frequency", 1.0));
  }


  s.lacunarity = float(p.number("lacunarity", 2.0));
  s.gain = float(p.number("gain", 0.5));
  s.amplitude = float(p.number("amplitude", 1.0));
  s.offset = float(p.number("offset", 0.0));
  s.ridged = float(p.number("ridged", 0.0));
  s.octaves = uint32_t(std::clamp<int64_t>(p.integer("octaves", 1), 1, 16));
  // ЗЕРНО ПРИХОДИТ ОБЩЕЙ ШАПКОЙ, А НЕ ПАРАМЕТРОМ, и иначе быть не может: push-константа несёт
  // `float`, а float32 держит целое точно только до 2^24 — тридцатидвухбитное зерно в неё не влезает.
  // В шапке оно уже есть (`device_call_header::seed`), поэтому оба пути берут ОДНО число.
  //
  // Смещение — маленькое объявленное целое, и вот оно параметром едет: два поля одного шага, которым
  // нужен РАЗНЫЙ шум, различаются только им. Отрицательное отклоняется, потому что на устройстве
  // `uint(отрицательный float)` не определён вовсе.
  const auto seed_offset = p.integer("seed_offset", 0);
  if (seed_offset < 0 || seed_offset >= (1 << 24)) {
    utils::error{}("originator step '{}': noise takes a seed_offset in [0, 2^24), got {} — it rides in a float "
                   "push constant, and beyond that it stops being itself",
                   call.step_name, seed_offset);
  }
  s.seed = shared::prng2(fold_seed(call.seed) + 0x9E3779B9u, uint32_t(seed_offset) + 0x85EBCA6Bu);
  return s;
}

// Позиция элемента: две компоненты означают СРЕЗ `z = 0`, и это то же правило, что на устройстве.
void read_position(const const_field_accessor& positions, const size_t i, const uint32_t components,
                   float& x, float& y, float& z) noexcept {
  x = float(positions.get(i, 0));
  y = float(positions.get(i, 1));
  z = components >= 3 ? float(positions.get(i, 2)) : 0.0f;
}

template <typename noise_t>
void run_field(const tool_call& call, const size_t begin, const size_t end, const noise_t& noise) {
  const auto& source = call.input(0);
  const auto positions = source.read();
  auto target = call.output(0).write();

  const uint32_t components = source.type().components;
  if (components < 2) {
    utils::error{}("originator step '{}': noise reads positions and needs a 2- or 3-component field, '{}.{}' has {}",
                   call.step_name, source.buffer_name(), source.field_name(), components);
  }

  const auto settings = read_field_settings(call);
  for (size_t i = begin; i < end; ++i) {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    read_position(positions, i, components, x, y, z);
    target.set(i, double(octaves_of(settings, noise, x, y, z)));
  }
}

void tool_noise_value(const tool_call& call, const size_t begin, const size_t end) {
  run_field(call, begin, end, value_noise);
}

void tool_noise_perlin(const tool_call& call, const size_t begin, const size_t end) {
  run_field(call, begin, end, perlin_noise);
}

void tool_noise_cellular(const tool_call& call, const size_t begin, const size_t end) {
  run_field(call, begin, end, cellular_noise);
}

// ТОТ ЖЕ АЛГОРИТМ НА GLSL. Написан он дважды, и это дублирование названо вслух: одну реализацию на два
// пути дать нечем — на CPU это C++, на устройстве текст шейдера, — поэтому совпадение держится ТЕСТОМ,
// который гоняет оба пути и сравнивает. Хеш при этом общий, а не переписанный: `originator_prng2` из
// преамбулы это `utils::shared::prng2`.
constexpr std::string_view noise_prelude = R"(uint noise_lattice_hash(uint seed, ivec3 cell) {
  uint h = originator_prng2(seed + 0x9E3779B9u, uint(cell.x) * 0x85EBCA6Bu + 0x27D4EB2Fu);
  h = originator_prng2(h + 0xC2B2AE35u, uint(cell.y) * 0x165667B1u + 0x9E3779B9u);
  return originator_prng2(h + 0x27D4EB2Fu, uint(cell.z) * 0x85EBCA6Bu + 0xC2B2AE35u);
}

float noise_lattice_value(uint seed, ivec3 cell) {
  return originator_normalize(noise_lattice_hash(seed, cell));
}

float noise_smooth(float t) { return t * t * (3.0 - 2.0 * t); }
float noise_quintic(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

float noise_value(uint seed, vec3 p) {
  vec3 f = floor(p);
  ivec3 c = ivec3(f);
  vec3 t = vec3(noise_smooth(p.x - f.x), noise_smooth(p.y - f.y), noise_smooth(p.z - f.z));

  float v000 = noise_lattice_value(seed, c);
  float v100 = noise_lattice_value(seed, c + ivec3(1, 0, 0));
  float v010 = noise_lattice_value(seed, c + ivec3(0, 1, 0));
  float v110 = noise_lattice_value(seed, c + ivec3(1, 1, 0));
  float v001 = noise_lattice_value(seed, c + ivec3(0, 0, 1));
  float v101 = noise_lattice_value(seed, c + ivec3(1, 0, 1));
  float v011 = noise_lattice_value(seed, c + ivec3(0, 1, 1));
  float v111 = noise_lattice_value(seed, c + ivec3(1, 1, 1));

  float x00 = mix(v000, v100, t.x);
  float x10 = mix(v010, v110, t.x);
  float x01 = mix(v001, v101, t.x);
  float x11 = mix(v011, v111, t.x);
  return mix(mix(x00, x10, t.y), mix(x01, x11, t.y), t.z) * 2.0 - 1.0;
}

vec3 noise_gradient(uint hash) {
  uint pick = hash & 15u;
  float u = pick < 8u ? 1.0 : 0.0;
  float v = pick < 4u ? 1.0 : 0.0;
  vec3 g;
  g.x = u != 0.0 ? 1.0 : 0.0;
  g.y = v != 0.0 ? 1.0 : ((pick == 12u || pick == 14u) ? 1.0 : 0.0);
  g.z = u != 0.0 ? 0.0 : (v != 0.0 ? 0.0 : 1.0);
  g.x = (hash & 1u) != 0u ? -g.x : g.x;
  g.y = (hash & 2u) != 0u ? -g.y : g.y;
  g.z = (hash & 4u) != 0u ? -g.z : g.z;
  return g;
}

float noise_gradient_dot(uint seed, ivec3 cell, vec3 d) {
  return dot(noise_gradient(noise_lattice_hash(seed, cell)), d);
}

float noise_perlin(uint seed, vec3 p) {
  vec3 f = floor(p);
  ivec3 c = ivec3(f);
  vec3 d = p - f;
  vec3 t = vec3(noise_quintic(d.x), noise_quintic(d.y), noise_quintic(d.z));

  float n000 = noise_gradient_dot(seed, c, d);
  float n100 = noise_gradient_dot(seed, c + ivec3(1, 0, 0), d - vec3(1.0, 0.0, 0.0));
  float n010 = noise_gradient_dot(seed, c + ivec3(0, 1, 0), d - vec3(0.0, 1.0, 0.0));
  float n110 = noise_gradient_dot(seed, c + ivec3(1, 1, 0), d - vec3(1.0, 1.0, 0.0));
  float n001 = noise_gradient_dot(seed, c + ivec3(0, 0, 1), d - vec3(0.0, 0.0, 1.0));
  float n101 = noise_gradient_dot(seed, c + ivec3(1, 0, 1), d - vec3(1.0, 0.0, 1.0));
  float n011 = noise_gradient_dot(seed, c + ivec3(0, 1, 1), d - vec3(0.0, 1.0, 1.0));
  float n111 = noise_gradient_dot(seed, c + ivec3(1, 1, 1), d - vec3(1.0, 1.0, 1.0));

  float x00 = mix(n000, n100, t.x);
  float x10 = mix(n010, n110, t.x);
  float x01 = mix(n001, n101, t.x);
  float x11 = mix(n011, n111, t.x);
  return mix(mix(x00, x10, t.y), mix(x01, x11, t.y), t.z) * 1.1547005;
}

float noise_cellular(uint seed, vec3 p) {
  ivec3 base = ivec3(floor(p));
  float nearest = 3.0;
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        ivec3 c = base + ivec3(dx, dy, dz);
        uint h = noise_lattice_hash(seed, c);
        vec3 point = vec3(c) + vec3(originator_normalize(h),
                                    originator_normalize(originator_prng2(h + 0x9E3779B9u, 0x85EBCA6Bu)),
                                    originator_normalize(originator_prng2(h + 0xC2B2AE35u, 0x27D4EB2Fu)));
        vec3 e = point - p;
        nearest = min(nearest, dot(e, e));
      }
    }
  }
  return min(sqrt(nearest), 1.0) * 2.0 - 1.0;
}

float noise_octaves(uint kind, vec3 p) {
  float value = 0.0;
  float amplitude = 1.0;
  float normalization = 0.0;
  // Правило то же, что на хосте: объявленный размер формы побеждает частоту, потому что он и есть
  // способ говорить о шуме на одном языке с миром.
  float frequency = args.feature > 0.0 ? 1.0 / args.feature : args.frequency;
  uint octaves = uint(max(args.octaves, 1.0));

  for (uint octave = 0u; octave < octaves; ++octave) {
    uint field_seed = originator_prng2(args.seed + 0x9E3779B9u, uint(args.seed_offset) + 0x85EBCA6Bu);
    uint octave_seed = originator_prng2(field_seed + 0x9E3779B9u, octave + 0x85EBCA6Bu);
    vec3 at = p * frequency;
    float sample_value = kind == 0u ? noise_value(octave_seed, at)
                       : (kind == 1u ? noise_perlin(octave_seed, at) : noise_cellular(octave_seed, at));
    sample_value = args.ridged != 0.0 ? 1.0 - abs(sample_value) * 2.0 : sample_value;
    value += amplitude * sample_value;
    normalization += amplitude;
    amplitude *= args.gain;
    frequency *= args.lacunarity;
  }

  return args.offset + args.amplitude * (value / normalization);
}

vec3 noise_position(uint at) {
#if ORIGINATOR_IN_0_COMPONENTS >= 3
  return vec3(in_0_at(at, 0u), in_0_at(at, 1u), in_0_at(at, 2u));
#else
  return vec3(in_0_at(at, 0u), in_0_at(at, 1u), 0.0);
#endif
}
)";

// Параметры у трёх инструментов одни и те же, поэтому и объявлены один раз: разойдясь, они дали бы
// три разных словаря для одного и того же понятия.
std::vector<device_param> noise_params() {
  // `feature` ЕДЕТ НА УСТРОЙСТВО ТОЖЕ, и это не удобство: push-константа выкладывает объявленные
  // параметры КАК ЕСТЬ, поэтому перевод размера формы в частоту обязан случиться там же, где считается
  // шум. Оставь его только на хосте — и вызов с `feature` посчитал бы на устройстве частоту по
  // умолчанию, то есть поле другого масштаба, ничего при этом не сказав.
  //
  // Ноль означает «не объявлен»: размер формы нулевым не бывает, а второго способа сказать «этого
  // параметра нет» у push-константы нет.
  return {{"frequency", 1.0}, {"feature", 0.0},    {"lacunarity", 2.0}, {"gain", 0.5},
          {"amplitude", 1.0}, {"offset", 0.0},     {"ridged", 0.0},     {"octaves", 1.0},
          {"seed_offset", 0.0}};
}

std::string noise_body(const uint32_t kind) {
  return std::string("  out_0_set(index, noise_octaves(") + std::to_string(kind) + "u, noise_position(index)));\n";
}
} // namespace

void add_noise_field_tools(tool_registry& registry) {
  registry.add(tool_description{
    .name = "noise_value", .shape = aperture::pointwise, .input_count = 1, .output_count = 1,
    .body = tool_noise_value, .footprint = no_temporary_memory,
    .device_body = noise_body(0),
    .device_prelude = std::string(noise_prelude),
    .device_params = noise_params()});
  registry.add(tool_description{
    .name = "noise_perlin", .shape = aperture::pointwise, .input_count = 1, .output_count = 1,
    .body = tool_noise_perlin, .footprint = no_temporary_memory,
    .device_body = noise_body(1),
    .device_prelude = std::string(noise_prelude),
    .device_params = noise_params()});
  registry.add(tool_description{
    .name = "noise_cellular", .shape = aperture::pointwise, .input_count = 1, .output_count = 1,
    .body = tool_noise_cellular, .footprint = no_temporary_memory,
    .device_body = noise_body(2),
    .device_prelude = std::string(noise_prelude),
    .device_params = noise_params()});
}

} // namespace originator
} // namespace devils_engine
