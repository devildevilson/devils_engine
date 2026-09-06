#include <algorithm>
#include <cmath>
#include <limits>

#include "devils_engine/originator/tools.h"
#include "devils_engine/utils/core.h"

// Набор инструментов ядра. Важна не полнота, а то, что каждый ОБЪЯВЛЯЕТ апертуру и живёт по её
// правилам; у каждого два пути — быстрый через непрерывный span (soa) и общий через аксессор (aos).
//
// ПОЧЕМУ АРИФМЕТИКА ЖИВЁТ ИНСТРУМЕНТАМИ, А НЕ ПРАВИЛОМ НА `ds`. Программа `devils_script` возвращает
// ОДНО значение в ОДНО поле, а вызов функции в этой версии ds не является операндом выражения; к
// тому же у контекста восемь слотов аргументов. Значит сложить два слагаемых, ограничить величину
// маской, поделить и посчитать спад нечем — а из этих операций состоит почти любая мировая формула.
// Поэтому арифметику считает движок, а конфиг остаётся местом, где слагаемые СКЛАДЫВАЮТСЯ, то есть
// где живёт смысл. Это же объясняет `blend`, `modulate`, `ratio`, `decay`, `maximum`/`minimum`.

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

  const auto shape = resolve_extent(call, out, "size_x", "size_y", "size_z");
  const size_t size_x = shape.x;
  const size_t size_y = shape.y;

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
  auto settings = read_noise_settings(call);
  const auto& out = call.output(0);
  // Форма растра приходит от БУФЕРА, а прежнее написание (`width` в параметрах) остаётся, пока
  // документы переводятся. Оба сразу — громкая ошибка, см. resolve_extent.
  settings.width = uint32_t(resolve_extent(call, out, "width").x);
  auto accessor = out.write();

  // as_span сам отдаёт непустой span только для однокомпонентного поля точно совпадающего рода,
  // поэтому отдельная проверка компонент здесь не нужна.
  const auto span = accessor.as_span<float>();
  if (!span.empty()) {
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

// index: порядковый номер элемента как значение поля.
//
// Выглядит бессмысленно, пока не понадобится заливка, которая несёт не признак «здесь граница», а
// НОМЕР клетки границы. С признаком заливка отвечает только «далеко ли до границы», и всё, что у той
// границы происходит — скорость сближения, сторона субдукции, — остаётся недоступным: величина лежит
// в клетке, до которой дошли, а не в той, от которой шли. Номер превращает косвенность в данные, и
// дальше её читает lookup.
void tool_index(const tool_call& call, const size_t begin, const size_t end) {
  auto target = call.output(0).write();
  const double scale = call.params->number("scale", 1.0);
  const double offset = call.params->number("offset", 0.0);

  for (size_t i = begin; i < end; ++i) {
    target.set(i, double(i) * scale + offset);
  }
}

// Афинное преобразование с ограничением: clamp(scale * x + offset).
//
// Параметр `absolute` берёт модуль ДО умножения, и он здесь не ради экономии вызова: без него не
// выражается ridged-шум — |n|, сложенный обратно в единицу, — единственный дешёвый способ получить
// хребет со СКЛАДКОЙ. У обычного шума гребень круглый, у свёрнутого излом там, где значение проходит
// ноль, а гряда без излома читается как вал.
void tool_remap(const tool_call& call, const size_t begin, const size_t end) {
  const auto& in = call.input(0);
  const auto& out = call.output(0);
  const double scale = call.params->number("scale", 1.0);
  const double offset = call.params->number("offset", 0.0);
  const double lower = call.params->number("min", -std::numeric_limits<double>::infinity());
  const double upper = call.params->number("max", std::numeric_limits<double>::infinity());
  const bool absolute = call.params->integer("absolute", 0) != 0;
  // КОМПОНЕНТА ИСТОЧНИКА. Существует потому, что правило на devils_script компоненту вектора достать
  // не может (у GN02 из-за этого широта приезжала отдельным полем), а объёму она нужна на каждом
  // шаге: вертикальный градиент плотности — это высота, то есть вторая компонента поля позиций.
  // Инструмент, который умеет читать только нулевую компоненту, оставлял бы единственным выходом
  // копию поля позиций тремя скалярами.
  const auto component = uint32_t(std::max<int64_t>(call.params->integer("component", 0), 0));

  const auto source = in.read();
  auto target = out.write();

  if (component >= source.type().components) {
    utils::error{}("originator step '{}': remap was asked for component {} of '{}.{}', which has {}",
                   call.step_name, component, in.buffer_name(), in.field_name(), source.type().components);
  }

  for (size_t i = begin; i < end; ++i) {
    const double raw = source.get(i, component);
    const double sample = absolute ? std::abs(raw) : raw;
    const double value = sample * scale + offset;
    target.set(i, std::clamp(value, lower, upper));
  }
}

// Произведение двух полей: scale * a * b + offset.
//
// Нужно ровно там, где величину надо ОГРАНИЧИТЬ маской: сумма осадков по суше, население по
// пригодным клеткам, что угодно «только там, где». Через свёртку по полю с маской это не выражается —
// свёртка читает одно поле, — а заводить свёртку с маской значило бы удваивать каждую из них.
void tool_modulate(const tool_call& call, const size_t begin, const size_t end) {
  const auto first = call.input(0).read();
  const auto second = call.input(1).read();
  auto target = call.output(0).write();

  const double scale = call.params->number("scale", 1.0);
  const double offset = call.params->number("offset", 0.0);

  for (size_t i = begin; i < end; ++i) {
    target.set(i, scale * first.get(i) * second.get(i) + offset);
  }
}

// Экспоненциальный спад по расстоянию: offset + amplitude * exp(-x / width). Этой формой описывается
// почти всё, что «спадает от границы»: поднятие от стыка, глубина жёлоба, высота хребта, влияние
// берега.
void tool_decay(const tool_call& call, const size_t begin, const size_t end) {
  const auto source = call.input(0).read();
  auto target = call.output(0).write();

  const double width = call.params->number("width", 1.0);
  const double amplitude = call.params->number("amplitude", 1.0);
  const double offset = call.params->number("offset", 0.0);
  if (width <= 0.0) {
    utils::error{}("originator step '{}': decay needs a positive width, got {}", call.step_name, width);
  }

  for (size_t i = begin; i < end; ++i) {
    // Отрицательное расстояние — это метка «недостигнуто», а не расстояние. Растить от неё
    // экспоненту нельзя: получилось бы, что дальше всех влияет сильнее всех.
    const double distance = std::max(0.0, source.get(i));
    target.set(i, offset + amplitude * std::exp(-distance / width));
  }
}

// Взвешенная сумма двух полей: `a * x + b * y + offset` с ограничением.
void tool_blend(const tool_call& call, const size_t begin, const size_t end) {
  const auto first = call.input(0).read();
  const auto second = call.input(1).read();
  auto target = call.output(0).write();

  const double weight_first = call.params->number("first", 1.0);
  const double weight_second = call.params->number("second", 1.0);
  const double offset = call.params->number("offset", 0.0);
  const double lower = call.params->number("min", -std::numeric_limits<double>::infinity());
  const double upper = call.params->number("max", std::numeric_limits<double>::infinity());

  for (size_t i = begin; i < end; ++i) {
    const double value = weight_first * first.get(i) + weight_second * second.get(i) + offset;
    target.set(i, std::clamp(value, lower, upper));
  }
}

// Отношение двух полей: scale * a / b + offset, со знаменателем не меньше объявленного.
//
// Заведено НОРМИРОВКОЙ: смешивание нескольких правил по весам это сумма произведений, делённая на
// сумму весов, и без деления она не выражается вовсе. Первый потребитель — биомы, но нормировка
// нужна любому взвешенному среднему, поэтому инструмент общий, а не «биомный».
//
// Минимальный знаменатель — параметр, а не константа, и он ОБЯЗАТЕЛЕН по смыслу: у взвешенного
// среднего нулевая сумма весов означает «ни одно правило здесь не действует», и это ситуация автора
// конфига, а не движка. Молча вернуть ноль было бы худшим ответом: ноль плотности это поверхность,
// то есть в дырке покрытия появилась бы стена.
void tool_ratio(const tool_call& call, const size_t begin, const size_t end) {
  const auto first = call.input(0).read();
  const auto second = call.input(1).read();
  auto target = call.output(0).write();

  const double scale = call.params->number("scale", 1.0);
  const double offset = call.params->number("offset", 0.0);
  // ОБЯЗАТЕЛЕН, а не «по умолчанию мал». Значение по умолчанию здесь означало бы, что движок сам
  // решает, какая сумма весов считается нулевой, — а это ровно то решение, которое принадлежит автору
  // конфига: у поля плотности ноль это ПОВЕРХНОСТЬ, то есть в дырке покрытия появилась бы стена.
  if (!call.params->has("minimum_divisor")) {
    utils::error{}("originator step '{}': ratio needs minimum_divisor — a zero divisor means 'no rule acts "
                   "here', and what to answer there is the config author's decision",
                   call.step_name);
  }
  const double floor_value = call.params->number("minimum_divisor", 1.0e-9);
  if (floor_value <= 0.0) {
    utils::error{}("originator step '{}': ratio needs a positive minimum_divisor, got {}", call.step_name,
                   floor_value);
  }

  for (size_t i = begin; i < end; ++i) {
    const double divisor = second.get(i);
    const double safe = std::abs(divisor) < floor_value ? (divisor < 0.0 ? -floor_value : floor_value) : divisor;
    target.set(i, scale * first.get(i) / safe + offset);
  }
}

// Побольше и поменьше из двух полей.
//
// Нужны там, где два слоя НАКЛАДЫВАЮТСЯ, а не складываются: остров посреди океана поднимается над
// дном, а не прибавляется к спадающему материковому конусу — со сложением он опускался бы тем ниже,
// чем дальше от материка, что бессмыслица. Тем же выражается и объединение масок: сумма с обрезкой
// по единице ведёт себя как максимум только пока слагаемые не перекрываются.
void tool_maximum(const tool_call& call, const size_t begin, const size_t end) {
  const auto first = call.input(0).read();
  const auto second = call.input(1).read();
  auto target = call.output(0).write();

  for (size_t i = begin; i < end; ++i) {
    target.set(i, std::max(first.get(i), second.get(i)));
  }
}

void tool_minimum(const tool_call& call, const size_t begin, const size_t end) {
  const auto first = call.input(0).read();
  const auto second = call.input(1).read();
  auto target = call.output(0).write();

  for (size_t i = begin; i < end; ++i) {
    target.set(i, std::min(first.get(i), second.get(i)));
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

  const auto shape = resolve_extent(call, call.output(0), "width");
  const size_t width = shape.x;
  const auto radius = size_t(std::clamp<int64_t>(call.params->integer("radius", 1), 0, 64));
  const size_t count = source.count();
  const size_t height = shape.y;

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

// ЧТЕНИЕ МЕЖДУ ЭЛЕМЕНТАМИ — то единственное, ради чего поле на устройстве становится КАРТИНКОЙ
// (§6.3). Четыре отсчёта на `radius` текселей от центра, каждый из них — билинейная выборка, то есть
// уже среднее четырёх соседей. У буфера такого чтения нет: там пришлось бы читать шестнадцать
// элементов руками.
//
// И там же объявленная граница: точность фильтра в Vulkan implementation-defined, поэтому проход
// принадлежит классу ПРЕДСТАВЛЕНИЯ и в чанковую генерацию не пускается (`NETWORKING.md`).
double sample_bilinear(const const_field_accessor& field, const size_t width, const size_t height,
                       const double u, const double v) {
  // Повторяет то, что делает сэмплер: координата в текселях смещена на полтексела, край ЗАЖАТ.
  const double x = u * double(width) - 0.5;
  const double y = v * double(height) - 0.5;
  const auto x0 = int64_t(std::floor(x));
  const auto y0 = int64_t(std::floor(y));
  const double fx = x - double(x0);
  const double fy = y - double(y0);

  const auto at = [&](int64_t xi, int64_t yi) {
    xi = std::clamp<int64_t>(xi, 0, int64_t(width) - 1);
    yi = std::clamp<int64_t>(yi, 0, int64_t(height) - 1);
    return field.get(size_t(yi) * width + size_t(xi));
  };

  const double top = at(x0, y0) * (1.0 - fx) + at(x0 + 1, y0) * fx;
  const double bottom = at(x0, y0 + 1) * (1.0 - fx) + at(x0 + 1, y0 + 1) * fx;
  return top * (1.0 - fy) + bottom * fy;
}

void tool_filtered_blur(const tool_call& call, const size_t begin, const size_t end) {
  const auto source = call.input(0).read();
  auto target = call.output(0).write();

  const auto shape = resolve_extent(call, call.output(0), "width", "height");
  const size_t width = shape.x;
  const size_t height = shape.y == 0 ? 1 : shape.y;
  const double radius = call.params->number("radius", 1.0);

  const double texel_x = 1.0 / double(width);
  const double texel_y = 1.0 / double(height);
  const double offset_x = texel_x * radius;
  const double offset_y = texel_y * radius;

  for (size_t i = begin; i < end; ++i) {
    const double u = (double(i % width) + 0.5) * texel_x;
    const double v = (double(i / width) + 0.5) * texel_y;

    double sum = sample_bilinear(source, width, height, u - offset_x, v - offset_y);
    sum += sample_bilinear(source, width, height, u + offset_x, v - offset_y);
    sum += sample_bilinear(source, width, height, u - offset_x, v + offset_y);
    sum += sample_bilinear(source, width, height, u + offset_x, v + offset_y);
    target.set(i, sum * 0.25);
  }
}

// ЦВЕТ ИЗ МЕТКИ. Присутствует ради того, чтобы разметку было ВИДНО: таблицы цветов у метки нет и
// заводить её незачем, а один и тот же номер обязан давать один и тот же цвет — значит цвет
// ВЫВОДИТСЯ из номера хешем. Приёмник — упакованные RGBA8 в целом поле, ровно то, что лежит в
// видимой текстуре.
//
// Затенение приходит ВТОРЫМ входом уже готовым (0..1): какую кривую применить к близости границы,
// решает автор обычным `remap`, а не спрятанная внутри инструмента константа.
void tool_label_colour(const tool_call& call, const size_t begin, const size_t end) {
  const auto labels = call.input(0).read();
  const auto shade = call.input(1).read();
  auto target = call.output(0).write();
  const double dim = call.params->number("dim", 0.25);

  for (size_t i = begin; i < end; ++i) {
    const auto label = uint32_t(labels.get(i));
    const uint32_t hashed = label * 2654435761u;
    const double lit = dim + (1.0 - dim) * std::clamp(shade.get(i), 0.0, 1.0);

    uint32_t packed = 255u << 24;
    for (uint32_t channel = 0; channel < 3; ++channel) {
      const double base = double((hashed >> (16 - channel * 8)) & 255u) / 255.0;
      packed |= uint32_t(std::clamp(base * lit, 0.0, 1.0) * 255.0 + 0.5) << (channel * 8);
    }
    target.set(i, double(packed));
  }
}

} // namespace

// УСТРОЙСТВЕННЫЕ ФОРМЫ ИНСТРУМЕНТОВ. Соглашение о шейдере описано у `tool_description::device_body`;
// здесь важно только то, что каждое тело повторяет тело своего инструмента ФОРМУЛА В ФОРМУЛУ.
//
// Привязок в этих текстах нет намеренно: их собирает `build_device_shader`, уже зная выведенный род
// каждого поля. Инструмент обращается к полю аксессором (`in_0_at`, `out_0_set`) и поэтому одинаково
// работает и над буфером, и над картинкой — а какой из двух это будет, решает вся очередь целиком.
//
// Повторяет, а не «делает то же самое»: если бы устройственная форма считала чуть иначе, разница
// проявилась бы не отказом, а другим миром — и заметить это можно было бы только сверкой двух путей,
// то есть тем, что §4.2 как раз и запрещает делать на плавающей программе.
//
// `+/- INFINITY` у границ зажима приходит числом: у push-константы нет способа сказать «не задано»,
// а бесконечность во float32 представима точно и ведёт себя в `clamp` ровно как отсутствие границы.

void tool_registry::add_standard_tools() {
  // Собственный шум ядра ставится вместе со стандартным набором: внешних зависимостей у него нет, а
  // устройственные тела есть — в отличие от обвязки FastNoise2, которая живёт отдельной целью.
  add_noise_field_tools(*this);

  // Именованная инициализация намеренно: набор полей описания инструмента будет расти, и
  // позиционная запись ломалась бы на каждом новом поле.
  add(tool_description{
    .name = "fill", .shape = aperture::pointwise, .input_count = 0, .output_count = 1, .body = tool_fill,
    // Заливка не смотрит на род поля вовсе — она кладёт одно и то же число, и на устройстве тоже.
    // Поэтому она объявляет себя годной для любого рода: иначе очередь, начинающаяся с обнуления
    // целого счётчика, не переносилась бы вся из-за самого простого своего элемента.
    .device_body = "  for (uint c = 0u; c < out_0_components(); ++c) out_0_set(index, c, args.value);\n",
    .device_params = {{"value", 0.0}},
    .device_integer_ready = true});
  add(tool_description{
    .name = "position_grid", .shape = aperture::pointwise, .input_count = 0, .output_count = 1,
    .body = tool_position_grid,
    // Форма приходит ОБЩЕЙ ШАПКОЙ (`extent_x`, `extent_y`), потому что она объявлена буфером, а не
    // параметром: второго способа назвать её нет ни на одном из путей. Начало координат — параметром,
    // и по той же причине, по которой его нет у тела на CPU: инструмент не знает про чанки, ключ
    // переводит в смещение тело шага.
    //
    // Третья компонента пишется только если поле её имеет: у однокомпонентного и двухкомпонентного
    // поля аксессора с индексом 2 просто нет, поэтому спрашивается объявление, а не угадывается.
    .device_body = "  uint x = index % args.extent_x;\n"
                   "  uint y = (index / args.extent_x) % max(args.extent_y, 1u);\n"
                   "  out_0_set(index, 0u, args.origin_x + float(x) * args.cell_size);\n"
                   "  out_0_set(index, 1u, args.origin_y + float(y) * args.cell_size);\n"
                   "#if ORIGINATOR_OUT_0_COMPONENTS >= 3\n"
                   "  uint plane = args.extent_x * max(args.extent_y, 1u);\n"
                   "  uint z = plane == 0u ? 0u : index / plane;\n"
                   "  out_0_set(index, 2u, args.origin_z + float(z) * args.cell_size);\n"
                   "#endif\n",
    .device_params = {{"cell_size", 1.0}, {"origin_x", 0.0}, {"origin_y", 0.0}, {"origin_z", 0.0}}});
  add(tool_description{.name = "value_noise", .shape = aperture::pointwise, .input_count = 0, .output_count = 1, .body = tool_value_noise});
  add(tool_description{
    .name = "index", .shape = aperture::pointwise, .input_count = 0, .output_count = 1, .body = tool_index,
    // Номер элемента — целое, и поле под него объявляют целым; тело написано против `float`, поэтому
    // точность держится до 2^24 элементов. Дальше номер обязан ехать целым родом, и это цена, которую
    // объявляет `device_integer_ready`.
    .device_body = "  out_0_set(index, float(index) * args.scale + args.offset);\n",
    .device_params = {{"scale", 1.0}, {"offset", 0.0}},
    .device_integer_ready = true});
  add(tool_description{
    .name = "remap", .shape = aperture::pointwise, .input_count = 1, .output_count = 1, .body = tool_remap,
    // `component` в устройственную форму не входит: многокомпонентное поле в очередь не пускается
    // (§3.2), поэтому компонента там всегда нулевая, и параметр стал бы ложью.
    // `sample` в GLSL — ЗАРЕЗЕРВИРОВАННОЕ слово (квалификатор интерполяции), поэтому величина
    // называется иначе. Ловится это только компилятором, и хорошо, что им: имя, случайно ставшее
    // ключевым, — ровно тот случай, для которого проверку типов и отдали glslc.
    .device_body = "  float raw = in_0_at(index);\n"
                   "  float taken = args.absolute != 0.0 ? abs(raw) : raw;\n"
                   "  out_0_set(index, clamp(taken * args.scale + args.offset, args.lower, args.upper));\n",
    // `min`/`max` в GLSL — встроенные функции, поэтому поле push-константы называется иначе, а имя
    // параметра остаётся прежним: переименовывать его значило бы менять конфиги ради шейдера.
    .device_params = {{"scale", 1.0}, {"offset", 0.0},
                      {"min", -std::numeric_limits<double>::infinity(), "lower"},
                      {"max", std::numeric_limits<double>::infinity(), "upper"},
                      {"absolute", 0.0}}});
  add(tool_description{
    .name = "classify", .shape = aperture::pointwise, .input_count = 2, .output_count = 1, .body = tool_classify,
    // Ветвление переведено ТЕРНАРНИКАМИ в том же порядке, в каком стоят проверки на CPU: порядок
    // здесь и есть правило, а не оформление — первая сработавшая проверка решает.
    .device_body = "  float height = in_0_at(index);\n"
                   "  float moisture = in_1_at(index);\n"
                   "  float wet_class = moisture < args.wet ? 2.0 : 3.0;\n"
                   "  float land_class = moisture < args.dry ? 1.0 : wet_class;\n"
                   "  out_0_set(index, height < args.sea_level ? 0.0 : land_class);\n",
    .device_params = {{"sea_level", 0.5}, {"dry", 0.35}, {"wet", 0.65}},
    // Класс — маленькое целое, и поле под него объявляют узким (`ub1`). Тело написано против `float`,
    // и над узким родом это точно: до 2^24 float32 представляет целые без потерь.
    .device_integer_ready = true});
  add(tool_description{
    .name = "blend", .shape = aperture::pointwise, .input_count = 2, .output_count = 1, .body = tool_blend,
    .device_body = "  float value = args.weight_first * in_0_at(index) + args.weight_second * in_1_at(index) + args.offset;\n"
                   "  out_0_set(index, clamp(value, args.lower, args.upper));\n",
    .device_params = {{"first", 1.0, "weight_first"}, {"second", 1.0, "weight_second"}, {"offset", 0.0},
                      {"min", -std::numeric_limits<double>::infinity(), "lower"},
                      {"max", std::numeric_limits<double>::infinity(), "upper"}}});
  add(tool_description{
    .name = "decay", .shape = aperture::pointwise, .input_count = 1, .output_count = 1, .body = tool_decay,
    // Отрицательное расстояние — метка «недостигнуто», а не расстояние; растить от неё экспоненту
    // нельзя, иначе дальше всех влияло бы сильнее всех. Зажим тот же, что на CPU.
    .device_body = "  float distance = max(0.0, in_0_at(index));\n"
                   "  out_0_set(index, args.offset + args.amplitude * exp(-distance / args.width));\n",
    .device_params = {{"width", 1.0}, {"amplitude", 1.0}, {"offset", 0.0}}});
  add(tool_description{
    .name = "modulate", .shape = aperture::pointwise, .input_count = 2, .output_count = 1, .body = tool_modulate,
    .device_body = "  out_0_set(index, args.scale * in_0_at(index) * in_1_at(index) + args.offset);\n",
    .device_params = {{"scale", 1.0}, {"offset", 0.0}}});
  add(tool_description{
    .name = "ratio", .shape = aperture::pointwise, .input_count = 2, .output_count = 1, .body = tool_ratio,
    // Знак знаменателя СОХРАНЯЕТСЯ при подъёме до минимума — иначе у отрицательной суммы весов
    // результат менял бы знак ровно там, где она мала. Обязательность `minimum_divisor` проверяет
    // тело на CPU; у устройственной формы значение по умолчанию бессмысленно, поэтому здесь стоит
    // то же, что подставил бы CPU, и вызов без параметра до устройства просто не доходит.
    .device_body = "  float divisor = in_1_at(index);\n"
                   "  float lifted = divisor < 0.0 ? -args.minimum_divisor : args.minimum_divisor;\n"
                   "  float safe = abs(divisor) < args.minimum_divisor ? lifted : divisor;\n"
                   "  out_0_set(index, args.scale * in_0_at(index) / safe + args.offset);\n",
    .device_params = {{"scale", 1.0}, {"offset", 0.0}, {"minimum_divisor", 1.0e-9}}});
  add(tool_description{
    .name = "maximum", .shape = aperture::pointwise, .input_count = 2, .output_count = 1, .body = tool_maximum,
    .device_body = "  out_0_set(index, max(in_0_at(index), in_1_at(index)));\n",
    .device_params = {}});
  add(tool_description{
    .name = "minimum", .shape = aperture::pointwise, .input_count = 2, .output_count = 1, .body = tool_minimum,
    .device_body = "  out_0_set(index, min(in_0_at(index), in_1_at(index)));\n",
    .device_params = {}});
  add(tool_description{
    .name = "box_blur", .shape = aperture::gather, .input_count = 1, .output_count = 1, .body = tool_box_blur,
    // Форма растра приходит ОБЩЕЙ ШАПКОЙ, а не параметром: `extent` объявлен буфером, и второго
    // способа назвать ширину не осталось ни на одном из путей.
    .device_body = "  uint radius = uint(args.radius);\n"
                   "  uint width = args.extent_x;\n"
                   "  uint height = args.extent_y;\n"
                   "  uint x = index % width;\n"
                   "  uint y = index / width;\n"
                   "  uint x0 = x >= radius ? x - radius : 0u;\n"
                   "  uint x1 = min(x + radius, width - 1u);\n"
                   "  uint y0 = y >= radius ? y - radius : 0u;\n"
                   "  uint y1 = height == 0u ? y : min(y + radius, height - 1u);\n"
                   "  float sum = 0.0;\n"
                   "  uint taken = 0u;\n"
                   "  for (uint sy = y0; sy <= y1; ++sy) {\n"
                   "    for (uint sx = x0; sx <= x1; ++sx) {\n"
                   "      uint at = sy * width + sx;\n"
                   "      if (at >= in_0_length()) continue;\n"
                   "      sum += in_0_at(at);\n"
                   "      taken += 1u;\n"
                   "    }\n"
                   "  }\n"
                   "  out_0_set(index, taken == 0u ? 0.0 : sum / float(taken));\n",
    .device_params = {{"radius", 1.0}}});

  add(tool_description{
    .name = "filtered_blur", .shape = aperture::gather, .input_count = 1, .output_count = 1,
    .body = tool_filtered_blur,
    .device_body = "  vec2 texel = vec2(1.0) / vec2(float(args.extent_x), float(args.extent_y));\n"
                   "  vec2 uv = (vec2(float(index % args.extent_x), float(index / args.extent_x)) + vec2(0.5)) * texel;\n"
                   "  vec2 offset = texel * args.radius;\n"
                   "  float sum = in_0_sample(uv + vec2(-offset.x, -offset.y));\n"
                   "  sum += in_0_sample(uv + vec2(offset.x, -offset.y));\n"
                   "  sum += in_0_sample(uv + vec2(-offset.x, offset.y));\n"
                   "  sum += in_0_sample(uv + vec2(offset.x, offset.y));\n"
                   "  out_0_set(index, sum * 0.25);\n",
    .device_params = {{"radius", 1.0}},
    // ЕДИНСТВЕННОЕ ОБЪЯВЛЕНИЕ, ИЗ КОТОРОГО СЛЕДУЕТ КАРТИНКА. Не «сделай вход образом», а «этот вход
    // читают МЕЖДУ элементами»; в картинку его превращает план, потому что больше ничем картинка от
    // буфера в лучшую сторону не отличается.
    .device_filtered_inputs = {0}});

  add(tool_description{
    .name = "label_colour", .shape = aperture::pointwise, .input_count = 2, .output_count = 1,
    .body = tool_label_colour,
    .device_body = "  uint hashed = uint(in_0_at(index)) * 2654435761u;\n"
                   "  vec3 base = vec3(float((hashed >> 16) & 255u), float((hashed >> 8) & 255u),\n"
                   "                   float(hashed & 255u)) / 255.0;\n"
                   "  float lit = args.dim + (1.0 - args.dim) * clamp(in_1_at(index), 0.0, 1.0);\n"
                   "  uvec3 bytes = uvec3(clamp(base * lit, vec3(0.0), vec3(1.0)) * 255.0 + vec3(0.5));\n"
                   "  out_0_set(index, bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (255u << 24u));\n",
    .device_params = {{"dim", 0.25}},
    // Приёмник — упакованные байты в целом поле, и тело написано под целое: `out_0_set` здесь берёт
    // `uint`, а не `float`.
    .device_integer_ready = true});

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
  add_constraint_tools();
}

} // namespace originator
} // namespace devils_engine
