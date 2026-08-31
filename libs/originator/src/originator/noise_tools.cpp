#include <algorithm>
#include <memory>
#include <string>
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
};

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

  const auto width = size_t(std::max<int64_t>(call.params->integer("width", 1), 1));
  const float frequency = float(call.params->number("frequency", 0.01));
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

  const float frequency = float(call.params->number("frequency", 1.0));
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
