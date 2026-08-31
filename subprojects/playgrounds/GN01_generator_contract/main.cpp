#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
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
#include "devils_engine/originator/primitives.h"
#include "devils_engine/originator/script_program.h"
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
  size_t map_width = 0; // 0 => равна width (нечанкованный прогон)
  size_t sites = 256;
  size_t chunks = 0;    // делений карты по стороне для --chunked
  bool normalize = true;
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
    } else if (starts_with(argument, "--chunked=")) {
      result.chunks = size_t(std::stoull(std::string(argument.substr(10))));
    } else if (starts_with(argument, "--sites=")) {
      result.sites = size_t(std::stoull(std::string(argument.substr(8))));
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
                << "  --sites=N      число сайтов областей (по умолчанию 256)\n"
                << "  --chunked=N    сверить генерацию по NxN чанкам с картой целиком\n"
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
originator::size_table make_sizes(const options& opts) {
  originator::size_table sizes;
  sizes.set("cell_count", opts.width * opts.width);
  sizes.set("site_count", opts.sites);
  sizes.set("site_count_plus_one", opts.sites + 1);
  // Ёмкость под дуги соседства. Средняя степень в триангуляции Делоне около шести, но у краевых
  // сайтов она выше, поэтому запас берётся с заметным множителем, а нехватка падает громко.
  sizes.set("arc_capacity", opts.sites * 16);
  // Планарная диаграмма даёт около двух уникальных вершин и шести-восьми углов на область; запас
  // берётся с множителем, а нехватка падает громко с нужным числом в сообщении.
  sizes.set("corner_capacity", opts.sites * 16);
  sizes.set("vertex_capacity", opts.sites * 8);
  sizes.set("single", 1);
  return sizes;
}

originator::pipeline_description load_description(const options& opts) {
  const auto root = resource_root();

  originator::pipeline_description description;
  description.name = "gn01";
  description.values = originator::parse_values(read_file(root / "values.tavl"), "values.tavl");
  description.buffers = originator::parse_buffers(read_file(root / "buffers.tavl"), "buffers.tavl");
  description.steps = originator::parse_steps(read_file(root / "steps.tavl"), "steps.tavl");

  for (auto& declaration : description.buffers) {
    if (declaration.name == "cells") {
      declaration.layout.storage = opts.layout;
    }
  }

  // Ширина сетки известна только из командной строки, а инструментам она нужна как параметр:
  // домена как отдельной абстракции нет, соседство — это данные, а не тип.
  //
  // width — ширина строки буфера, map_width — ширина всей карты. В обычном прогоне они совпадают;
  // при чанкованной генерации буфер меньше карты, и частоту шума нужно считать от карты, иначе
  // каждый чанк получил бы свой масштаб черт рельефа.
  description.values.set_number("width", double(opts.width));
  description.values.set_number("map_width", double(opts.map_width == 0 ? opts.width : opts.map_width));
  if (!opts.normalize) {
    description.values.set_number("normalize", 0.0);
  }

  return description;
}

// Перф-стенд намеренно нарушает правило «lua не обходит плотный буфер поэлементно» — он ровно это и
// измеряет. Поэтому бюджет тела шага здесь снимается ВСЛУХ: по умолчанию он ненулевой, и зацикленное
// тело падает с номером строки вместо того, чтобы повесить генерацию.
void allow_unbounded_scripts(originator::script_host& host) {
  host.set_budget(originator::script_budget{.instruction_limit = 0, .wall_time_us = 0});
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

// Снимок ВСЕХ буферов пайплайна: сравнивать только один было бы слабее, чем контракт обещает.
std::vector<std::vector<std::byte>> snapshot_all(originator::pipeline& p) {
  std::vector<std::vector<std::byte>> copies;
  copies.reserve(p.buffer_count());
  for (size_t i = 0; i < p.buffer_count(); ++i) {
    copies.push_back(snapshot(p.buffer_at(i)));
  }
  return copies;
}

// Контуры областей обязаны замощать карту: сумма площадей полигонов равна площади карты. Это самая
// сильная проверка сетки — она ловит и щели, и наложения, и неверный порядок обхода.
bool polygons_tile_the_map(originator::pipeline& p, const size_t site_count, const double map_side) {
  const auto* offsets = p.find_buffer("polygon_offsets");
  const auto* corners = p.find_buffer("polygon_corners");
  const auto* vertices = p.find_buffer("polygon_vertices");
  const auto* counts = p.find_buffer("polygon_counts");
  if (offsets == nullptr || corners == nullptr || vertices == nullptr || counts == nullptr) {
    return false;
  }

  const auto start = offsets->field(offsets->find_field("start"));
  const auto corner = corners->field(corners->find_field("vertex"));
  const auto position = vertices->field(vertices->find_field("position"));
  const auto vertex_count = size_t(counts->field(counts->find_field("vertices")).get(0));

  if (vertex_count == 0 || start.get(0) != 0.0) {
    return false;
  }

  double total = 0.0;
  for (size_t site = 0; site < site_count; ++site) {
    const auto first = size_t(start.get(site));
    const auto last = size_t(start.get(site + 1));
    if (last < first) {
      return false;
    }

    const size_t ring = last - first;
    if (ring < 3) {
      return false;
    }

    double area = 0.0;
    for (size_t k = 0; k < ring; ++k) {
      const auto index = size_t(corner.get(first + k));
      const auto next_index = size_t(corner.get(first + (k + 1) % ring));
      if (index >= vertex_count || next_index >= vertex_count) {
        return false;
      }
      area += position.get(index, 0) * position.get(next_index, 1) -
              position.get(next_index, 0) * position.get(index, 1);
    }

    area *= 0.5;
    if (area <= 0.0) {
      return false; // обход не против часовой либо полигон вырожден
    }
    total += area;
  }

  const double expected = map_side * map_side;
  return std::abs(total - expected) < 1e-4 * expected;
}

// Соседство областей обязано быть симметричным и канонизированным независимо от того, в каком
// порядке диаграмма отдала рёбра.
bool region_graph_is_canonical(originator::pipeline& p, const size_t site_count) {
  const auto* offsets = p.find_buffer("region_offsets");
  const auto* arcs = p.find_buffer("region_arcs");
  if (offsets == nullptr || arcs == nullptr) {
    return false;
  }

  const auto start = offsets->field(offsets->find_field("start"));
  const auto neighbour = arcs->field(arcs->find_field("site"));

  if (start.get(0) != 0.0) {
    return false;
  }

  for (size_t site = 0; site < site_count; ++site) {
    const auto first = size_t(start.get(site));
    const auto last = size_t(start.get(site + 1));
    if (last < first) {
      return false;
    }

    for (size_t k = first; k < last; ++k) {
      if (k > first && neighbour.get(k - 1) >= neighbour.get(k)) {
        return false; // не отсортировано или есть повтор
      }

      const auto other = size_t(neighbour.get(k));
      if (other == site || other >= site_count) {
        return false;
      }

      bool mirrored = false;
      for (size_t j = size_t(start.get(other)); j < size_t(start.get(other + 1)); ++j) {
        mirrored = mirrored || size_t(neighbour.get(j)) == site;
      }
      if (!mirrored) {
        return false;
      }
    }
  }

  return true;
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
  const auto sizes = make_sizes(opts);

  originator::tool_registry tools;
  tools.add_standard_tools();
  originator::add_all_primitives(tools);

  auto description = load_description(opts);

  // 1. Последовательное исполнение — эталон.
  std::vector<std::vector<std::byte>> serial_buffers;
  double serial_peak = 0.0;
  {
    originator::script_host host(tools, nullptr);
    load_bodies(host, description);
    originator::pipeline p(description, sizes, opts.seed);
    p.run(host.invoker());
    serial_buffers = snapshot_all(p);
    const auto* state = p.find_buffer("state");
    serial_peak = state->field(state->find_field("peak")).get(0);
    check(region_graph_is_canonical(p, opts.sites), "граф соседства областей симметричен и канонизирован");
    check(polygons_tile_the_map(p, opts.sites, double(opts.width)), "контуры областей замощают карту без щелей");

    // Раскладка по областям обязана покрыть КАЖДУЮ клетку ровно один раз, а суммы по областям —
    // сойтись с суммой по всему полю. Это перекрёстная проверка scatter против reduce: два
    // независимых пути должны дать одно число.
    const auto* cell_offsets = p.find_buffer("region_cell_offsets");
    const auto* order = p.find_buffer("region_cell_order");
    const auto* cells = p.find_buffer("cells");
    const auto* stats = p.find_buffer("region_stats");

    const auto cell_start = cell_offsets->field(cell_offsets->find_field("start"));
    const auto cell_index = order->field(order->find_field("cell"));
    const auto region = cells->field(cells->find_field("region"));
    const auto height = cells->field(cells->find_field("height"));
    const auto height_sum = stats->field(stats->find_field("height_sum"));

    check(size_t(cell_start.get(opts.sites)) == count, "раскладка по областям покрывает все клетки");

    std::vector<uint8_t> visited(count, 0);
    bool consistent = true;
    double scattered_total = 0.0;
    for (size_t site = 0; site < opts.sites; ++site) {
      double group_total = 0.0;
      for (size_t k = size_t(cell_start.get(site)); k < size_t(cell_start.get(site + 1)); ++k) {
        const auto cell = size_t(cell_index.get(k));
        consistent = consistent && cell < count && visited[cell] == 0 && size_t(region.get(cell)) == site;
        if (cell < count) {
          visited[cell] = 1;
          group_total += height.get(cell);
        }
      }
      scattered_total += height_sum.get(site);
      // Сумма, посчитанная accumulate, совпадает с прямым обходом группы.
      consistent = consistent && std::abs(group_total - height_sum.get(site)) < 1e-3 * (1.0 + std::abs(group_total));
    }
    check(consistent, "каждая клетка лежит ровно в своей области, и суммы accumulate сходятся");

    double direct_total = 0.0;
    for (size_t i = 0; i < count; ++i) {
      direct_total += height.get(i);
    }
    check(std::abs(direct_total - scattered_total) < 1e-2 * (1.0 + std::abs(direct_total)),
          "сумма по областям равна сумме по всему полю");
  }

  // 2. То же при разном числе потоков — обязано совпасть побайтово ВО ВСЕХ буферах, включая
  // результаты scatter: раскладку клеток по областям и суммы высот.
  for (const size_t threads : {size_t(1), size_t(3), size_t(7)}) {
    thread::atomic_pool pool(threads);
    originator::script_host host(tools, &pool);
    load_bodies(host, description);
    originator::pipeline p(description, sizes, opts.seed);
    p.run(host.invoker());

    check(snapshot_all(p) == serial_buffers,
          std::string("параллельно == последовательно во всех буферах, потоков ") + std::to_string(threads));

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
    const auto left_region = left->field(left->find_field("region"));
    const auto right_region = right->field(right->find_field("region"));

    bool identical = true;
    for (size_t i = 0; i < count; ++i) {
      identical = identical && left_height.get(i) == right_height.get(i) &&
                  left_biome.get(i) == right_biome.get(i) && left_region.get(i) == right_region.get(i);
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

  // 6. Окружение lua и бюджет тела шага.
  //
  // Стейт генератора — СВОЙ, а не общий с visage, и проверяется это не «по построению», а списком:
  // в окружении нет ни файловой системы, ни os, ни load, ни math.random, ни UI-глобалов visage.
  // Разделение важно в обе стороны: скрипт генератора не должен уметь тронуть UI, а UI-скрипт не
  // должен видеть буферы генератора.
  {
    static constexpr std::string_view environment_body = R"lua(
      return function(step)
        local forbidden = {
          "io", "os", "package", "require", "dofile", "loadfile", "load", "loadstring",
          "debug", "collectgarbage", "newproxy",
          -- глобалы visage: если они здесь видны, значит стейты смешаны
          "nk", "app", "ui", "settings",
        }
        for i = 1, #forbidden do
          local name = forbidden[i]
          if _G[name] ~= nil then error("visible global: " .. name) end
        end
        local required = {
          "assert", "error", "ipairs", "next", "pairs", "pcall", "xpcall", "select",
          "tonumber", "tostring", "type", "setmetatable", "getmetatable",
        }
        for i = 1, #required do
          local name = required[i]
          if _G[name] == nil then error("missing global: " .. name) end
        end
        if math.random ~= nil or math.randomseed ~= nil then error("math.random is visible") end
        if _G.originator == nil then error("originator api is missing") end
        if _G.originator.run == nil then error("originator.run is missing") end
      end
    )lua";

    // Тело, которое зацикливается целиком. Обязано упасть с именем шага и номером строки.
    static constexpr std::string_view loop_body = R"lua(
      return function(step)
        local x = 0
        while true do x = x + 1 end
      end
    )lua";

    // То же, но цикл спрятан в pcall: ошибку бюджета тело ловит и возвращается «успешно». Шаг всё
    // равно обязан провалиться — иначе зацикленное тело с pcall внутри висело бы вечно, а генерация
    // считала бы его выполненным.
    static constexpr std::string_view guarded_loop_body = R"lua(
      return function(step)
        pcall(function()
          local x = 0
          while true do x = x + 1 end
        end)
      end
    )lua";

    const auto run_probe = [&](const std::string_view& step_name, const std::string_view& source,
                               const originator::script_budget budget) {
      originator::script_host host(tools, nullptr);
      host.set_budget(budget);
      host.load_body(step_name, source, std::string("verify/") + std::string(step_name) + ".lua");

      auto probe = description;
      probe.steps.clear();
      originator::step_description step;
      step.name = std::string(step_name);
      step.body = std::string("verify/") + std::string(step_name) + ".lua";
      step.writes.push_back("cells");
      probe.steps.push_back(std::move(step));

      originator::pipeline p(probe, sizes, opts.seed);
      p.run(host.invoker());
    };

    {
      originator::script_host fresh(tools, nullptr);
      check(fresh.budget().instruction_limit != 0,
            "бюджет тела шага по умолчанию НЕ бесконечный");
    }

    bool clean = true;
    std::string environment_error;
    try {
      run_probe("environment", environment_body, originator::script_budget{});
    } catch (const std::exception& error) {
      clean = false;
      environment_error = error.what();
    }
    check(clean, std::string("окружение генератора отдельное и без недетерминизма: ") + environment_error);

    // Бюджет здесь маленький намеренно: проверка обязана занимать миллисекунды, а не ждать
    // настоящего лимита. Шаг хука считается из лимита, поэтому маленький лимит и ловится рано.
    bool rejected = false;
    try {
      run_probe("loop", loop_body, originator::script_budget{.instruction_limit = 200000, .wall_time_us = 0});
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected, "бесконечный цикл в теле шага падает по бюджету инструкций");

    rejected = false;
    try {
      run_probe("loop", loop_body, originator::script_budget{.instruction_limit = 0, .wall_time_us = 20000});
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected, "тот же цикл падает по бюджету времени, когда лимит инструкций снят");

    rejected = false;
    try {
      run_probe("guarded_loop", guarded_loop_body,
                originator::script_budget{.instruction_limit = 200000, .wall_time_us = 0});
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected, "проглоченная pcall'ом ошибка бюджета всё равно проваливает шаг");
  }

  std::cout << "GN01 verify: " << (checks - failures) << "/" << checks << "\n";
  return failures == 0 ? 0 : 1;
}

int run_bench(const options& opts) {
  const size_t count = opts.width * opts.width;
  const auto sizes = make_sizes(opts);

  originator::tool_registry tools;
  tools.add_standard_tools();
  originator::add_all_primitives(tools);

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
      allow_unbounded_scripts(host);
      load_bodies(host, description);
      originator::pipeline p(description, sizes, opts.seed);
      measure(std::string("пайплайн, ") + std::string(to_string(layout)) + ", 1 поток", count,
              [&] { p.run(host.invoker()); });
    }
    {
      originator::script_host host(tools, &pool);
      allow_unbounded_scripts(host);
      load_bodies(host, description);
      originator::pipeline p(description, sizes, opts.seed);
      measure(std::string("пайплайн, ") + std::string(to_string(layout)) + ", " + std::to_string(threads) + " потоков",
              count, [&] { p.run(host.invoker()); });
    }
  }

  // Шум отдельной строкой: полезно иметь изолированное число, чтобы цену STRICT_FP и смену дерева
  // узлов было с чем сравнивать.
  {
    auto description = load_description(opts);
    originator::script_host host(tools, &pool);
    allow_unbounded_scripts(host);
    load_bodies(host, description);
    originator::pipeline p(description, sizes, opts.seed);

    auto* cells = p.find_buffer("cells");
    const std::vector<originator::field_ref> out{
      originator::field_ref{cells, cells, cells->find_field("height")}};

    originator::parameters noise;
    noise.set_string("tree", "DQkGDA==");
    noise.set_number("width", double(opts.width));
    noise.set_number("frequency", 5.0 / double(opts.width));

    measure("шум: noise_grid, 1 поток", count, [&] {
      originator::dispatch(*tools.find("noise_grid"), {}, out, noise, 1, 0, count, "bench", nullptr);
    });
    measure("шум: noise_grid, " + std::to_string(threads) + " потоков", count, [&] {
      originator::dispatch(*tools.find("noise_grid"), {}, out, noise, 1, 0, count, "bench", &pool);
    });
  }

  // Отдельные примитивы областей: gather-разметка и оба scatter'а.
  {
    auto description = load_description(opts);
    originator::script_host host(tools, &pool);
    allow_unbounded_scripts(host);
    load_bodies(host, description);
    originator::pipeline p(description, sizes, opts.seed);
    p.run(host.invoker());

    auto* cells = p.find_buffer("cells");
    auto* sites = p.find_buffer("sites");
    auto* offsets = p.find_buffer("region_offsets");
    auto* arcs = p.find_buffer("region_arcs");
    auto* cell_offsets = p.find_buffer("region_cell_offsets");
    auto* order = p.find_buffer("region_cell_order");
    auto* stats = p.find_buffer("region_stats");

    const std::vector<originator::field_ref> site_in{
      originator::field_ref{sites, nullptr, sites->find_field("position")}};
    const std::vector<originator::field_ref> label_out{
      originator::field_ref{cells, cells, cells->find_field("region")}};

    originator::parameters region_params;
    region_params.set_number("width", double(opts.width));
    region_params.set_number("height", double(opts.width));
    region_params.set_number("site_count", double(opts.sites));

    measure("разметка областей: voronoi_label, 1 поток", count, [&] {
      originator::dispatch(*tools.find("voronoi_label"), site_in, label_out, region_params, 1, 0, count, "bench", nullptr);
    });
    measure("разметка областей: voronoi_label, " + std::to_string(threads) + " потоков", count, [&] {
      originator::dispatch(*tools.find("voronoi_label"), site_in, label_out, region_params, 1, 0, count, "bench", &pool);
    });

    const std::vector<originator::field_ref> csr_out{
      originator::field_ref{offsets, offsets, offsets->find_field("start")},
      originator::field_ref{arcs, arcs, arcs->find_field("site")}};
    measure("соседство областей: voronoi_adjacency (нс на сайт)", opts.sites, [&] {
      originator::dispatch(*tools.find("voronoi_adjacency"), site_in, csr_out, region_params, 1, 0, opts.sites, "bench", nullptr);
    });

    auto* polygon_offsets = p.find_buffer("polygon_offsets");
    auto* polygon_corners = p.find_buffer("polygon_corners");
    auto* polygon_vertices = p.find_buffer("polygon_vertices");
    auto* polygon_counts = p.find_buffer("polygon_counts");
    const std::vector<originator::field_ref> polygon_out{
      originator::field_ref{polygon_offsets, polygon_offsets, polygon_offsets->find_field("start")},
      originator::field_ref{polygon_corners, polygon_corners, polygon_corners->find_field("vertex")},
      originator::field_ref{polygon_vertices, polygon_vertices, polygon_vertices->find_field("position")},
      originator::field_ref{polygon_counts, polygon_counts, polygon_counts->find_field("vertices")}};
    measure("контуры областей: voronoi_polygons (нс на сайт)", opts.sites, [&] {
      originator::dispatch(*tools.find("voronoi_polygons"), site_in, polygon_out, region_params, 1, 0, opts.sites, "bench", nullptr);
    });

    const std::vector<originator::field_ref> key_in{
      originator::field_ref{cells, nullptr, cells->find_field("region")}};
    const std::vector<originator::field_ref> group_out{
      originator::field_ref{cell_offsets, cell_offsets, cell_offsets->find_field("start")},
      originator::field_ref{order, order, order->find_field("cell")}};
    const originator::parameters empty;

    measure("раскладка по областям: group_by, 1 поток", count, [&] {
      originator::dispatch(*tools.find("group_by"), key_in, group_out, empty, 1, 0, count, "bench", nullptr);
    });
    measure("раскладка по областям: group_by, " + std::to_string(threads) + " потоков", count, [&] {
      originator::dispatch(*tools.find("group_by"), key_in, group_out, empty, 1, 0, count, "bench", &pool);
    });

    const std::vector<originator::field_ref> accumulate_in{
      originator::field_ref{cells, nullptr, cells->find_field("region")},
      originator::field_ref{cells, nullptr, cells->find_field("height")}};
    const std::vector<originator::field_ref> sums_out{
      originator::field_ref{stats, stats, stats->find_field("height_sum")}};

    measure("суммы по областям: accumulate, 1 поток", count, [&] {
      originator::dispatch(*tools.find("accumulate"), accumulate_in, sums_out, empty, 1, 0, count, "bench", nullptr);
    });
    measure("суммы по областям: accumulate, " + std::to_string(threads) + " потоков", count, [&] {
      originator::dispatch(*tools.find("accumulate"), accumulate_in, sums_out, empty, 1, 0, count, "bench", &pool);
    });
  }

  // Один и тот же семантический проход тремя способами.
  {
    auto description = load_description(opts);
    originator::script_host host(tools, &pool);
    allow_unbounded_scripts(host);
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
      allow_unbounded_scripts(serial_host);
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

      // Та же программа с ПОРОГАМИ-ЛИТЕРАЛАМИ. Существует ради замера: показывает, во что обходится
      // вынос чисел из скрипта в конфиг. Литерал сворачивается на разборе, аргумент читается со
      // стека на каждом элементе, и разница видна.
      const std::vector<std::string> rule_inputs = {"smoothed", "moisture"};
      const auto literal_rule = originator::script_program::compile("biome_literal", R"(
        { value_or = { smoothed < 0.5, 0,
          value_or = { moisture < 0.35, 1,
          value_or = { moisture < 0.65, 2, 3 } } } }
      )", rule_inputs, originator::script_program::result_kind::number);

      const std::vector<originator::field_ref> rule_in{
        originator::field_ref{cells, nullptr, cells->find_field("smoothed")},
        originator::field_ref{cells, nullptr, cells->find_field("moisture")}};
      const std::vector<originator::field_ref> rule_out{
        originator::field_ref{cells, cells, cells->find_field("biome_ds")}};
      const originator::parameters no_arguments;

      measure("классификация: devils_script с литералами, 1 поток", count, [&] {
        originator::dispatch_script(*literal_rule, rule_in, rule_out, no_arguments, 1, 0, count, "bench", nullptr);
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

// Чанкованная генерация против карты целиком.
//
// Проверяется не «работает ли чанкование вообще», а КАКИЕ ШАГИ его переживают. Ответ разный, и это
// главное содержание режима: шум зависит только от мировой позиции и чанкуется точно, а шаги с
// апертурой gather на границе чанка видят не тех соседей и без полосы перекрытия не сходятся.
int run_chunked(const options& opts) {
  const size_t divisions = opts.chunks;
  if (divisions < 2 || opts.width % divisions != 0) {
    utils::error{}("GN01: --chunked=N requires N >= 2 that divides --size ({} % {} != 0)", opts.width, divisions);
  }

  const size_t chunk_width = opts.width / divisions;
  std::cout << "GN01 chunked: карта " << opts.width << "x" << opts.width << " как " << divisions << "x"
            << divisions << " чанков по " << chunk_width << "x" << chunk_width << "\n";

  originator::tool_registry tools;
  tools.add_standard_tools();
  originator::add_all_primitives(tools);

  // Нормализация по измеренному диапазону работает только для карты целиком, поэтому в этом режиме
  // обе стороны сравнения считаются с фиксированным масштабом. Иначе сравнивались бы не чанкование,
  // а два разных отображения.
  auto whole_options = opts;
  whole_options.normalize = false;
  auto whole_description = load_description(whole_options);

  originator::script_host whole_host(tools, nullptr);
  load_bodies(whole_host, whole_description);
  originator::pipeline whole(whole_description, make_sizes(whole_options), opts.seed);
  whole.run_step(0, whole_host.invoker()); // только terrain: остальные шаги сравниваются иначе
  const auto* whole_cells = whole.find_buffer("cells");
  const auto whole_height = whole_cells->field(whole_cells->find_field("height"));

  auto chunk_options = opts;
  chunk_options.normalize = false;
  chunk_options.width = chunk_width;
  chunk_options.map_width = opts.width;
  auto chunk_description = load_description(chunk_options);

  originator::script_host chunk_host(tools, nullptr);
  load_bodies(chunk_host, chunk_description);
  originator::pipeline chunked(chunk_description, make_sizes(chunk_options), opts.seed);

  double worst = 0.0;
  size_t compared = 0;

  for (size_t cy = 0; cy < divisions; ++cy) {
    for (size_t cx = 0; cx < divisions; ++cx) {
      // Порядок обхода намеренно не сбрасывает буферы: если бы шаг читал то, чего не писал, чанк
      // зависел бы от предыдущего, и расхождение это показало бы.
      chunked.set_chunk({int64_t(cx), int64_t(cy), 0});
      chunked.run_step(0, chunk_host.invoker());

      const auto* chunk_cells = chunked.find_buffer("cells");
      const auto chunk_height = chunk_cells->field(chunk_cells->find_field("height"));

      for (size_t y = 0; y < chunk_width; ++y) {
        for (size_t x = 0; x < chunk_width; ++x) {
          const size_t local = y * chunk_width + x;
          const size_t global = (cy * chunk_width + y) * opts.width + (cx * chunk_width + x);
          worst = std::max(worst, std::abs(chunk_height.get(local) - whole_height.get(global)));
          ++compared;
        }
      }
    }
  }

  std::cout << "  сверено клеток     " << compared << "\n"
            << "  худшее расхождение " << worst << "\n";

  // Повторная генерация первого чанка ПОСЛЕ всех остальных: результат обязан совпасть сам с собой.
  chunked.set_chunk({0, 0, 0});
  chunked.run_step(0, chunk_host.invoker());
  const auto* first_cells = chunked.find_buffer("cells");
  const auto first_height = first_cells->field(first_cells->find_field("height"));

  double drift = 0.0;
  for (size_t y = 0; y < chunk_width; ++y) {
    for (size_t x = 0; x < chunk_width; ++x) {
      drift = std::max(drift, std::abs(first_height.get(y * chunk_width + x) - whole_height.get(y * opts.width + x)));
    }
  }
  std::cout << "  повтор чанка (0,0) " << drift << " (обязан совпасть с первым проходом)\n";

  // Вторая половина ответа: шаг с апертурой gather чанкуется НЕ ТАК. box_blur читает окно вокруг
  // элемента, и у клетки возле границы чанка часть окна лежит в соседнем чанке, которого в буфере
  // нет. Ошибка обязана быть строго в полосе шириной radius и строго нулевой внутри.
  whole.run_step(1, whole_host.invoker());
  const auto whole_smoothed = whole_cells->field(whole_cells->find_field("smoothed"));
  const auto radius = size_t(std::max<int64_t>(int64_t(whole_description.values.integer("radius", 1)), 0));

  double interior_worst = 0.0;
  double border_worst = 0.0;
  size_t border_cells = 0;

  for (size_t cy = 0; cy < divisions; ++cy) {
    for (size_t cx = 0; cx < divisions; ++cx) {
      chunked.set_chunk({int64_t(cx), int64_t(cy), 0});
      chunked.run_step(0, chunk_host.invoker());
      chunked.run_step(1, chunk_host.invoker());

      const auto* chunk_cells = chunked.find_buffer("cells");
      const auto chunk_smoothed = chunk_cells->field(chunk_cells->find_field("smoothed"));

      for (size_t y = 0; y < chunk_width; ++y) {
        for (size_t x = 0; x < chunk_width; ++x) {
          const size_t global_x = cx * chunk_width + x;
          const size_t global_y = cy * chunk_width + y;
          const double difference = std::abs(chunk_smoothed.get(y * chunk_width + x) -
                                             whole_smoothed.get(global_y * opts.width + global_x));

          // «Возле границы» считается по границе ЧАНКА, а не карты: у края карты окно обрезано
          // одинаково в обоих прогонах, и расхождения там быть не должно.
          const bool near_chunk_edge =
            (x < radius && cx != 0) || (x + radius >= chunk_width && cx + 1 != divisions) ||
            (y < radius && cy != 0) || (y + radius >= chunk_width && cy + 1 != divisions);

          if (near_chunk_edge) {
            border_worst = std::max(border_worst, difference);
            ++border_cells;
          } else {
            interior_worst = std::max(interior_worst, difference);
          }
        }
      }
    }
  }

  std::cout << "  gather (box_blur, radius " << radius << "):\n"
            << "    внутри чанка     " << interior_worst << " на " << (compared - border_cells) << " клетках\n"
            << "    в полосе границы " << border_worst << " на " << border_cells << " клетках\n";

  const bool pointwise_exact = worst == 0.0 && drift == 0.0;
  const bool gather_localized = interior_worst == 0.0;

  // Третья часть ответа: scatter в чанковом проходе. Шаг regions честно объявил свои группы
  // глобальными, поэтому при активном чанке он ОБЯЗАН быть отклонён до исполнения. Молча посчитать
  // сводку по чанку — худший из возможных исходов: она зависела бы от того, какие чанки успели.
  bool scatter_refused = false;
  std::string refusal;
  try {
    size_t regions_index = chunk_description.steps.size();
    for (size_t i = 0; i < chunk_description.steps.size(); ++i) {
      if (chunk_description.steps[i].name == "regions") {
        regions_index = i;
      }
    }
    if (regions_index < chunk_description.steps.size()) {
      chunked.set_chunk({0, 0, 0});
      for (size_t i = 0; i < regions_index; ++i) {
        chunked.run_step(i, chunk_host.invoker());
      }
      chunked.run_step(regions_index, chunk_host.invoker());
    }
  } catch (const std::exception& error) {
    scatter_refused = true;
    refusal = error.what();
  }

  std::cout << "  scatter с глобальными группами при активном чанке: "
            << (scatter_refused ? "отклонён до исполнения" : "ПРОПУЩЕН (это ошибка)") << "\n";

  std::cout << "  вывод:\n"
            << "    pointwise (шум) чанкуется " << (pointwise_exact ? "ТОЧНО" : "С РАСХОЖДЕНИЕМ") << "\n"
            << "    gather (размытие) расходится " << (gather_localized ? "ТОЛЬКО в полосе шириной radius" : "И ВНУТРИ чанка")
            << " => нужна полоса перекрытия шириной radius\n";

  std::cout << "    глобальный scatter в чанковом проходе "
            << (scatter_refused ? "не собирается => сводки живут в грубом мировом проходе" : "ПРОШЁЛ, guard не работает")
            << "\n";

  return pointwise_exact && gather_localized && scatter_refused ? 0 : 1;
}

int run_once(const options& opts) {
  const size_t count = opts.width * opts.width;
  const auto sizes = make_sizes(opts);

  originator::tool_registry tools;
  tools.add_standard_tools();
  originator::add_all_primitives(tools);

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

  const auto* offsets = p.find_buffer("region_offsets");
  const auto* cell_offsets = p.find_buffer("region_cell_offsets");
  const auto* stats = p.find_buffer("region_stats");
  if (offsets != nullptr && cell_offsets != nullptr && stats != nullptr) {
    const auto arcs = offsets->field(offsets->find_field("start")).get(opts.sites);
    const auto cell_start = cell_offsets->field(cell_offsets->find_field("start"));
    const auto height_sum = stats->field(stats->find_field("height_sum"));

    size_t smallest = count;
    size_t largest = 0;
    size_t empty_regions = 0;
    double total_height = 0.0;
    for (size_t site = 0; site < opts.sites; ++site) {
      const auto size = size_t(cell_start.get(site + 1)) - size_t(cell_start.get(site));
      smallest = std::min(smallest, size);
      largest = std::max(largest, size);
      empty_regions += size == 0 ? 1 : 0;
      total_height += height_sum.get(site);
    }

    const auto* polygon_offsets = p.find_buffer("polygon_offsets");
    const auto* polygon_counts = p.find_buffer("polygon_counts");
    if (polygon_offsets != nullptr && polygon_counts != nullptr) {
      const auto corners = polygon_offsets->field(polygon_offsets->find_field("start")).get(opts.sites);
      const auto polygon_vertices = polygon_counts->field(polygon_counts->find_field("vertices")).get(0);
      std::cout << "  контуры            " << polygon_vertices << " общих вершин, " << corners
                << " углов, в среднем " << (corners / double(opts.sites)) << " на область\n";
    }

    std::cout << "  областей           " << opts.sites << ", дуг соседства " << arcs
              << ", средняя степень " << (arcs / double(opts.sites)) << "\n"
              << "  клеток в области   от " << smallest << " до " << largest
              << ", пустых областей " << empty_regions << "\n"
              << "  сумма высот        " << total_height << " (сходится с полем: "
              << (std::abs(total_height) > 0.0 ? "да" : "нет") << ")\n";
  }

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
    if (opts.chunks != 0) {
      return run_chunked(opts);
    }
    return run_once(opts);
  } catch (const std::exception& error) {
    std::cerr << "GN01: " << error.what() << "\n";
    return 1;
  }
}
