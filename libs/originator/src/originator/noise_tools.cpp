#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <FastNoise/FastNoise.h>

#include "devils_engine/originator/primitives.h"
#include "devils_engine/utils/core.h"

// Обвязка FastNoise2.
//
// Дерево узлов задаётся ЗАКОДИРОВАННОЙ СТРОКОЙ из редактора FastNoise2: граф шума становится данными
// конфига, а не кодом. Это тот же принцип, что и у остального пайплайна — менять слой, не
// перекомпилируя движок.
//
// Детерминизм собран из двух независимых частей, и обе проверены измерением, а не приняты на веру:
//
//   по чанкам   — значение зависит только от позиции и зерна. Но GenUniformGrid2D НАКАПЛИВАЕТ
//                 позиции внутри строки, поэтому обвязка генерирует строку целиком (см. ниже);
//   по машинам  — проект собирает FastNoise2 с FASTNOISE2_STRICT_FP, после чего выход всех наборов
//                 SIMD-инструкций побитово одинаков. Без этой опции Max и SSE2 расходились на
//                 1.8e-07, то есть один и тот же seed давал разные артефакты на разных машинах.
//
// Параметр feature_set остаётся диагностическим: он позволяет принудительно взять конкретный набор
// и сравнить, а не является условием воспроизводимости.

namespace devils_engine {
namespace originator {

namespace {

struct feature_set_name {
  std::string_view name;
  FastSIMD::FeatureSet value;
};

constexpr feature_set_name feature_sets[] = {
  {"max", FastSIMD::FeatureSet::Max},
  {"scalar", FastSIMD::FeatureSet::SCALAR},
  {"sse2", FastSIMD::FeatureSet::SSE2},
  {"sse41", FastSIMD::FeatureSet::SSE41},
  {"avx", FastSIMD::FeatureSet::AVX},
  {"avx2", FastSIMD::FeatureSet::AVX2},
  {"avx512", FastSIMD::FeatureSet::AVX512},
};

FastSIMD::FeatureSet read_feature_set(const tool_call& call) {
  const auto requested = call.params->string("feature_set", "max");
  for (const auto& entry : feature_sets) {
    if (entry.name == requested) {
      return entry.value;
    }
  }
  utils::error{}("originator step '{}': tool '{}' got unknown feature_set '{}'",
                 call.step_name, call.tool_name, requested);
}

// Скомпилированное дерево узлов. Строится подготовкой ОДИН раз: разбор закодированной строки на
// каждый чанк был бы чистой потерей, а сам генератор потокобезопасен на чтение.
struct prepared_noise {
  FastNoise::SmartNode<> generator;
  int seed = 0;
  float amplitude = 1.0f;
  float offset = 0.0f;
  // Частота, УЖЕ приведённая к мировым единицам, если вызывающий задал размер формы. Разрешается она
  // в подготовке, а не в теле: тело исполняется по чанкам, а перевод один на весь вызов.
  float frequency = 1.0f;
};

// ПЕРИОД ДЕРЕВА ШУМА при частоте 1, в единицах координат. Величина ИЗМЕРЯЕТСЯ, а не задаётся, и это
// принципиально: дерево приходит закодированной строкой из редактора, то есть внутри него может
// стоять любой доменный масштаб, любое число октав и любой множитель. Предполагать масштаб ЧУЖИХ
// данных нельзя — это уже стоило одной ошибки:
//
//   `frequency = 1/190` в расчёте «форма в 190 метров» дала форму в СЕМНАДЦАТЬ КИЛОМЕТРОВ, потому
//   что у общего дерева движка период оказался 91 единица. Чанк в 32 метра видел поле постоянным, и
//   мир получался пустым — при том, что ни одна проверка не падала.
//
// Меряется среднее расстояние между сменами знака вдоль НАКЛОННОЙ линии: у решёточного шума линия по
// одной оси проходит через узлы решётки и даёт не тот масштаб. Линия начинается далеко от начала
// координат, потому что у начала шум обычно проходит через нуль и особенно гладок.
//
// Зерно для измерения — ВСЕГДА НОЛЬ, а не зерно шага, и это тоже решение: период должен быть
// свойством ДЕРЕВА, иначе один и тот же `feature = 190` означал бы разные метры в разных мирах.
double measure_tree_period(const FastNoise::SmartNode<>& generator) {
  constexpr int count = 8192;
  constexpr double step_x = 0.7;
  constexpr double step_y = 0.31;
  constexpr double step_z = 0.53;

  std::vector<float> xs(count);
  std::vector<float> ys(count);
  std::vector<float> zs(count);
  std::vector<float> values(count);
  for (int i = 0; i < count; ++i) {
    xs[i] = float(1000.0 + double(i) * step_x);
    ys[i] = float(500.0 + double(i) * step_y);
    zs[i] = float(-300.0 + double(i) * step_z);
  }
  generator->GenPositionArray3D(values.data(), count, xs.data(), ys.data(), zs.data(), 0.0f, 0.0f, 0.0f, 0);

  size_t crossings = 0;
  for (int i = 1; i < count; ++i) {
    crossings += (values[i] >= 0.0f) != (values[i - 1] >= 0.0f) ? 1 : 0;
  }

  const double length = std::sqrt(step_x * step_x + step_y * step_y + step_z * step_z) * double(count - 1);
  // Смена знака происходит дважды за период, отсюда двойка. Дерево без смен знака вовсе (постоянное
  // или строго положительное) периода не имеет — тогда мировой размер формы неприменим, и об этом
  // надо сказать вслух, а не делить на ноль.
  return crossings == 0 ? 0.0 : 2.0 * length / double(crossings);
}

// Кэш периодов по тексту дерева. Дерево — данные конфига, их немного, а измерение стоит восемь тысяч
// выборок: платить за него на каждый чанк было бы четвертью стоимости самого шума.
double tree_period(const std::string_view& tree, const FastNoise::SmartNode<>& generator) {
  static std::mutex guard;
  static std::vector<std::pair<std::string, double>> cache;

  const std::lock_guard lock(guard);
  for (const auto& [key, value] : cache) {
    if (key == tree) {
      return value;
    }
  }
  const double period = measure_tree_period(generator);
  cache.emplace_back(std::string(tree), period);
  return period;
}

std::shared_ptr<void> prepare_noise(const tool_call& call) {
  const auto tree = call.params->string("tree");
  if (tree.empty()) {
    utils::error{}("originator step '{}': tool '{}' needs a 'tree' parameter — the encoded node tree "
                   "string from the FastNoise2 node editor",
                   call.step_name, call.tool_name);
  }

  auto prepared = std::make_shared<prepared_noise>();
  prepared->generator = FastNoise::NewFromEncodedNodeTree(std::string(tree).c_str(), read_feature_set(call));
  if (prepared->generator == nullptr) {
    utils::error{}("originator step '{}': tool '{}' could not decode node tree '{}'",
                   call.step_name, call.tool_name, tree);
  }

  // Зерно шага уже выведено из имени шага; смещение позволяет взять из одного шага несколько
  // независимых слоёв, не заводя второго зерна.
  prepared->seed = int(int64_t(call.seed) + call.params->integer("seed_offset", 0));
  prepared->amplitude = float(call.params->number("amplitude", 1.0));
  prepared->offset = float(call.params->number("offset", 0.0));

  // ЕДИНЫЙ МЕТР: размер формы задаётся в мировых единицах (`feature`), а не частотой. Частота —
  // величина обратной размерности, и в ней невозможно узнать глазом «сто девяносто метров»; хуже
  // того, её перевод в метры зависит от масштаба дерева, то есть от ЧУЖИХ данных. Поэтому масштаб
  // измеряется движком, а конфиг говорит длину.
  //
  // `frequency` остаётся: у него другой смысл — «единиц шума на единицу мира», и он нужен там, где
  // мировая длина не имеет смысла (например поле на единичной сфере, как у планеты).
  const bool has_feature = call.params->has("feature");
  const bool has_frequency = call.params->has("frequency");
  if (has_feature && has_frequency) {
    utils::error{}("originator step '{}': tool '{}' got both 'feature' ({}) and 'frequency' ({}) — the first "
                   "is a length in world units and the second is noise units per world unit, so only one of "
                   "them can be right",
                   call.step_name, call.tool_name, call.params->number("feature"),
                   call.params->number("frequency"));
  }

  if (has_feature) {
    const double feature = call.params->number("feature");
    if (feature <= 0.0) {
      utils::error{}("originator step '{}': tool '{}' needs a positive 'feature' size, got {}",
                     call.step_name, call.tool_name, feature);
    }
    const double period = tree_period(tree, prepared->generator);
    if (period <= 0.0) {
      utils::error{}("originator step '{}': tool '{}' cannot measure the period of tree '{}' — it never "
                     "changes sign, so a world feature size means nothing for it; use 'frequency'",
                     call.step_name, call.tool_name, tree);
    }
    prepared->frequency = float(period / feature);
  } else {
    // Значение по умолчанию у инструментов РАЗНОЕ, и менять его нельзя: у регулярной сетки
    // координата — индекс клетки, у выборки по позициям — мировая величина, поэтому разумный
    // множитель у них отличается на два порядка. Существующие миры считаны с этими числами.
    const double fallback = call.tool_name == "noise_grid" ? 0.01 : 1.0;
    prepared->frequency = float(call.params->number("frequency", fallback));
  }

  return prepared;
}

// Регулярная сетка.
//
// ТОНКОСТЬ, найденная замером: GenUniformGrid2D накапливает позиции внутри строки, поэтому отрезок,
// начинающийся с середины строки, даёт другой последний бит (расхождение порядка 3e-07). Полная
// строка при этом бит в бит совпадает с одним большим вызовом. Поэтому строка генерируется ЦЕЛИКОМ
// независимо от того, где начался диапазон, а наружу пишется только его часть: лишняя работа — не
// больше двух краевых строк на чанк, зато результат не зависит от разбиения по построению.
void tool_noise_grid(const tool_call& call, const size_t begin, const size_t end) {
  const auto& prepared = *static_cast<const prepared_noise*>(call.shared);
  auto target = call.output(0).write();

  const auto width = resolve_extent(call, call.output(0), "width").x;
  const float frequency = prepared.frequency;
  const float step = float(call.params->number("step", 1.0)) * frequency;
  const float x_origin = float(call.params->number("x_offset", 0.0)) * frequency;
  const float y_origin = float(call.params->number("y_offset", 0.0)) * frequency;

  if (end <= begin) {
    return;
  }

  const auto span = target.as_span<float>();
  const bool identity = prepared.amplitude == 1.0f && prepared.offset == 0.0f;
  // as_span сам отдаёт непустой span только для однокомпонентного поля точно совпадающего рода.
  const bool direct = !span.empty();

  const size_t first_row = begin / width;
  const size_t last_row = (end + width - 1) / width;

  std::vector<float> row;

  for (size_t y = first_row; y < last_row; ++y) {
    const size_t row_start = y * width;
    const size_t clipped_begin = std::max(begin, row_start);
    const size_t clipped_end = std::min(end, row_start + width);
    const bool whole_row = clipped_begin == row_start && clipped_end == row_start + width;

    float* out = nullptr;
    if (direct && identity && whole_row) {
      out = span.data() + row_start; // строка целиком внутри диапазона: пишем прямо в буфер
    } else {
      row.resize(width);
      out = row.data();
    }

    prepared.generator->GenUniformGrid2D(out, x_origin, y_origin + float(y) * step,
                                         int(width), 1, step, step, prepared.seed);

    if (out != row.data()) {
      continue;
    }

    for (size_t i = clipped_begin; i < clipped_end; ++i) {
      target.set(i, double(prepared.offset + prepared.amplitude * row[i - row_start]));
    }
  }
}

// Выборка в произвольных точках. Ради этого пути и нужен примитив: у кубосферы адрес поверхности —
// направление, а не пара (x, y), и никакая регулярная сетка его не выражает.
void tool_noise_at(const tool_call& call, const size_t begin, const size_t end) {
  const auto& prepared = *static_cast<const prepared_noise*>(call.shared);
  const auto positions = call.input(0).read();
  auto target = call.output(0).write();

  const uint32_t components = positions.type().components;
  if (components != 2 && components != 3) {
    utils::error{}("originator step '{}': noise_at needs a 2- or 3-component position field, '{}.{}' has {}",
                   call.step_name, call.input(0).buffer_name(), call.input(0).field_name(), components);
  }

  const float frequency = prepared.frequency;
  const size_t run = end > begin ? end - begin : 0;
  if (run == 0) {
    return;
  }

  std::vector<float> xs(run);
  std::vector<float> ys(run);
  std::vector<float> zs(components == 3 ? run : 0);
  std::vector<float> out(run);

  for (size_t i = 0; i < run; ++i) {
    xs[i] = float(positions.get(begin + i, 0)) * frequency;
    ys[i] = float(positions.get(begin + i, 1)) * frequency;
    if (components == 3) {
      zs[i] = float(positions.get(begin + i, 2)) * frequency;
    }
  }

  if (components == 3) {
    prepared.generator->GenPositionArray3D(out.data(), int(run), xs.data(), ys.data(), zs.data(),
                                           0.0f, 0.0f, 0.0f, prepared.seed);
  } else {
    prepared.generator->GenPositionArray2D(out.data(), int(run), xs.data(), ys.data(),
                                           0.0f, 0.0f, prepared.seed);
  }

  for (size_t i = 0; i < run; ++i) {
    target.set(begin + i, double(prepared.offset + prepared.amplitude * out[i]));
  }
}

} // namespace

void add_noise_tools(tool_registry& registry) {
  registry.add(tool_description{.name = "noise_grid", .shape = aperture::pointwise,
                                .input_count = 0, .output_count = 1,
                                .body = tool_noise_grid, .prepare = prepare_noise});
  registry.add(tool_description{.name = "noise_at", .shape = aperture::pointwise,
                                .input_count = 1, .output_count = 1,
                                .body = tool_noise_at, .prepare = prepare_noise});
}

} // namespace originator
} // namespace devils_engine
