#include <algorithm>
#include <cmath>
#include <limits>

#include "devils_engine/originator/tools.h"
#include "devils_engine/utils/core.h"

// Набор инструментов, поставляемых движком. Здесь важна не полнота (шумы и вороной автора приедут
// отдельно), а то, что каждый инструмент ОБЪЯВЛЯЕТ апертуру и живёт по её правилам.
//
// У каждого инструмента два пути: быстрый через непрерывный span, когда поле лежит подряд (soa), и
// общий через аксессор, когда поле идёт с шагом (aos). Разница между ними и есть то, что меряет
// первый вертикальный срез.

namespace devils_engine {
namespace originator {

namespace {

uint64_t hash_u64(uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

// Значение решётки в [0,1). Зависит ТОЛЬКО от координат и зерна, поэтому выборка не зависит от
// порядка обхода и параллельное исполнение совпадает с последовательным.
float lattice(const int32_t x, const int32_t y, const uint64_t seed) noexcept {
  const uint64_t key = (uint64_t(uint32_t(x)) | (uint64_t(uint32_t(y)) << 32)) ^ seed;
  return float(hash_u64(key) >> 40) * (1.0f / 16777216.0f);
}

float smooth(const float t) noexcept {
  return t * t * (3.0f - 2.0f * t);
}

float value_noise_2d(const float x, const float y, const uint64_t seed) noexcept {
  const float fx = std::floor(x);
  const float fy = std::floor(y);
  const auto ix = int32_t(fx);
  const auto iy = int32_t(fy);
  const float tx = smooth(x - fx);
  const float ty = smooth(y - fy);

  const float v00 = lattice(ix, iy, seed);
  const float v10 = lattice(ix + 1, iy, seed);
  const float v01 = lattice(ix, iy + 1, seed);
  const float v11 = lattice(ix + 1, iy + 1, seed);

  const float a = v00 + (v10 - v00) * tx;
  const float b = v01 + (v11 - v01) * tx;
  return a + (b - a) * ty;
}

struct noise_settings {
  float frequency = 0.01f;
  float lacunarity = 2.0f;
  float gain = 0.5f;
  float amplitude = 1.0f;
  float offset = 0.0f;
  uint32_t octaves = 4;
  uint32_t width = 1;
  uint64_t seed = 0;
};

noise_settings read_noise_settings(const tool_call& call) {
  const auto& p = *call.params;
  noise_settings s;
  s.frequency = float(p.number("frequency", 0.01));
  s.lacunarity = float(p.number("lacunarity", 2.0));
  s.gain = float(p.number("gain", 0.5));
  s.amplitude = float(p.number("amplitude", 1.0));
  s.offset = float(p.number("offset", 0.0));
  s.octaves = uint32_t(std::clamp<int64_t>(p.integer("octaves", 4), 1, 16));
  s.width = uint32_t(std::max<int64_t>(p.integer("width", 1), 1));
  s.seed = hash_u64(call.seed ^ uint64_t(p.integer("salt", 0)));
  return s;
}

float fbm(const noise_settings& s, const float x, const float y) noexcept {
  float value = 0.0f;
  float amplitude = 1.0f;
  float normalization = 0.0f;
  float frequency = s.frequency;

  for (uint32_t octave = 0; octave < s.octaves; ++octave) {
    value += amplitude * value_noise_2d(x * frequency, y * frequency, s.seed + octave * 0x9e3779b9ull);
    normalization += amplitude;
    amplitude *= s.gain;
    frequency *= s.lacunarity;
  }

  return s.offset + s.amplitude * (value / normalization);
}

// Поле МИРОВЫХ ПОЗИЦИЙ регулярной сетки. Без него не работает единственный точный путь генерации
// объёма: `noise_at` берёт позиции полем, а заполнить это поле было нечем.
//
// Раскладка индекса — x внутренний, затем y, затем z. Это не произвольный выбор: та же раскладка у
// `noise_grid` и у самого FastNoise2 (`out[(z*yCount + y)*xCount + x]`), поэтому поле, посчитанное
// здесь, скармливается выборке по позициям без перестановки.
//
// Инструмент НЕ знает про чанки: начало координат приходит параметром. Так тело шага остаётся
// единственным местом, которое переводит ключ чанка в мировое смещение, и инструмент не приобретает
// знания о понятиях пайплайна.
void tool_position_grid(const tool_call& call, const size_t begin, const size_t end) {
  const auto& out = call.output(0);
  auto target = out.write();

  const uint32_t components = target.type().components;
  if (components < 2) {
    utils::error{}("originator step '{}': position_grid needs a 2- or 3-component field, '{}.{}' has {}",
                   call.step_name, out.buffer_name(), out.field_name(), components);
  }

  const auto size_x = size_t(std::max<int64_t>(call.params->integer("size_x", 1), 1));
  const auto size_y = size_t(std::max<int64_t>(call.params->integer("size_y", int64_t(size_x)), 1));

  const double cell = call.params->number("cell_size", 1.0);
  const double origin_x = call.params->number("origin_x", 0.0);
  const double origin_y = call.params->number("origin_y", 0.0);
  const double origin_z = call.params->number("origin_z", 0.0);

  const size_t plane = size_x * size_y;
  const bool volume = components >= 3;

  for (size_t i = begin; i < end; ++i) {
    const size_t x = i % size_x;
    const size_t y = (i / size_x) % size_y;

    target.set(i, origin_x + double(x) * cell, 0);
    target.set(i, origin_y + double(y) * cell, 1);
    if (volume) {
      const size_t z = plane == 0 ? 0 : i / plane;
      target.set(i, origin_z + double(z) * cell, 2);
    }
  }
}

void tool_fill(const tool_call& call, const size_t begin, const size_t end) {
  const auto& out = call.output(0);
  const double value = call.params->number("value", 0.0);
  auto accessor = out.write();
  const uint32_t components = accessor.type().components;

  for (size_t i = begin; i < end; ++i) {
    for (uint32_t c = 0; c < components; ++c) {
      accessor.set(i, value, c);
    }
  }
}

void tool_value_noise(const tool_call& call, const size_t begin, const size_t end) {
  const auto settings = read_noise_settings(call);
  const auto& out = call.output(0);
  auto accessor = out.write();

  const auto span = accessor.as_span<float>();
  if (!span.empty() && accessor.type().components == 1) {
    // Быстрый путь: поле лежит подряд, компилятор видит обычный цикл по float.
    for (size_t i = begin; i < end; ++i) {
      const float x = float(i % settings.width);
      const float y = float(i / settings.width);
      span[i] = fbm(settings, x, y);
    }
    return;
  }

  for (size_t i = begin; i < end; ++i) {
    const float x = float(i % settings.width);
    const float y = float(i / settings.width);
    accessor.set(i, double(fbm(settings, x, y)));
  }
}

void tool_remap(const tool_call& call, const size_t begin, const size_t end) {
  const auto& in = call.input(0);
  const auto& out = call.output(0);
  const double scale = call.params->number("scale", 1.0);
  const double offset = call.params->number("offset", 0.0);
  const double lower = call.params->number("min", -std::numeric_limits<double>::infinity());
  const double upper = call.params->number("max", std::numeric_limits<double>::infinity());

  const auto source = in.read();
  auto target = out.write();

  for (size_t i = begin; i < end; ++i) {
    const double value = source.get(i) * scale + offset;
    target.set(i, std::clamp(value, lower, upper));
  }
}

// Семантическая классификация: тот же расчёт позже повторяется на devils_script и на lua, чтобы
// сравнение трёх уровней шло по ОДНОЙ задаче, а не по трём разным.
void tool_classify(const tool_call& call, const size_t begin, const size_t end) {
  const auto height = call.input(0).read();
  const auto moisture = call.input(1).read();
  auto biome = call.output(0).write();

  const double sea_level = call.params->number("sea_level", 0.5);
  const double dry = call.params->number("dry", 0.35);
  const double wet = call.params->number("wet", 0.65);

  for (size_t i = begin; i < end; ++i) {
    const double h = height.get(i);
    if (h < sea_level) {
      biome.set(i, 0.0);
      continue;
    }

    const double m = moisture.get(i);
    if (m < dry) {
      biome.set(i, 1.0);
    } else if (m < wet) {
      biome.set(i, 2.0);
    } else {
      biome.set(i, 3.0);
    }
  }
}

// Апертура gather: читает окно вокруг элемента, пишет только свой. Источник и приёмник обязаны быть
// разными полями — это проверяется в check_dispatch, здесь можно на это опираться.
void tool_box_blur(const tool_call& call, const size_t begin, const size_t end) {
  const auto source = call.input(0).read();
  auto target = call.output(0).write();

  const auto width = size_t(std::max<int64_t>(call.params->integer("width", 1), 1));
  const auto radius = size_t(std::clamp<int64_t>(call.params->integer("radius", 1), 0, 64));
  const size_t count = source.count();
  const size_t height = width == 0 ? 0 : count / width;

  for (size_t i = begin; i < end; ++i) {
    const size_t x = i % width;
    const size_t y = i / width;

    double sum = 0.0;
    size_t taken = 0;
    const size_t x0 = x >= radius ? x - radius : 0;
    const size_t x1 = std::min(x + radius, width - 1);
    const size_t y0 = y >= radius ? y - radius : 0;
    const size_t y1 = height == 0 ? y : std::min(y + radius, height - 1);

    for (size_t sy = y0; sy <= y1; ++sy) {
      for (size_t sx = x0; sx <= x1; ++sx) {
        const size_t index = sy * width + sx;
        if (index >= count) {
          continue;
        }
        sum += source.get(index);
        ++taken;
      }
    }

    target.set(i, taken == 0 ? 0.0 : sum / double(taken));
  }
}

double reduce_min_partial(const tool_call& call, const size_t begin, const size_t end) {
  const auto source = call.input(0).read();
  double value = std::numeric_limits<double>::infinity();
  for (size_t i = begin; i < end; ++i) {
    value = std::min(value, source.get(i));
  }
  return value;
}

double reduce_max_partial(const tool_call& call, const size_t begin, const size_t end) {
  const auto source = call.input(0).read();
  double value = -std::numeric_limits<double>::infinity();
  for (size_t i = begin; i < end; ++i) {
    value = std::max(value, source.get(i));
  }
  return value;
}

double reduce_sum_partial(const tool_call& call, const size_t begin, const size_t end) {
  const auto source = call.input(0).read();
  double value = 0.0;
  for (size_t i = begin; i < end; ++i) {
    value += source.get(i);
  }
  return value;
}

double reduce_count_above_partial(const tool_call& call, const size_t begin, const size_t end) {
  const auto source = call.input(0).read();
  const double threshold = call.params->number("threshold", 0.0);
  double value = 0.0;
  for (size_t i = begin; i < end; ++i) {
    value += source.get(i) > threshold ? 1.0 : 0.0;
  }
  return value;
}

} // namespace

void tool_registry::add_standard_tools() {
  // Именованная инициализация намеренно: набор полей описания инструмента будет расти, и
  // позиционная запись ломалась бы на каждом новом поле.
  add(tool_description{.name = "fill", .shape = aperture::pointwise, .input_count = 0, .output_count = 1, .body = tool_fill});
  add(tool_description{.name = "position_grid", .shape = aperture::pointwise, .input_count = 0, .output_count = 1,
                       .body = tool_position_grid});
  add(tool_description{.name = "value_noise", .shape = aperture::pointwise, .input_count = 0, .output_count = 1, .body = tool_value_noise});
  add(tool_description{.name = "remap", .shape = aperture::pointwise, .input_count = 1, .output_count = 1, .body = tool_remap});
  add(tool_description{.name = "classify", .shape = aperture::pointwise, .input_count = 2, .output_count = 1, .body = tool_classify});
  add(tool_description{.name = "box_blur", .shape = aperture::gather, .input_count = 1, .output_count = 1, .body = tool_box_blur});

  add(tool_description{.name = "reduce_min", .shape = aperture::reduce, .input_count = 1, .output_count = 0,
                       .partial = reduce_min_partial,
                       .combine = [](const double a, const double b) { return std::min(a, b); },
                       .initial = std::numeric_limits<double>::infinity()});
  add(tool_description{.name = "reduce_max", .shape = aperture::reduce, .input_count = 1, .output_count = 0,
                       .partial = reduce_max_partial,
                       .combine = [](const double a, const double b) { return std::max(a, b); },
                       .initial = -std::numeric_limits<double>::infinity()});
  add(tool_description{.name = "reduce_sum", .shape = aperture::reduce, .input_count = 1, .output_count = 0,
                       .partial = reduce_sum_partial,
                       .combine = [](const double a, const double b) { return a + b; }, .initial = 0.0});
  add(tool_description{.name = "reduce_count_above", .shape = aperture::reduce, .input_count = 1, .output_count = 0,
                       .partial = reduce_count_above_partial,
                       .combine = [](const double a, const double b) { return a + b; }, .initial = 0.0});

  add_scatter_tools();
}

} // namespace originator
} // namespace devils_engine
