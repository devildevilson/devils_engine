#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <string_view>
#include <vector>

#include "devils_engine/originator/pipeline.h"
#include "devils_engine/originator/primitives.h"
#include "devils_engine/originator/script_host.h"
#include "devils_engine/originator/script_program.h"
#include "devils_engine/originator/tools.h"
#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/utils/kd_tree.h"

#include "package.h"
#include "planet_tools.h"

// GN02 — генератор планеты. Вторая площадка кампании генератора и первый её ПОТРЕБИТЕЛЬ: GN01
// отвечала на вопрос «как устроен генератор», GN02 отвечает на вопрос «что из этого получается мир».
//
// Площадка headless. Планета здесь — данные: клетки на замкнутой поверхности, плиты, рельеф, климат,
// провинции, морские зоны, культуры и короткая история. Итог генерации уходит в ПАКЕТ на диск, и
// именно пакет, а не картинка, является результатом: генератор одноразовый, играют уже с него.
//
// Что проверяется числами вместо глаз: воспроизводимость (то же зерно => тот же отпечаток),
// независимость от числа потоков, свойства топологии (симметричное соседство, связность), и
// свойства мира, которые обязаны держаться при любом зерне — доля суши, связность провинций,
// покрытие суши провинциями, лето не холоднее зимы.

namespace {

namespace fs = std::filesystem;
using namespace devils_engine;

struct options {
  size_t cells = 65536;
  size_t plates = 24;
  size_t provinces = 900;
  size_t sea_zones = 90;
  size_t cultures = 48;
  size_t threads = 0; // 0 => по числу ядер
  uint64_t seed = 20260901;
  fs::path dump;
  std::string stats;
  bool verify = false;
  bool report = true;
  bool map = false;
  bool quiet = false;
};

std::string read_file(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    utils::error{}("GN02: could not open '{}'", path.string());
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

fs::path resource_root() {
  return fs::path(GN02_RESOURCE_ROOT);
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
    } else if (argument == "--quiet") {
      result.quiet = true;
      result.report = false;
    } else if (argument == "--map") {
      result.map = true;
    } else if (starts_with(argument, "--cells=")) {
      result.cells = size_t(std::stoull(std::string(argument.substr(8))));
    } else if (starts_with(argument, "--plates=")) {
      result.plates = size_t(std::stoull(std::string(argument.substr(9))));
    } else if (starts_with(argument, "--provinces=")) {
      result.provinces = size_t(std::stoull(std::string(argument.substr(12))));
    } else if (starts_with(argument, "--sea-zones=")) {
      result.sea_zones = size_t(std::stoull(std::string(argument.substr(12))));
    } else if (starts_with(argument, "--cultures=")) {
      result.cultures = size_t(std::stoull(std::string(argument.substr(11))));
    } else if (starts_with(argument, "--threads=")) {
      result.threads = size_t(std::stoull(std::string(argument.substr(10))));
    } else if (starts_with(argument, "--seed=")) {
      result.seed = std::stoull(std::string(argument.substr(7)));
    } else if (starts_with(argument, "--stats=")) {
      result.stats = std::string(argument.substr(8));
    } else if (starts_with(argument, "--dump=")) {
      result.dump = fs::path(std::string(argument.substr(7)));
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "GN02 planet generator lab\n"
                << "  --cells=N       клеток планеты (по умолчанию 65536)\n"
                << "  --plates=N      тектонических плит (24)\n"
                << "  --provinces=N   провинций суши (900)\n"
                << "  --sea-zones=N   морских зон (90)\n"
                << "  --cultures=N    культур (48)\n"
                << "  --seed=N        зерно мира\n"
                << "  --threads=N     рабочих потоков (0 = по числу ядер)\n"
                << "  --dump=PATH     записать пакет планеты\n"
                << "  --map           напечатать ASCII-карту климата\n"
                << "  --stats=A,B     напечатать min/max/среднее полей буфера cells\n"
                << "  --verify        прогнать контрактные проверки\n"
                << "  --quiet         без отчёта\n";
      std::exit(0);
    } else {
      utils::error{}("GN02: unknown argument '{}'", argument);
    }
  }
  return result;
}

originator::size_table make_sizes(const options& opts) {
  originator::size_table sizes;
  sizes.set("cell_count", opts.cells);
  sizes.set("cell_count_plus_one", opts.cells + 1);
  // Ёмкость под дуги соседства. Симметризованный граф K ближайших даёт в среднем чуть больше 2K
  // дуг на клетку у краевых случаев и ровно 2K в среднем; запас берётся кратным, а нехватка падает
  // громко с нужным числом в сообщении.
  sizes.set("arc_capacity", opts.cells * 10);

  sizes.set("plate_capacity", opts.plates * 2);
  sizes.set("plate_capacity_plus_one", opts.plates * 2 + 1);
  // Пуассоновский выбор затравок даёт немного БОЛЬШЕ цели: расстояние подобрано по площади, а не
  // по счёту. Поэтому ёмкость под области берётся с запасом, а не равной цели.
  sizes.set("province_capacity", opts.provinces * 2);
  sizes.set("province_capacity_plus_one", opts.provinces * 2 + 1);
  sizes.set("sea_capacity", opts.sea_zones * 3);
  sizes.set("sea_capacity_plus_one", opts.sea_zones * 3 + 1);
  sizes.set("culture_capacity", opts.cultures * 3);
  sizes.set("culture_capacity_plus_one", opts.cultures * 3 + 1);
  sizes.set("history_capacity", 4096);
  sizes.set("single", 1);
  return sizes;
}

originator::pipeline_description load_description(const options& opts) {
  const auto root = resource_root();

  originator::pipeline_description description;
  description.name = "gn02";
  description.values = originator::parse_values(read_file(root / "values.tavl"), "values.tavl");
  description.buffers = originator::parse_buffers(read_file(root / "buffers.tavl"), "buffers.tavl");
  description.steps = originator::parse_steps(read_file(root / "steps.tavl"), "steps.tavl");

  // Значения, известные только из командной строки. Всё остальное живёт в конфиге: числа мира не
  // должны быть спрятаны в C++, иначе автор мира не сможет его настроить.
  description.values.set_number("cell_count", double(opts.cells));
  description.values.set_number("plate_count", double(opts.plates));
  description.values.set_number("province_count", double(opts.provinces));
  description.values.set_number("sea_zone_count", double(opts.sea_zones));
  description.values.set_number("culture_count", double(opts.cultures));
  return description;
}

void load_bodies(originator::script_host& host, const originator::pipeline_description& description) {
  const auto root = resource_root();
  for (const auto& step : description.steps) {
    if (step.body.empty()) {
      utils::error{}("GN02: step '{}' has no body", step.name);
    }
    host.load_body(step.name, read_file(root / step.body), step.body);
    for (const auto& [name, path] : step.programs) {
      host.load_program(name, read_file(root / path));
    }
  }
}

std::vector<std::string> split_words(const std::string_view& text) {
  std::vector<std::string> result;
  size_t position = 0;
  while (position < text.size()) {
    while (position < text.size() && (text[position] == ' ' || text[position] == ',' || text[position] == '\n')) {
      ++position;
    }
    const size_t start = position;
    while (position < text.size() && text[position] != ' ' && text[position] != ',' && text[position] != '\n') {
      ++position;
    }
    if (position > start) {
      result.emplace_back(text.substr(start, position - start));
    }
  }
  return result;
}

std::vector<std::byte> snapshot(const originator::buffer& source) {
  std::vector<std::byte> bytes(source.byte_size());
  std::memcpy(bytes.data(), source.base_pointer(), bytes.size());
  return bytes;
}

std::vector<std::vector<std::byte>> snapshot_all(originator::pipeline& source) {
  std::vector<std::vector<std::byte>> result;
  result.reserve(source.buffer_count());
  for (size_t i = 0; i < source.buffer_count(); ++i) {
    result.push_back(snapshot(source.buffer_at(i)));
  }
  return result;
}

// Готовый мир: пайплайн плюс всё, что нужно, чтобы его расспросить.
struct world {
  std::unique_ptr<originator::pipeline> line;
  double milliseconds = 0.0;
  // Время по шагам. Генератор — не кадр: у него нет бюджета в миллисекундах, зато есть вопрос «где
  // время», и ответ на него меняет решения. Здесь он видно сразу: у планеты время уходит не в
  // поэлементную арифметику, а в последовательные заливки.
  std::vector<std::pair<std::string, double>> steps;
};

world generate(const options& opts, const originator::tool_registry& tools,
               const originator::pipeline_description& description, thread::atomic_pool* pool) {
  originator::script_host host(const_cast<originator::tool_registry&>(tools), pool);
  load_bodies(host, description);

  world result;
  result.line = std::make_unique<originator::pipeline>(description, make_sizes(opts), opts.seed);

  // Шаги исполняются по одному, чтобы каждый был измерен отдельно. Порядок и результат от этого не
  // меняются: run() делает ровно то же самое, просто без замера.
  const auto invoker = host.invoker();
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < result.line->step_count(); ++i) {
    const auto step_start = std::chrono::steady_clock::now();
    result.line->run_step(i, invoker);
    const auto step_stop = std::chrono::steady_clock::now();
    result.steps.emplace_back(result.line->step_at(i).name,
                              std::chrono::duration<double, std::milli>(step_stop - step_start).count());
  }
  const auto stop = std::chrono::steady_clock::now();
  result.milliseconds = std::chrono::duration<double, std::milli>(stop - start).count();
  return result;
}

double field_value(originator::pipeline& line, const std::string_view& buffer_name,
                   const std::string_view& field_name, const size_t element, const uint32_t component = 0) {
  auto* buffer = line.find_buffer(buffer_name);
  if (buffer == nullptr) {
    utils::error{}("GN02: no buffer '{}'", buffer_name);
  }
  const size_t index = buffer->find_field(field_name);
  if (index == originator::buffer_layout::npos) {
    utils::error{}("GN02: buffer '{}' has no field '{}'", buffer_name, field_name);
  }
  return buffer->field(index).get(element, component);
}

// Аксессор поля по именам: удобно для проверок, где важна ясность, а не скорость.
originator::const_field_accessor field_of(originator::pipeline& line, const std::string_view& buffer_name,
                                          const std::string_view& field_name) {
  auto* buffer = line.find_buffer(buffer_name);
  if (buffer == nullptr) {
    utils::error{}("GN02: no buffer '{}'", buffer_name);
  }
  const size_t index = buffer->find_field(field_name);
  if (index == originator::buffer_layout::npos) {
    utils::error{}("GN02: buffer '{}' has no field '{}'", buffer_name, field_name);
  }
  return buffer->field(index);
}

struct climate_names {
  const char* name;
  char symbol;
};

// Порядок совпадает с идентификаторами в scripts/climate_zone.ds: там правило, здесь только имена.
constexpr climate_names climate_table[] = {
  {"океан", '~'},      {"морской лёд", '*'}, {"ледник", '#'},   {"тундра", '-'},
  {"тайга", 'T'},      {"лес", 'F'},         {"степь", 's'},    {"пустыня", '.'},
  {"саванна", 'v'},    {"джунгли", 'J'},     {"высокогорье", '^'},
};

constexpr size_t climate_count = sizeof(climate_table) / sizeof(climate_table[0]);

void print_step_times(const world& value) {
  std::cout << "  время по шагам:\n";
  for (const auto& [name, milliseconds] : value.steps) {
    const double share = value.milliseconds <= 0.0 ? 0.0 : 100.0 * milliseconds / value.milliseconds;
    std::cout << "    " << std::format("{:<12}", name) << std::format("{:>9.1f}", milliseconds) << " мс  "
              << std::format("{:>5.1f}", share) << "%\n";
  }
}

void print_report(originator::pipeline& line, const options& opts, const double milliseconds) {
  const size_t count = opts.cells;

  std::cout << "GN02: " << count << " клеток, зерно " << opts.seed << ", " << std::format("{:.1f}", milliseconds)
            << " мс, память " << std::format("{:.1f}", double(line.total_byte_size()) / (1024.0 * 1024.0)) << " MB\n";

  const auto land = field_of(line, "cells", "land");
  const auto height = field_of(line, "cells", "height");
  const auto summer = field_of(line, "cells", "temperature_summer");
  const auto winter = field_of(line, "cells", "temperature_winter");
  const auto precipitation = field_of(line, "cells", "precipitation");
  const auto climate = field_of(line, "cells", "climate");
  const auto population = field_of(line, "cells", "population");
  const auto lat_sin = field_of(line, "cells", "lat_sin");

  size_t land_cells = 0;
  double highest = -1e9;
  double deepest = 1e9;
  double temperature_sum = 0.0;
  double land_precipitation = 0.0;
  double driest = 1e9;
  double wettest = -1e9;
  double population_sum = 0.0;
  std::array<size_t, climate_count> zones{};
  // Широтные полосы: то самое место, где видно связь суши с вращением — распределение суши по
  // широте не должно быть равномерным, если плиты дрейфуют поперёк широт.
  std::array<size_t, 6> land_by_latitude{};
  std::array<size_t, 6> cells_by_latitude{};

  for (size_t i = 0; i < count; ++i) {
    const bool is_land = land.get(i) != 0.0;
    land_cells += is_land ? 1 : 0;
    highest = std::max(highest, height.get(i));
    deepest = std::min(deepest, height.get(i));
    temperature_sum += 0.5 * (summer.get(i) + winter.get(i));
    population_sum += population.get(i);
    // Осадки считаются ПО СУШЕ: они и нормированы на среднее по суше, поэтому среднее по планете
    // (где океан всегда мокрый) не значит ничего.
    if (is_land) {
      land_precipitation += precipitation.get(i);
      driest = std::min(driest, precipitation.get(i));
      wettest = std::max(wettest, precipitation.get(i));
    }

    const auto zone = size_t(climate.get(i));
    if (zone < climate_count) {
      zones[zone] += 1;
    }

    const double latitude = std::asin(std::clamp(lat_sin.get(i), -1.0, 1.0)) * 180.0 / std::numbers::pi;
    const auto band = size_t(std::clamp((latitude + 90.0) / 30.0, 0.0, 5.999));
    cells_by_latitude[band] += 1;
    land_by_latitude[band] += is_land ? 1 : 0;
  }

  std::cout << "  суша               " << land_cells << " клеток ("
            << std::format("{:.1f}", 100.0 * double(land_cells) / double(count)) << "%)\n"
            << "  рельеф             " << std::format("{:.0f}", deepest) << " .. "
            << std::format("{:.0f}", highest) << " м\n"
            << "  средняя температура " << std::format("{:.1f}", temperature_sum / double(count)) << " °C\n"
            << "  осадки на суше     " << std::format("{:.2f}", land_cells == 0 ? 0.0 : driest) << " .. "
            << std::format("{:.2f}", land_cells == 0 ? 0.0 : wettest) << ", среднее "
            << std::format("{:.2f}", land_cells == 0 ? 0.0 : land_precipitation / double(land_cells))
            << " (в долях среднего по суше)\n"
            << "  население          " << std::format("{:.0f}", population_sum) << "\n";

  std::cout << "  плит               " << size_t(field_value(line, "state", "plate_count", 0)) << "\n"
            << "  провинций          " << size_t(field_value(line, "state", "province_count", 0)) << "\n"
            << "  морских зон        " << size_t(field_value(line, "state", "sea_zone_count", 0)) << "\n"
            << "  культур            " << size_t(field_value(line, "state", "culture_count", 0)) << "\n"
            << "  событий истории    " << size_t(field_value(line, "state", "event_count", 0)) << "\n";

  std::cout << "  климат:\n";
  for (size_t zone = 0; zone < climate_count; ++zone) {
    if (zones[zone] == 0) {
      continue;
    }
    std::cout << "    " << climate_table[zone].symbol << " " << climate_table[zone].name << " — " << zones[zone]
              << " (" << std::format("{:.1f}", 100.0 * double(zones[zone]) / double(count)) << "%)\n";
  }

  std::cout << "  суша по широте:\n";
  for (size_t band = 0; band < land_by_latitude.size(); ++band) {
    const double share = cells_by_latitude[band] == 0
                           ? 0.0
                           : 100.0 * double(land_by_latitude[band]) / double(cells_by_latitude[band]);
    std::cout << "    " << std::format("{:>4}", int(-90 + int(band) * 30)) << "…"
              << std::format("{:>4}", int(-60 + int(band) * 30)) << "°  " << std::format("{:.1f}", share) << "%\n";
  }
}

// Диапазон поля по именам. Существует потому, что headless-генератор отлаживается ЧИСЛАМИ: «население
// 17 миллионов при ёмкости в два» — это ответ, а «мир выглядит странно» — нет.
void print_field_stats(originator::pipeline& line, const options& opts, const std::string& names) {
  for (const auto& name : split_words(names)) {
    const auto field = field_of(line, "cells", name);
    double lowest = std::numeric_limits<double>::infinity();
    double highest = -std::numeric_limits<double>::infinity();
    double total = 0.0;
    for (size_t i = 0; i < opts.cells; ++i) {
      const double value = field.get(i);
      lowest = std::min(lowest, value);
      highest = std::max(highest, value);
      total += value;
    }
    std::cout << "  " << std::format("{:<20}", name) << std::format("{:>14.5g}", lowest) << " .. "
              << std::format("{:>14.5g}", highest) << "   среднее " << std::format("{:>12.5g}", total / double(opts.cells))
              << "\n";
  }
}

// ASCII-карта: единственная «картинка» headless-площадки. Существует не для красоты — по ней сразу
// видно то, что числами видно плохо: остались ли материки материками, легли ли пустыни в пояса, есть
// ли дождевая тень за горами.
void print_map(originator::pipeline& line, const options& opts) {
  constexpr size_t width = 128;
  constexpr size_t height_rows = 40;

  const auto positions = field_of(line, "cells", "position");
  const auto climate = field_of(line, "cells", "climate");

  utils::kd_tree<uint32_t, std::array<float, 3>, 3> tree;
  tree.reserve(opts.cells);
  for (size_t i = 0; i < opts.cells; ++i) {
    tree.insert(std::array<float, 3>{float(positions.get(i, 0)), float(positions.get(i, 1)),
                                     float(positions.get(i, 2))},
                uint32_t(i));
  }
  tree.build();

  std::cout << "\n";
  for (size_t row = 0; row < height_rows; ++row) {
    const double latitude = (0.5 - (double(row) + 0.5) / double(height_rows)) * std::numbers::pi;
    std::string line_text;
    line_text.reserve(width);
    for (size_t column = 0; column < width; ++column) {
      const double longitude = ((double(column) + 0.5) / double(width) - 0.5) * 2.0 * std::numbers::pi;
      const std::array<float, 3> query{float(std::cos(latitude) * std::cos(longitude)), float(std::sin(latitude)),
                                       float(std::cos(latitude) * std::sin(longitude))};
      const auto* node = tree.nearest(query, 0.5f, [](const uint32_t&) { return true; });
      if (node == nullptr) {
        line_text.push_back(' ');
        continue;
      }
      const auto zone = size_t(climate.get(node->payload));
      line_text.push_back(zone < climate_count ? climate_table[zone].symbol : '?');
    }
    std::cout << line_text << "\n";
  }
  std::cout << "\n";
}

// Связность метки по графу: все клетки с одной меткой обязаны образовывать ОДНУ область. Для
// провинции это не косметика: разорванная провинция ломает и отрисовку границы, и всякий геймплей,
// который считает соседство.
bool labels_are_connected(originator::pipeline& line, const std::string_view& label_field,
                          const size_t count, const size_t label_limit) {
  const auto offsets = field_of(line, "cell_offsets", "start");
  const auto arcs = field_of(line, "cell_arcs", "cell");
  const auto labels = field_of(line, "cells", label_field);

  std::vector<uint32_t> first_cell(label_limit + 1, uint32_t(-1));
  for (size_t i = 0; i < count; ++i) {
    const auto label = size_t(labels.get(i));
    if (label == 0 || label > label_limit) {
      continue;
    }
    if (first_cell[label] == uint32_t(-1)) {
      first_cell[label] = uint32_t(i);
    }
  }

  std::vector<uint8_t> seen(count, 0);
  std::vector<uint32_t> stack;
  size_t reachable = 0;
  size_t labelled = 0;

  for (size_t i = 0; i < count; ++i) {
    labelled += labels.get(i) != 0.0 ? 1 : 0;
  }

  for (size_t label = 1; label <= label_limit; ++label) {
    if (first_cell[label] == uint32_t(-1)) {
      continue;
    }
    stack.clear();
    stack.push_back(first_cell[label]);
    seen[first_cell[label]] = 1;
    while (!stack.empty()) {
      const auto cell = stack.back();
      stack.pop_back();
      ++reachable;
      const auto begin = size_t(offsets.get(cell));
      const auto end = size_t(offsets.get(size_t(cell) + 1));
      for (size_t k = begin; k < end; ++k) {
        const auto other = uint32_t(arcs.get(k));
        if (other < count && seen[other] == 0 && size_t(labels.get(other)) == label) {
          seen[other] = 1;
          stack.push_back(other);
        }
      }
    }
  }

  return reachable == labelled;
}

int run_verify(const options& opts) {
  std::cout << "GN02 verify: " << opts.cells << " клеток, зерно " << opts.seed << "\n";

  size_t checks = 0;
  size_t failures = 0;
  const auto check = [&](const bool condition, const std::string& label) {
    ++checks;
    if (!condition) {
      ++failures;
      std::cout << "  ПРОВАЛ: " << label << "\n";
    }
  };

  originator::tool_registry tools;
  tools.add_standard_tools(); // включает scatter-инструменты
  tools.add_graph_tools();
  originator::add_all_primitives(tools);
  gn02::add_planet_tools(tools);

  const auto description = load_description(opts);
  const size_t count = opts.cells;

  auto reference = generate(opts, tools, description, nullptr);
  auto& line = *reference.line;
  const auto serial_buffers = snapshot_all(line);

  // 1. Топология замкнутой поверхности.
  {
    const auto positions = field_of(line, "cells", "position");
    const auto offsets = field_of(line, "cell_offsets", "start");
    const auto arcs = field_of(line, "cell_arcs", "cell");

    bool unit_length = true;
    for (size_t i = 0; i < count; ++i) {
      const double x = positions.get(i, 0);
      const double y = positions.get(i, 1);
      const double z = positions.get(i, 2);
      unit_length = unit_length && std::abs(std::sqrt(x * x + y * y + z * z) - 1.0) < 1e-5;
    }
    check(unit_length, "все клетки лежат на единичной сфере");

    bool symmetric = true;
    bool degrees_sane = true;
    size_t total_arcs = 0;
    for (size_t i = 0; i < count && symmetric; ++i) {
      const auto begin = size_t(offsets.get(i));
      const auto end = size_t(offsets.get(i + 1));
      degrees_sane = degrees_sane && end >= begin + 3 && end <= begin + 24;
      total_arcs += end - begin;
      for (size_t k = begin; k < end; ++k) {
        const auto other = size_t(arcs.get(k));
        bool back = false;
        const auto other_begin = size_t(offsets.get(other));
        const auto other_end = size_t(offsets.get(other + 1));
        for (size_t j = other_begin; j < other_end; ++j) {
          back = back || size_t(arcs.get(j)) == i;
        }
        symmetric = symmetric && back && other != i;
      }
    }
    check(symmetric, "соседство симметрично и без петель");
    check(degrees_sane, "степень каждой клетки в разумных границах");
    check(total_arcs == size_t(offsets.get(count)), "смещения CSR сходятся с числом дуг");

    // Связность: одна заливка от одной клетки обязана накрыть планету целиком. Дырявый граф не
    // виден ни в одной локальной проверке, а ломает всё дальнейшее.
    std::vector<uint8_t> seen(count, 0);
    std::vector<uint32_t> stack{0};
    seen[0] = 1;
    size_t reached = 0;
    while (!stack.empty()) {
      const auto cell = stack.back();
      stack.pop_back();
      ++reached;
      const auto begin = size_t(offsets.get(cell));
      const auto end = size_t(offsets.get(size_t(cell) + 1));
      for (size_t k = begin; k < end; ++k) {
        const auto other = uint32_t(arcs.get(k));
        if (other < count && seen[other] == 0) {
          seen[other] = 1;
          stack.push_back(other);
        }
      }
    }
    check(reached == count, std::format("граф соседства связен ({} из {} клеток)", reached, count));
  }

  // 2. Тектоника: плита есть у каждой клетки, и суммы по плитам сходятся.
  {
    const auto plate = field_of(line, "cells", "plate");
    const auto plate_offsets = field_of(line, "plate_offsets", "start");
    const auto plate_count = size_t(field_value(line, "state", "plate_count", 0));

    bool assigned = true;
    for (size_t i = 0; i < count; ++i) {
      const auto value = size_t(plate.get(i));
      assigned = assigned && value >= 1 && value <= plate_count;
    }
    check(assigned, "каждая клетка принадлежит плите");
    check(plate_count >= 4, std::format("плит получилось достаточно ({})", plate_count));
    check(size_t(plate_offsets.get(plate_offsets.count() - 1)) == count,
          "раскладка клеток по плитам покрывает планету");
    check(labels_are_connected(line, "plate", count, plate_count), "каждая плита связна");
  }

  // 3. Поверхность: доля суши держится около заданной, рельеф не выродился.
  {
    const auto land = field_of(line, "cells", "land");
    const auto height = field_of(line, "cells", "height");
    const auto sea_level = field_value(line, "state", "sea_level", 0);

    size_t land_cells = 0;
    double lowest = 1e30;
    double highest = -1e30;
    bool mask_matches = true;
    for (size_t i = 0; i < count; ++i) {
      const bool is_land = land.get(i) != 0.0;
      land_cells += is_land ? 1 : 0;
      lowest = std::min(lowest, height.get(i));
      highest = std::max(highest, height.get(i));
      // Маска суши обязана согласоваться с уровнем моря с точностью до поправки на вращение: клетка
      // выше уровня моря плюс максимальная поправка обязана быть сушей.
      if (height.get(i) > sea_level + 2000.0) {
        mask_matches = mask_matches && is_land;
      }
      if (height.get(i) < sea_level - 2000.0) {
        mask_matches = mask_matches && !is_land;
      }
    }

    const double share = double(land_cells) / double(count);
    const double target = field_value(line, "state", "land_target", 0);
    check(std::abs(share - target) < 0.02,
          std::format("доля суши {:.3f} близка к заданной {:.3f}", share, target));
    check(mask_matches, "маска суши согласована с уровнем моря");
    check(highest > 3000.0 && lowest < -3000.0,
          std::format("рельеф имеет и горы, и глубины ({:.0f} .. {:.0f} м)", lowest, highest));
  }

  // 4. Климат: лето не холоднее зимы, осадки неотрицательны, дождевая тень существует.
  {
    const auto summer = field_of(line, "cells", "temperature_summer");
    const auto winter = field_of(line, "cells", "temperature_winter");
    const auto precipitation = field_of(line, "cells", "precipitation");
    const auto lat_sin = field_of(line, "cells", "lat_sin");
    const auto land = field_of(line, "cells", "land");

    bool ordered = true;
    bool finite = true;
    double equator_temperature = 0.0;
    size_t equator_cells = 0;
    double polar_temperature = 0.0;
    size_t polar_cells = 0;
    for (size_t i = 0; i < count; ++i) {
      ordered = ordered && summer.get(i) >= winter.get(i) - 1e-6;
      finite = finite && std::isfinite(summer.get(i)) && std::isfinite(winter.get(i)) &&
               precipitation.get(i) >= 0.0 && std::isfinite(precipitation.get(i));

      const double s = std::abs(lat_sin.get(i));
      if (s < 0.1) {
        equator_temperature += 0.5 * (summer.get(i) + winter.get(i));
        ++equator_cells;
      }
      if (s > 0.95) {
        polar_temperature += 0.5 * (summer.get(i) + winter.get(i));
        ++polar_cells;
      }
    }
    check(ordered, "лето нигде не холоднее зимы");
    check(finite, "температуры и осадки — конечные неотрицательные числа");
    check(equator_cells > 0 && polar_cells > 0 &&
            equator_temperature / double(equator_cells) > polar_temperature / double(polar_cells) + 20.0,
          "на экваторе теплее, чем у полюсов, минимум на 20 °C");

    // Континентальность: сезонная амплитуда внутри материка обязана быть больше, чем на воде. Это
    // проверка не формулы, а того, что расстояние до океана вообще доехало до температуры.
    double inland_amplitude = 0.0;
    size_t inland_cells = 0;
    double ocean_amplitude = 0.0;
    size_t ocean_cells = 0;
    const auto ocean_distance = field_of(line, "cells", "ocean_distance");
    for (size_t i = 0; i < count; ++i) {
      const double amplitude = summer.get(i) - winter.get(i);
      if (land.get(i) != 0.0 && ocean_distance.get(i) > 6.0) {
        inland_amplitude += amplitude;
        ++inland_cells;
      }
      if (land.get(i) == 0.0) {
        ocean_amplitude += amplitude;
        ++ocean_cells;
      }
    }
    check(inland_cells > 0 && ocean_cells > 0 &&
            inland_amplitude / double(inland_cells) > ocean_amplitude / double(ocean_cells) + 1.0,
          "внутри материка сезонная амплитуда больше, чем на воде");
  }

  // 5. Области: провинции покрывают сушу, морские зоны — воду, и те и другие связны.
  {
    const auto land = field_of(line, "cells", "land");
    const auto province = field_of(line, "cells", "province");
    const auto sea_zone = field_of(line, "cells", "sea_zone");
    const auto province_count = size_t(field_value(line, "state", "province_count", 0));
    const auto sea_zone_count = size_t(field_value(line, "state", "sea_zone_count", 0));

    bool land_covered = true;
    bool water_covered = true;
    bool disjoint = true;
    for (size_t i = 0; i < count; ++i) {
      const bool is_land = land.get(i) != 0.0;
      const bool has_province = province.get(i) != 0.0;
      const bool has_zone = sea_zone.get(i) != 0.0;
      land_covered = land_covered && (!is_land || has_province);
      water_covered = water_covered && (is_land || has_zone);
      disjoint = disjoint && !(has_province && has_zone);
    }
    check(land_covered, "каждая клетка суши лежит в провинции");
    check(water_covered, "каждая клетка воды лежит в морской зоне");
    check(disjoint, "провинции и морские зоны не пересекаются");
    check(province_count >= 8 && sea_zone_count >= 4,
          std::format("областей достаточно: {} провинций, {} морских зон", province_count, sea_zone_count));
    check(labels_are_connected(line, "province", count, province_count), "каждая провинция связна");
    check(labels_are_connected(line, "sea_zone", count, sea_zone_count), "каждая морская зона связна");
  }

  // 6. Люди: население стоит там, где пригодно, культуры покрывают обитаемую сушу.
  {
    const auto habitability = field_of(line, "cells", "habitability");
    const auto population = field_of(line, "cells", "population");
    const auto culture = field_of(line, "cells", "culture");
    const auto land = field_of(line, "cells", "land");
    const auto culture_count = size_t(field_value(line, "state", "culture_count", 0));

    bool water_empty = true;
    bool bounded = true;
    double habitable_population = 0.0;
    double barren_population = 0.0;
    size_t cultured = 0;
    size_t habitable_cells = 0;
    size_t barren_cells = 0;
    for (size_t i = 0; i < count; ++i) {
      const bool is_land = land.get(i) != 0.0;
      water_empty = water_empty && (is_land || population.get(i) <= 1e-6);
      bounded = bounded && habitability.get(i) >= 0.0 && habitability.get(i) <= 1.0 &&
                std::isfinite(population.get(i)) && population.get(i) >= 0.0;
      if (habitability.get(i) > 0.5) {
        habitable_population += population.get(i);
        ++habitable_cells;
      } else if (is_land) {
        barren_population += population.get(i);
        ++barren_cells;
      }
      cultured += culture.get(i) != 0.0 ? 1 : 0;
    }
    check(water_empty, "на воде никто не живёт");
    check(bounded, "пригодность в [0,1], население конечно и неотрицательно");
    // Сравниваются ПЛОТНОСТИ, а не суммы, и это исправление после провала на одном из зёрен: у сухой
    // планеты пригодных клеток мало, поэтому их суммарное население законно меньше, а плотность —
    // нет. Проверять надо то свойство, которое обязано держаться при любом мире.
    const double habitable_density = habitable_cells == 0 ? 0.0 : habitable_population / double(habitable_cells);
    const double barren_density = barren_cells == 0 ? 0.0 : barren_population / double(barren_cells);
    check(habitable_cells > 0 && habitable_density > barren_density,
          std::format("плотность населения в пригодных клетках выше ({:.3g} против {:.3g})",
                      habitable_density, barren_density));
    check(culture_count >= 4, std::format("культур получилось достаточно ({})", culture_count));
    check(cultured > 0, "культуры заняли обитаемую сушу");
    check(labels_are_connected(line, "culture", count, culture_count), "каждая культура связна");
    check(size_t(field_value(line, "state", "event_count", 0)) >= culture_count,
          "история записала как минимум основание каждой культуры");
  }

  // 7. Число потоков не влияет ни на один буфер.
  for (const size_t threads : {size_t(1), size_t(3), size_t(7)}) {
    thread::atomic_pool pool(threads);
    auto parallel = generate(opts, tools, description, &pool);
    check(snapshot_all(*parallel.line) == serial_buffers,
          std::format("параллельно == последовательно во всех буферах, потоков {}", threads));
  }

  // 8. Пакет: он и есть результат, поэтому круг «записали — прочитали — сошлось» обязателен.
  {
    const auto section_names = split_words(description.values.string("package"));
    check(!section_names.empty(), "конфиг перечисляет секции пакета");

    const auto written = gn02::build_package(line, section_names, opts.seed, count);
    const auto path = fs::temp_directory_path() / std::format("gn02_verify_{}.planet", opts.seed);
    gn02::write_package(written, path);
    const auto read_back = gn02::read_package(path);

    bool sections_match = read_back.sections.size() == written.sections.size();
    for (size_t i = 0; sections_match && i < written.sections.size(); ++i) {
      sections_match = written.sections[i].name == read_back.sections[i].name &&
                       written.sections[i].count == read_back.sections[i].count &&
                       written.sections[i].bytes == read_back.sections[i].bytes;
    }
    check(sections_match, "пакет читается обратно секция в секцию");
    check(read_back.fingerprint == written.fingerprint && read_back.seed == opts.seed,
          "отпечаток и зерно пакета совпадают");

    // Тот же мир из того же зерна обязан дать тот же отпечаток, а другое зерно — другой. Первое
    // проверяет воспроизводимость, второе — что зерно вообще доехало до формул.
    auto twin = generate(opts, tools, description, nullptr);
    const auto twin_package = gn02::build_package(*twin.line, section_names, opts.seed, count);
    check(twin_package.fingerprint == written.fingerprint, "то же зерно даёт тот же отпечаток");

    auto other_options = opts;
    other_options.seed = opts.seed + 1;
    auto other = generate(other_options, tools, load_description(other_options), nullptr);
    const auto other_package = gn02::build_package(*other.line, section_names, other_options.seed, count);
    check(other_package.fingerprint != written.fingerprint, "другое зерно даёт другой отпечаток");

    fs::remove(path);
  }

  std::cout << "GN02 verify: " << (checks - failures) << "/" << checks << "\n";
  return failures == 0 ? 0 : 1;
}

int run_once(const options& opts) {
  originator::tool_registry tools;
  tools.add_standard_tools(); // включает scatter-инструменты
  tools.add_graph_tools();
  originator::add_all_primitives(tools);
  gn02::add_planet_tools(tools);

  const auto description = load_description(opts);

  const size_t threads = opts.threads == 0
                           ? std::max<size_t>(std::thread::hardware_concurrency(), 1) - 1
                           : opts.threads;
  thread::atomic_pool pool(threads);

  auto result = generate(opts, tools, description, threads == 0 ? nullptr : &pool);

  if (opts.report) {
    print_report(*result.line, opts, result.milliseconds);
    print_step_times(result);
  }
  if (!opts.stats.empty()) {
    print_field_stats(*result.line, opts, opts.stats);
  }
  if (opts.map) {
    print_map(*result.line, opts);
  }

  if (!opts.dump.empty()) {
    const auto section_names = split_words(description.values.string("package"));
    const auto value = gn02::build_package(*result.line, section_names, opts.seed, opts.cells);
    gn02::write_package(value, opts.dump);
    if (!opts.quiet) {
      std::cout << "  пакет             " << opts.dump.string() << ", отпечаток "
                << std::format("{:#018x}", value.fingerprint) << "\n";
    }
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
    return run_once(opts);
  } catch (const std::exception& error) {
    std::cerr << "GN02: " << error.what() << "\n";
    return 1;
  }
}
