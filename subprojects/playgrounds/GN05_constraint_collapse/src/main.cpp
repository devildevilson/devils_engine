#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/originator/generator_resource.h"
#include "devils_engine/originator/pipeline.h"
#include "devils_engine/originator/script_host.h"
#include "devils_engine/originator/tools.h"
#include "devils_engine/utils/core.h"

// GN05 — РАСКЛАДКА ПО ЖЁСТКИМ ЗАПРЕТАМ (wave function collapse).
//
// Вопрос площадки: что даёт генератору решатель ограничений, чего не дают уже имеющиеся инструменты.
// Ответ короткий — ЖЁСТКИЙ ЛОКАЛЬНЫЙ ЗАПРЕТ. Шум даёт плавность, вороной даёт области, заливка по
// графу даёт достижимость; ни один из них не умеет сказать «вода никогда не касается травы». Решатель
// умеет, и это единственное, что он ОБЕЩАЕТ, — поэтому проверяется именно это.
//
// Он же — единственный инструмент библиотеки, который может НЕ НАЙТИ ответа. Противоречие здесь
// нормальный исход, а не сбой: распространение ограничений только отсекает, существование решения оно
// не гарантирует. Отсюда объявленное число попыток и громкий отказ при их исчерпании: недособранная
// сетка — это другой мир под тем же зерном, и по результату этого не видно.
//
// ТРИ РЕЖИМА, И ОТВЕЧАЮТ ОНИ НА РАЗНЫЕ ВОПРОСЫ:
//
//   declared  правила ОБЪЯВЛЕНЫ автором. Автор точно знает, что именно запрещено, и может это
//             сказать словами;
//   learned   правила СНЯТЫ С ОБРАЗЦА. Автор не умеет сформулировать «берег выглядит вот так», зато
//             умеет это нарисовать — и тогда обещание меняется: ни одного окна в результате,
//             которого не было в образце;
//   graph     соседство приходит ДАННЫМИ, а не формой буфера. У дуги нет направления, поэтому
//             матрица одна и она обязана быть симметричной.
//
// И он же — ровно тот случай, ради которого в очереди есть отказ на `sequential`: какую клетку
// наблюдать следующей, решает поле, оставшееся после предыдущего распространения. Ни в очередь, ни на
// устройство он не попадёт никогда, и это не «пока не сделали».

namespace {

namespace fs = std::filesystem;
using namespace devils_engine;

enum class layout_mode { declared, learned, graph };

struct options {
  layout_mode mode = layout_mode::declared;
  size_t side = 96;
  uint64_t seed = 20260905;
  bool verify = false;
  bool frame = true;
  int64_t rollbacks = -1; // -1 => как объявлено в конфиге
  int64_t window = -1;
  std::string dump;
};

fs::path resource_root() {
  return fs::path(GN05_RESOURCE_ROOT);
}

// Три генератора, один набор ресурсов. Лесенка тайлов у них общая — все три шага `table` указывают на
// ОДИН файл, а не на три копии таблицы.
struct generator_registry {
  demiurg::module_system modules;
  demiurg::resource_system resources;
  originator::generator_config declared;
  originator::generator_config learned;
  originator::generator_config planet;

  generator_registry() : modules(resource_root().generic_string() + "/") {
    modules.load_modules({demiurg::module_system::list_entry{"gn05/", "", ""}});
    originator::register_generator_resources(resources);
    resources.parse_resources(&modules);
    declared = originator::load_generator(resources, "generator/tiles");
    learned = originator::load_generator(resources, "generator/learned");
    planet = originator::load_generator(resources, "generator/planet");
  }
};

const generator_registry& generator() {
  static const generator_registry registry;
  return registry;
}

const originator::generator_config& package_of(const layout_mode mode) {
  switch (mode) {
    case layout_mode::learned: return generator().learned;
    case layout_mode::graph: return generator().planet;
    default: return generator().declared;
  }
}

std::string_view mode_name(const layout_mode mode) {
  switch (mode) {
    case layout_mode::learned: return "learned";
    case layout_mode::graph: return "graph";
    default: return "declared";
  }
}

bool starts_with(const std::string_view& text, const std::string_view& prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

options parse_options(const int argc, const char** argv) {
  options result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--verify") {
      result.verify = true;
    } else if (argument == "--no-frame") {
      result.frame = false;
    } else if (starts_with(argument, "--mode=")) {
      const auto name = argument.substr(7);
      if (name == "declared") result.mode = layout_mode::declared;
      else if (name == "learned") result.mode = layout_mode::learned;
      else if (name == "graph") result.mode = layout_mode::graph;
      else utils::error{}("GN05: unknown mode '{}'", name);
    } else if (starts_with(argument, "--size=")) {
      result.side = std::stoul(std::string(argument.substr(7)));
    } else if (starts_with(argument, "--seed=")) {
      result.seed = std::stoull(std::string(argument.substr(7)));
    } else if (starts_with(argument, "--rollbacks=")) {
      result.rollbacks = std::stoll(std::string(argument.substr(12)));
    } else if (starts_with(argument, "--window=")) {
      result.window = std::stoll(std::string(argument.substr(9)));
    } else if (starts_with(argument, "--dump=")) {
      result.dump = std::string(argument.substr(7));
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "GN05 constraint collapse lab\n"
                << "  --mode=NAME     declared (по объявленным правилам), learned (по образцу), graph (по сфере)\n"
                << "  --size=N        сторона растра (по умолчанию 96)\n"
                << "  --seed=N        зерно пайплайна\n"
                << "  --rollbacks=N   сколько откатов разрешить решателю (переопределяет конфиг)\n"
                << "  --window=N      сторона окна обучения (переопределяет конфиг)\n"
                << "  --no-frame      не обводить карту глубокой водой заранее\n"
                << "  --verify        прогнать контрактные проверки всех трёх режимов\n"
                << "  --dump=PATH     сохранить раскладку в PPM — ДЛЯ ГЛАЗА, не для конвейера\n";
      std::exit(0);
    } else {
      utils::error{}("GN05: unknown argument '{}'", argument);
    }
  }
  if (result.side < 2) {
    utils::error{}("GN05: a raster smaller than two cells has no neighbours to constrain");
  }
  return result;
}

originator::tool_registry& tools() {
  static originator::tool_registry registry;
  if (registry.size() == 0) {
    registry.add_standard_tools();
    // Инструменты графа нужны обоим новым режимам: решётка и соседство на сфере — режиму graph,
    // выборка по индексу — режиму learned, где раскладка узоров переводится в тайлы представителями.
    registry.add_graph_tools();
  }
  return registry;
}

int64_t declared_number(const char* name) {
  const auto value = generator().declared.description.values.integer(name, 0);
  if (value <= 0) {
    utils::error{}("GN05: the config declares no {}", name);
  }
  return value;
}

size_t declared_tiles() {
  return size_t(declared_number("tile_count"));
}

originator::size_table make_sizes(const options& opts) {
  const auto tiles = declared_tiles();
  const auto capacity = size_t(declared_number("capacity"));
  const auto cells = size_t(declared_number("cell_count"));
  const auto neighbours = size_t(declared_number("neighbours"));

  originator::size_table sizes;
  sizes.set("single", 1);
  sizes.set("side", opts.side);
  sizes.set("tile_count", tiles);
  // Матрица соседства: две оси, тайл на тайл. Размер ВЫВОДИТСЯ из объявленного числа тайлов —
  // генератор обязан уметь назвать свою стоимость по памяти до запуска.
  sizes.set("rule_size", 2 * tiles * tiles);
  // На графе матрица ОДНА: у дуги нет направления.
  sizes.set("symmetric_rule_size", tiles * tiles);

  sizes.set("sample_side", size_t(declared_number("sample_side")));
  sizes.set("capacity", capacity);
  sizes.set("learned_rule_size", 2 * capacity * capacity);

  sizes.set("cell_count", cells);
  sizes.set("offset_count", cells + 1);
  // Соседство симметризуется, поэтому дуг у клетки может оказаться больше объявленных K: запас
  // объявлен, а переполнение инструмент ловит сам.
  sizes.set("arc_count", cells * (neighbours + 4));
  return sizes;
}

originator::pipeline_description load_description(const options& opts, const layout_mode mode) {
  auto description = package_of(mode).description;
  description.values.set_number("frame", opts.frame ? 1.0 : 0.0);
  if (opts.rollbacks >= 0) {
    description.values.set_number("rollbacks", double(opts.rollbacks));
  }
  if (opts.window > 0) {
    description.values.set_number("window", double(opts.window));
  }
  return description;
}

double run_pipeline(originator::pipeline& p, const originator::pipeline_description& description,
                    const layout_mode mode) {
  originator::script_host host(tools(), nullptr);
  const auto& package = package_of(mode);
  for (const auto& step : description.steps) {
    host.load_body(step.name, package.source(step.body), step.body);
  }
  const auto start = std::chrono::steady_clock::now();
  p.run(host.invoker());
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::vector<uint32_t> read_field(const originator::buffer& source, const std::string_view& name) {
  const auto field = source.field(source.find_field(name));
  std::vector<uint32_t> values(field.count());
  for (size_t i = 0; i < values.size(); ++i) {
    values[i] = uint32_t(field.get(i));
  }
  return values;
}

double read_one(originator::pipeline& p, const char* buffer, const char* field) {
  auto* source = p.find_buffer(buffer);
  return source->field(source->find_field(field)).get(0);
}

// ЕДИНСТВЕННОЕ, ЧТО АЛГОРИТМ ОБЕЩАЕТ. Всё остальное — эвристика, которую можно менять; это контракт,
// и его менять нельзя. Проверяется он ПО ТОЙ ЖЕ матрице, которую читал решатель, а не по копии правил
// в коде площадки: копия однажды разъехалась бы, и проверка стала бы проверять себя.
size_t broken_pairs(originator::pipeline& p, const char* rules_buffer, const std::vector<uint32_t>& tiles,
                    const size_t side, const size_t alphabet) {
  const auto* rules = p.find_buffer(rules_buffer);
  const auto allowed = rules->field(rules->find_field("allowed"));
  const auto permits = [&](const size_t axis, const size_t from, const size_t to) {
    return allowed.get((axis * alphabet + from) * alphabet + to) != 0.0;
  };

  size_t broken = 0;
  for (size_t y = 0; y < side; ++y) {
    for (size_t x = 0; x < side; ++x) {
      const auto own = tiles[y * side + x];
      if (x + 1 < side) broken += size_t(!permits(0, own, tiles[y * side + x + 1]));
      if (y + 1 < side) broken += size_t(!permits(1, own, tiles[(y + 1) * side + x]));
    }
  }
  return broken;
}

// То же обещание на графе: ни одной дуги, соединяющей запрещённую пару. Матрица ОДНА — направления у
// дуги нет.
size_t broken_arcs(originator::pipeline& p, size_t& checked) {
  const auto* rules = p.find_buffer("rules");
  const auto allowed = rules->field(rules->find_field("allowed"));
  const auto* offsets_buffer = p.find_buffer("cell_offsets");
  const auto offsets = offsets_buffer->field(offsets_buffer->find_field("start"));
  const auto* arcs_buffer = p.find_buffer("cell_arcs");
  const auto arcs = arcs_buffer->field(arcs_buffer->find_field("cell"));
  const auto* cells = p.find_buffer("cells");
  const auto tiles = cells->field(cells->find_field("tile"));

  const size_t alphabet = declared_tiles();
  const size_t count = cells->count();
  size_t broken = 0;
  checked = 0;
  for (size_t cell = 0; cell < count; ++cell) {
    const auto own = size_t(tiles.get(cell));
    const auto first = size_t(offsets.get(cell));
    const auto last = size_t(offsets.get(cell + 1));
    for (size_t k = first; k < last; ++k) {
      const auto other = size_t(arcs.get(k));
      if (other >= count) continue;
      broken += size_t(allowed.get(own * alphabet + size_t(tiles.get(other))) == 0.0);
      checked += 1;
    }
  }
  return broken;
}

bool write_ppm(const std::string& path, const std::vector<uint32_t>& tiles,
               const std::vector<uint32_t>& palette, const size_t width, const size_t height,
               const size_t scale) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << "P6\n" << width * scale << " " << height * scale << "\n255\n";
  for (size_t y = 0; y < height * scale; ++y) {
    for (size_t x = 0; x < width * scale; ++x) {
      const auto colour = palette[tiles[(y / scale) * width + (x / scale)]];
      out.put(char((colour >> 16) & 255u));
      out.put(char((colour >> 8) & 255u));
      out.put(char(colour & 255u));
    }
  }
  return bool(out);
}

void print_histogram(const std::vector<uint32_t>& tiles, const size_t tile_count) {
  std::vector<size_t> counts(tile_count, 0);
  for (const auto tile : tiles) {
    if (tile < tile_count) ++counts[tile];
  }
  std::cout << "  доли тайлов        ";
  for (size_t i = 0; i < tile_count; ++i) {
    std::cout << i << ":" << (100.0 * double(counts[i]) / double(tiles.size())) << "% ";
  }
  std::cout << "\n";
}

std::vector<uint32_t> palette_of(originator::pipeline& p) {
  return read_field(*p.find_buffer("tiles"), "colour");
}

// ОКНА ОБРАЗЦА. Обещание обученных правил формулируется именно окнами: в результате не должно быть ни
// одного окна `window x window`, которого не было в образце. Это заметно сильнее, чем «нет
// запрещённой пары», и проверяется по САМОМУ ОБРАЗЦУ, а не по снятой с него таблице: таблица — то,
// что проверяется.
std::unordered_set<std::string> windows_of(const std::vector<uint32_t>& values, const size_t width,
                                           const size_t height, const size_t window) {
  std::unordered_set<std::string> seen;
  if (window > width || window > height) {
    return seen;
  }
  std::string key(window * window * sizeof(uint32_t), '\0');
  for (size_t y = 0; y + window <= height; ++y) {
    for (size_t x = 0; x + window <= width; ++x) {
      for (size_t j = 0; j < window; ++j) {
        for (size_t i = 0; i < window; ++i) {
          const auto value = values[(y + j) * width + (x + i)];
          std::memcpy(key.data() + (j * window + i) * sizeof(uint32_t), &value, sizeof(value));
        }
      }
      seen.insert(key);
    }
  }
  return seen;
}

size_t windows_absent_from(const std::vector<uint32_t>& values, const size_t width, const size_t height,
                           const size_t window, const std::unordered_set<std::string>& allowed) {
  size_t absent = 0;
  const auto present = windows_of(values, width, height, window);
  for (const auto& key : present) {
    absent += size_t(allowed.find(key) == allowed.end());
  }
  return absent;
}

std::shared_ptr<originator::pipeline> run_mode(const options& opts, const layout_mode mode, double& milliseconds) {
  const auto description = load_description(opts, mode);
  auto p = std::make_shared<originator::pipeline>(description, make_sizes(opts), opts.seed);
  milliseconds = run_pipeline(*p, description, mode);
  return p;
}

int run_once(const options& opts) {
  double milliseconds = 0.0;
  auto p = run_mode(opts, opts.mode, milliseconds);
  const auto tile_count = declared_tiles();

  std::cout << "GN05 (" << mode_name(opts.mode) << "): зерно " << opts.seed << "\n";

  if (opts.mode == layout_mode::graph) {
    const auto tiles = read_field(*p->find_buffer("cells"), "tile");
    size_t checked = 0;
    const auto broken = broken_arcs(*p, checked);
    std::cout << "  клеток на сфере    " << tiles.size() << ", дуг " << checked << "\n"
              << "  решено за          " << milliseconds << " мс, попыток "
              << read_one(*p, "state", "attempts") << ", откатов " << read_one(*p, "state", "rollbacks") << "\n"
              << "  запрещённых дуг    " << broken << "\n"
              << "  память пайплайна   " << (double(p->total_byte_size()) / 1024.0) << " КиБ\n";
    print_histogram(tiles, tile_count);
    return 0;
  }

  const auto* raster = p->find_buffer("cells");
  const auto tiles = read_field(*raster, "tile");
  std::cout << "  растр              " << opts.side << "x" << opts.side << ", тайлов " << tile_count << "\n";
  if (opts.mode == layout_mode::learned) {
    std::cout << "  узоров из образца  " << read_one(*p, "state", "patterns") << " (окно "
              << load_description(opts, opts.mode).values.integer("window", 1) << ")\n";
  }
  std::cout << "  решено за          " << milliseconds << " мс, попыток "
            << read_one(*p, "state", "attempts") << ", откатов " << read_one(*p, "state", "rollbacks") << "\n";
  if (opts.mode == layout_mode::declared) {
    std::cout << "  запрещённых пар    " << broken_pairs(*p, "rules", tiles, opts.side, tile_count) << "\n";
  } else {
    const auto patterns = read_field(*raster, "pattern");
    const auto alphabet = size_t(declared_number("capacity"));
    std::cout << "  запрещённых пар    " << broken_pairs(*p, "learned_rules", patterns, opts.side, alphabet)
              << "\n";
  }
  std::cout << "  память пайплайна   " << (double(p->total_byte_size()) / 1024.0) << " КиБ\n";
  print_histogram(tiles, tile_count);

  if (!opts.dump.empty()) {
    if (!write_ppm(opts.dump, tiles, palette_of(*p), opts.side, opts.side, 6)) {
      utils::error{}("GN05: could not write '{}'", opts.dump);
    }
    std::cout << "  картинка для глаза " << opts.dump << "\n";
  }
  return 0;
}

struct checker {
  size_t checks = 0;
  size_t failures = 0;

  void operator()(const bool condition, const std::string_view& label) {
    ++checks;
    if (!condition) {
      ++failures;
      std::cout << "  ПРОВАЛ: " << label << "\n";
    }
  }
};

void verify_declared(const options& opts, checker& check) {
  const auto tile_count = declared_tiles();
  std::cout << "GN05 verify declared: растр " << opts.side << "x" << opts.side << ", тайлов " << tile_count << "\n";

  double milliseconds = 0.0;
  auto first = run_mode(opts, layout_mode::declared, milliseconds);
  const auto tiles = read_field(*first->find_buffer("cells"), "tile");

  // 1. ЕДИНСТВЕННОЕ ОБЕЩАНИЕ АЛГОРИТМА: ни одной запрещённой пары соседей.
  check(broken_pairs(*first, "rules", tiles, opts.side, tile_count) == 0,
        "ни одна пара соседей не нарушает правил");

  // 2. Решение СОДЕРЖАТЕЛЬНО. Сетка из одного тайла тоже не нарушает ни одного правила, но задачи не
  //    решает — а без этой проверки такой исход выглядел бы успехом.
  std::vector<size_t> present(tile_count, 0);
  bool in_range = true;
  for (const auto tile : tiles) {
    in_range = in_range && tile < tile_count;
    if (tile < tile_count) ++present[tile];
  }
  check(in_range, "тайл в пределах объявленной таблицы");
  size_t used = 0;
  for (const auto seen : present) {
    used += size_t(seen != 0);
  }
  check(used >= tile_count - 1, "в раскладке участвуют почти все объявленные тайлы");

  // 3. ЛЕСЕНКА СОБЛЮДЕНА ПО СМЫСЛУ, а не только по букве матрицы: между водой и травой обязан быть
  //    песок. Это та же проверка, что и первая, но сказанная на языке задачи — если бы таблица
  //    заполнялась неверно, первая проверка прошла бы, а эта нет.
  size_t water_touches_grass = 0;
  for (size_t y = 0; y < opts.side; ++y) {
    for (size_t x = 0; x < opts.side; ++x) {
      const auto own = tiles[y * opts.side + x];
      const auto pair = [&](const uint32_t other) {
        return (own == 1 && other == 3) || (own == 3 && other == 1);
      };
      if (x + 1 < opts.side) water_touches_grass += size_t(pair(tiles[y * opts.side + x + 1]));
      if (y + 1 < opts.side) water_touches_grass += size_t(pair(tiles[(y + 1) * opts.side + x]));
    }
  }
  check(water_touches_grass == 0, "между водой и травой всегда есть песок");

  // 4. ЗАРАНЕЕ ЗАНЯТЫЕ КЛЕТКИ соблюдены: рамка осталась глубокой водой.
  if (opts.frame) {
    bool framed = true;
    for (size_t x = 0; x < opts.side; ++x) {
      framed = framed && tiles[x] == 0 && tiles[(opts.side - 1) * opts.side + x] == 0;
    }
    for (size_t y = 0; y < opts.side; ++y) {
      framed = framed && tiles[y * opts.side] == 0 && tiles[y * opts.side + opts.side - 1] == 0;
    }
    check(framed, "заранее занятые клетки остались такими, какими их задали");
  }

  // 5. ПОВТОРЯЕМОСТЬ. Выбор клетки идёт по ЦЕЛОМУ критерию, а ничья ломается хешем от номера и зерна,
  //    поэтому порядок наблюдений один и тот же на любой машине. Откаты этого не меняют: они меняют
  //    ПУТЬ решателя, а не его определённость.
  auto again = run_mode(opts, layout_mode::declared, milliseconds);
  check(read_field(*again->find_buffer("cells"), "tile") == tiles, "то же зерно даёт ту же раскладку");

  auto other_seed = opts;
  other_seed.seed += 1;
  auto other = run_mode(other_seed, layout_mode::declared, milliseconds);
  check(read_field(*other->find_buffer("cells"), "tile") != tiles, "другое зерно даёт другую раскладку");

  // 6. ПАМЯТЬ НАЗВАНА. У решателя без откатов она вся в волне — `клетки x ceil(тайлы/32)` слов, — и
  //    объявленный размер пайплайна обязан быть того же порядка, а не на порядок меньше правды.
  const double declared = double(first->total_byte_size()) / 1024.0;
  const double wave = double(opts.side * opts.side * ((tile_count + 31) / 32) * sizeof(uint32_t)) / 1024.0;
  std::cout << "  объявлено " << declared << " КиБ, волна решателя " << wave << " КиБ\n";
  check(wave < declared * 4.0, "временная память решателя того же порядка, что объявленная");
}

void verify_learned(const options& opts, checker& check) {
  const auto description = load_description(opts, layout_mode::learned);
  const auto window = size_t(std::max<int64_t>(description.values.integer("window", 1), 1));
  const auto sample_side = size_t(declared_number("sample_side"));
  const auto tile_count = declared_tiles();

  std::cout << "GN05 verify learned: растр " << opts.side << "x" << opts.side << ", образец " << sample_side
            << "x" << sample_side << ", окно " << window << "\n";

  double milliseconds = 0.0;
  auto first = run_mode(opts, layout_mode::learned, milliseconds);

  const auto sample = read_field(*first->find_buffer("sample"), "tile");
  const auto tiles = read_field(*first->find_buffer("cells"), "tile");
  const auto patterns = read_field(*first->find_buffer("cells"), "pattern");
  const auto found = size_t(read_one(*first, "state", "patterns"));

  std::cout << "  узоров из образца " << found << ", попыток " << read_one(*first, "state", "attempts")
            << ", откатов " << read_one(*first, "state", "rollbacks") << "\n";

  // 1. ОБЕЩАНИЕ ОБУЧЕННЫХ ПРАВИЛ, и оно сильнее, чем у объявленных: в результате нет ни одного окна,
  //    которого не было в образце. Проверяется по САМОМУ ОБРАЗЦУ, а не по снятой с него таблице —
  //    таблица здесь то, что проверяется.
  const auto allowed = windows_of(sample, sample_side, sample_side, window);
  check(!allowed.empty(), "в образце есть хотя бы одно окно");
  check(windows_absent_from(tiles, opts.side, opts.side, window, allowed) == 0,
        "ни одного окна, которого не было в образце");

  // 2. Алфавит УЗОРОВ богаче алфавита тайлов — ровно за этим окно и нужно. При окне в одну клетку
  //    обучение вырождается в простые соседства тайлов, и это другой инструмент того же дела.
  check(found > tile_count, "узоров нашлось больше, чем тайлов");

  // 3. ПЕРЕВОД УЗОРОВ В ТАЙЛЫ сделан обычным `lookup` по представителям, и второго механизма для
  //    этого не понадобилось. Проверяется, что перевод сошёлся на каждой клетке.
  const auto representative = read_field(*first->find_buffer("alphabet"), "representative");
  bool decoded = true;
  for (size_t i = 0; i < tiles.size(); ++i) {
    decoded = decoded && patterns[i] < representative.size() && tiles[i] == representative[patterns[i]];
  }
  check(decoded, "раскладка узоров переведена в тайлы представителями");

  // 4. ЛЕСЕНКА, КОТОРУЮ НИКТО НЕ ОБЪЯВЛЯЛ. В этом режиме правил не писал никто — их сняли с
  //    картинки, — и всё же вода не касается травы, потому что на картинке между ними песок.
  size_t water_touches_grass = 0;
  for (size_t y = 0; y < opts.side; ++y) {
    for (size_t x = 0; x < opts.side; ++x) {
      const auto own = tiles[y * opts.side + x];
      const auto pair = [&](const uint32_t other) {
        return (own == 1 && other == 3) || (own == 3 && other == 1);
      };
      if (x + 1 < opts.side) water_touches_grass += size_t(pair(tiles[y * opts.side + x + 1]));
      if (y + 1 < opts.side) water_touches_grass += size_t(pair(tiles[(y + 1) * opts.side + x]));
    }
  }
  check(water_touches_grass == 0, "вода не касается травы, хотя этого никто не объявлял");

  // 5. ПОВТОРЯЕМОСТЬ. Обучение идёт по образцу в порядке обхода, поэтому номера узоров и снятая
  //    таблица одни и те же всегда.
  auto again = run_mode(opts, layout_mode::learned, milliseconds);
  check(read_field(*again->find_buffer("cells"), "tile") == tiles, "то же зерно даёт ту же раскладку");
}

void verify_graph(const options& opts, checker& check) {
  const auto tile_count = declared_tiles();
  const auto cell_count = size_t(declared_number("cell_count"));
  std::cout << "GN05 verify graph: клеток на сфере " << cell_count << ", тайлов " << tile_count << "\n";

  double milliseconds = 0.0;
  auto first = run_mode(opts, layout_mode::graph, milliseconds);
  const auto tiles = read_field(*first->find_buffer("cells"), "tile");

  // 1. ТО ЖЕ ОБЕЩАНИЕ, но соседство приходит данными: ни одной дуги между запрещённой парой.
  size_t checked = 0;
  const auto broken = broken_arcs(*first, checked);
  check(checked > 0, "у графа есть дуги");
  check(broken == 0, "ни одна дуга не соединяет запрещённую пару");

  // 2. СИММЕТРИЯ СОСЕДСТВА. Матрица правил здесь одна ИМЕННО потому, что у дуги нет направления, — и
  //    это утверждение о графе, а не о вкусе: если бы CSR оказался несимметричным, одна матрица
  //    означала бы не то, что написано.
  const auto* offsets_buffer = first->find_buffer("cell_offsets");
  const auto offsets = offsets_buffer->field(offsets_buffer->find_field("start"));
  const auto* arcs_buffer = first->find_buffer("cell_arcs");
  const auto arcs = arcs_buffer->field(arcs_buffer->find_field("cell"));
  bool symmetric = true;
  for (size_t cell = 0; cell < cell_count && symmetric; ++cell) {
    for (size_t k = size_t(offsets.get(cell)); k < size_t(offsets.get(cell + 1)); ++k) {
      const auto other = size_t(arcs.get(k));
      bool back = false;
      for (size_t j = size_t(offsets.get(other)); j < size_t(offsets.get(other + 1)); ++j) {
        back = back || size_t(arcs.get(j)) == cell;
      }
      symmetric = symmetric && back;
    }
  }
  check(symmetric, "соседство симметрично, поэтому одна матрица правил говорит именно то, что сказано");

  // 3. Решение содержательно.
  std::vector<size_t> present(tile_count, 0);
  for (const auto tile : tiles) {
    if (tile < tile_count) ++present[tile];
  }
  size_t used = 0;
  for (const auto seen : present) {
    used += size_t(seen != 0);
  }
  check(used >= tile_count - 1, "в раскладке участвуют почти все объявленные тайлы");

  // 4. ПОВТОРЯЕМОСТЬ.
  auto again = run_mode(opts, layout_mode::graph, milliseconds);
  check(read_field(*again->find_buffer("cells"), "tile") == tiles, "то же зерно даёт ту же раскладку");
}

int run_verify(const options& opts) {
  checker check;
  verify_declared(opts, check);
  verify_learned(opts, check);
  verify_graph(opts, check);
  std::cout << "GN05 verify: " << (check.checks - check.failures) << "/" << check.checks << "\n";
  return check.failures == 0 ? 0 : 1;
}
} // namespace

int main(const int argc, const char** argv) {
  try {
    const auto opts = parse_options(argc, argv);
    if (opts.verify) {
      return run_verify(opts);
    }
    return run_once(opts);
  } catch (const std::exception& error) {
    std::cerr << "GN05: " << error.what() << "\n";
    return 1;
  }
}
