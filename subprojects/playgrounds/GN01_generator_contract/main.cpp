#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "devils_engine/originator/pipeline.h"
#include "devils_engine/originator/script_host.h"
#include "devils_engine/originator/tools.h"
#include "devils_engine/thread/atomic_pool.h"

// GN01 — первая площадка кампании генератора.
//
// Вопрос площадки: где именно проходит граница между C++, devils_script и lua, и держится ли
// контракт «параллельно == последовательно бит в бит» на настоящем объёме данных.
//
// Площадка headless: результат генератора — данные, а не картинка, поэтому он проверяется числами.

namespace {

namespace fs = std::filesystem;
using namespace devils_engine;

struct options {
  size_t width = 1024;
  size_t threads = 0; // 0 => по числу ядер
  uint64_t seed = 20260831;
  originator::storage_kind::values layout = originator::storage_kind::soa;
  bool verify = false;
  bool bench = false;
  bool quiet = false;
};

struct timing {
  std::string label;
  double milliseconds = 0.0;
  size_t elements = 0;

  double nanoseconds_per_element() const {
    return elements == 0 ? 0.0 : milliseconds * 1000000.0 / double(elements);
  }
};

std::string read_file(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    utils::error{}("GN01: could not open '{}'", path.string());
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

fs::path resource_root() {
  const fs::path shipped = fs::path(GN01_RESOURCE_ROOT);
  return shipped;
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
    } else if (argument == "--bench") {
      result.bench = true;
    } else if (argument == "--quiet") {
      result.quiet = true;
    } else if (starts_with(argument, "--size=")) {
      result.width = size_t(std::stoull(std::string(argument.substr(7))));
    } else if (starts_with(argument, "--threads=")) {
      result.threads = size_t(std::stoull(std::string(argument.substr(10))));
    } else if (starts_with(argument, "--seed=")) {
      result.seed = std::stoull(std::string(argument.substr(7)));
    } else if (starts_with(argument, "--layout=")) {
      const auto value = argument.substr(9);
      result.layout = originator::parse_storage_kind(value);
      if (result.layout >= originator::storage_kind::count) {
        utils::error{}("GN01: unknown layout '{}', expected aos or soa", value);
      }
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "GN01 generator contract lab\n"
                << "  --size=N       ширина квадратной сетки (по умолчанию 1024)\n"
                << "  --layout=aos|soa  раскладка буфера cells\n"
                << "  --threads=N    число рабочих потоков (0 = по числу ядер)\n"
                << "  --seed=N       зерно пайплайна\n"
                << "  --verify       прогнать контрактные проверки\n"
                << "  --bench        замерить уровни исполнения\n";
      std::exit(0);
    } else {
      utils::error{}("GN01: unknown argument '{}'", argument);
    }
  }
  return result;
}

// Загружает пайплайн из конфига и подставляет размеры, известные только в рантайме.
originator::pipeline_description load_description(const options& opts) {
  const auto root = resource_root();

  originator::pipeline_description description;
  description.name = "gn01";
  description.buffers = originator::parse_buffers(read_file(root / "buffers.tavl"), "buffers.tavl");
  description.steps = originator::parse_steps(read_file(root / "steps.tavl"), "steps.tavl");

  for (auto& declaration : description.buffers) {
    if (declaration.name == "cells") {
      declaration.layout.storage = opts.layout;
    }
  }

  // Ширина сетки известна только из командной строки, а инструментам она нужна как параметр:
  // домена как отдельной абстракции нет, соседство — это данные, а не тип.
  for (auto& step : description.steps) {
    step.params.set_number("width", double(opts.width));
  }

  return description;
}

void load_bodies(originator::script_host& host, const originator::pipeline_description& description) {
  const auto root = resource_root();
  for (const auto& step : description.steps) {
    host.load_body(step.name, read_file(root / step.body), step.body);
    for (const auto& [program_name, path] : step.programs) {
      host.load_program(program_name, read_file(root / path));
    }
  }
}

std::vector<std::byte> snapshot(const originator::buffer& source) {
  std::vector<std::byte> copy(source.byte_size());
  std::memcpy(copy.data(), source.base_pointer(), copy.size());
  return copy;
}

double run_and_measure(originator::pipeline& p, originator::script_host& host) {
  const auto start = std::chrono::steady_clock::now();
  p.run(host.invoker());
  const auto stop = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

void print_state(const originator::pipeline_description& description, originator::pipeline& p, const options& opts) {
  (void)description;
  const auto* state = p.find_buffer("state");
  const auto peak = state->field(state->find_field("peak")).get(0);
  const auto land = state->field(state->find_field("land_cells")).get(0);
  const size_t count = opts.width * opts.width;

  std::cout << "  элементов          " << count << "\n"
            << "  память пайплайна   " << (double(p.total_byte_size()) / (1024.0 * 1024.0)) << " MB\n"
            << "  максимум height    " << peak << "\n"
            << "  клеток суши        " << land << " (" << (100.0 * land / double(count)) << "%)\n";
}

int run_verify(const options& opts) {
  std::cout << "GN01 verify: сетка " << opts.width << "x" << opts.width << ", зерно " << opts.seed << "\n";

  size_t checks = 0;
  size_t failures = 0;
  const auto check = [&](const bool condition, const std::string_view& label) {
    ++checks;
    if (!condition) {
      ++failures;
      std::cout << "  ПРОВАЛ: " << label << "\n";
    }
  };

  const size_t count = opts.width * opts.width;
  originator::size_table sizes;
  sizes.set("cell_count", count);
  sizes.set("single", 1);

  originator::tool_registry tools;
  tools.add_standard_tools();

  auto description = load_description(opts);

  // 1. Последовательное исполнение — эталон.
  std::vector<std::byte> serial_cells;
  double serial_peak = 0.0;
  {
    originator::script_host host(tools, nullptr);
    load_bodies(host, description);
    originator::pipeline p(description, sizes, opts.seed);
    p.run(host.invoker());
    serial_cells = snapshot(*p.find_buffer("cells"));
    const auto* state = p.find_buffer("state");
    serial_peak = state->field(state->find_field("peak")).get(0);
  }

  // 2. То же при разном числе потоков — обязано совпасть побайтово.
  for (const size_t threads : {size_t(1), size_t(3), size_t(7)}) {
    thread::atomic_pool pool(threads);
    originator::script_host host(tools, &pool);
    load_bodies(host, description);
    originator::pipeline p(description, sizes, opts.seed);
    p.run(host.invoker());

    const auto parallel_cells = snapshot(*p.find_buffer("cells"));
    check(parallel_cells == serial_cells,
          std::string("параллельно == последовательно, потоков ") + std::to_string(threads));

    const auto* state = p.find_buffer("state");
    check(state->field(state->find_field("peak")).get(0) == serial_peak,
          std::string("свёртка не зависит от числа потоков, потоков ") + std::to_string(threads));
  }

  // 3. Другая раскладка — те же ЗНАЧЕНИЯ по именам, хотя байты в памяти лежат иначе.
  {
    auto other = opts;
    other.layout = opts.layout == originator::storage_kind::soa ? originator::storage_kind::aos
                                                                : originator::storage_kind::soa;
    auto other_description = load_description(other);

    originator::script_host host(tools, nullptr);
    load_bodies(host, other_description);
    originator::pipeline p(other_description, sizes, opts.seed);
    p.run(host.invoker());

    originator::script_host reference_host(tools, nullptr);
    load_bodies(reference_host, description);
    originator::pipeline reference(description, sizes, opts.seed);
    reference.run(reference_host.invoker());

    auto* left = p.find_buffer("cells");
    auto* right = reference.find_buffer("cells");
    const auto left_height = left->field(left->find_field("height"));
    const auto right_height = right->field(right->find_field("height"));
    const auto left_biome = left->field(left->find_field("biome"));
    const auto right_biome = right->field(right->find_field("biome"));

    bool identical = true;
    for (size_t i = 0; i < count; ++i) {
      identical = identical && left_height.get(i) == right_height.get(i) && left_biome.get(i) == right_biome.get(i);
    }
    check(identical, "aos и soa дают одинаковые значения по именам полей");
    check(left->byte_size() != right->byte_size() || left->layout().storage == right->layout().storage,
          "раскладки действительно различаются размещением");
  }

  // 4. Три уровня исполнения считают одно и то же правило.
  {
    originator::script_host host(tools, nullptr);
    load_bodies(host, description);
    host.load_body("classify_lua", read_file(resource_root() / "scripts/classify_lua.lua"), "scripts/classify_lua.lua");

    auto extended = description;
    auto lua_step = extended.steps[1];
    lua_step.name = "classify_lua";
    lua_step.body = "scripts/classify_lua.lua";
    extended.steps.push_back(lua_step);

    originator::pipeline p(extended, sizes, opts.seed);
    p.run(host.invoker());

    auto* cells = p.find_buffer("cells");
    const auto native = cells->field(cells->find_field("biome"));
    const auto scripted = cells->field(cells->find_field("biome_lua"));

    const auto by_script = cells->field(cells->find_field("biome_ds"));

    bool same_lua = true;
    bool same_ds = true;
    for (size_t i = 0; i < count; ++i) {
      same_lua = same_lua && native.get(i) == scripted.get(i);
      same_ds = same_ds && native.get(i) == by_script.get(i);
    }
    check(same_lua, "нативный classify и его lua-двойник совпадают поэлементно");
    check(same_ds, "нативный classify и правило на devils_script совпадают поэлементно");
  }

  // 5. Контракты, которые обязаны падать громко.
  {
    originator::script_host host(tools, nullptr);
    load_bodies(host, description);

    bool rejected = false;
    try {
      // box_blur читает соседей: источник и приёмник обязаны различаться.
      auto broken = description;
      broken.steps.resize(1);
      originator::pipeline p(broken, sizes, opts.seed);
      auto* cells = p.find_buffer("cells");
      const auto field = cells->find_field("height");
      const std::vector<originator::field_ref> same_in{originator::field_ref{cells, nullptr, field}};
      const std::vector<originator::field_ref> same_out{originator::field_ref{cells, cells, field}};
      originator::parameters params;
      params.set_number("width", double(opts.width));
      originator::dispatch(*tools.find("box_blur"), same_in, same_out, params, 1, 0, count, "verify", nullptr);
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected, "gather с совпадающими источником и приёмником отклоняется");

    rejected = false;
    try {
      auto broken = description;
      broken.steps[0].writes.clear();
      broken.steps[0].reads.push_back("cells");
      originator::pipeline p(broken, sizes, opts.seed);
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected, "чтение буфера до первой записи отклоняется");
  }

  std::cout << "GN01 verify: " << (checks - failures) << "/" << checks << "\n";
  return failures == 0 ? 0 : 1;
}

int run_bench(const options& opts) {
  const size_t count = opts.width * opts.width;

  originator::size_table sizes;
  sizes.set("cell_count", count);
  sizes.set("single", 1);

  originator::tool_registry tools;
  tools.add_standard_tools();

  const size_t threads = opts.threads == 0 ? std::max<size_t>(std::thread::hardware_concurrency(), 1) - 1 : opts.threads;
  thread::atomic_pool pool(threads);

  std::cout << "GN01 bench: сетка " << opts.width << "x" << opts.width << " = " << count
            << " элементов, потоков " << threads << "\n\n";

  std::vector<timing> results;

  const auto measure = [&](const std::string& label, const size_t elements, const auto& body) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto stop = std::chrono::steady_clock::now();
    results.push_back(timing{label, std::chrono::duration<double, std::milli>(stop - start).count(), elements});
  };

  for (const auto layout : {originator::storage_kind::soa, originator::storage_kind::aos}) {
    auto local = opts;
    local.layout = layout;
    auto description = load_description(local);

    // Полный пайплайн последовательно и параллельно.
    {
      originator::script_host host(tools, nullptr);
      load_bodies(host, description);
      originator::pipeline p(description, sizes, opts.seed);
      measure(std::string("пайплайн, ") + std::string(to_string(layout)) + ", 1 поток", count,
              [&] { p.run(host.invoker()); });
    }
    {
      originator::script_host host(tools, &pool);
      load_bodies(host, description);
      originator::pipeline p(description, sizes, opts.seed);
      measure(std::string("пайплайн, ") + std::string(to_string(layout)) + ", " + std::to_string(threads) + " потоков",
              count, [&] { p.run(host.invoker()); });
    }
  }

  // Один и тот же семантический проход тремя способами.
  {
    auto description = load_description(opts);
    originator::script_host host(tools, &pool);
    load_bodies(host, description);
    host.load_body("classify_lua", read_file(resource_root() / "scripts/classify_lua.lua"), "scripts/classify_lua.lua");

    originator::pipeline p(description, sizes, opts.seed);
    p.run(host.invoker());

    auto* cells = p.find_buffer("cells");
    const std::vector<originator::field_ref> inputs{
      originator::field_ref{cells, nullptr, cells->find_field("smoothed")},
      originator::field_ref{cells, nullptr, cells->find_field("moisture")},
    };
    const std::vector<originator::field_ref> outputs{
      originator::field_ref{cells, cells, cells->find_field("biome")},
    };

    originator::parameters params;
    params.set_number("sea_level", 0.5);
    params.set_number("dry", 0.35);
    params.set_number("wet", 0.65);

    measure("классификация: нативный kernel, 1 поток", count, [&] {
      originator::dispatch(*tools.find("classify"), inputs, outputs, params, 1, 0, count, "bench", nullptr);
    });
    measure("классификация: нативный kernel, " + std::to_string(threads) + " потоков", count, [&] {
      originator::dispatch(*tools.find("classify"), inputs, outputs, params, 1, 0, count, "bench", &pool);
    });

    // Тот же расчёт поэлементно из lua. Именно эта строка объясняет, почему lua — дирижёр, а не
    // исполнитель: сравнивать её надо не с процентами, а с порядком величины.
    //
    // Предыдущие шаги обязаны отработать по-настоящему: на нулевом буфере ветвление всегда уходило
    // бы в самую дешёвую ветку, и замер сравнивал бы разную работу.
    auto lua_description = description;
    auto lua_step = lua_description.steps[1];
    lua_step.name = "classify_lua";
    lua_description.steps.push_back(lua_step);
    originator::pipeline lua_pipeline(lua_description, sizes, opts.seed);

    const size_t last = lua_pipeline.step_count() - 1;
    for (size_t i = 0; i < last; ++i) {
      lua_pipeline.run_step(i, host.invoker());
    }

    measure("классификация: поэлементный lua, 1 поток", count, [&] {
      lua_pipeline.run_step(last, host.invoker());
    });

    // Средний уровень: то же правило на devils_script. Шаг semantics уже объявлен в конфиге, его
    // индекс известен, а буферы к этому моменту заполнены по-настоящему.
    size_t semantics_index = description.steps.size();
    for (size_t i = 0; i < description.steps.size(); ++i) {
      if (description.steps[i].name == "semantics") {
        semantics_index = i;
      }
    }

    if (semantics_index < description.steps.size()) {
      originator::script_host serial_host(tools, nullptr);
      load_bodies(serial_host, description);
      originator::pipeline serial_pipeline(description, sizes, opts.seed);
      for (size_t i = 0; i < semantics_index; ++i) {
        serial_pipeline.run_step(i, serial_host.invoker());
      }
      // Первый вызов компилирует программу; замер должен мерить исполнение, а не компиляцию.
      serial_pipeline.run_step(semantics_index, serial_host.invoker());
      measure("классификация: devils_script, 1 поток", count, [&] {
        serial_pipeline.run_step(semantics_index, serial_host.invoker());
      });

      p.run_step(semantics_index, host.invoker());
      measure("классификация: devils_script, " + std::to_string(threads) + " потоков", count, [&] {
        p.run_step(semantics_index, host.invoker());
      });
    }
  }

  // Ширина колонки считается в СИМВОЛАХ, а не в байтах: подписи кириллические, и по длине строки
  // выравнивание разъезжается.
  const auto display_width = [](const std::string_view& text) {
    size_t width = 0;
    for (const char c : text) {
      width += (static_cast<unsigned char>(c) & 0xc0) != 0x80 ? 1 : 0;
    }
    return width;
  };

  constexpr size_t label_width = 50;
  std::cout << "  " << std::string(label_width, ' ') << "       мс   нс/элемент\n";
  for (const auto& entry : results) {
    std::string label = entry.label;
    const size_t width = display_width(label);
    if (width < label_width) {
      label.append(label_width - width, ' ');
    }
    std::cout << "  " << label << std::format(" {:8.2f} {:12.2f}", entry.milliseconds, entry.nanoseconds_per_element())
              << "\n";
  }

  return 0;
}

int run_once(const options& opts) {
  const size_t count = opts.width * opts.width;

  originator::size_table sizes;
  sizes.set("cell_count", count);
  sizes.set("single", 1);

  originator::tool_registry tools;
  tools.add_standard_tools();

  const size_t threads = opts.threads == 0 ? std::max<size_t>(std::thread::hardware_concurrency(), 1) - 1 : opts.threads;
  thread::atomic_pool pool(threads);

  auto description = load_description(opts);
  originator::script_host host(tools, threads == 0 ? nullptr : &pool);
  load_bodies(host, description);

  originator::pipeline p(description, sizes, opts.seed);
  const double milliseconds = run_and_measure(p, host);

  std::cout << "GN01: пайплайн '" << p.name() << "', раскладка " << to_string(opts.layout)
            << ", потоков " << threads << "\n";
  print_state(description, p, opts);
  std::cout << "  время              " << milliseconds << " ms\n";

  for (size_t i = 0; i < p.step_count(); ++i) {
    std::cout << "  шаг " << i << " '" << p.step_at(i).name << "' публикует:";
    for (const auto* published : p.published_after(i)) {
      std::cout << " " << published->name();
    }
    std::cout << "\n";
  }

  return 0;
}

} // namespace

int main(const int argc, const char** argv) {
  try {
    const auto opts = parse_options(argc, argv);
    if (opts.verify) {
      return run_verify(opts);
    }
    if (opts.bench) {
      return run_bench(opts);
    }
    return run_once(opts);
  } catch (const std::exception& error) {
    std::cerr << "GN01: " << error.what() << "\n";
    return 1;
  }
}
