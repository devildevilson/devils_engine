#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <tuple>
#include <string>
#include <string_view>
#include <vector>

#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/originator/generator_resource.h"
#include "devils_engine/originator/pipeline.h"
#include "devils_engine/originator/primitives.h"
#include "devils_engine/originator/script_host.h"
#include "devils_engine/originator/tools.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"

#include "memory.h"
#include "skeleton.h"
#include "streaming.h"
#include "viewer.h"

// GN03 — потоковая генерация окружающей геометрии.
//
// Вопрос площадки: как генератор отдаёт ГЕОМЕТРИЮ и как она собирается вокруг движущегося
// наблюдателя, оставаясь воспроизводимой. У GN02 результат был поле на клетках, то есть данные
// известной длины; здесь результат — треугольники, и сколько их будет, до исполнения не знает никто.
//
// Площадка headless по сути: всё, что она доказывает, проверяется числами (`--verify`). Окно
// существует потому, что у объёма есть один вопрос, на который числа отвечают плохо: «нет ли шва».
// Шов шириной в один треугольник — это ноль в отчёте и ясно видимая полоса на экране.

namespace fs = std::filesystem;
using namespace devils_engine;

namespace {

struct options {
  uint64_t seed = 1;
  int64_t radius = 4;
  int64_t vertical_radius = 2;
  size_t threads = 0; // 0 = по числу ядер минус один
  size_t arena_vertices = 6u << 20;
  bool verify = false;
  bool validation = false;
  bool uncapped = false;
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t frames = 0;
  size_t view_mode = 0;
  bool grid = false;
  std::string dump_path;
  // Файл каркаса. Грубый проход считается один раз, поэтому его результат естественно лежит пакетом:
  // есть файл — читаем, нет — считаем и пишем.
  std::string skeleton_path;
  // Файл памяти мира. Это и есть сохранение: остальное — функция от зерна, поэтому сохранять больше
  // нечего. Пустой путь означает «помнить только в этом запуске».
  std::string memory_path;
  std::vector<std::pair<std::string, double>> overrides;
  // Чанк, на котором идёт проверка. Ключ существует потому, что «где именно» у объёма имеет
  // значение: у поля шума есть места, где оно почти постоянно, и проверка, ставшая на такое место,
  // отчитывается о пустом мире, ничего при этом не сломав.
  originator::chunk_key probe{2, 0, -1};
  // С какого чанка начинает камера. Ключ существует ради проверки плавающего начала координат: мир
  // должен рисоваться одинаково и рядом с нулём, и в миллионе чанков от него, а нажать кнопку в
  // автоматическом прогоне некому.
  originator::chunk_key start{0, 2, 0};
  // Посадить камеру НА МАРШРУТ каркаса: коридор это узкая труба, и увидеть её можно только изнутри.
  // Ключ принимает номер точки маршрута, потому что «начало» у маршрута одно, а посмотреть хочется в
  // разных местах.
  bool on_route = false;
  size_t route_index = 0;
};

fs::path resource_root() {
  return fs::path(GN03_RESOURCE_ROOT);
}

// Генератор приезжает через demiurg — тем же путём, каким приехал бы из мода игрока. Хост знает
// ровно ОДНО имя, `generator/volume`; всё остальное называет сама точка входа.
struct generator_registry {
  demiurg::module_system modules;
  demiurg::resource_system resources;
  originator::generator_config config;
  // ДВА ГЕНЕРАТОРА, а не один с ветвлением: грубый проход и чанковый — разные пайплайны с разными
  // буферами и разной ценой, и объединять их значило бы держать в одном описании два масштаба.
  originator::generator_config skeleton_config;

  generator_registry() : modules(resource_root().generic_string() + "/") {
    modules.load_modules({demiurg::module_system::list_entry{"gn03/", "", ""}});
    originator::register_generator_resources(resources);
    resources.parse_resources(&modules);
    config = originator::load_generator(resources, "generator/volume");
    skeleton_config = originator::load_generator(resources, "generator/skeleton");
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
    const std::string_view argument(argv[i]);
    const auto value_of = [&](const std::string_view prefix) { return std::string(argument.substr(prefix.size())); };

    if (argument == "--verify") {
      result.verify = true;
    } else if (argument == "--validation") {
      result.validation = true;
    } else if (argument == "--uncapped") {
      result.uncapped = true;
    } else if (starts_with(argument, "--seed=")) {
      result.seed = std::stoull(value_of("--seed="));
    } else if (starts_with(argument, "--radius=")) {
      result.radius = std::stoll(value_of("--radius="));
    } else if (starts_with(argument, "--vertical=")) {
      // РАДИУС в чанках, а не пара границ. Мир генерируется в любом направлении от наблюдателя:
      // выше уровня земли он не кончается (там висят острова), ниже идут пещеры. Отдельный радиус
      // от горизонтального — не принцип, а цена: слой вверх стоит столько же, сколько кольцо в
      // стороны, а смотрят чаще вдоль, чем вверх.
      result.vertical_radius = std::stoll(value_of("--vertical="));
    } else if (starts_with(argument, "--threads=")) {
      result.threads = size_t(std::stoull(value_of("--threads=")));
    } else if (starts_with(argument, "--arena=")) {
      result.arena_vertices = size_t(std::stoull(value_of("--arena=")));
    } else if (starts_with(argument, "--width=")) {
      result.width = uint32_t(std::stoul(value_of("--width=")));
    } else if (starts_with(argument, "--height=")) {
      result.height = uint32_t(std::stoul(value_of("--height=")));
    } else if (starts_with(argument, "--frames=")) {
      result.frames = uint32_t(std::stoul(value_of("--frames=")));
    } else if (argument == "--grid") {
      result.grid = true;
    } else if (starts_with(argument, "--view=")) {
      // Режим представления ключом, а не только кнопкой: снимок отладочного вида нужен и в
      // автоматическом прогоне, где кнопку нажать некому.
      result.view_mode = size_t(std::stoull(value_of("--view=")));
    } else if (starts_with(argument, "--start=")) {
      const std::string text = value_of("--start=");
      const auto first = text.find(':');
      const auto second = text.rfind(':');
      if (first == std::string::npos || first == second) {
        utils::error{}("GN03: --start expects x:y:z, got '{}'", text);
      }
      result.start.x = std::stoll(text.substr(0, first));
      result.start.y = std::stoll(text.substr(first + 1, second - first - 1));
      result.start.z = std::stoll(text.substr(second + 1));
    } else if (starts_with(argument, "--probe=")) {
      const std::string text = value_of("--probe=");
      const auto first = text.find(':');
      const auto second = text.rfind(':');
      if (first == std::string::npos || first == second) {
        utils::error{}("GN03: --probe expects x:y:z, got '{}'", text);
      }
      result.probe.x = std::stoll(text.substr(0, first));
      result.probe.y = std::stoll(text.substr(first + 1, second - first - 1));
      result.probe.z = std::stoll(text.substr(second + 1));
    } else if (argument == "--on-route") {
      result.on_route = true;
    } else if (starts_with(argument, "--on-route=")) {
      result.on_route = true;
      result.route_index = size_t(std::stoull(value_of("--on-route=")));
    } else if (starts_with(argument, "--skeleton=")) {
      result.skeleton_path = value_of("--skeleton=");
    } else if (starts_with(argument, "--memory=")) {
      result.memory_path = value_of("--memory=");
    } else if (starts_with(argument, "--dump=")) {
      result.dump_path = value_of("--dump=");
    } else if (starts_with(argument, "--set=")) {
      // Значение генератора из командной строки: имя=число. Границы всё равно проверит диапазон из
      // конфига, поэтому окно и ключ не могут задать разное.
      const std::string text = value_of("--set=");
      const auto equals = text.find('=');
      if (equals == std::string::npos) {
        utils::error{}("GN03: --set expects name=value, got '{}'", text);
      }
      result.overrides.emplace_back(text.substr(0, equals), std::stod(text.substr(equals + 1)));
    } else {
      utils::error{}("GN03: unknown argument '{}'", argument);
    }
  }
  return result;
}

size_t worker_count(const options& opts) {
  if (opts.threads != 0) {
    return opts.threads;
  }
  const size_t hardware = std::max<size_t>(std::thread::hardware_concurrency(), 2);
  // Один поток остаётся кадру: генерация — фоновая работа, и отдать ей все ядра означает получить
  // рывки там, где как раз проверяется плавность.
  return hardware - 1;
}

// Описание пайплайна с наложенными значениями. Значение, у которого объявлен диапазон, зажимается по
// нему: окно и ключ командной строки не могут задать разное, потому что границы объявлены рядом с
// самим числом, в конфиге генератора.
originator::pipeline_description make_description(const std::vector<std::pair<std::string, double>>& overrides) {
  auto description = generator().config.description;
  const auto& ranges = generator().config.ranges;

  for (const auto& [name, raw] : overrides) {
    if (!description.values.has(name)) {
      utils::error{}("GN03: the generator has no value '{}'", name);
    }
    double value = raw;
    for (const auto& range : ranges) {
      if (range.name == name) {
        value = range.clamp(raw);
        if (value != raw) {
          utils::warn("GN03: '{}' clamped from {} to {} by the declared range", name, raw, value);
        }
        break;
      }
    }
    description.values.set_number(name, value);
  }
  return description;
}

struct chunk_geometry_sizes {
  size_t chunk_cells = 0;
  size_t side = 0; // отсчётов по оси: клетки плюс угол плюс полоса перекрытия
  double cell_size = 1.0;
  size_t vertex_capacity = 0;
  size_t prop_capacity = 0;
  size_t route_capacity = 0;
  size_t route_chain_capacity = 0;
};

chunk_geometry_sizes read_sizes(const originator::pipeline_description& description) {
  chunk_geometry_sizes sizes;
  sizes.chunk_cells = size_t(description.values.integer("chunk_cells", 0));
  sizes.cell_size = description.values.number("cell_size", 0.0);
  sizes.vertex_capacity = size_t(description.values.integer("vertex_capacity", 0));
  sizes.prop_capacity = size_t(description.values.integer("prop_capacity", 0));
  sizes.route_capacity = size_t(description.values.integer("chunk_route_capacity", 0));
  sizes.route_chain_capacity = size_t(description.values.integer("chunk_route_chain_capacity", 0));
  if (sizes.chunk_cells == 0 || sizes.cell_size <= 0.0 || sizes.vertex_capacity == 0 ||
      sizes.prop_capacity == 0) {
    utils::error{}("GN03: the generator must declare chunk_cells, cell_size, vertex_capacity and prop_capacity");
  }
  sizes.side = sizes.chunk_cells + 3;
  return sizes;
}

originator::size_table make_size_table(const chunk_geometry_sizes& sizes) {
  originator::size_table table;
  table.set("sample_count", sizes.side * sizes.side * sizes.side);
  table.set("vertex_capacity", sizes.vertex_capacity);
  table.set("prop_capacity", sizes.prop_capacity);
  table.set("chunk_route_capacity", sizes.route_capacity);
  table.set("chunk_route_chain_capacity", sizes.route_chain_capacity);
  table.set("state_count", 1);
  return table;
}

// Один рабочий генератор: пайплайн, хост скриптов и перевод результата в вершины устройства.
//
// Живёт он в ОДНОМ потоке от начала до конца, поэтому ни пайплайн, ни sol::state не защищены
// ничем — и не должны быть. Смена ключа чанка буферы не перевыделяет, значит поток считает тысячи
// чанков без единой аллокации внутри пайплайна.
class chunk_worker {
public:
  chunk_worker(const originator::pipeline_description& description, const originator::size_table& sizes,
               const uint64_t seed, const gn03::world_skeleton* skeleton = nullptr) : skeleton_(skeleton) {
    tools_.add_standard_tools();
    tools_.add_volume_tools();
    originator::add_all_primitives(tools_);

    host_ = std::make_unique<originator::script_host>(tools_, nullptr);
    const auto& package = generator().config;
    for (const auto& step : description.steps) {
      if (step.body.empty()) {
        utils::error{}("GN03: step '{}' has no body", step.name);
      }
      host_->load_body(step.name, package.source(step.body), step.body);
      for (const auto& [name, id] : step.programs) {
        host_->load_program(name, package.source(id));
      }
    }

    span_ = double(description.values.integer("chunk_cells", 1)) * description.values.number("cell_size", 1.0);
    cells_ = size_t(description.values.integer("chunk_cells", 1));
    cell_ = description.values.number("cell_size", 1.0);
    line_ = std::make_unique<originator::pipeline>(description, sizes, seed);
    vertices_field_ = line_->find_buffer("vertices");
    props_field_ = line_->find_buffer("props");
    state_field_ = line_->find_buffer("state");
    if (vertices_field_ == nullptr || props_field_ == nullptr || state_field_ == nullptr) {
      utils::error{}("GN03: the generator must declare the 'vertices', 'props' and 'state' buffers");
    }
  }

  void generate(const originator::chunk_key& key, gn03::chunk_mesh& mesh) {
    line_->set_chunk(key);
    fill_route(key);
    const auto invoker = host_->invoker();

    // Шаги идут по одному ради ЗАМЕРА: поле и поверхность стоят разного, и знать, сколько стоит
    // каждое, надо не из любопытства — от этого зависит, что вообще имеет смысл ускорять.
    for (size_t i = 0; i < line_->step_count(); ++i) {
      const auto start = std::chrono::steady_clock::now();
      line_->run_step(i, invoker);
      const double milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      if (i == 0) {
        mesh.field_milliseconds = milliseconds;
      } else {
        mesh.surface_milliseconds += milliseconds;
      }
    }

    const size_t count = size_t(state_field_->field(state_field_->find_field("vertex_count")).get(0));
    const auto position = vertices_field_->field(vertices_field_->find_field("position"));
    const auto normal = vertices_field_->field(vertices_field_->find_field("normal"));

    mesh.vertices.resize(count);
    for (size_t i = 0; i < count; ++i) {
      auto& vertex = mesh.vertices[i];
      for (uint32_t axis = 0; axis < 3; ++axis) {
        // Позиция ЛОКАЛЬНАЯ (инструмент получил решётку от начала чанка) и уезжает в фиксированную
        // точку: шаг решётки `span / 65535` это полмиллиметра на чанке в 32 метра, то есть точнее
        // float32 на любом удалении от начала мира — и, в отличие от него, точность не меняется от
        // того, как далеко улетел наблюдатель.
        vertex.position[axis] = gn03::encode_local_position(position.get(i, axis), span_);
        // Нормаль единичная, поэтому знаковый байт даёт шаг 0.45 градуса — глазу этого достаточно с
        // запасом, а вершина остаётся двенадцатибайтной.
        const double component = std::clamp(normal.get(i, axis), -1.0, 1.0);
        vertex.normal[axis] = int8_t(std::lround(component * 127.0));
      }
      // Слот чанка проставит арена при вставке: до неё геометрия своего слота не знает.
      vertex.chunk = 0;
      vertex.padding = 0;
    }

    // Сущности. Их немного, поэтому они переносятся как есть, без упаковки: приводить десяток вех к
    // системе чанка камеры дешевле каждый кадр, чем заводить им вторую арену со слотами.
    const size_t prop_count = size_t(state_field_->field(state_field_->find_field("prop_count")).get(0));
    const auto prop_position = props_field_->field(props_field_->find_field("position"));
    const auto prop_normal = props_field_->field(props_field_->find_field("normal"));
    const auto prop_size = props_field_->field(props_field_->find_field("size"));
    const auto prop_kind = props_field_->field(props_field_->find_field("kind"));
    const auto prop_origin = props_field_->field(props_field_->find_field("origin"));

    mesh.props.resize(prop_count);
    for (size_t i = 0; i < prop_count; ++i) {
      auto& prop = mesh.props[i];
      for (uint32_t axis = 0; axis < 3; ++axis) {
        prop.position[axis] = prop_position.get(i, axis);
        prop.normal[axis] = prop_normal.get(i, axis);
      }
      prop.size = prop_size.get(i);
      prop.kind = uint32_t(prop_kind.get(i));
      prop.origin = uint32_t(prop_origin.get(i));
    }
  }

  originator::pipeline& line() noexcept { return *line_; }

private:
  // ВХОД ПАЙПЛАЙНА заполняется здесь, до первого шага, и это единственное место, которое знает про
  // два масштаба сразу: генератор чанка про каркас не знает вовсе (он читает обычный буфер), а
  // каркас не знает про чанки.
  //
  // Область запроса — решётка отсчётов, а не клетки: поле считается на всей решётке, включая полосу
  // перекрытия, и отрезок, влияющий на её крайний узел, обязан попасть в запрос. Радиус влияния
  // прибавляет сам каркас.
  void fill_route(const originator::chunk_key& key) {
    auto* points = line_->find_buffer("route_points");
    auto* offsets = line_->find_buffer("route_offsets");
    if (points == nullptr || offsets == nullptr) {
      utils::error{}("GN03: the generator must declare the 'route_points' and 'route_offsets' inputs");
    }

    auto point_field = points->field(points->find_field("position"));
    auto offset_field = offsets->field(offsets->find_field("offset"));

    // Пустой маршрут — это ОДНА пустая цепочка во всех смещениях: у пустой цепочки нет отрезков,
    // поэтому инструмент честно вернёт предел, а не прочитает мусор.
    const auto clear_offsets = [&offset_field, offsets](const size_t value) {
      for (size_t i = 0; i < offsets->count(); ++i) {
        offset_field.set(i, double(value));
      }
    };

    if (skeleton_ == nullptr || skeleton_->empty()) {
      clear_offsets(0);
      return;
    }

    const std::array<double, 3> low{double(key.x * int64_t(cells_) - 1) * cell_,
                                    double(key.y * int64_t(cells_) - 1) * cell_,
                                    double(key.z * int64_t(cells_) - 1) * cell_};
    const std::array<double, 3> high{double(key.x * int64_t(cells_) + int64_t(cells_) + 1) * cell_,
                                     double(key.y * int64_t(cells_) + int64_t(cells_) + 1) * cell_,
                                     double(key.z * int64_t(cells_) + int64_t(cells_) + 1) * cell_};

    if (!skeleton_->query(low, high, points->count(), offsets->count(), route_)) {
      // ОТКАЗ, А НЕ ОБРЕЗКА. Обрезанный маршрут даёт коридор, кончающийся в середине горы, и найти
      // причину по картинке нельзя; а неполный вход означает другой мир при том же ключе.
      utils::error{}("GN03: the skeleton route in chunk ({}, {}, {}) does not fit the declared meta capacity "
                     "({} points, {} chains) — raise chunk_route_capacity",
                     key.x, key.y, key.z, points->count(), offsets->count());
    }

    for (size_t i = 0; i < route_.points.size(); ++i) {
      for (uint32_t axis = 0; axis < 3; ++axis) {
        point_field.set(i, route_.points[i][axis], axis);
      }
    }
    clear_offsets(route_.points.size());
    for (size_t i = 0; i < route_.offsets.size(); ++i) {
      offset_field.set(i, double(route_.offsets[i]));
    }
  }

  originator::tool_registry tools_;
  std::unique_ptr<originator::script_host> host_;
  std::unique_ptr<originator::pipeline> line_;
  originator::buffer* vertices_field_ = nullptr;
  originator::buffer* props_field_ = nullptr;
  originator::buffer* state_field_ = nullptr;
  double span_ = 1.0; // размер чанка в мире: шаг фиксированной точки выводится из него
  size_t cells_ = 1;
  double cell_ = 1.0;
  const gn03::world_skeleton* skeleton_ = nullptr;
  gn03::world_skeleton::query_result route_;
};

// ГРУБЫЙ ПРОХОД. Считается один раз, не знает про чанки и покрывает область много больше видимого
// мира. Отдельный пайплайн со своими буферами: объединять два масштаба в одно описание значило бы
// держать в нём и сорок узлов, и сорок три тысячи отсчётов.
gn03::world_skeleton build_skeleton(const std::vector<std::pair<std::string, double>>& overrides,
                                    const uint64_t seed) {
  auto description = generator().skeleton_config.description;
  for (const auto& [name, value] : overrides) {
    if (description.values.has(name)) {
      description.values.set_number(name, value);
    }
  }

  originator::size_table sizes;
  sizes.set("node_capacity", size_t(description.values.integer("node_capacity", 0)));
  sizes.set("route_capacity", size_t(description.values.integer("route_capacity", 0)));
  sizes.set("route_chain_capacity", size_t(description.values.integer("route_chain_capacity", 0)));
  sizes.set("state_count", 1);

  originator::tool_registry tools;
  tools.add_standard_tools();
  tools.add_volume_tools();
  originator::add_all_primitives(tools);

  originator::script_host host(tools, nullptr);
  const auto& package = generator().skeleton_config;
  for (const auto& step : description.steps) {
    host.load_body(step.name, package.source(step.body), step.body);
    for (const auto& [name, id] : step.programs) {
      host.load_program(name, package.source(id));
    }
  }

  originator::pipeline line(description, sizes, seed);
  const auto start = std::chrono::steady_clock::now();
  line.run(host.invoker());
  const double milliseconds =
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

  auto* nodes = line.find_buffer("nodes");
  auto* points = line.find_buffer("route_points");
  auto* offsets = line.find_buffer("route_offsets");
  auto* state = line.find_buffer("skeleton_state");
  if (nodes == nullptr || points == nullptr || offsets == nullptr || state == nullptr) {
    utils::error{}("GN03: the skeleton generator must declare 'nodes', 'route_points', 'route_offsets' and "
                   "'skeleton_state'");
  }

  const size_t node_count = size_t(state->field(state->find_field("node_count")).get(0));
  const size_t point_count = size_t(state->field(state->find_field("point_count")).get(0));
  const size_t chain_count = size_t(state->field(state->find_field("chain_count")).get(0));

  std::vector<gn03::skeleton_node> node_list(node_count);
  const auto node_position = nodes->field(nodes->find_field("position"));
  const auto node_kind = nodes->field(nodes->find_field("kind"));
  for (size_t i = 0; i < node_count; ++i) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
      node_list[i].position[axis] = node_position.get(i, axis);
    }
    node_list[i].kind = uint32_t(node_kind.get(i));
  }

  std::vector<std::array<double, 3>> point_list(point_count);
  const auto point_position = points->field(points->find_field("position"));
  for (size_t i = 0; i < point_count; ++i) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
      point_list[i][axis] = point_position.get(i, axis);
    }
  }

  std::vector<uint32_t> offset_list(chain_count + 1);
  const auto offset_field = offsets->field(offsets->find_field("offset"));
  for (size_t i = 0; i < offset_list.size(); ++i) {
    offset_list[i] = uint32_t(offset_field.get(i));
  }

  gn03::world_skeleton::description about;
  about.seed = seed;
  about.world_span = description.values.number("world_span", 0.0);
  // РАДИУС ВЛИЯНИЯ объявляет каркас, а не читатель: за радиусом коридора плюс спад поле уже не
  // меняется, значит спрашивать дальше незачем — но и ближе нельзя, иначе отрезок, гнущий поле внутри
  // чанка, в запрос не попадёт.
  about.influence = description.values.number("corridor_radius", 0.0) +
                    description.values.number("corridor_falloff", 0.0);

  gn03::world_skeleton skeleton;
  skeleton.build(about, std::move(node_list), std::move(point_list), std::move(offset_list));

  utils::info("GN03 skeleton: {} nodes and {} route points over {} m in {:.1f} ms (influence {} m)", node_count,
              point_count, about.world_span, milliseconds, about.influence);
  return skeleton;
}

gn03::generator_factory make_factory(const originator::pipeline_description& description,
                                     const originator::size_table& sizes, const uint64_t seed,
                                     const gn03::world_skeleton* skeleton) {
  // Каркас отдаётся рабочим потокам ПО УКАЗАТЕЛЮ и только на чтение: после сборки он неизменен,
  // поэтому запрос по области — чистая функция, и делить его между потоками можно без всякой защиты.
  return [description, sizes, seed, skeleton]() -> gn03::chunk_generator {
    auto worker = std::make_shared<chunk_worker>(description, sizes, seed, skeleton);
    return [worker](const originator::chunk_key& key, gn03::chunk_mesh& mesh) { worker->generate(key, mesh); };
  };
}

// ---------------------------------------------------------------------------------------------
// Проверки. Все они — свойства, а не эталонные снимки: смена хеша или правила поля сдвигает КАЖДОЕ
// число мира, а свойства обязаны держаться (этот довод уже был оплачен на GN02).

struct verification {
  size_t passed = 0;
  size_t total = 0;

  void check(const bool condition, const std::string& description) {
    ++total;
    if (condition) {
      ++passed;
      utils::info("GN03   ok   {}", description);
      return;
    }
    utils::warn("GN03   FAIL {}", description);
  }
};

using vertex_list = std::vector<gn03::gpu_vertex>;

bool same_geometry(const vertex_list& a, const vertex_list& b) {
  if (a.size() != b.size()) {
    return false;
  }
  return std::memcmp(a.data(), b.data(), a.size() * sizeof(gn03::gpu_vertex)) == 0;
}

std::vector<gn03::chunk_prop> props_of_twin(chunk_worker& twin, const originator::chunk_key& key) {
  gn03::chunk_mesh mesh;
  twin.generate(key, mesh);
  return mesh.props;
}

vertex_list generate_one(chunk_worker& worker, const originator::chunk_key& key) {
  gn03::chunk_mesh mesh;
  worker.generate(key, mesh);
  return mesh.vertices;
}

// Ребро поверхности как пара вершин, приведённая к порядку. Замкнутость считается по НАПРАВЛЕННЫМ
// рёбрам: у щели ребро проходят один раз, у касания двух листов — по два в каждую сторону.
using point = std::array<double, 3>;
using edge = std::pair<point, point>;

// Вершина лежит в координатах СВОЕГО чанка, поэтому мировая точка собирается из ключа и локальной
// позиции. Сложение точное: ключ целый, а `span` умножается на целое, поэтому вершина на общей грани
// двух чанков даёт у обоих ОДНО И ТО ЖЕ число, и проверка шва остаётся точной, а не «с допуском».
point point_of(const gn03::gpu_vertex& vertex, const originator::chunk_key& key, const double span) {
  const std::array<int64_t, 3> origin{key.x, key.y, key.z};
  point result{};
  for (uint32_t axis = 0; axis < 3; ++axis) {
    result[axis] = double(origin[axis]) * span + gn03::decode_local_position(vertex.position[axis], span);
  }
  return result;
}

int main_verify(const options& opts) {
  const auto description = make_description(opts.overrides);
  const auto sizes = read_sizes(description);
  const auto table = make_size_table(sizes);

  utils::info("GN03 verify: chunk {} cells of {} m, {} samples per chunk, capacity {} vertices",
              sizes.chunk_cells, sizes.cell_size, sizes.side * sizes.side * sizes.side, sizes.vertex_capacity);

  // Каркас в проверке строится ТОТ ЖЕ, что и в окне: иначе проверялся бы другой мир.
  const auto skeleton = build_skeleton(opts.overrides, opts.seed);
  chunk_worker worker(description, table, opts.seed, &skeleton);
  verification checks;

  // 1. Правило независимости чанков: результат зависит только от (зерно, ключ).
  const originator::chunk_key probe = opts.probe;
  const auto first_pass = generate_one(worker, probe);
  checks.check(!first_pass.empty(), "the probe chunk has geometry at all");

  // ДИАПАЗОНЫ ПОЛЕЙ пробного чанка. Печатаются всегда, а не при отказе: пустой чанк и чанк, у
  // которого поле не сложилось, снаружи выглядят одинаково — ноль вершин, — а различает их только
  // то, проходит ли плотность через нуль вообще.
  {
    auto* samples = worker.line().find_buffer("samples");
    if (samples == nullptr) {
      utils::error{}("GN03: the generator must declare the 'samples' buffer");
    }
    for (const auto& field : samples->layout().fields) {
      const auto accessor = samples->field(samples->find_field(field.name));
      const uint32_t components = accessor.type().components;
      for (uint32_t component = 0; component < components; ++component) {
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < samples->count(); ++i) {
          const double value = accessor.get(i, component);
          low = std::min(low, value);
          high = std::max(high, value);
        }
        utils::info("GN03 verify: field {}[{}] in [{:.3f} .. {:.3f}]", field.name, component, low, high);
      }
    }
  }

  // Тот же чанк после других чанков в том же пайплайне.
  generate_one(worker, originator::chunk_key{-3, 1, 4});
  generate_one(worker, originator::chunk_key{7, -2, 2});
  const auto after_others = generate_one(worker, probe);
  checks.check(same_geometry(first_pass, after_others),
               "the same chunk is bit-identical after other chunks ran in the same pipeline");

  // И с обнулёнными буферами: если результат меняется, значит какой-то шаг читает то, чего не писал.
  worker.line().clear_buffers();
  const auto after_clear = generate_one(worker, probe);
  checks.check(same_geometry(first_pass, after_clear), "clearing every buffer changes nothing in the result");

  // Второй пайплайн с тем же зерном — то же самое.
  {
    chunk_worker twin(description, table, opts.seed, &skeleton);
    checks.check(same_geometry(first_pass, generate_one(twin, probe)),
                 "a second pipeline with the same seed gives the same chunk");
  }

  // Другое зерно — другой мир. Проверка того, что зерно вообще доходит до поля.
  {
    // Другое зерно — другой мир И другой каркас: маршрут тоже функция зерна.
    const auto other_skeleton = build_skeleton(opts.overrides, opts.seed + 1);
    chunk_worker other(description, table, opts.seed + 1, &other_skeleton);
    checks.check(!same_geometry(first_pass, generate_one(other, probe)), "another seed gives another world");
  }

  // 2. Порядок обхода набора чанков ничего не меняет.
  std::vector<originator::chunk_key> block;
  for (int64_t z = 0; z < 2; ++z) {
    for (int64_t y = -1; y < 1; ++y) {
      for (int64_t x = 0; x < 2; ++x) {
        block.push_back(originator::chunk_key{x, y, z});
      }
    }
  }
  std::map<std::tuple<int64_t, int64_t, int64_t>, vertex_list> forward;
  for (const auto& key : block) {
    forward[{key.x, key.y, key.z}] = generate_one(worker, key);
  }
  bool order_matches = true;
  size_t largest = 0;
  size_t block_vertices = 0;
  for (auto it = block.rbegin(); it != block.rend(); ++it) {
    const auto& expected = forward[{it->x, it->y, it->z}];
    const auto produced = generate_one(worker, *it);
    order_matches = order_matches && same_geometry(expected, produced);
    largest = std::max(largest, produced.size());
    block_vertices += produced.size();
  }
  checks.check(order_matches, "generating a block back to front gives the same chunks bit for bit");
  checks.check(block_vertices > 0, "the test block has geometry");

  // 3. ШОВ. Вершины на общей плоскости двух соседних чанков обязаны совпадать как НАБОР: обе стороны
  // считают их из одной пары узлов, поэтому совпадение точное, а не приблизительное.
  {
    const double span = double(sizes.chunk_cells) * sizes.cell_size;
    const auto left = forward[{0, 0, 0}];
    const originator::chunk_key right_key{1, 0, 0};
    const auto right = generate_one(worker, right_key);
    // Вершина на общей грани: у левого чанка её локальный x равен дальней границе (65535), у правого
    // — нулю. Сравниваются МИРОВЫЕ точки, поэтому обе стороны обязаны дать одно и то же число.
    const auto on_plane = [span](const vertex_list& source, const originator::chunk_key& key,
                                 const uint16_t local_x) {
      std::vector<point> result;
      for (const auto& vertex : source) {
        if (vertex.position[0] == local_x) {
          result.push_back(point_of(vertex, key, span));
        }
      }
      std::sort(result.begin(), result.end());
      result.erase(std::unique(result.begin(), result.end()), result.end());
      return result;
    };
    const auto left_side = on_plane(left, originator::chunk_key{0, 0, 0}, 65535);
    const auto right_side = on_plane(right, right_key, 0);
    checks.check(!left_side.empty(), "the surface actually crosses the shared face of two chunks");
    checks.check(left_side == right_side, "both chunks put exactly the same vertices on their shared face");
  }

  // 4. ЗАМКНУТОСТЬ на внутренних швах. Считается по блоку 2x2x2 чанков: рёбра, целиком лежащие
  // внутри блока, обязаны быть сбалансированы. Ребро на внешней границе блока законно непарно —
  // там поверхность просто продолжается в чанк, которого в блоке нет.
  {
    struct balance {
      int forward = 0;
      int backward = 0;
    };
    std::map<edge, balance> usage;
    double low[3]{};
    double high[3]{};
    for (uint32_t axis = 0; axis < 3; ++axis) {
      low[axis] = std::numeric_limits<double>::infinity();
      high[axis] = -std::numeric_limits<double>::infinity();
    }
    // Границы блока в мире: у чанков ключи 0..1 по x и z, -1..0 по y.
    const double span = double(sizes.chunk_cells) * sizes.cell_size;
    low[0] = 0.0;
    high[0] = span * 2.0;
    low[1] = -span;
    high[1] = span;
    low[2] = 0.0;
    high[2] = span * 2.0;

    const auto inside = [&](const point& p) {
      for (uint32_t axis = 0; axis < 3; ++axis) {
        if (double(p[axis]) <= low[axis] || double(p[axis]) >= high[axis]) {
          return false;
        }
      }
      return true;
    };

    for (const auto& [triple, list] : forward) {
      const auto& [key_x, key_y, key_z] = triple;
      const originator::chunk_key key{key_x, key_y, key_z};
      for (size_t i = 0; i + 2 < list.size(); i += 3) {
        for (size_t k = 0; k < 3; ++k) {
          // Вершины лежат в координатах своего чанка, поэтому в мировые их переводит ключ. Без этого
          // все чанки блока накладывались бы друг на друга, и «замкнутость» проверялась бы у мешанины.
          const auto from = point_of(list[i + k], key, span);
          const auto to = point_of(list[i + (k + 1) % 3], key, span);
          if (from == to) {
            continue;
          }
          const bool ordered = from < to;
          const edge id = ordered ? edge{from, to} : edge{to, from};
          auto& counter = usage[id];
          if (ordered) {
            counter.forward += 1;
          } else {
            counter.backward += 1;
          }
        }
      }
    }

    size_t unbalanced = 0;
    size_t interior = 0;
    for (const auto& [id, counter] : usage) {
      if (!inside(id.first) || !inside(id.second)) {
        continue;
      }
      ++interior;
      if (counter.forward != counter.backward) {
        ++unbalanced;
      }
    }
    checks.check(interior > 1000, "the block has interior edges to check at all");
    checks.check(unbalanced == 0, "no crack on any internal seam of a 2x2x2 chunk block");
  }

  // 5. Ёмкость объявлена не наугад: самый густой чанк выборки против объявленного числа.
  checks.check(largest <= sizes.vertex_capacity, "the densest chunk of the sample fits the declared capacity");
  utils::info("GN03 verify: densest sampled chunk {} vertices of the declared {} ({:.1f}%)", largest,
              sizes.vertex_capacity, 100.0 * double(largest) / double(sizes.vertex_capacity));

  // 6. Арена: место возвращается в оборот, а выгруженный отрезок не переиспользуется сразу.
  {
    gn03::vertex_arena arena(4096, 96, 2);
    const vertex_list small(100);
    const vertex_list large(1000);
    checks.check(arena.insert(originator::chunk_key{0, 0, 0}, small), "the arena takes a small chunk");
    checks.check(arena.insert(originator::chunk_key{1, 0, 0}, large), "the arena takes a large chunk");
    const size_t water = arena.high_water();
    arena.remove(originator::chunk_key{0, 0, 0});
    checks.check(arena.hole_vertices() > 0, "a removed chunk leaves a hole");
    // Отрезок ещё не свободен: его читают кадры, уже отданные устройству.
    checks.check(arena.insert(originator::chunk_key{2, 0, 0}, small) && arena.high_water() > water,
                 "a freshly removed block is not reused until its frames are done");
    arena.advance_frame();
    arena.advance_frame();
    arena.advance_frame();
    const size_t before = arena.high_water();
    checks.check(arena.insert(originator::chunk_key{3, 0, 0}, small) && arena.high_water() == before,
                 "after the retirement frames the hole is reused and the arena stops growing");
    arena.remove(originator::chunk_key{1, 0, 0});
    arena.remove(originator::chunk_key{2, 0, 0});
    arena.remove(originator::chunk_key{3, 0, 0});
    arena.remove(originator::chunk_key{0, 0, 0});
    for (size_t i = 0; i < 4; ++i) {
      arena.advance_frame();
    }
    checks.check(arena.high_water() == 0 && arena.live_blocks() == 0, "an emptied arena reports nothing to draw");
    const vertex_list huge(5000);
    checks.check(!arena.insert(originator::chunk_key{9, 9, 9}, huge), "the arena refuses what does not fit");
  }

  // 7. Окно чанков: объём, попадание и приоритет.
  {
    gn03::chunk_window window;
    window.centre = originator::chunk_key{5, 0, -3};
    window.horizontal_radius = 2;
    window.vertical_radius = 1;
    checks.check(window.volume() == 5 * 5 * 3, "the window volume matches its radii");
    checks.check(window.contains(originator::chunk_key{7, 1, -1}), "the window contains its corner");
    checks.check(!window.contains(originator::chunk_key{8, 0, -3}), "the window excludes the ring beyond it");
    checks.check(!window.contains(originator::chunk_key{5, 2, -3}), "the window excludes the layer above it");
    // Окно СИММЕТРИЧНО по вертикали: мир генерируется в любом направлении от наблюдателя, и слой
    // под ним такой же полноправный, как слой над ним.
    checks.check(window.contains(originator::chunk_key{5, -1, -3}), "the window contains the layer below it");
    checks.check(!window.contains(originator::chunk_key{5, -2, -3}), "the window excludes the layer under that");

    // Окно, сдвинутое вслед за камерой, накрывает СТОЛЬКО ЖЕ чанков: у радиусов нет края мира.
    gn03::chunk_window moved = window;
    moved.centre = originator::chunk_key{1000, 40, -7};
    checks.check(moved.volume() == window.volume(), "moving the window does not change how much it holds");
    checks.check(moved.contains(originator::chunk_key{1000, 41, -7}), "the moved window follows the camera up");
  }

  // 8. Стоимость чанка. Не критерий, а измерение: от него зависит, сколько чанков успевает
  // появиться за кадр и есть ли смысл в фоновом потоке вообще.
  {
    constexpr size_t sample_size = 8;
    const auto start = std::chrono::steady_clock::now();
    size_t vertices = 0;
    for (size_t i = 0; i < sample_size; ++i) {
      gn03::chunk_mesh mesh;
      worker.generate(originator::chunk_key{int64_t(i) * 3, 0, int64_t(i)}, mesh);
      vertices += mesh.vertices.size();
    }
    const double milliseconds =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    utils::info("GN03 verify: {:.2f} ms per chunk in one thread, {} vertices on average", milliseconds / sample_size,
                vertices / sample_size);
  }

  // 9. ПЛАВАЮЩЕЕ НАЧАЛО КООРДИНАТ. Проверяется не «работает ли перенос», а то, ради чего он заведён:
  // точность не должна зависеть от того, как далеко улетел наблюдатель.
  {
    const double span = double(sizes.chunk_cells) * sizes.cell_size;
    constexpr size_t steps = 1000000;
    constexpr double step = 0.37; // 370 километров пути шагом, не представимым во float

    // Шаг НЕ представим во float точно, и это условие честного замера: шагами по метру наивная
    // позиция тоже не ошибётся (целые до 16 миллионов во float32 точны), и сравнение показало бы
    // ноль против нуля.
    //
    // Считаются ТРИ схемы сразу, потому что болезней две и лечатся они разным:
    //   naive     — мировая позиция во float32: и разрешение потеряно, и накопление есть;
    //   in_chunk  — плавающее начало координат с накопителем во float32: разрешение спасено,
    //               накопление осталось (это и был снос в 0.36 метра);
    //   frame     — плавающее начало координат с накопителем в double: нет ни того, ни другого.
    // ЧЕТВЁРТАЯ СХЕМА, и она отвечает на вопрос до конца: смещение внутри чанка в ФИКСИРОВАННОЙ
    // ТОЧКЕ. Единица — 1/65536 метра (15.3 микрона), размер чанка в единицах целый, поэтому и
    // сложение, и перенос через границу чанка — целочисленные. Целое сложение НЕ ОКРУГЛЯЕТ, значит
    // накопления нет вовсе, и нет его ПО ПОСТРОЕНИЮ, а не по величине ошибки.
    //
    // Остаётся ровно один источник расхождения с идеальным путём — однократное квантование САМОГО
    // ШАГА (0.37 м не кратно единице). Но это не накопление, а представление: величина фиксирована,
    // одинакова на всех машинах и не растёт со временем.
    constexpr int64_t units_per_metre = 65536;
    const int64_t fixed_step = int64_t(step * double(units_per_metre));
    const int64_t span_units = int64_t(span * double(units_per_metre));
    int64_t fixed_local = 0;
    int64_t fixed_key = 0;

    gn03::local_frame frame;
    originator::chunk_key float_key{};
    float float_local = 0.0f;
    float naive = 0.0f;
    bool inside_chunk = true;
    for (size_t i = 0; i < steps; ++i) {
      fixed_local += fixed_step;
      while (fixed_local >= span_units) {
        fixed_local -= span_units;
        fixed_key += 1;
      }

      frame.position.x += step;
      gn03::rebase(frame, span);
      inside_chunk = inside_chunk && frame.position.x >= 0.0 && frame.position.x < span;

      // Тот же перенос, но накопитель во float — ради числа, а не ради работы.
      float_local += float(step);
      while (float_local >= float(span)) {
        float_local -= float(span);
        float_key.x += 1;
      }

      naive += float(step);
    }
    const double exact = double(steps) * step;
    const double framed_error = std::abs(gn03::absolute_position(frame, span).x - exact);
    const double in_chunk_error = std::abs(double(float_key.x) * span + double(float_local) - exact);
    const double naive_error = std::abs(double(naive) - exact);

    // Идеал для фиксированной точки считается В ЕЁ ЖЕ единицах: иначе замер показал бы квантование
    // шага, а проверяется накопление. Оба числа целые и меньше 2^53, поэтому double их представляет
    // точно, и «ровно ноль» здесь означает ровно ноль.
    const double fixed_exact = double(steps) * double(fixed_step);
    const double fixed_position = double(fixed_key) * double(span_units) + double(fixed_local);
    const double fixed_error = std::abs(fixed_position - fixed_exact) / double(units_per_metre);
    const double fixed_step_bias = std::abs(step - double(fixed_step) / double(units_per_metre));
    // Что там вообще можно ВЫРАЗИТЬ: расстояние до следующего представимого числа. Это и есть предел
    // наивной схемы — ошибку меньше этого она не то что не накопит, она её не сможет записать.
    const double naive_resolution = double(std::nextafterf(float(exact), 2.0f * float(exact)) - float(exact));
    const double framed_resolution = double(std::nextafterf(float(span), 2.0f * float(span)) - float(span));

    checks.check(inside_chunk, "the local position never leaves its chunk during the whole flight");
    checks.check(frame.key.x == int64_t(std::floor(exact / span)),
                 "the chunk key counts exactly as many chunks as flown");

    // ДВЕ БОЛЕЗНИ, ДВА ЛЕКАРСТВА, и оба измерены.
    //
    // РАЗРЕШЕНИЕ — что число вообще способно выразить. Лечит плавающее начало координат: локальная
    // позиция всегда внутри чанка, поэтому шаг сетки остаётся микронным на любом удалении.
    //
    // НАКОПЛЕНИЕ — сумма миллиона округлений. От начала координат не зависит вовсе, зависит от ШИРИНЫ
    // НАКОПИТЕЛЯ: во float32 внутри чанка снос вышел 0.36 метра (за пару игровых сессий видимая
    // величина), в double — порядка 10^-9 метра. Поэтому накопитель в double, а во float
    // преобразуется только позиция для матрицы вида, один раз на кадр и без возврата в накопитель.
    checks.check(framed_resolution < naive_resolution / 1000.0,
                 "the in-chunk resolution stays microns no matter how far out the observer flew");
    checks.check(in_chunk_error < span, "a float accumulator inside a chunk at least never loses the chunk");
    checks.check(framed_error < 1.0e-6, "a double accumulator inside a chunk leaves no drift worth naming");
    checks.check(framed_error < in_chunk_error / 1000.0,
                 "widening the accumulator is what removes the drift, not moving the origin");
    // РОВНО НОЛЬ, и это утверждение про арифметику, а не про величину: целое сложение не округляет.
    checks.check(fixed_error == 0.0, "a fixed-point offset inside a chunk accumulates no error at all");
    checks.check(fixed_key == int64_t(std::floor(exact / span)),
                 "the fixed-point walk counts the same chunks as the exact one");
    utils::info("GN03 verify: after {:.0f} m of flight the drift is {:.6f} m naive float32, {:.6f} m in-chunk "
                "float32, {:.3e} m in-chunk double, {:.1f} m in-chunk fixed point; resolution there is {:.6f} m "
                "naive against {:.8f} m in-chunk float and {:.8f} m fixed",
                exact, naive_error, in_chunk_error, framed_error, fixed_error, naive_resolution,
                framed_resolution, 1.0 / double(units_per_metre));
    utils::info("GN03 verify: the fixed-point step itself is quantised once by {:.3e} m — a fixed bias, equal on "
                "every machine, that does not grow with time; that is the whole price of integer positions",
                fixed_step_bias);

    // Обратный ход возвращает РОВНО в начало, и здесь шаг взят представимым (четверть чанка):
    // проверяется механизм переноса, а не сложение float, поэтому и утверждение побитовое.
    gn03::local_frame exact_walk;
    const double exact_step = span / 4.0;
    for (size_t i = 0; i < 4096; ++i) {
      exact_walk.position.x += exact_step;
      gn03::rebase(exact_walk, span);
    }
    checks.check(exact_walk.key.x == 1024 && exact_walk.position.x == 0.0,
                 "an exact walk lands on the chunk boundary with a zero offset");
    for (size_t i = 0; i < 4096; ++i) {
      exact_walk.position.x -= exact_step;
      gn03::rebase(exact_walk, span);
    }
    checks.check(exact_walk.key.x == 0 && exact_walk.position.x == 0.0,
                 "flying back returns to exactly the same place");

    // Отрицательная сторона: у ключа нет «нуля посередине». Округление ВНИЗ, а не усечение к нулю —
    // иначе чанк по обе стороны от нуля был бы один и тот же, и у начала координат окно оказалось бы
    // перекошено на целый чанк.
    gn03::local_frame negative;
    negative.position.x = -span * 0.5;
    gn03::rebase(negative, span);
    checks.check(negative.key.x == -1 && negative.position.x > 0.0,
                 "a step to the left of the origin lands in the chunk to the left, not in the same one");

    // Смещение чанка относительно наблюдателя зависит только от РАЗНОСТИ ключей: мир, отнесённый на
    // миллион чанков, даёт те же самые числа кадра.
    const auto near_offset = gn03::chunk_offset(originator::chunk_key{3, -1, 2}, originator::chunk_key{0, 0, 0}, span);
    const auto far_offset = gn03::chunk_offset(originator::chunk_key{1000003, 999999, 1000002},
                                               originator::chunk_key{1000000, 1000000, 1000000}, span);
    checks.check(near_offset == far_offset, "chunk offsets depend on the key difference alone, not on where the world is");
  }

  // 10. ПРЕДЕЛ САМОГО ПОЛЯ, а не отрисовки. Плавающее начало координат лечит кадр: вершины лежат в
  // координатах своего чанка, и мир рисуется одинаково на любом удалении. У ПОЛЯ предел свой и
  // другой: мировые позиции узлов хранятся во float32 и в таком же виде уходят в шум, поэтому далеко
  // от начала мира соседние узлы решётки получают побитово ОДНУ И ТУ ЖЕ координату — и мир идёт
  // ступенями. Видно это было сразу (острова в тридцати двух миллионах метров стали кубами), но
  // мерить надо не глазом.
  //
  // Мерится доля соседних узлов с равной ПОЗИЦИЕЙ, а не с равной плотностью, и это не придирка:
  // первая версия считала плотность и показала 32% уже у начала координат — потому что в воздухе
  // поле упирается в постоянную полку островов, и метрика измеряла полку, а не точность.
  {
    const double span = double(sizes.chunk_cells) * sizes.cell_size;
    auto* samples = worker.line().find_buffer("samples");
    const size_t position_index = samples->find_field("position");
    const size_t side = sizes.side;

    const auto collapsed_share = [&](const double distance) {
      const auto key = originator::chunk_key{int64_t(distance / span), 0, 0};
      gn03::chunk_mesh mesh;
      worker.generate(key, mesh);
      const auto position = samples->field(position_index);
      size_t equal = 0;
      size_t pairs = 0;
      for (size_t z = 0; z < side; ++z) {
        for (size_t y = 0; y < side; ++y) {
          for (size_t x = 0; x + 1 < side; ++x) {
            const size_t index = x + side * (y + side * z);
            ++pairs;
            equal += position.get(index, 0) == position.get(index + 1, 0) ? 1 : 0;
          }
        }
      }
      return pairs == 0 ? 0.0 : double(equal) / double(pairs);
    };

    for (const double distance : {0.0, 1.0e5, 1.0e6, 1.0e7, 1.0e8}) {
      const float sample = float(distance);
      const double resolution = double(std::nextafterf(sample, 2.0f * sample + 1.0f) - sample);
      utils::info("GN03 verify: at {:.0e} m from the origin float32 resolves {:.4f} m and {:.1f}% of "
                  "neighbouring lattice nodes collapse onto one position",
                  distance, resolution, 100.0 * collapsed_share(distance));
    }

    // Рабочий радиус объявляется числом, а не надеждой: на миллионе метров решётка обязана остаться
    // различимой. Дальше она деградирует постепенно, и это свойство float32 у входа шума, а не
    // дефект чанкования — лечится оно только другой арифметикой внутри шума.
    checks.check(collapsed_share(1.0e6) == 0.0, "the lattice is still distinct a million metres from the origin");
  }

  // 11. АРЕНА НЕ МЕНЯЕТ ГЕОМЕТРИЮ, кроме номера слота. Проверка нужна потому, что арена — единственное
  // место, где посчитанные вершины ПЕРЕПИСЫВАЮТСЯ (клеймение слотом, выравнивание отрезка, обнуление
  // хвоста), и ошибка здесь выглядела бы как ошибка генератора, которой нет.
  {
    const auto vertices = generate_one(worker, probe);
    checks.check(!vertices.empty(), "the probe chunk has geometry to store");

    gn03::vertex_arena arena(1u << 20, 768, 2);
    checks.check(arena.insert(probe, vertices), "the arena takes the probe chunk");
    const auto mirror = arena.mirror();
    const auto live = arena.live();
    checks.check(live.size() == 1 && live[0].key == probe, "the arena reports exactly the chunk it holds");

    bool geometry_intact = true;
    bool slot_stamped = true;
    for (size_t i = 0; i < vertices.size(); ++i) {
      const auto& source = vertices[i];
      const auto& stored = mirror[i];
      geometry_intact = geometry_intact && stored.position[0] == source.position[0] &&
                        stored.position[1] == source.position[1] && stored.position[2] == source.position[2] &&
                        stored.normal[0] == source.normal[0] && stored.normal[1] == source.normal[1] &&
                        stored.normal[2] == source.normal[2];
      slot_stamped = slot_stamped && stored.chunk == uint16_t(live[0].slot);
    }
    checks.check(geometry_intact, "the arena stores the geometry byte for byte as generated");
    checks.check(slot_stamped, "every vertex of the chunk carries the slot the arena gave it");

    // Хвост отрезка обязан быть вырожденным: он рисуется вместе со всем остальным.
    bool tail_degenerate = true;
    for (size_t i = vertices.size(); i < arena.high_water(); ++i) {
      tail_degenerate = tail_degenerate && mirror[i].position[0] == 0 && mirror[i].position[1] == 0 &&
                        mirror[i].position[2] == 0;
    }
    checks.check(tail_degenerate, "the padding tail of a span is degenerate, not stale geometry");
  }

  // 12. НАСКОЛЬКО ПОВЕРХНОСТЬ УЯЗВИМА К РАСХОЖДЕНИЮ FLOAT. Вопрос «стоит ли вообще париться про
  // разную арифметику на разных платформах» имеет числовой ответ, и вот он.
  //
  // Расхождение в последних битах поля превращается в РАЗНУЮ ГЕОМЕТРИЮ только через решения, а
  // решений у marching cubes ровно два:
  //
  //   ЗНАК узла относительно порога — он выбирает случай куба, то есть ТОПОЛОГИЮ. Перевернуться он
  //   может только у узла, чья плотность стоит ближе к порогу, чем величина расхождения;
  //   ДОЛЯ вдоль ребра — она двигает вершину, но вершина всё равно уезжает в фиксированную точку с
  //   шагом span/65535, поэтому разница меньше кванта исчезает целиком, а больше кванта не бывает
  //   при таких расхождениях.
  //
  // Мерится первое: доля узлов у самого порога. Умножив её на число узлов в чанке, получаем «один
  // перевёрнутый узел на N чанков», а перевёрнутый узел меняет геометрию не более восьми клеток
  // вокруг себя. То есть цена расхождения не «другой мир», а «несколько треугольников в одном месте».
  {
    auto* samples = worker.line().find_buffer("samples");
    const size_t density_index = samples->find_field("density");
    const double iso = description.values.number("iso", 0.0);
    const size_t side = sizes.side;
    const size_t node_count = side * side * side;

    // Усреднение по нескольким чанкам: у одного доля зависит от того, много ли в нём поверхности.
    constexpr size_t chunk_sample = 6;
    std::array<double, 3> thresholds{1.0e-7, 1.0e-5, 1.0e-3};
    std::array<size_t, 3> near{};
    size_t total_nodes = 0;

    for (size_t i = 0; i < chunk_sample; ++i) {
      gn03::chunk_mesh mesh;
      worker.generate(originator::chunk_key{int64_t(i) * 7 - 3, int64_t(i % 3) - 1, int64_t(i) * 5}, mesh);
      const auto density = samples->field(density_index);
      for (size_t node = 0; node < node_count; ++node) {
        const double distance = std::abs(density.get(node) - iso);
        for (size_t k = 0; k < thresholds.size(); ++k) {
          near[k] += distance < thresholds[k] ? 1 : 0;
        }
      }
      total_nodes += node_count;
    }

    for (size_t k = 0; k < thresholds.size(); ++k) {
      const double share = double(near[k]) / double(total_nodes);
      const double chunks_per_flip = share > 0.0 ? 1.0 / (share * double(node_count)) : 0.0;
      utils::info("GN03 verify: {} of {} nodes sit within {:.0e} of the iso level — that is one flipped cube "
                  "case per {} chunks at that error scale",
                  near[k], total_nodes, thresholds[k],
                  chunks_per_flip > 0.0 ? std::format("{:.0f}", chunks_per_flip) : std::string("(none seen)"));
    }

    // Порог 1e-3 взят как заведомо больший любого расхождения арифметики: если и на нём доля мала,
    // то вопрос закрыт числом, а не рассуждением. Проверяется именно это.
    checks.check(double(near[2]) / double(total_nodes) < 1.0e-4,
                 "even a thousandth-of-a-unit field disagreement could flip only a negligible share of nodes");
  }

  // 13. СУЩНОСТИ ЧАНКА. Проверяется не «есть ли вехи», а то, что делает их частью МИРА, а не
  // украшением: они выводятся из того же поля, лежат в своём чанке и зависят только от (зерно, ключ).
  {
    auto* samples = worker.line().find_buffer("samples");
    const size_t density_index = samples->find_field("density");
    const double iso = description.values.number("iso", 0.0);
    const double slope_limit = description.values.number("prop_slope_limit", 0.0);
    const double cell = sizes.cell_size;
    const double span = double(sizes.chunk_cells) * cell;
    const size_t side = sizes.side;

    const auto props_of = [&](const originator::chunk_key& key) {
      gn03::chunk_mesh mesh;
      worker.generate(key, mesh);
      return mesh.props;
    };

    const auto same_props = [](const std::vector<gn03::chunk_prop>& a, const std::vector<gn03::chunk_prop>& b) {
      if (a.size() != b.size()) {
        return false;
      }
      return std::memcmp(a.data(), b.data(), a.size() * sizeof(gn03::chunk_prop)) == 0;
    };

    // Чанк с вехами: у пустого проверять нечего, поэтому он ищется, а не предполагается.
    std::vector<gn03::chunk_prop> sample_props;
    originator::chunk_key populated{};
    size_t total = 0;
    size_t densest = 0;
    std::array<size_t, 3> kinds{};
    for (int64_t i = 0; i < 12; ++i) {
      const auto key = originator::chunk_key{i * 3 - 5, (i % 3) - 1, i * 2 - 7};
      const auto found = props_of(key);
      total += found.size();
      densest = std::max(densest, found.size());
      for (const auto& prop : found) {
        if (prop.kind < kinds.size()) {
          kinds[prop.kind] += 1;
        }
      }
      if (sample_props.empty() && !found.empty()) {
        sample_props = found;
        populated = key;
      }
    }
    checks.check(!sample_props.empty(), "some chunk of the sample has entities at all");
    utils::info("GN03 verify: {} entities over 12 chunks, densest chunk {} of the declared {}; kinds: {} open, "
                "{} slope, {} roofed",
                total, densest, sizes.prop_capacity, kinds[0], kinds[1], kinds[2]);
    checks.check(densest <= sizes.prop_capacity, "the most populated chunk fits the declared entity capacity");

    // Воспроизводимость — тем же набором способов, что у геометрии.
    props_of(originator::chunk_key{101, 3, -77});
    checks.check(same_props(sample_props, props_of(populated)),
                 "a chunk's entities are the same after other chunks ran in the same pipeline");
    worker.line().clear_buffers();
    checks.check(same_props(sample_props, props_of(populated)),
                 "clearing every buffer changes none of the entities");
    {
      chunk_worker twin(description, table, opts.seed, &skeleton);
      checks.check(same_props(sample_props, props_of_twin(twin, populated)),
                   "a second pipeline with the same seed places the same entities");
    }

    // Каждая веха стоит НА ПОВЕРХНОСТИ, и это проверяется по тому же полю: узел под ней плотный, узел
    // над ней пустой. Позиция веха выбирает по столбцу решётки, поэтому индексы точны, а не примерны.
    // Поле пересчитывается под этот чанк ещё раз: между проверками пайплайн считал другие чанки, и
    // в буфере сейчас лежит поле последнего из них.
    {
      gn03::chunk_mesh scratch;
      worker.generate(populated, scratch);
    }
    const auto density = samples->field(density_index);
    size_t off_surface = 0;
    size_t outside_chunk = 0;
    size_t too_steep = 0;
    for (const auto& prop : sample_props) {
      const auto x = size_t(std::lround(prop.position[0] / cell) + 1);
      const auto z = size_t(std::lround(prop.position[2] / cell) + 1);
      const auto below = size_t(std::floor(prop.position[1] / cell) + 1);
      if (x < 1 || z < 1 || below < 1 || x + 1 >= side || z + 1 >= side || below + 1 >= side) {
        ++outside_chunk;
        continue;
      }
      const size_t under = x + side * (below + side * z);
      const size_t over = x + side * ((below + 1) + side * z);
      if (!(density.get(under) >= iso && density.get(over) < iso)) {
        ++off_surface;
      }
      for (uint32_t axis = 0; axis < 3; ++axis) {
        if (prop.position[axis] < 0.0 || prop.position[axis] > span) {
          ++outside_chunk;
          break;
        }
      }
      if (prop.normal[1] < slope_limit) {
        ++too_steep;
      }
    }
    checks.check(off_surface == 0, "every entity stands exactly where the field crosses the iso level");
    checks.check(outside_chunk == 0, "every entity lies inside its own chunk");
    checks.check(too_steep == 0, "no entity stands on a slope steeper than the declared limit");
  }

  // 14. КАРКАС И МЕТА ЧАНКА. Двухмасштабная генерация добавляет ровно один новый риск, и он не в
  // геометрии, а в ЗАПРОСЕ: если чанк получит не всю свою область каркаса, тот же ключ даст другой
  // мир. Поэтому проверяется не «коридор выглядит правильно», а полнота запроса.
  {
    const double span = double(sizes.chunk_cells) * sizes.cell_size;
    checks.check(!skeleton.empty(), "the skeleton has a route at all");
    checks.check(skeleton.about().influence > 0.0, "the skeleton declares its radius of influence");

    // ПОЛНОТА ИНДЕКСА: то же, что находит полный перебор. Пропущенный отрезок иначе виден только как
    // коридор, оборвавшийся в середине горы, и только если случайно пролететь именно там.
    size_t mismatches = 0;
    size_t chunks_with_route = 0;
    size_t densest_meta = 0;
    for (int64_t z = -3; z <= 3; ++z) {
      for (int64_t y = -1; y <= 1; ++y) {
        for (int64_t x = -3; x <= 3; ++x) {
          const originator::chunk_key key{x * 3, y, z * 3};
          const std::array<double, 3> low{double(key.x) * span - sizes.cell_size,
                                          double(key.y) * span - sizes.cell_size,
                                          double(key.z) * span - sizes.cell_size};
          const std::array<double, 3> high{low[0] + span + 2.0 * sizes.cell_size,
                                           low[1] + span + 2.0 * sizes.cell_size,
                                           low[2] + span + 2.0 * sizes.cell_size};

          gn03::world_skeleton::query_result indexed;
          gn03::world_skeleton::query_result exhaustive;
          const bool indexed_fits = skeleton.query(low, high, sizes.route_capacity, sizes.route_chain_capacity,
                                                   indexed);
          const bool exhaustive_fits = skeleton.query_exhaustive(low, high, sizes.route_capacity,
                                                                 sizes.route_chain_capacity, exhaustive);
          if (indexed_fits != exhaustive_fits || indexed.points != exhaustive.points ||
              indexed.offsets != exhaustive.offsets) {
            ++mismatches;
          }
          chunks_with_route += indexed.points.empty() ? 0 : 1;
          densest_meta = std::max(densest_meta, indexed.points.size());
        }
      }
    }
    checks.check(mismatches == 0, "the spatial index finds exactly what an exhaustive scan finds");
    checks.check(chunks_with_route > 0, "the sampled chunks include some the route passes through");
    utils::info("GN03 verify: {} of 147 sampled chunks carry route meta, the densest {} points of the declared "
                "{} — a chunk gets a QUERY over its area, not the skeleton",
                chunks_with_route, densest_meta, sizes.route_capacity);

    // ОТКАЗ, А НЕ ОБРЕЗКА: с ёмкостью в одну точку запрос обязан сказать «не влезло».
    {
      gn03::world_skeleton::query_result tiny;
      bool refused_somewhere = false;
      for (int64_t x = -3; x <= 3 && !refused_somewhere; ++x) {
        const originator::chunk_key key{x * 3, 0, 0};
        const std::array<double, 3> low{double(key.x) * span, -span, 0.0};
        const std::array<double, 3> high{low[0] + span, span, span};
        gn03::world_skeleton::query_result full;
        if (skeleton.query(low, high, sizes.route_capacity, sizes.route_chain_capacity, full) &&
            !full.points.empty()) {
          refused_somewhere = !skeleton.query(low, high, 1, sizes.route_chain_capacity, tiny);
        }
      }
      checks.check(refused_somewhere, "a query that does not fit the declared capacity refuses instead of "
                                      "truncating");
    }

    // ПАКЕТ: грубый проход считается один раз, поэтому его результат живёт файлом. Круг «записал —
    // прочитал» обязан давать тот же запрос, иначе мир после перезапуска другой.
    {
      const std::string path = std::string("gn03_skeleton_check.bin");
      checks.check(skeleton.save(path), "the skeleton writes its package to disk");
      gn03::world_skeleton restored;
      checks.check(restored.load(path), "the skeleton reads its package back");
      checks.check(restored.about().seed == skeleton.about().seed &&
                     restored.about().influence == skeleton.about().influence &&
                     restored.nodes().size() == skeleton.nodes().size() &&
                     restored.points().size() == skeleton.points().size(),
                   "the package keeps the description, the nodes and the route");

      const std::array<double, 3> low{-span, -span, -span};
      const std::array<double, 3> high{span * 2.0, span, span * 2.0};
      gn03::world_skeleton::query_result before;
      gn03::world_skeleton::query_result after;
      skeleton.query(low, high, sizes.route_capacity, sizes.route_chain_capacity, before);
      restored.query(low, high, sizes.route_capacity, sizes.route_chain_capacity, after);
      checks.check(before.points == after.points && before.offsets == after.offsets,
                   "a reloaded package answers a query with exactly the same route");
      std::filesystem::remove(path);
    }

    // КОРИДОР НЕПРЕРЫВЕН ЧЕРЕЗ ШОВ, и это главное свойство двухмасштабной генерации: у двух соседних
    // чанков МЕТА РАЗНАЯ (каждый спросил свою область), а поле на общей грани обязано совпасть.
    // Проверяется на паре чанков, через которую маршрут действительно проходит.
    {
      bool checked_pair = false;
      bool faces_match = false;
      // Имя переменной не `point`: так называется тип мировой точки в этом файле, и тень над типом
      // выглядела бы как ошибка шаблона, а не как то, чем является.
      for (const auto& route_point : skeleton.points()) {
        const originator::chunk_key left{int64_t(std::floor(route_point[0] / span)),
                                         int64_t(std::floor(route_point[1] / span)),
                                         int64_t(std::floor(route_point[2] / span))};
        const originator::chunk_key right{left.x + 1, left.y, left.z};

        const auto left_mesh = generate_one(worker, left);
        const auto right_mesh = generate_one(worker, right);
        if (left_mesh.empty() || right_mesh.empty()) {
          continue;
        }

        const auto on_plane = [span](const vertex_list& source, const originator::chunk_key& key,
                                     const uint16_t local_x) {
          std::vector<point> result;
          for (const auto& vertex : source) {
            if (vertex.position[0] == local_x) {
              result.push_back(point_of(vertex, key, span));
            }
          }
          std::sort(result.begin(), result.end());
          result.erase(std::unique(result.begin(), result.end()), result.end());
          return result;
        };
        const auto left_face = on_plane(left_mesh, left, 65535);
        const auto right_face = on_plane(right_mesh, right, 0);
        if (left_face.empty()) {
          continue;
        }
        checked_pair = true;
        faces_match = left_face == right_face;
        break;
      }
      checks.check(checked_pair, "some chunk pair on the route shares a face with geometry on it");
      checks.check(faces_match, "two chunks with DIFFERENT route meta still agree on their shared face");
    }
  }

  // 15. ПАМЯТЬ МИРА. Проверяется главное свойство модели «выводимое + отличие»: соединение это
  // ФУНКЦИЯ от (выводимые сущности, склад), и ничего кроме. Иначе «вернулся — а стало иначе» ловилось
  // бы только глазами и только иногда.
  {
    const originator::chunk_key key{4, 0, -2};
    gn03::chunk_mesh mesh;
    worker.generate(key, mesh);
    const auto derived = mesh.props;
    checks.check(!derived.empty(), "the memory probe chunk has entities to remember");

    gn03::world_memory memory;
    std::vector<gn03::world_memory::joined_prop> visible;

    // Пустой склад означает «мир таков, каким его посчитал генератор».
    checks.check(memory.join(key, derived, visible) == derived.size(),
                 "an empty memory leaves the derived world exactly as it is");
    checks.check(memory.size() == 0, "an untouched world stores nothing at all");

    // Идентичность: номера попыток различны, иначе память об одной сущности стала бы памятью о другой.
    std::set<uint32_t> origins;
    for (const auto& prop : derived) {
      origins.insert(prop.origin);
    }
    checks.check(origins.size() == derived.size(), "every entity of a chunk carries a distinct identity");

    // Забрать: сущность исчезает из мира, а склад растёт РОВНО на одну запись.
    const gn03::prop_id taken{key, derived.front().origin};
    memory.take(taken);
    checks.check(memory.join(key, derived, visible) == derived.size() - 1, "a taken entity leaves the world");
    checks.check(memory.size() == 1, "memory grows with what was touched, not with what exists");

    // ГЛАВНОЕ: чанк, посчитанный ЗАНОВО, соединяется со складом в то же самое. Это и есть «улетел и
    // вернулся»: выводимое пересчитано с нуля, а мир помнит.
    gn03::chunk_mesh again;
    worker.generate(originator::chunk_key{-40, 2, 17}, again); // между ними считался другой чанк
    worker.generate(key, again);
    std::vector<gn03::world_memory::joined_prop> after;
    const size_t after_count = memory.join(key, again.props, after);
    bool same_after_reload = after_count == derived.size() - 1;
    for (size_t i = 0; i < after.size() && same_after_reload; ++i) {
      same_after_reload = after[i].prop->origin == visible[i].prop->origin &&
                          after[i].delta.taken == visible[i].delta.taken &&
                          after[i].delta.marked == visible[i].delta.marked;
    }
    checks.check(same_after_reload, "a chunk recomputed from scratch joins the memory into the same world");

    // Порядок появления отличий не имеет значения: склад это множество, а не журнал.
    gn03::world_memory forward;
    gn03::world_memory backward;
    const gn03::prop_id first{key, derived[0].origin};
    const gn03::prop_id second{key, derived[derived.size() / 2].origin};
    forward.take(first);
    forward.mark(second);
    backward.mark(second);
    backward.take(first);
    std::vector<gn03::world_memory::joined_prop> forward_visible;
    std::vector<gn03::world_memory::joined_prop> backward_visible;
    forward.join(key, derived, forward_visible);
    backward.join(key, derived, backward_visible);
    bool order_free = forward_visible.size() == backward_visible.size();
    for (size_t i = 0; i < forward_visible.size() && order_free; ++i) {
      order_free = forward_visible[i].prop->origin == backward_visible[i].prop->origin &&
                   forward_visible[i].delta.marked == backward_visible[i].delta.marked;
    }
    checks.check(order_free, "the order in which the world was changed does not change the world");

    // Пометка ПЕРЕКЛЮЧАЕТСЯ, и снятая пометка не оставляет мусора. Проверка эта нашла настоящую
    // развилку: сначала счётчик касаний учитывался в «запись пуста», и снятая пометка оставляла
    // запись навсегда — то есть игрок, потрогавший и вернувший как было тысячу вех, платил тысячей
    // записей в сохранении. Критерий должен быть «МИР ОТЛИЧАЕТСЯ», а не «что-то происходило».
    gn03::world_memory toggling;
    toggling.mark(second);
    checks.check(toggling.size() == 1, "marking an entity is remembered");
    toggling.mark(second);
    checks.check(toggling.size() == 0, "unmarking forgets the entry: a counter nothing depends on is a "
                                       "statistic, not a difference");

    // Отличие, которому в мире больше ничего не соответствует: мир изменился под сохранением. Оно НЕ
    // выбрасывается (сущность может вернуться), но должно быть посчитано вслух.
    gn03::world_memory stale;
    stale.take(gn03::prop_id{key, 9999});
    checks.check(stale.unmatched(key, derived) == 1, "a delta with nothing to match is reported, not dropped");
    checks.check(memory.unmatched(key, derived) == 0, "a delta that matches the world is not reported as stale");

    // Файл склада И ЕСТЬ сохранение мира. Круг «записал — прочитал» обязан быть точным, а второй
    // записанный файл — побайтово тем же: порядок записей в файле фиксирован сортировкой, а не
    // порядком обхода таблицы.
    const std::string path = std::string("gn03_memory_check.bin");
    memory.mark(second);
    checks.check(memory.save(path), "the memory writes itself to disk");
    gn03::world_memory restored;
    checks.check(restored.load(path), "the memory reads itself back");
    checks.check(restored.size() == memory.size(), "the round trip keeps every entry");
    const auto* original_delta = memory.find(second);
    const auto* restored_delta = restored.find(second);
    checks.check(original_delta != nullptr && restored_delta != nullptr &&
                   original_delta->marked == restored_delta->marked &&
                   original_delta->taken == restored_delta->taken &&
                   original_delta->touches == restored_delta->touches,
                 "the round trip keeps flags and counters");
    const std::string twin_path = std::string("gn03_memory_check_twin.bin");
    checks.check(restored.save(twin_path), "the restored memory writes itself out again");
    const auto first_bytes = file_io::read<char>(path, file_io::type::binary);
    const auto second_bytes = file_io::read<char>(twin_path, file_io::type::binary);
    checks.check(!first_bytes.empty() && first_bytes.size() == second_bytes.size() &&
                   std::memcmp(first_bytes.data(), second_bytes.data(), first_bytes.size()) == 0,
                 "saving the same memory twice gives the same bytes");
    utils::info("GN03 verify: the memory of {} touched entities is {} bytes on disk — a save file is the size of "
                "what the player disturbed, not of the world",
                memory.size(), first_bytes.size());
    std::filesystem::remove(path);
    std::filesystem::remove(twin_path);
  }

  // 16. ОТПЕЧАТОК ЭТАЛОННЫХ ЧАНКОВ. Это ДИАГНОСТИКА, а не критерий, и разница принципиальная: как
  // только меняется правило поля или хеш, отпечаток меняется целиком, и проверять его равенство
  // означало бы падать при каждой правке мира (довод уже оплачен на GN02 — там смена хеша сдвинула
  // каждое число планеты, и ни одно СВОЙСТВО не сломалось).
  //
  // Зачем он тогда: сравнить ДВЕ МАШИНЫ одной строкой. Внутри чанка всё уже целочисленное (позиции в
  // фиксированной точке, нормали байтами), поэтому отпечаток отвечает именно на вопрос «одинаково ли
  // посчиталось», а не «похоже ли».
  {
    uint64_t fingerprint = 0xcbf29ce484222325ull;
    size_t total = 0;
    for (int64_t i = 0; i < 4; ++i) {
      for (int64_t layer = -1; layer <= 1; ++layer) {
        const auto key = originator::chunk_key{i * 5 - 7, layer * 2, 3 - i * 4};
        const auto vertices = generate_one(worker, key);
        total += vertices.size();
        const auto* bytes = reinterpret_cast<const uint8_t*>(vertices.data());
        for (size_t byte = 0; byte < vertices.size() * sizeof(gn03::gpu_vertex); ++byte) {
          fingerprint = (fingerprint ^ bytes[byte]) * 0x100000001b3ull;
        }
      }
    }
    utils::info("GN03 verify: fingerprint of 12 reference chunks ({} vertices) is {:016x} — compare it between "
                "machines and compilers, not between versions",
                total, fingerprint);
  }

  // 17. ОКНО, СДВИНУТОЕ НА ХОДУ, не теряет чанки. Проверка написана под НАЙДЕННЫЙ баг: при
  // перемещении часть чанков не появлялась никогда, а после отлёта и возврата появлялась. Причина
  // была в пересборке очереди — чанк, ждавший своей очереди в прошлом окне, оставался помеченным как
  // ждущий, но ни в одной очереди не лежал.
  //
  // Один рабочий поток здесь намеренно: с одиннадцатью очередь рассасывается быстрее, чем окно
  // успевает сдвинуться, и баг просто не воспроизводится.
  {
    const auto factory = make_factory(description, table, opts.seed, &skeleton);
    gn03::chunk_streamer streamer(factory, 1);

    gn03::chunk_window window;
    window.horizontal_radius = 1;
    window.vertical_radius = 1;
    window.centre = originator::chunk_key{0, 0, 0};
    streamer.set_window(window);

    // Окно сдвигается ДО того, как очередь успела рассосаться: именно так его двигает камера.
    window.centre = originator::chunk_key{1, 0, 0};
    streamer.set_window(window);
    window.centre = originator::chunk_key{1, 0, 1};
    streamer.set_window(window);

    std::set<std::tuple<int64_t, int64_t, int64_t>> wanted;
    for (int64_t z = -1; z <= 1; ++z) {
      for (int64_t y = -1; y <= 1; ++y) {
        for (int64_t x = -1; x <= 1; ++x) {
          wanted.insert({window.centre.x + x, window.centre.y + y, window.centre.z + z});
        }
      }
    }

    std::set<std::tuple<int64_t, int64_t, int64_t>> arrived;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    gn03::chunk_mesh mesh;
    while (arrived.size() < wanted.size() && std::chrono::steady_clock::now() < deadline) {
      if (streamer.pop_ready(mesh)) {
        arrived.insert({mesh.key.x, mesh.key.y, mesh.key.z});
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (arrived != wanted) {
      const auto stats = streamer.stats();
      utils::warn("GN03 verify: {} of {} arrived; pending {}, in flight {}, generated {}", arrived.size(),
                  wanted.size(), stats.pending, stats.in_flight, stats.generated);
      for (const auto& key : wanted) {
        if (!arrived.contains(key)) {
          utils::warn("GN03 verify: missing chunk ({}, {}, {})", std::get<0>(key), std::get<1>(key),
                      std::get<2>(key));
        }
      }
    }
    checks.check(arrived == wanted, "every chunk of the final window arrives even if the window moved while "
                                    "the queue was still full");
  }

  // 18. ПРОПУСКНАЯ СПОСОБНОСТЬ фонового стримера. Замер, а не критерий, но он отвечает на вопрос, от
  // которого зависит вся конструкция: во что превращается «4 миллисекунды на чанк», когда чанки
  // считаются в несколько потоков. Ускорение здесь заведомо не линейное — каждый поток тащит свой
  // буфер отсчётов в 1.4 мегабайта, и упирается всё в память, а не в арифметику.
  {
    const auto factory = make_factory(description, table, opts.seed, &skeleton);
    // Окно берётся большим НЕ ради солидности числа: у каждого рабочего потока есть цена запуска
    // (свой sol::state, свой пайплайн, свои буферы), и на двух десятках чанков она и есть весь
    // замер. Двести сорок три чанка её размазывают.
    gn03::chunk_window window;
    window.horizontal_radius = 4;
    window.vertical_radius = 1;
    const size_t wanted = window.volume();

    for (const size_t workers : {size_t(1), worker_count(opts)}) {
      gn03::chunk_streamer streamer(factory, workers);
      const auto start = std::chrono::steady_clock::now();
      streamer.set_window(window);
      size_t taken = 0;
      gn03::chunk_mesh mesh;
      while (taken < wanted) {
        if (streamer.pop_ready(mesh)) {
          ++taken;
          continue;
        }
        std::this_thread::yield();
      }
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      utils::info("GN03 verify: {} worker(s) produced {} chunks in {:.2f} s — {:.1f} chunks per second",
                  workers, taken, seconds, double(taken) / seconds);
    }
  }

  utils::info("GN03 verify: {}/{} checks passed", checks.passed, checks.total);
  return checks.passed == checks.total ? 0 : 1;
}

} // namespace

int main(const int argc, const char** argv) {
  const auto opts = parse_options(argc, argv);

  if (opts.verify) {
    return main_verify(opts);
  }

  const auto description = make_description(opts.overrides);
  const auto sizes = read_sizes(description);
  const auto table = make_size_table(sizes);

  gn03::viewer_options view;
  view.width = opts.width;
  view.height = opts.height;
  view.frames = opts.frames;
  view.mode = opts.view_mode;
  view.grid = opts.grid;
  view.validation = opts.validation;
  view.uncapped = opts.uncapped;
  view.dump_path = opts.dump_path;
  view.seed = opts.seed;
  view.chunk_cells = sizes.chunk_cells;
  view.cell_size = sizes.cell_size;
  view.arena_vertices = opts.arena_vertices;
  view.workers = worker_count(opts);
  view.window.horizontal_radius = opts.radius;
  view.window.vertical_radius = opts.vertical_radius;
  view.start = opts.start;

  // Настраиваемые значения приезжают из конфига генератора вместе с границами: окно крутит число,
  // ничего не зная о его смысле, и не может ни вывести его за границы, ни сойти с сетки шага.
  for (const auto& range : generator().config.ranges) {
    gn03::tunable_value tunable;
    tunable.range = range;
    tunable.value = range.clamp(description.values.number(range.name, range.minimum));
    view.tunables.push_back(tunable);
  }

  utils::info("GN03: {} workers, window {}x{}x{} chunks around the camera, arena {} vertices ({} MiB)",
              view.workers, opts.radius * 2 + 1, opts.vertical_radius * 2 + 1, opts.radius * 2 + 1,
              opts.arena_vertices, opts.arena_vertices * sizeof(gn03::gpu_vertex) / (1024 * 1024));

  // КАРКАС: грубый проход считается ОДИН раз. Есть пакет — читаем, нет — считаем и пишем. Это и есть
  // «каркас может считаться на фоне, но быть много крупнее видимого мира»: его цена не зависит от
  // того, сколько чанков вокруг игрока, и платится она однажды.
  gn03::world_skeleton skeleton;
  bool skeleton_ready = false;
  if (!opts.skeleton_path.empty() && skeleton.load(opts.skeleton_path)) {
    if (skeleton.about().seed != opts.seed) {
      // Пакет от ДРУГОГО зерна — это другой мир, и молча взять его нельзя: коридоры пошли бы не там,
      // где их проложил каркас этого мира.
      utils::warn("GN03: the skeleton at '{}' was built for seed {} but the world runs on {} — rebuilding",
                  opts.skeleton_path, skeleton.about().seed, opts.seed);
    } else {
      skeleton_ready = true;
      utils::info("GN03: skeleton loaded from '{}': {} nodes, {} route points, influence {} m",
                  opts.skeleton_path, skeleton.nodes().size(), skeleton.points().size(),
                  skeleton.about().influence);
    }
  }
  if (!skeleton_ready) {
    skeleton = build_skeleton(opts.overrides, opts.seed);
    if (!opts.skeleton_path.empty() && !skeleton.save(opts.skeleton_path)) {
      utils::warn("GN03: could not write the skeleton package to '{}'", opts.skeleton_path);
    }
  }

  // ПАМЯТЬ МИРА принадлежит хосту: окно её меняет, но читает и пишет на диск тот, кто знает, где
  // лежит сохранение. Если файла ещё нет, склад просто пуст — мир при этом полностью выводим.
  gn03::world_memory memory;
  if (!opts.memory_path.empty()) {
    if (memory.load(opts.memory_path)) {
      utils::info("GN03: world memory loaded from '{}': {} entries", opts.memory_path, memory.size());
    } else {
      utils::info("GN03: no world memory at '{}' yet — the world is entirely derived", opts.memory_path);
    }
  }
  view.memory = &memory;

  // Посадка на маршрут: место и направление берутся у КАРКАСА, то есть у грубого прохода. Мировая
  // точка тут же разбирается на ключ чанка и смещение внутри него — другого способа задать место в
  // этом мире нет вовсе.
  if (opts.on_route && !skeleton.empty()) {
    const auto points = skeleton.points();
    const size_t index = std::min(opts.route_index, points.size() - 1);
    const size_t next = std::min(index + 1, points.size() - 1);
    const double span = double(sizes.chunk_cells) * sizes.cell_size;

    originator::chunk_key key{};
    glm::dvec3 offset{};
    const std::array<int64_t*, 3> axis_key{&key.x, &key.y, &key.z};
    for (uint32_t axis = 0; axis < 3; ++axis) {
      const double world = points[index][axis];
      *axis_key[axis] = int64_t(std::floor(world / span));
      offset[axis] = world - double(*axis_key[axis]) * span;
    }

    view.start = key;
    view.start_offset = offset;
    view.start_offset_valid = true;
    const double dx = points[next][0] - points[index][0];
    const double dz = points[next][2] - points[index][2];
    // Взгляд вдоль маршрута: у камеры угол считается от оси x, как и atan2 здесь.
    view.start_yaw = float(std::atan2(dz, dx));
    view.start_pitch = 0.0f;
    utils::info("GN03: camera starts on route point {} of {} — chunk ({}, {}, {}) at ({:.1f}, {:.1f}, {:.1f})",
                index, points.size(), key.x, key.y, key.z, offset.x, offset.y, offset.z);
  }

  // Пересборка генератора принадлежит хосту, а не окну: окно умеет попросить другой мир, но собрать
  // пайплайн из конфига — работа того, кто знает, откуда конфиг приехал.
  const auto builder = [&opts, &skeleton](const std::vector<std::pair<std::string, double>>& overrides,
                                          const uint64_t seed) -> gn03::generator_factory {
    auto description = make_description(overrides);
    // Значения из командной строки накладываются ПОСЛЕ значений окна: ключ задаёт то, с чего мир
    // начался, а окно — то, во что его крутят, и последнее слово за тем, кто крутит сейчас.
    for (const auto& [name, value] : opts.overrides) {
      description.values.set_number(name, value);
    }
    const auto sizes = read_sizes(description);
    // Смена зерна или значений мира — это и другой КАРКАС: он тоже функция зерна, поэтому
    // пересчитывается вместе с миром, а не остаётся от прошлого.
    if (seed != skeleton.about().seed) {
      skeleton = build_skeleton(overrides, seed);
    }
    return make_factory(description, make_size_table(sizes), seed, &skeleton);
  };

  const int code = gn03::run_viewer(view, builder);

  if (!opts.memory_path.empty()) {
    if (!memory.save(opts.memory_path)) {
      utils::warn("GN03: could not write the world memory to '{}'", opts.memory_path);
    } else {
      utils::info("GN03: world memory saved to '{}': {} entries ({} bytes)", opts.memory_path, memory.size(),
                  16 + memory.size() * 40);
    }
  }
  return code;
}
