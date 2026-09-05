#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
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
// И он же — ровно тот случай, ради которого в очереди есть отказ на `sequential`: какую клетку
// наблюдать следующей, решает поле, оставшееся после предыдущего распространения. Ни в очередь, ни на
// устройство он не попадёт никогда, и это не «пока не сделали».

namespace {

namespace fs = std::filesystem;
using namespace devils_engine;

struct options {
  size_t side = 96;
  uint64_t seed = 20260905;
  bool verify = false;
  bool frame = true;
  std::string dump;
};

fs::path resource_root() {
  return fs::path(GN05_RESOURCE_ROOT);
}

struct generator_registry {
  demiurg::module_system modules;
  demiurg::resource_system resources;
  originator::generator_config config;

  generator_registry() : modules(resource_root().generic_string() + "/") {
    modules.load_modules({demiurg::module_system::list_entry{"gn05/", "", ""}});
    originator::register_generator_resources(resources);
    resources.parse_resources(&modules);
    config = originator::load_generator(resources, "generator/tiles");
  }
};

const generator_registry& generator() {
  static const generator_registry registry;
  return registry;
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
    } else if (starts_with(argument, "--size=")) {
      result.side = std::stoul(std::string(argument.substr(7)));
    } else if (starts_with(argument, "--seed=")) {
      result.seed = std::stoull(std::string(argument.substr(7)));
    } else if (starts_with(argument, "--dump=")) {
      result.dump = std::string(argument.substr(7));
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "GN05 constraint collapse lab\n"
                << "  --size=N     сторона растра (по умолчанию 96)\n"
                << "  --seed=N     зерно пайплайна\n"
                << "  --no-frame   не обводить карту глубокой водой заранее\n"
                << "  --verify     прогнать контрактные проверки\n"
                << "  --dump=PATH  сохранить раскладку в PPM — ДЛЯ ГЛАЗА, не для конвейера\n";
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
  }
  return registry;
}

size_t declared_tiles() {
  const auto count = generator().config.description.values.integer("tile_count", 0);
  if (count <= 0) {
    utils::error{}("GN05: the config declares no tile_count");
  }
  return size_t(count);
}

originator::size_table make_sizes(const options& opts) {
  originator::size_table sizes;
  const auto tiles = declared_tiles();
  sizes.set("side", opts.side);
  sizes.set("tile_count", tiles);
  // Матрица соседства: две оси, тайл на тайл. Размер ВЫВОДИТСЯ из объявленного числа тайлов —
  // генератор обязан уметь назвать свою стоимость по памяти до запуска.
  sizes.set("rule_size", 2 * tiles * tiles);
  sizes.set("single", 1);
  return sizes;
}

originator::pipeline_description load_description(const options& opts) {
  auto description = generator().config.description;
  description.values.set_number("frame", opts.frame ? 1.0 : 0.0);
  return description;
}

void load_bodies(originator::script_host& host, const originator::pipeline_description& description) {
  const auto& package = generator().config;
  for (const auto& step : description.steps) {
    host.load_body(step.name, package.source(step.body), step.body);
  }
}

double run_pipeline(originator::pipeline& p, const originator::pipeline_description& description) {
  originator::script_host host(tools(), nullptr);
  load_bodies(host, description);
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

// ЕДИНСТВЕННОЕ, ЧТО АЛГОРИТМ ОБЕЩАЕТ. Всё остальное — эвристика, которую можно менять; это контракт,
// и его менять нельзя. Проверяется он ПО ТОЙ ЖЕ матрице, которую читал решатель, а не по копии
// правил в коде площадки: копия однажды разъехалась бы, и проверка стала бы проверять себя.
size_t broken_pairs(originator::pipeline& p, const std::vector<uint32_t>& tiles, const size_t side,
                    const size_t tile_count) {
  const auto* rules = p.find_buffer("rules");
  const auto allowed = rules->field(rules->find_field("allowed"));
  const auto permits = [&](const size_t axis, const size_t from, const size_t to) {
    return allowed.get((axis * tile_count + from) * tile_count + to) != 0.0;
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

bool write_ppm(const std::string& path, const std::vector<uint32_t>& tiles,
               const std::vector<uint32_t>& palette, const size_t side, const size_t scale) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << "P6\n" << side * scale << " " << side * scale << "\n255\n";
  for (size_t y = 0; y < side * scale; ++y) {
    for (size_t x = 0; x < side * scale; ++x) {
      const auto colour = palette[tiles[(y / scale) * side + (x / scale)]];
      out.put(char((colour >> 16) & 255u));
      out.put(char((colour >> 8) & 255u));
      out.put(char(colour & 255u));
    }
  }
  return bool(out);
}

void print_histogram(const std::vector<uint32_t>& tiles, const size_t tile_count, const size_t cells) {
  std::vector<size_t> counts(tile_count, 0);
  for (const auto tile : tiles) {
    ++counts[tile];
  }
  std::cout << "  доли тайлов        ";
  for (size_t i = 0; i < tile_count; ++i) {
    std::cout << i << ":" << (100.0 * double(counts[i]) / double(cells)) << "% ";
  }
  std::cout << "\n";
}

int run_once(const options& opts) {
  const auto description = load_description(opts);
  const auto sizes = make_sizes(opts);
  originator::pipeline p(description, sizes, opts.seed);
  const auto milliseconds = run_pipeline(p, description);

  const auto tile_count = declared_tiles();
  const auto cells = opts.side * opts.side;
  const auto* raster = p.find_buffer("cells");
  const auto tiles = read_field(*raster, "tile");
  const auto* state = p.find_buffer("state");
  const auto attempts = size_t(state->field(state->find_field("attempts")).get(0));

  std::cout << "GN05: растр " << opts.side << "x" << opts.side << ", тайлов " << tile_count << ", зерно "
            << opts.seed << "\n"
            << "  решено за          " << milliseconds << " мс, попыток " << attempts << "\n"
            << "  запрещённых пар    " << broken_pairs(p, tiles, opts.side, tile_count) << "\n"
            << "  память пайплайна   " << (double(p.total_byte_size()) / 1024.0) << " КиБ\n";
  print_histogram(tiles, tile_count, cells);

  if (!opts.dump.empty()) {
    const auto* table = p.find_buffer("tiles");
    const auto palette = read_field(*table, "colour");
    if (!write_ppm(opts.dump, tiles, palette, opts.side, 6)) {
      utils::error{}("GN05: could not write '{}'", opts.dump);
    }
    std::cout << "  картинка для глаза " << opts.dump << "\n";
  }
  return 0;
}

int run_verify(const options& opts) {
  size_t checks = 0;
  size_t failures = 0;
  const auto check = [&](const bool condition, const std::string_view& label) {
    ++checks;
    if (!condition) {
      ++failures;
      std::cout << "  ПРОВАЛ: " << label << "\n";
    }
  };

  const auto tile_count = declared_tiles();
  const auto sizes = make_sizes(opts);
  const auto cells = opts.side * opts.side;

  std::cout << "GN05 verify: растр " << opts.side << "x" << opts.side << ", тайлов " << tile_count << "\n";

  const auto description = load_description(opts);
  originator::pipeline first(description, sizes, opts.seed);
  run_pipeline(first, description);
  const auto tiles = read_field(*first.find_buffer("cells"), "tile");

  // 1. ЕДИНСТВЕННОЕ ОБЕЩАНИЕ АЛГОРИТМА: ни одной запрещённой пары соседей.
  check(broken_pairs(first, tiles, opts.side, tile_count) == 0, "ни одна пара соседей не нарушает правил");

  // 2. Решение СОДЕРЖАТЕЛЬНО. Сетка из одного тайла тоже не нарушает ни одного правила, но задачи не
  //    решает — а без этой проверки такой исход выглядел бы успехом.
  std::vector<size_t> present(tile_count, 0);
  for (const auto tile : tiles) {
    check(tile < tile_count, "тайл в пределах объявленной таблицы");
    checks -= 1; // проверка на клетку не должна раздувать счёт: важен её ИТОГ, а не число элементов
    ++present[tile];
  }
  ++checks;
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
  //    поэтому порядок наблюдений один и тот же на любой машине.
  originator::pipeline again(description, sizes, opts.seed);
  run_pipeline(again, description);
  check(read_field(*again.find_buffer("cells"), "tile") == tiles, "то же зерно даёт ту же раскладку");

  originator::pipeline other(description, sizes, opts.seed + 1);
  run_pipeline(other, description);
  check(read_field(*other.find_buffer("cells"), "tile") != tiles, "другое зерно даёт другую раскладку");

  // 6. ПАМЯТЬ НАЗВАНА. У решателя она вся в волне — `клетки x ceil(тайлы/32)` слов, — и объявленный
  //    размер пайплайна обязан быть того же порядка, а не на порядок меньше правды.
  const double declared = double(first.total_byte_size()) / 1024.0;
  const double wave = double(cells * ((tile_count + 31) / 32) * sizeof(uint32_t)) / 1024.0;
  std::cout << "  объявлено " << declared << " КиБ, волна решателя " << wave << " КиБ\n";
  check(wave < declared * 4.0, "временная память решателя того же порядка, что объявленная");

  std::cout << "GN05 verify: " << (checks - failures) << "/" << checks << "\n";
  return failures == 0 ? 0 : 1;
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
