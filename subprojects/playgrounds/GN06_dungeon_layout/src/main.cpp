#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
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

#include "motif_demo.h"
#include "motif_viewer.h"
#include "site_graph.h"

// GN06 — ПОСТРОЙКА: КОМНАТЫ И КОРИДОРЫ.
//
// Вопрос площадки: чем генерация ПОСТРОЙКИ отличается от генерации мира, который вырос сам.
//
// Ответ ровно один, и он про род обещания. Шум обещает плавность, вороной — области, заливка по
// графу — достижимость по стоимости, решатель ограничений — жёсткий локальный запрет. Все эти
// обещания ПОЭЛЕМЕНТНЫ: что бы инструмент ни сказал про клетку, он говорит это про клетку и её
// окрестность. У подземелья обещание ГЛОБАЛЬНОЕ — из любой комнаты можно дойти до любой другой, — и
// никакое локальное правило его не даёт: карта, где законна каждая пара соседей, спокойно
// распадается на два отрезанных друг от друга куска, и по виду это не отличить.
//
// ОТСЮДА УСТРОЙСТВО ПЛОЩАДКИ. Раз обещание глобальное, проверять его надо на всей карте сразу —
// заливкой по полу, — и проверять ОТДЕЛЬНО ОТ ПЛАНА. Остов над комнатами и связность растра это
// РАЗНЫЕ утверждения: первое говорит, что связей достаточно, второе — что вырезанное ими проходимо.
// Второе из первого не следует (коридор, уехавший за край карты, обрезается), и площадка ловит
// именно этот разрыв, а не считает его невозможным.
//
// ЧТО ЗДЕСЬ ЕЩЁ НЕ СДЕЛАНО, сказано в README: срез первый, и он про комнаты и коридоры. Двери,
// лабиринт в мёртвом пространстве, ключи и замки (первое ПОРЯДКОВОЕ ограничение), этажи и
// содержательное наполнение комнат — следующие срезы, и каждый из них добавляет обещание, а не
// украшение.

namespace {
namespace fs = std::filesystem;
using namespace devils_engine;

enum class generator_mode { cave, building, motifs, site };

struct options {
  generator_mode mode = generator_mode::building;
  // СТОРОНА УЧАСТКА В МЕРЕ. У норы мера и клетка — одно и то же (в камне вырезают клетки), у здания
  // это два разных числа, и в этом весь смысл второго режима.
  size_t side = 64;
  // Сторона КАРТИНКИ в клетках. Ноль значит «как участок», то есть один к одному.
  size_t view = 0;
  bool view_set = false;
  // ОКНО ПОКАЗА в мере. Отрицательная ширина значит «весь участок»: окно выбирает смотрящий, и
  // отсутствие выбора — это тоже выбор, просто объявленный по умолчанию.
  int64_t window_x = 0;
  int64_t window_y = 0;
  int64_t window_w = -1;
  int64_t window_h = -1;
  bool window_set = false;
  uint64_t seed = 20260912;
  int64_t rooms = -1;          // -1 => как объявлено в конфиге
  int64_t extra_links = -1;
  int64_t corridor_width = -1;
  bool verify = false;
  bool ascii = false;
  std::string dump;
  std::string motif_scale = "medium";
  bool motif_scale_set = false;
  gn06::site_bounds site_bounds;
  bool site_bounds_set = false;
  bool side_set = false;
  int32_t motif_entry_y = -1;
  bool motif_entry_set = false;
  bool surface_cut = false;
  bool surface_set = false;
  bool viewer = false;
  uint32_t viewer_frames = 0;
  bool viewer_frames_set = false;

  size_t view_side() const { return view > 0 ? view : side; }
  int64_t view_w() const { return window_w > 0 ? window_w : int64_t(side); }
  int64_t view_h() const { return window_h > 0 ? window_h : int64_t(side); }
  bool one_to_one() const {
    return view_w() == int64_t(view_side()) && view_h() == int64_t(view_side());
  }
};

fs::path resource_root() {
  return fs::path(GN06_RESOURCE_ROOT);
}

struct generator_registry {
  demiurg::module_system modules;
  demiurg::resource_system resources;
  originator::generator_config dungeon;
  originator::generator_config building;

  generator_registry() : modules(resource_root().generic_string() + "/") {
    modules.load_modules({demiurg::module_system::list_entry{"gn06/", "", ""}});
    originator::register_generator_resources(resources);
    resources.parse_resources(&modules);
    dungeon = originator::load_generator(resources, "generator/dungeon");
    building = originator::load_generator(resources, "generator/building");
  }
};

const generator_registry& generator() {
  static const generator_registry registry;
  return registry;
}

originator::tool_registry& tools() {
  static originator::tool_registry registry;
  if (registry.size() == 0) {
    registry.add_standard_tools();
    // Решатель ограничений приезжает вместе со стандартным набором: виды зон он раскладывает по
    // графу их смежности, и ничего нового для этого не понадобилось — тот же `graph_collapse`, что
    // в GN05 раскладывал тайлы по сфере.
    registry.add_structure_tools();
  }
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
    } else if (argument == "--ascii") {
      result.ascii = true;
    } else if (argument == "--viewer") {
      result.viewer = true;
    } else if (starts_with(argument, "--frames=")) {
      result.viewer_frames = uint32_t(std::stoul(std::string(argument.substr(9))));
      result.viewer_frames_set = true;
    } else if (starts_with(argument, "--mode=")) {
      const auto name = argument.substr(7);
      if (name == "cave") result.mode = generator_mode::cave;
      else if (name == "building") result.mode = generator_mode::building;
      else if (name == "motifs") result.mode = generator_mode::motifs;
      else if (name == "site") result.mode = generator_mode::site;
      else utils::error{}("GN06: unknown mode '{}'", name);
    } else if (starts_with(argument, "--size=")) {
      result.side = std::stoul(std::string(argument.substr(7)));
      result.side_set = true;
    } else if (starts_with(argument, "--view=")) {
      result.view = std::stoul(std::string(argument.substr(7)));
      result.view_set = true;
    } else if (starts_with(argument, "--window=")) {
      // Четыре числа через запятую: окно участка В МЕРЕ. Разбор нарочно строгий — окно из трёх чисел
      // означало бы, что автор имел в виду что-то ещё.
      const auto text = std::string(argument.substr(9));
      int64_t values[4] = {0, 0, 0, 0};
      size_t from = 0;
      size_t taken = 0;
      while (taken < 4 && from <= text.size()) {
        const auto comma = text.find(',', from);
        const auto piece = text.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
        if (piece.empty()) break;
        values[taken++] = std::stoll(piece);
        if (comma == std::string::npos) break;
        from = comma + 1;
      }
      if (taken != 4) {
        utils::error{}("GN06: --window wants four numbers x,y,w,h in plot units, got '{}'", text);
      }
      result.window_x = values[0];
      result.window_y = values[1];
      result.window_w = values[2];
      result.window_h = values[3];
      result.window_set = true;
    } else if (starts_with(argument, "--seed=")) {
      result.seed = std::stoull(std::string(argument.substr(7)));
    } else if (starts_with(argument, "--rooms=")) {
      result.rooms = std::stoll(std::string(argument.substr(8)));
    } else if (starts_with(argument, "--loops=")) {
      result.extra_links = std::stoll(std::string(argument.substr(8)));
    } else if (starts_with(argument, "--corridor=")) {
      result.corridor_width = std::stoll(std::string(argument.substr(11)));
    } else if (starts_with(argument, "--dump=")) {
      result.dump = std::string(argument.substr(7));
    } else if (starts_with(argument, "--scale=")) {
      result.motif_scale = std::string(argument.substr(8));
      result.motif_scale_set = true;
    } else if (starts_with(argument, "--bounds=")) {
      const auto text = argument.substr(9);
      const auto comma = text.find(',');
      if (comma == std::string_view::npos) {
        utils::error{}("GN06: --bounds wants width,height in plot units");
      }
      const auto parse_dimension = [&](const std::string_view number) {
        uint32_t dimension = 0;
        const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), dimension);
        if (error != std::errc{} || end != number.data() + number.size() || dimension < 32 || dimension > 4096) {
          utils::error{}("GN06: --bounds dimensions must be in 32..4096, got '{}'", number);
        }
        return dimension;
      };
      result.site_bounds = {parse_dimension(text.substr(0, comma)), parse_dimension(text.substr(comma + 1))};
      result.site_bounds_set = true;
    } else if (starts_with(argument, "--entry-y=")) {
      result.motif_entry_y = std::stoi(std::string(argument.substr(10)));
      result.motif_entry_set = true;
    } else if (argument == "--surface=cut") {
      result.surface_cut = true;
      result.surface_set = true;
    } else if (argument == "--surface=flat") {
      result.surface_cut = false;
      result.surface_set = true;
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "GN06 dungeon layout lab\n"
                << "  --mode=NAME     building (по умолчанию), cave, motifs, site (граф одной локации)\n"
                << "  --scale=NAME    small, medium или large для motifs/site\n"
                << "  --bounds=W,H    жёсткий прямоугольный участок site, независимо от --scale\n"
                << "  --entry-y=N     высота основного входа на западном краю (motifs/site)\n"
                << "  --surface=NAME  flat или cut: синтетическое пересечение с поверхностью (motifs/site)\n"
                << "  --viewer        интерактивный 2D-просмотр motifs/site (N/B seed, Tab части, R маршрут, PgUp/PgDn свойства)\n"
                << "  --frames=N      закрыть просмотрщик после N кадров (для smoke-проверки)\n"
                << "  --size=N        сторона УЧАСТКА в мере (по умолчанию 64)\n"
                << "  --view=N        сторона картинки в клетках (по умолчанию как участок, один к одному)\n"
                << "  --window=X,Y,W,H окно участка в мере (по умолчанию весь участок)\n"
                << "  --seed=N        зерно пайплайна\n"
                << "  --rooms=N       ёмкость списка комнат (переопределяет конфиг)\n"
                << "  --loops=N       сколько петель добавить поверх остова\n"
                << "  --corridor=N    толщина коридора в клетках\n"
                << "  --ascii         напечатать карту в терминал\n"
                << "  --dump=PATH     сохранить карту в PPM — ДЛЯ ГЛАЗА, не для конвейера\n"
                << "  --verify        прогнать контрактные проверки\n";
      std::exit(0);
    } else {
      utils::error{}("GN06: unknown argument '{}'", argument);
    }
  }
  if (result.side < 8) {
    utils::error{}("GN06: a plot smaller than eight units holds no room with a wall around it");
  }
  if (result.view_side() < 8) {
    utils::error{}("GN06: a picture smaller than eight cells shows nothing worth looking at");
  }
  if (result.window_w == 0 || result.window_h == 0) {
    utils::error{}("GN06: an empty window shows an empty picture, which is not a question worth asking");
  }
  return result;
}

int64_t building_number(const char* name) {
  const auto value = generator().building.description.values.integer(name, -1);
  if (value < 0) {
    utils::error{}("GN06: the building config declares no {}", name);
  }
  return value;
}

int64_t declared_number(const char* name) {
  const auto value = generator().dungeon.description.values.integer(name, 0);
  if (value <= 0) {
    utils::error{}("GN06: the config declares no {}", name);
  }
  return value;
}

size_t room_capacity(const options& opts) {
  return opts.rooms > 0 ? size_t(opts.rooms) : size_t(declared_number("room_capacity"));
}

size_t wanted_loops(const options& opts) {
  const auto declared = generator().dungeon.description.values.integer("extra_links", 0);
  return size_t(std::max<int64_t>(opts.extra_links >= 0 ? opts.extra_links : declared, 0));
}

size_t link_capacity(const options& opts) {
  return room_capacity(opts) + wanted_loops(opts);
}

// Ёмкость СВЯЗЕЙ считается, а не объявляется: остов это M-1 связей, петли добираются поверх, значит
// ёмкости комнат плюс число петель хватает всегда. Отдельное имя существует ради проверки ОТКАЗА:
// урезанная ёмкость обязана давать громкий отказ, а не подземелье с отрезанными комнатами.
originator::size_table make_sizes(const options& opts, const size_t link_capacity) {
  originator::size_table sizes;
  sizes.set("single", 1);
  sizes.set("side", opts.side);
  sizes.set("room_capacity", room_capacity(opts));
  sizes.set("link_capacity", link_capacity);
  return sizes;
}

originator::pipeline_description load_description(const options& opts) {
  auto description = generator().dungeon.description;
  // Сторона растра — ОДНО число на два объявления: форму буфера задаёт таблица размеров, а границы
  // плана приходят значением. Оба берутся здесь; совпадение проверяет шаг carve.
  description.values.set_number("side", double(opts.side));
  if (opts.extra_links >= 0) {
    description.values.set_number("extra_links", double(opts.extra_links));
  }
  if (opts.corridor_width > 0) {
    description.values.set_number("corridor_width", double(opts.corridor_width));
  }
  return description;
}

double run_pipeline(originator::pipeline& p, const originator::pipeline_description& description) {
  originator::script_host host(tools(), nullptr);
  for (const auto& step : description.steps) {
    host.load_body(step.name, generator().dungeon.source(step.body), step.body);
  }
  const auto start = std::chrono::steady_clock::now();
  p.run(host.invoker());
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::shared_ptr<originator::pipeline> run_once(const options& opts, double& milliseconds,
                                               const size_t link_capacity) {
  const auto description = load_description(opts);
  auto p = std::make_shared<originator::pipeline>(description, make_sizes(opts, link_capacity), opts.seed);
  milliseconds = run_pipeline(*p, description);
  return p;
}

double read_one(originator::pipeline& p, const char* buffer, const char* field) {
  auto* source = p.find_buffer(buffer);
  return source->field(source->find_field(field)).get(0);
}

struct room {
  int64_t x = 0;
  int64_t y = 0;
  int64_t w = 0;
  int64_t h = 0;

  int64_t centre_x() const { return x + w / 2; }
  int64_t centre_y() const { return y + h / 2; }
  int64_t area() const { return w * h; }
};

std::vector<room> read_rooms(originator::pipeline& p) {
  const auto* buffer = p.find_buffer("rooms");
  const auto rect = buffer->field(buffer->find_field("rect"));
  const auto count = size_t(read_one(p, "state", "rooms"));
  std::vector<room> result(count);
  for (size_t i = 0; i < count; ++i) {
    result[i] = room{int64_t(rect.get(i, 0)), int64_t(rect.get(i, 1)),
                     int64_t(rect.get(i, 2)), int64_t(rect.get(i, 3))};
  }
  return result;
}

std::vector<std::pair<uint32_t, uint32_t>> read_links(originator::pipeline& p) {
  const auto* buffer = p.find_buffer("links");
  const auto pair = buffer->field(buffer->find_field("pair"));
  const auto count = size_t(read_one(p, "state", "links"));
  std::vector<std::pair<uint32_t, uint32_t>> result(count);
  for (size_t i = 0; i < count; ++i) {
    result[i] = {uint32_t(pair.get(i, 0)), uint32_t(pair.get(i, 1))};
  }
  return result;
}

// Карта пола: номер комнаты плюс один, коридор отдельным признаком. Читается ОДИН раз и отдаётся
// всем проверкам сразу — иначе каждая из них читала бы буфер по-своему.
struct map_view {
  std::vector<uint32_t> room;
  std::vector<uint8_t> corridor;
  size_t side = 0;

  bool floor(const size_t index) const { return room[index] != 0 || corridor[index] != 0; }
  size_t floor_count() const {
    size_t total = 0;
    for (size_t i = 0; i < room.size(); ++i) {
      total += size_t(floor(i));
    }
    return total;
  }
};

map_view read_map(originator::pipeline& p, const size_t side) {
  const auto* cells = p.find_buffer("cells");
  const auto room_field = cells->field(cells->find_field("room"));
  const auto corridor_field = cells->field(cells->find_field("corridor"));
  map_view view;
  view.side = side;
  view.room.resize(cells->count());
  view.corridor.resize(cells->count());
  for (size_t i = 0; i < cells->count(); ++i) {
    view.room[i] = uint32_t(room_field.get(i));
    view.corridor[i] = uint8_t(corridor_field.get(i));
  }
  return view;
}

// ГЛАВНОЕ ОБЕЩАНИЕ ПЛОЩАДКИ, проверенное на РАСТРЕ, а не на плане: заливка по четырём соседям от
// первой клетки пола обязана накрыть весь пол. Это и есть «из любой комнаты можно дойти до любой».
size_t unreachable_floor(const map_view& view) {
  const size_t count = view.room.size();
  size_t start = count;
  for (size_t i = 0; i < count && start == count; ++i) {
    if (view.floor(i)) start = i;
  }
  if (start == count) {
    return 0;
  }

  std::vector<uint8_t> seen(count, 0);
  std::vector<size_t> stack{start};
  seen[start] = 1;
  size_t reached = 0;
  while (!stack.empty()) {
    const auto index = stack.back();
    stack.pop_back();
    ++reached;

    const int64_t x = int64_t(index % view.side);
    const int64_t y = int64_t(index / view.side);
    const int64_t steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& step : steps) {
      const int64_t nx = x + step[0];
      const int64_t ny = y + step[1];
      if (nx < 0 || ny < 0 || nx >= int64_t(view.side) || ny >= int64_t(view.side)) continue;
      const auto next = size_t(ny) * view.side + size_t(nx);
      if (seen[next] != 0 || !view.floor(next)) continue;
      seen[next] = 1;
      stack.push_back(next);
    }
  }
  return view.floor_count() - reached;
}

// То же обещание, но на ПЛАНЕ: связи обязаны соединять все комнаты в одно целое. Утверждение это
// другое, и проверяется оно другим счётом — по списку связей, а не по клеткам.
size_t plan_components(const std::vector<room>& rooms, const std::vector<std::pair<uint32_t, uint32_t>>& links) {
  if (rooms.empty()) {
    return 0;
  }
  std::vector<uint32_t> parent(rooms.size());
  for (size_t i = 0; i < parent.size(); ++i) {
    parent[i] = uint32_t(i);
  }
  const std::function<uint32_t(uint32_t)> root = [&](const uint32_t node) {
    return parent[node] == node ? node : parent[node] = root(parent[node]);
  };
  for (const auto& link : links) {
    if (link.first >= rooms.size() || link.second >= rooms.size()) continue;
    parent[root(link.first)] = root(link.second);
  }
  size_t components = 0;
  for (size_t i = 0; i < parent.size(); ++i) {
    components += size_t(root(uint32_t(i)) == i);
  }
  return components;
}

void print_ascii(const map_view& view) {
  for (size_t y = 0; y < view.side; ++y) {
    std::string line(view.side, ' ');
    for (size_t x = 0; x < view.side; ++x) {
      const auto index = y * view.side + x;
      line[x] = view.room[index] != 0 ? '.' : (view.corridor[index] != 0 ? '+' : ' ');
    }
    std::cout << line << "\n";
  }
}

bool write_ppm(const std::string& path, const map_view& view, const size_t scale) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  const auto colour_of = [&](const size_t index) -> uint32_t {
    if (view.room[index] != 0) {
      // Цвет комнаты — от её номера: соседние комнаты обязаны отличаться, а какой именно оттенок,
      // значения не имеет.
      const uint32_t tone = (view.room[index] * 2654435761u) >> 8;
      return 0x404040u | (tone & 0x7f7f7fu);
    }
    return view.corridor[index] != 0 ? 0x9a8c6eu : 0x14161cu;
  };

  out << "P6\n" << view.side * scale << " " << view.side * scale << "\n255\n";
  for (size_t y = 0; y < view.side * scale; ++y) {
    for (size_t x = 0; x < view.side * scale; ++x) {
      const auto colour = colour_of((y / scale) * view.side + (x / scale));
      out.put(char((colour >> 16) & 255u));
      out.put(char((colour >> 8) & 255u));
      out.put(char(colour & 255u));
    }
  }
  return bool(out);
}

int run_report(const options& opts) {
  double milliseconds = 0.0;
  auto p = run_once(opts, milliseconds, link_capacity(opts));

  const auto rooms = read_rooms(*p);
  const auto links = read_links(*p);
  const auto view = read_map(*p, opts.side);
  const auto floor = view.floor_count();

  const size_t tree_links = rooms.empty() ? 0 : rooms.size() - 1;
  const size_t loops = links.size() > tree_links ? links.size() - tree_links : 0;
  int64_t room_cells = 0;
  for (const auto& r : rooms) {
    room_cells += r.area();
  }

  std::cout << "GN06: зерно " << opts.seed << ", растр " << opts.side << "x" << opts.side << "\n"
            << "  комнат             " << rooms.size() << " из " << room_capacity(opts)
            << " объявленных, попыток " << read_one(*p, "state", "attempts") << "\n"
            << "  связей             " << links.size() << " (остов " << tree_links << " + петель " << loops
            << " из " << wanted_loops(opts) << " запрошенных)\n"
            << "  пол                " << floor << " клеток ("
            << (100.0 * double(floor) / double(view.room.size())) << "% растра), из них комнаты "
            << room_cells << "\n"
            << "  кусков плана       " << plan_components(rooms, links) << "\n"
            << "  недостижимый пол   " << unreachable_floor(view) << " клеток\n"
            << "  собрано за         " << milliseconds << " мс\n"
            << "  память пайплайна   " << (double(p->total_byte_size()) / 1024.0) << " КиБ\n";

  if (opts.ascii) {
    print_ascii(view);
  }
  if (!opts.dump.empty()) {
    if (!write_ppm(opts.dump, view, 6)) {
      utils::error{}("GN06: could not write '{}'", opts.dump);
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

int run_verify(const options& opts) {
  checker check;
  const auto capacity = room_capacity(opts);
  const auto min_room = declared_number("min_room");
  const auto max_room = declared_number("max_room");
  const auto gap = generator().dungeon.description.values.integer("gap", 0);
  const auto border = generator().dungeon.description.values.integer("border", 0);

  double milliseconds = 0.0;
  auto first = run_once(opts, milliseconds, link_capacity(opts));
  const auto rooms = read_rooms(*first);
  const auto links = read_links(*first);
  const auto view = read_map(*first, opts.side);

  std::cout << "GN06 verify: растр " << opts.side << "x" << opts.side << ", комнат " << rooms.size()
            << " из " << capacity << ", связей " << links.size() << ", собрано за " << milliseconds << " мс\n";

  // 1. ПЛАН ЗАКОНЕН САМ ПО СЕБЕ: комнаты объявленных размеров, внутри границ с отступом.
  bool sized = !rooms.empty();
  bool inside = true;
  for (const auto& r : rooms) {
    sized = sized && r.w >= min_room && r.w <= max_room && r.h >= min_room && r.h <= max_room;
    inside = inside && r.x >= border && r.y >= border && r.x + r.w <= int64_t(opts.side) - border &&
             r.y + r.h <= int64_t(opts.side) - border;
  }
  check(sized, "каждая комната объявленного размера");
  check(inside, "каждая комната внутри растра с объявленным отступом");

  // 2. ЗАЗОР СОБЛЮДЁН. Без него комнаты сливаются на растре в одну странной формы, и план перестаёт
  //    описывать вырезанное — при этом ни одна другая проверка этого не заметит.
  size_t touching = 0;
  for (size_t a = 0; a < rooms.size(); ++a) {
    for (size_t b = a + 1; b < rooms.size(); ++b) {
      const auto& first_room = rooms[a];
      const auto& second = rooms[b];
      touching += size_t(first_room.x < second.x + second.w + gap && second.x < first_room.x + first_room.w + gap &&
                         first_room.y < second.y + second.h + gap && second.y < first_room.y + first_room.h + gap);
    }
  }
  check(touching == 0, "никакие две комнаты не ближе объявленного зазора");

  // 3. ОСТОВ НАД КОМНАТАМИ: связи соединяют план в ОДИН кусок. Это обещание шага links, и живёт оно
  //    на графе.
  check(plan_components(rooms, links) == 1, "связи соединяют все комнаты в один кусок");
  check(links.size() == rooms.size() - 1 + wanted_loops(opts), "связей ровно остов плюс запрошенные петли");

  // 4. РАСТР СОВПАДАЕТ С ПЛАНОМ: каждая клетка каждой комнаты помечена её номером, и помеченных
  //    клеток ровно столько, сколько площади у комнат. Первое ловит несовпадение границ, второе —
  //    лишние пометки.
  bool carved = true;
  int64_t area = 0;
  for (size_t i = 0; i < rooms.size(); ++i) {
    const auto& r = rooms[i];
    area += r.area();
    for (int64_t y = r.y; y < r.y + r.h && carved; ++y) {
      for (int64_t x = r.x; x < r.x + r.w && carved; ++x) {
        carved = view.room[size_t(y) * opts.side + size_t(x)] == uint32_t(i + 1);
      }
    }
  }
  size_t marked = 0;
  for (const auto value : view.room) {
    marked += size_t(value != 0);
  }
  check(carved, "каждая клетка комнаты помечена её номером");
  check(marked == size_t(area), "помеченных клеток ровно столько, сколько площади у комнат");

  // 5. ГЛАВНОЕ ОБЕЩАНИЕ, И ОНО НЕ СЛЕДУЕТ ИЗ ТРЕТЬЕГО: пол связен НА РАСТРЕ. Остов говорит, что
  //    связей достаточно; связность растра говорит, что вырезанное ими проходимо. Между этими
  //    утверждениями лежит вырезание, и обрезанный по краю коридор рвёт второе, не трогая первое.
  check(unreachable_floor(view) == 0, "весь пол достижим от любой его клетки");

  // 6. ПОДЗЕМЕЛЬЕ СОДЕРЖАТЕЛЬНО. Одна комната на всю карту тоже связна, но задачи не решает.
  check(rooms.size() >= 4, "комнат набралось больше горстки");
  size_t corridor_cells = 0;
  for (const auto value : view.corridor) {
    corridor_cells += size_t(value != 0);
  }
  check(corridor_cells > 0, "коридоры вырезаны");

  // 7. ПОВТОРЯЕМОСТЬ. Ни одно решение здесь не принимается сравнением плавающих чисел: размеры и
  //    координаты целые, расстояние манхэттенское, выбор — остаток от деления хеша.
  double again_ms = 0.0;
  auto again = run_once(opts, again_ms, link_capacity(opts));
  check(read_map(*again, opts.side).room == view.room, "то же зерно даёт то же подземелье");

  auto other_opts = opts;
  other_opts.seed += 1;
  auto other = run_once(other_opts, again_ms, link_capacity(opts));
  check(read_map(*other, opts.side).room != view.room, "другое зерно даёт другое подземелье");

  // 8. ЁМКОСТЬ — ЖЕЛАНИЕ, СЧЁТЧИК — ФАКТ. За счётчиком в списке лежат нули, и инструмент, который
  //    прочитал бы ёмкость вместо счётчика, получил бы комнату нулевого размера в углу.
  const auto* rooms_buffer = first->find_buffer("rooms");
  const auto rect = rooms_buffer->field(rooms_buffer->find_field("rect"));
  bool tail_empty = rooms.size() <= capacity;
  for (size_t i = rooms.size(); i < capacity; ++i) {
    tail_empty = tail_empty && rect.get(i, 2) == 0.0 && rect.get(i, 3) == 0.0;
  }
  check(tail_empty, "за счётчиком список комнат пуст");

  // 9. ГРОМКИЙ ОТКАЗ ВМЕСТО ОТРЕЗАННЫХ КОМНАТ. Ёмкости связей, которой не хватает на остов,
  //    достаточно, чтобы собрать карту — и карта будет выглядеть обычной картой, просто пройти её
  //    целиком нельзя. Поэтому это отказ, а не обрезание.
  std::cout << "  (ниже ОЖИДАЕМЫЙ отказ: ёмкость связей урезана нарочно)\n";
  bool refused = false;
  try {
    double ignored = 0.0;
    run_once(opts, ignored, 2);
  } catch (const std::exception&) {
    refused = true;
  }
  check(refused, "ёмкость связей меньше остова — громкий отказ");

  std::cout << "GN06 verify: " << (check.checks - check.failures) << "/" << check.checks << "\n";
  return check.failures == 0 ? 0 : 1;
}
// ============================== РЕЖИМ «ЗДАНИЕ» ==============================
//
// Здесь у площадки другая постановка, пришедшая из PF09: деления на комнаты и коридоры нет, есть
// ЗОНЫ и их ВИДЫ. Коридор — вид без предела связей, кухня — вид, которому дверь разрешена в зал и в
// кладовую, но не в покои. Дверь — тоже зона.
//
// И проверяется здесь другое. У норы обещание было одно (весь пол достижим); у здания их два и они
// живут в РАЗНЫХ представлениях: связность по СВЯЗЯМ (граф мест) и связность по РАСТРУ (заливка).
// PF09 доказала, что эти два не следуют друг из друга — бывает связь без общего ребра (лестница) и
// общее ребро без связи (перекрытие), — поэтому здесь, где они ОБЯЗАНЫ совпадать, совпадение
// проверяется отдельной проверкой, а не считается самоочевидным.

struct zone_record {
  int64_t x = 0;
  int64_t y = 0;
  int64_t w = 0;
  int64_t h = 0;
  uint32_t kind = 0;

  int64_t area() const { return w * h; }
  bool operator==(const zone_record& other) const noexcept = default;
};

struct link_record {
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t door = 0; // номер зоны-двери плюс один; ноль — широкий проём
  uint32_t spare = 0;
};

struct kind_record {
  uint32_t glyph = 0;
  uint32_t colour = 0;
  uint32_t max_links = 0;
  bool passable = false;
};

// Колонна — кусок стены, а не зона: по ней не ходят, связей у неё нет, вид ей не нужен. Поэтому и
// список отдельный, и запись в нём короче зоны ровно на всё, чем зона отличается от прямоугольника.
struct column_record {
  int64_t x = 0;
  int64_t y = 0;
  int64_t w = 0;
  int64_t h = 0;

  bool operator==(const column_record& other) const noexcept = default;
};

struct building {
  std::vector<zone_record> zones;   // включая двери
  std::vector<link_record> links;
  std::vector<kind_record> kinds;
  std::vector<column_record> columns;
  std::vector<uint32_t> cells;      // КАРТИНКА: номер зоны плюс один, ноль — стена
  std::vector<uint8_t> door_rules;
  std::vector<uint8_t> adjacent_rules;
  std::vector<uint8_t> required_rules;
  size_t side = 0;                  // сторона КАРТИНКИ в клетках
  size_t plot = 0;                  // сторона УЧАСТКА в мере
  int64_t view_x = 0;
  int64_t view_y = 0;
  int64_t view_w = 0;
  int64_t view_h = 0;
  size_t planned = 0;               // зон до дверей
  size_t joins = 0;
  size_t direct_joins = 0;
  size_t lost = 0;                  // мест в окне, не попавших ни в одну клетку
  size_t merged = 0;                // границ, где масштаб съел стену
  double milliseconds = 0.0;
  double memory_kib = 0.0;

  // КАРТИНКА — ЭТО ПЛАН один в один ровно тогда, когда она ничего не потеряла и ничего не слепила.
  // Только при этом условии заливка по клеткам говорит что-то о плане, а не о масштабе.
  bool faithful() const { return lost == 0 && merged == 0; }

  size_t kind_count() const { return kinds.size(); }
  bool passable_zone(const size_t zone) const {
    return zone < zones.size() && zones[zone].kind < kinds.size() && kinds[zones[zone].kind].passable;
  }
  bool passable_cell(const size_t index) const {
    const auto raw = cells[index];
    return raw != 0 && passable_zone(raw - 1);
  }
  bool door_allowed(const uint32_t a, const uint32_t b) const {
    return door_rules[a * kind_count() + b] != 0;
  }
  bool adjacent_allowed(const uint32_t a, const uint32_t b) const {
    return adjacent_rules[a * kind_count() + b] != 0;
  }
  bool required_door(const uint32_t a, const uint32_t b) const {
    return required_rules[a * kind_count() + b] != 0;
  }
};

originator::size_table building_sizes(const options& opts) {
  const auto kinds = size_t(building_number("kind_count"));

  // ЁМКОСТЬ ОБЪЯВЛЯЕТ ПЛОЩАДКА, потому что только она знает, какое пятно попросили ключом. Оценка
  // берётся ВЕРХНЯЯ и из тех же чисел, которыми режет раздел: мест не больше, чем поместится
  // прямоугольников минимального размера, а дверей не больше, чем мест. Числа из конфига остаются
  // ПОЛОМ: маленькое пятно не должно получать ёмкость меньше объявленной.
  const auto smallest = size_t(std::max<int64_t>(building_number("min_room") * building_number("min_band"), 1));
  const auto places = opts.side * opts.side / smallest + 8;
  const auto zones = std::max(size_t(building_number("zone_capacity")), 3 * places);

  originator::size_table sizes;
  sizes.set("single", 1);
  sizes.set("side", opts.side);
  // СТОРОНА КАРТИНКИ — ОТДЕЛЬНОЕ ЧИСЛО, и это вся суть переезда: форма буфера клеток больше не
  // имеет отношения к тому, какой участок нарезали.
  sizes.set("view", opts.view_side());
  sizes.set("column_capacity", std::max(size_t(building_number("column_capacity")), 2 * places));
  sizes.set("kind_count", kinds);
  sizes.set("rule_size", kinds * kinds);
  sizes.set("zone_capacity", zones);
  sizes.set("zone_offset_count", zones + 1);
  sizes.set("zone_arc_capacity", std::max(size_t(building_number("zone_arc_capacity")), 12 * places));
  sizes.set("join_capacity", std::max(size_t(building_number("join_capacity")), 8 * places));
  sizes.set("link_capacity", std::max(size_t(building_number("link_capacity")), 4 * places));
  return sizes;
}

originator::pipeline_description building_description(const options& opts) {
  auto description = generator().building.description;
  // УЧАСТОК И ОКНО — ЗНАЧЕНИЯ ПАЙПЛАЙНА. Участок знает площадка (его попросили ключом), окно выбирает
  // смотрящий; генератору обоих хватает, и растр в этих числах не участвует.
  description.values.set_number("plot_width", double(opts.side));
  description.values.set_number("plot_height", double(opts.side));
  description.values.set_number("view_x", double(opts.window_x));
  description.values.set_number("view_y", double(opts.window_y));
  description.values.set_number("view_w", double(opts.view_w()));
  description.values.set_number("view_h", double(opts.view_h()));
  if (opts.extra_links >= 0) {
    description.values.set_number("extra_doors", double(opts.extra_links));
  }
  if (opts.corridor_width > 0) {
    description.values.set_number("corridor", double(opts.corridor_width));
  }
  return description;
}

building run_building(const options& opts) {
  const auto description = building_description(opts);
  originator::pipeline p(description, building_sizes(opts), opts.seed);

  originator::script_host host(tools(), nullptr);
  for (const auto& step : description.steps) {
    host.load_body(step.name, generator().building.source(step.body), step.body);
  }
  const auto start = std::chrono::steady_clock::now();
  p.run(host.invoker());

  building result;
  result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  result.side = opts.view_side();
  result.plot = opts.side;
  result.view_x = opts.window_x;
  result.view_y = opts.window_y;
  result.view_w = opts.view_w();
  result.view_h = opts.view_h();
  result.memory_kib = double(p.total_byte_size()) / 1024.0;

  const auto one = [&](const char* buffer, const char* field) { return size_t(read_one(p, buffer, field)); };
  result.planned = one("state", "zones");
  result.joins = one("state", "joins");
  result.lost = one("state", "lost");
  result.merged = one("state", "merged");

  const auto* column_list = p.find_buffer("columns");
  const auto column_rect = column_list->field(column_list->find_field("rect"));
  result.columns.resize(one("state", "columns"));
  for (size_t i = 0; i < result.columns.size(); ++i) {
    result.columns[i] = column_record{int64_t(column_rect.get(i, 0)), int64_t(column_rect.get(i, 1)),
                                      int64_t(column_rect.get(i, 2)), int64_t(column_rect.get(i, 3))};
  }

  const auto* zones = p.find_buffer("zones");
  const auto rect = zones->field(zones->find_field("rect"));
  const auto kind = zones->field(zones->find_field("kind"));
  const auto places = one("state", "places");
  result.zones.resize(places);
  for (size_t i = 0; i < places; ++i) {
    result.zones[i] = zone_record{int64_t(rect.get(i, 0)), int64_t(rect.get(i, 1)), int64_t(rect.get(i, 2)),
                                  int64_t(rect.get(i, 3)), uint32_t(kind.get(i))};
  }

  const auto* links = p.find_buffer("links");
  const auto edge = links->field(links->find_field("edge"));
  const auto link_count = one("state", "links");
  result.links.resize(link_count);
  for (size_t i = 0; i < link_count; ++i) {
    result.links[i] = link_record{uint32_t(edge.get(i, 0)), uint32_t(edge.get(i, 1)), uint32_t(edge.get(i, 2)),
                                  uint32_t(edge.get(i, 3))};
  }

  const auto* kinds = p.find_buffer("kinds");
  const auto glyph = kinds->field(kinds->find_field("glyph"));
  const auto colour = kinds->field(kinds->find_field("colour"));
  const auto max_links = kinds->field(kinds->find_field("max_links"));
  const auto passable = kinds->field(kinds->find_field("passable"));
  result.kinds.resize(kinds->count());
  for (size_t i = 0; i < result.kinds.size(); ++i) {
    result.kinds[i] = kind_record{uint32_t(glyph.get(i)), uint32_t(colour.get(i)), uint32_t(max_links.get(i)),
                                  passable.get(i) != 0.0};
  }

  const auto* cells = p.find_buffer("cells");
  const auto zone_field = cells->field(cells->find_field("zone"));
  result.cells.resize(cells->count());
  for (size_t i = 0; i < result.cells.size(); ++i) {
    result.cells[i] = uint32_t(zone_field.get(i));
  }

  const auto read_rules = [&](const char* buffer) {
    const auto* source = p.find_buffer(buffer);
    const auto allowed = source->field(source->find_field("allowed"));
    std::vector<uint8_t> table(source->count());
    for (size_t i = 0; i < table.size(); ++i) {
      table[i] = uint8_t(allowed.get(i));
    }
    return table;
  };
  result.door_rules = read_rules("door_rules");
  result.adjacent_rules = read_rules("adjacent_rules");
  result.required_rules = read_rules("required_doors");

  const auto* joins = p.find_buffer("joins");
  const auto pair = joins->field(joins->find_field("pair"));
  for (size_t i = 0; i < result.joins; ++i) {
    result.direct_joins += size_t(pair.get(i, 2) == 0.0);
  }
  return result;
}

// СВЯЗНОСТЬ ПО СВЯЗЯМ: кусков графа мест. Утверждение о ПЛАНЕ, и считается оно по списку связей —
// том самом, который прочитает игра.
size_t link_components(const building& house, const std::vector<size_t>& skip, const uint32_t door_kind) {
  std::vector<uint32_t> parent(house.zones.size());
  for (size_t i = 0; i < parent.size(); ++i) {
    parent[i] = uint32_t(i);
  }
  const auto root = [&](uint32_t node) {
    while (parent[node] != node) {
      parent[node] = parent[parent[node]];
      node = parent[node];
    }
    return node;
  };

  for (size_t i = 0; i < house.links.size(); ++i) {
    if (std::find(skip.begin(), skip.end(), i) != skip.end()) {
      continue;
    }
    const auto& link = house.links[i];
    parent[root(link.a)] = root(link.b);
    if (link.door != 0) {
      // Дверь — МЕСТО на пути: связь на самом деле «зона — дверь — зона», и в куске графа она тоже
      // участвует. Иначе запертая дверь была бы неотличима от отсутствующей.
      parent[root(link.door - 1)] = root(link.a);
    }
  }

  // Считаются МЕСТА, а не двери: дверь это проход, и закрытая дверь обязана оставлять достижимым всё
  // остальное — вот это и проверяется. Сама закрытая дверь при этом недостижима по определению.
  std::set<uint32_t> roots;
  for (size_t i = 0; i < house.zones.size(); ++i) {
    if (!house.passable_zone(i) || house.zones[i].kind == door_kind) {
      continue;
    }
    roots.insert(root(uint32_t(i)));
  }
  return roots.size();
}

// СВЯЗНОСТЬ ПО РАСТРУ: заливка по четырём соседям. Утверждение о ГЕОМЕТРИИ, и между ним и предыдущим
// лежит вырезание дверей.
//
// `interesting` выбирает, что считать: проходимое, стену или стену ДО ДВЕРЕЙ. Последнее — не
// причуда: стена это дополнение, и пока дверей нет, она связна, то есть это ОДНО место. Дверь режет
// не только проход, но и стену, и после неё кусков становится несколько. Связность стены до дверей —
// свойство РАЗДЕЛА, и проверять надо именно его.
enum class cell_class { passable, wall, wall_before_doors };

size_t unreachable_cells(const building& house, const cell_class wanted, const uint32_t door_kind = 0) {
  const size_t count = house.cells.size();
  const auto interesting = [&](const size_t index) {
    const auto raw = house.cells[index];
    switch (wanted) {
      case cell_class::passable: return house.passable_cell(index);
      case cell_class::wall: return raw == 0;
      default: return raw == 0 || house.zones[raw - 1].kind == door_kind;
    }
  };

  size_t total = 0;
  size_t start = count;
  for (size_t i = 0; i < count; ++i) {
    if (!interesting(i)) continue;
    ++total;
    if (start == count) start = i;
  }
  if (start == count) {
    return 0;
  }

  std::vector<uint8_t> seen(count, 0);
  std::vector<size_t> stack{start};
  seen[start] = 1;
  size_t reached = 0;
  while (!stack.empty()) {
    const auto index = stack.back();
    stack.pop_back();
    ++reached;
    const int64_t x = int64_t(index % house.side);
    const int64_t y = int64_t(index / house.side);
    const int64_t steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& step : steps) {
      const int64_t nx = x + step[0];
      const int64_t ny = y + step[1];
      if (nx < 0 || ny < 0 || nx >= int64_t(house.side) || ny >= int64_t(house.side)) continue;
      const auto next = size_t(ny) * house.side + size_t(nx);
      if (seen[next] != 0 || !interesting(next)) continue;
      seen[next] = 1;
      stack.push_back(next);
    }
  }
  return total - reached;
}

// Сколько кусков у стены. После дверей их несколько, и это не дефект, а факт, который стоит видеть:
// как только стена понадобится ЗОНОЙ (владелец, видимость, баррикада), мест будет столько.
//
// `door_kind` отличает две разные величины: с ним считается стена ДО дверей (клетки дверей ещё
// стена), без него — то, что осталось после. Первая — свойство РАЗДЕЛА и колоннад, вторая — факт.
size_t wall_pieces(const building& house, const uint32_t door_kind = 0, const bool before_doors = false) {
  const auto is_wall = [&](const size_t index) {
    const auto raw = house.cells[index];
    return raw == 0 || (before_doors && house.zones[raw - 1].kind == door_kind);
  };

  std::vector<uint8_t> seen(house.cells.size(), 0);
  size_t pieces = 0;
  for (size_t start = 0; start < house.cells.size(); ++start) {
    if (!is_wall(start) || seen[start] != 0) {
      continue;
    }
    ++pieces;
    std::vector<size_t> stack{start};
    seen[start] = 1;
    while (!stack.empty()) {
      const auto index = stack.back();
      stack.pop_back();
      const int64_t x = int64_t(index % house.side);
      const int64_t y = int64_t(index / house.side);
      const int64_t steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& step : steps) {
        const int64_t nx = x + step[0];
        const int64_t ny = y + step[1];
        if (nx < 0 || ny < 0 || nx >= int64_t(house.side) || ny >= int64_t(house.side)) continue;
        const auto next = size_t(ny) * house.side + size_t(nx);
        if (seen[next] != 0 || !is_wall(next)) continue;
        seen[next] = 1;
        stack.push_back(next);
      }
    }
  }
  return pieces;
}


// СКОЛЬКО КУСКОВ У КАЖДОГО МЕСТА. С появлением форм это отдельное обещание: срез, скругление или
// колоннада обязаны МЕНЯТЬ место, а не разрывать его. Эллипс в слишком узком прямоугольнике,
// чрезмерный срез, колонна, севшая поперёк, — всё это выглядит на карте правдоподобно и ловится
// только счётом кусков.
size_t zones_in_pieces(const building& house) {
  std::vector<uint8_t> seen(house.cells.size(), 0);
  std::vector<size_t> pieces(house.zones.size(), 0);

  for (size_t start = 0; start < house.cells.size(); ++start) {
    const auto raw = house.cells[start];
    if (raw == 0 || seen[start] != 0) {
      continue;
    }
    pieces[raw - 1] += 1;

    std::vector<size_t> stack{start};
    seen[start] = 1;
    while (!stack.empty()) {
      const auto index = stack.back();
      stack.pop_back();
      const int64_t x = int64_t(index % house.side);
      const int64_t y = int64_t(index / house.side);
      const int64_t steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& step : steps) {
        const int64_t nx = x + step[0];
        const int64_t ny = y + step[1];
        if (nx < 0 || ny < 0 || nx >= int64_t(house.side) || ny >= int64_t(house.side)) continue;
        const auto next = size_t(ny) * house.side + size_t(nx);
        if (seen[next] != 0 || house.cells[next] != raw) continue;
        seen[next] = 1;
        stack.push_back(next);
      }
    }
  }

  size_t broken = 0;
  for (size_t zone = 0; zone < house.zones.size(); ++zone) {
    broken += size_t(pieces[zone] != 1);
  }
  return broken;
}

// Пары зон, КАСАЮЩИЕСЯ на растре: и напрямую, и через одну клетку стены. Нужны, чтобы проверить
// правила смежности — независимо от того, что решатель считал соседством.
std::set<std::pair<uint32_t, uint32_t>> touching_pairs(const building& house, const bool through_wall) {
  std::set<std::pair<uint32_t, uint32_t>> pairs;
  const auto at = [&](const int64_t x, const int64_t y) { return house.cells[size_t(y) * house.side + size_t(x)]; };
  const int64_t side = int64_t(house.side);
  const int64_t gap = through_wall ? 1 : 0;

  for (int64_t y = 0; y < side; ++y) {
    for (int64_t x = 0; x < side; ++x) {
      for (int64_t axis = 0; axis < 2; ++axis) {
        const int64_t nx = axis == 0 ? x + gap + 1 : x;
        const int64_t ny = axis == 0 ? y : y + gap + 1;
        if (nx >= side || ny >= side) continue;
        const auto a = at(x, y);
        const auto b = at(nx, ny);
        if (a == 0 || b == 0 || a == b) continue;
        if (through_wall && at(axis == 0 ? x + 1 : x, axis == 0 ? y : y + 1) != 0) continue;
        pairs.insert({std::min(a, b) - 1, std::max(a, b) - 1});
      }
    }
  }
  return pairs;
}

// ГЛУБИНА ОТ ЦИРКУЛЯЦИИ — то число, которым здание отличается от норы. В здании до любого места
// дверь-другая от коридора; в норе глубина растёт вместе с числом комнат.
std::vector<int64_t> depth_from_corridors(const building& house, const uint32_t corridor_kind) {
  std::vector<int64_t> depth(house.zones.size(), -1);
  std::vector<std::vector<uint32_t>> neighbours(house.zones.size());
  for (const auto& link : house.links) {
    neighbours[link.a].push_back(link.b);
    neighbours[link.b].push_back(link.a);
  }

  std::vector<uint32_t> wave;
  for (size_t i = 0; i < house.zones.size(); ++i) {
    if (house.zones[i].kind == corridor_kind) {
      depth[i] = 0;
      wave.push_back(uint32_t(i));
    }
  }
  for (size_t head = 0; head < wave.size(); ++head) {
    const auto zone = wave[head];
    for (const auto other : neighbours[zone]) {
      if (depth[other] >= 0) continue;
      depth[other] = depth[zone] + 1;
      wave.push_back(other);
    }
  }
  return depth;
}

void print_building_map(const building& house) {
  for (size_t y = 0; y < house.side; ++y) {
    std::string line(house.side, ' ');
    for (size_t x = 0; x < house.side; ++x) {
      const auto raw = house.cells[y * house.side + x];
      const auto kind = raw == 0 ? 0u : house.zones[raw - 1].kind;
      const auto glyph = kind < house.kinds.size() ? house.kinds[kind].glyph : uint32_t('?');
      line[x] = char(glyph);
    }
    std::cout << line << "\n";
  }
}

bool write_building_ppm(const std::string& path, const building& house, const size_t scale) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << "P6\n" << house.side * scale << " " << house.side * scale << "\n255\n";
  for (size_t y = 0; y < house.side * scale; ++y) {
    for (size_t x = 0; x < house.side * scale; ++x) {
      const auto raw = house.cells[(y / scale) * house.side + (x / scale)];
      const auto kind = raw == 0 ? 0u : house.zones[raw - 1].kind;
      const auto colour = kind < house.kinds.size() ? house.kinds[kind].colour : 0xff00ffu;
      // Соседние места одного вида обязаны различаться, иначе ряд комнат читается как один зал.
      // Оттенок кладётся ПОКАНАЛЬНО с зажимом: сложение с упакованным цветом переносится в соседний
      // канал, и светлая дверь становилась от этого бирюзовой — видно на картинке, но не в числах.
      const int32_t tone = raw == 0 ? 0 : int32_t((raw * 2654435761u) >> 27) - 16;
      for (int shift = 16; shift >= 0; shift -= 8) {
        const int32_t channel = int32_t((colour >> shift) & 255u) + tone;
        out.put(char(std::clamp(channel, 0, 255)));
      }
    }
  }
  return bool(out);
}

int run_building_report(const options& opts) {
  const auto house = run_building(opts);
  const auto corridor_kind = uint32_t(building_number("corridor_kind"));
  const auto door_kind = uint32_t(building_number("door_kind"));

  std::vector<size_t> per_kind(house.kinds.size(), 0);
  for (const auto& zone : house.zones) {
    if (zone.kind < per_kind.size()) ++per_kind[zone.kind];
  }

  const auto street_kind = uint32_t(building_number("street_kind"));
  const auto yard_kind = uint32_t(building_number("yard_kind"));

  size_t doors = 0;
  size_t spare = 0;
  size_t openings = 0;
  size_t outer = 0;
  for (const auto& link : house.links) {
    doors += size_t(link.door != 0);
    spare += size_t(link.spare != 0);
    openings += size_t(link.door == 0);
    const auto ka = house.zones[link.a].kind;
    const auto kb = house.zones[link.b].kind;
    outer += size_t(ka == street_kind || kb == street_kind || ka == yard_kind || kb == yard_kind);
  }

  size_t passable_cells = 0;
  for (size_t i = 0; i < house.cells.size(); ++i) {
    passable_cells += size_t(house.passable_cell(i));
  }

  const auto depth = depth_from_corridors(house, corridor_kind);
  int64_t depth_sum = 0;
  int64_t deepest = 0;
  size_t counted = 0;
  for (size_t i = 0; i < house.zones.size(); ++i) {
    if (!house.passable_zone(i) || depth[i] < 0 || house.zones[i].kind == door_kind) continue;
    depth_sum += depth[i];
    deepest = std::max(deepest, depth[i]);
    ++counted;
  }

  std::cout << "GN06 building: зерно " << opts.seed << ", участок " << house.plot << "x" << house.plot
            << " мер, картинка " << house.side << "x" << house.side << " клеток, окно ("
            << house.view_x << ", " << house.view_y << ") " << house.view_w << "x" << house.view_h << "\n"
            << "  мест               " << house.zones.size() << " (раздел " << house.planned << " + дверей "
            << doors << ")\n  по видам           ";
  for (size_t i = 0; i < per_kind.size(); ++i) {
    if (per_kind[i] == 0) continue;
    std::cout << char(house.kinds[i].glyph) << ":" << per_kind[i] << " ";
  }
  std::cout << "\n  стыков             " << house.joins << " (прямых " << house.direct_joins << ", через стену "
            << (house.joins - house.direct_joins) << ")\n"
            << "  связей             " << house.links.size() << " (широких проёмов " << openings << ", дверей "
            << doors << ", из них лишних " << spare << ")\n"
            << "  наружных дверей    " << outer << " (в дом с улицы и со двора)\n"
            << "  кусков по связям   " << link_components(house, {}, door_kind) << "\n"
            << "  колонн             " << house.columns.size() << "\n"
            // ВОДОРАЗДЕЛ ОТЧЁТА. Всё выше сказано про ПЛАН и от картинки не зависит вовсе; всё ниже
            // посчитано ПО КЛЕТКАМ и потому верно ровно настолько, насколько картинка точна. Когда
            // проекция огрубляет, эти числа описывают её, а не здание, и сказано об этом здесь, а не
            // в примечании мелким шрифтом.
            << "  картинка           " << (house.faithful() ? "план один в один" : "ОГРУБЛЯЕТ")
            << " (потеряно мест " << house.lost << ", слиплось границ " << house.merged << ")\n"
            << "  недостижимо клеток " << unreachable_cells(house, cell_class::passable)
            << (house.faithful() ? "" : "  <- по огрублённой картинке, не по плану") << "\n"
            << "  кусков стены       " << wall_pieces(house) << " после дверей, "
            << wall_pieces(house, door_kind, true) << " до них (массив плюс колонны)"
            << (house.faithful() ? "" : "  <- по огрублённой картинке") << "\n"
            << "  проходимо          " << passable_cells << " клеток ("
            << (100.0 * double(passable_cells) / double(house.cells.size())) << "% картинки)\n"
            << "  глубина от коридора средняя "
            << (counted == 0 ? 0.0 : double(depth_sum) / double(counted)) << ", наибольшая " << deepest << "\n"
            << "  собрано за         " << house.milliseconds << " мс\n"
            << "  память пайплайна   " << house.memory_kib << " КиБ\n";

  if (opts.ascii) {
    print_building_map(house);
  }
  if (!opts.dump.empty()) {
    if (!write_building_ppm(opts.dump, house, 6)) {
      utils::error{}("GN06: could not write '{}'", opts.dump);
    }
    std::cout << "  картинка для глаза " << opts.dump << "\n";
  }
  return 0;
}

int run_building_verify(const options& opts) {
  checker check;

  // ПРОВЕРКА СМОТРИТ ТОЧНУЮ КАРТИНКУ, И НЕ ПОТОМУ, ЧТО ТАК УДОБНЕЕ. Проверки по растру говорят о
  // плане ровно в той мере, в какой картинка ему равна; на огрублённой они проверяли бы её саму, и
  // девять провалов из тридцати одного означали бы «показ огрубляет» — то есть ровно то, о чём
  // просили ключом. Поэтому окно и масштаб здесь принудительно один к одному, а то, что попросили
  // ключом, становится ДОПОЛНИТЕЛЬНЫМ утверждением: план от этого не меняется.
  auto plain = opts;
  plain.view = 0;
  plain.window_x = 0;
  plain.window_y = 0;
  plain.window_w = -1;
  plain.window_h = -1;
  const auto house = run_building(plain);
  const auto corridor_kind = uint32_t(building_number("corridor_kind"));
  const auto door_kind = uint32_t(building_number("door_kind"));

  size_t doors = 0;
  for (const auto& link : house.links) {
    doors += size_t(link.door != 0);
  }
  std::cout << "GN06 verify building: участок " << house.plot << "x" << house.plot << " мер, картинка "
            << house.side << "x" << house.side << " клеток, мест " << house.zones.size()
            << " (раздел " << house.planned << " + дверей " << doors << "), стыков " << house.joins << ", связей "
            << house.links.size() << ", собрано за " << house.milliseconds << " мс\n";

  // 1. РАЗДЕЛ ЗАКОНЕН: места внутри пятна и не перекрываются. Перекрытие на растре невидимо —
  //    вторая зона просто затирает первую, — поэтому считается площадь против помеченных клеток.
  bool inside = !house.zones.empty();
  for (const auto& zone : house.zones) {
    inside = inside && zone.x >= 0 && zone.y >= 0 && zone.w > 0 && zone.h > 0 &&
             zone.x + zone.w <= int64_t(house.plot) && zone.y + zone.h <= int64_t(house.plot);
  }
  check(inside, "каждое место внутри участка");

  // НЕПЕРЕСЕЧЕНИЕ проверяется по ПРЯМОУГОЛЬНИКАМ, а не сравнением площади с числом помеченных
  // клеток: у мест появилась ФОРМА, и площадь прямоугольника больше не равна числу клеток. Габариты
  // же не пересекаются по построению раздела, а форма всегда внутри габарита — значит и клетки не
  // пересекаются. Саму перезапись теперь ловит `paint_rects`: отказывать обязан тот, кто теряет
  // данные, а не тот, кто потом считает.
  size_t overlapping = 0;
  for (size_t a = 0; a < house.zones.size(); ++a) {
    for (size_t b = a + 1; b < house.zones.size(); ++b) {
      const auto& first = house.zones[a];
      const auto& second = house.zones[b];
      overlapping += size_t(first.x < second.x + second.w && second.x < first.x + first.w &&
                            first.y < second.y + second.h && second.y < first.y + first.h);
    }
  }
  check(overlapping == 0, "габариты мест не пересекаются");

  // 1а. КАРТИНКА — ЭТО ВИД, А НЕ ПЛАН, и вся остальная проверка по растру держится на этой.
  //
  //     Клетки больше не источник истины: план нарезан в мере, стыки выведены из плана, двери
  //     поставлены в мере, и до последнего шага растра не существует вовсе. Значит спрашивать по
  //     клеткам о связности можно ровно тогда, когда проекция НИЧЕГО НЕ ПОТЕРЯЛА (место, не поймавшее
  //     ни одного центра клетки) и НИЧЕГО НЕ СЛЕПИЛА (граница, где масштаб съел стену). Оба числа
  //     считает сам проектор — и это правильный адрес: отвечать за потерю обязан тот, кто теряет.
  check(house.faithful(), "проекция один к одному ничего не потеряла и ничего не слепила");

  // 1б. ПЛАН НЕ ЗАВИСИТ ОТ КАРТИНКИ. Ради этого всё и переезжало, поэтому утверждение проверяется
  //     прямо, а не считается очевидным: тот же участок, показанный вдвое крупнее, обязан дать ТЕ ЖЕ
  //     места, те же связи и те же колонны. Пока стыки выводились из растра, эта проверка была бы
  //     заведомо ложной — размер картинки менял длины пробегов, а значит и выбор дверей.
  auto zoomed_opts = plain;
  zoomed_opts.view = plain.side * 2;
  const auto zoomed = run_building(zoomed_opts);
  check(zoomed.zones == house.zones, "вдвое более крупная картинка не сдвинула ни одного места");
  check(zoomed.links.size() == house.links.size() && zoomed.columns == house.columns,
        "вдвое более крупная картинка не изменила ни связей, ни колоннад");

  // 1в. ОКНО ПОКАЗЫВАЕТ ТО ЖЕ САМОЕ. Второе следствие того же раскола: кусок участка, показанный
  //     отдельно и один к одному, обязан совпасть с тем же куском общей картинки клетка в клетку.
  //     Это проверка ОТОБРАЖЕНИЯ, а не плана: сместить окно и промахнуться на клетку — ровно тот
  //     дефект, который на глаз выглядит правдоподобно.
  const auto quarter = int64_t(house.plot) / 4;
  const auto half = int64_t(house.plot) / 2;
  size_t window_mismatch = 0;
  if (half >= 8) {
    auto window_opts = plain;
    window_opts.window_x = quarter;
    window_opts.window_y = quarter;
    window_opts.window_w = half;
    window_opts.window_h = half;
    window_opts.view = size_t(half);
    const auto framed = run_building(window_opts);
    for (int64_t y = 0; y < half; ++y) {
      for (int64_t x = 0; x < half; ++x) {
        const auto here = framed.cells[size_t(y) * framed.side + size_t(x)];
        const auto there = house.cells[size_t(quarter + y) * house.side + size_t(quarter + x)];
        window_mismatch += size_t(here != there);
      }
    }
  }
  check(window_mismatch == 0, "окно участка совпадает с тем же куском общей картинки клетка в клетку");

  // ЕСЛИ ОКНО ИЛИ МАСШТАБ ПОПРОСИЛИ КЛЮЧОМ, это становится ещё одним замером того же утверждения — и
  // замером, который выбрал не автор проверки, а тот, кто её запустил.
  if (opts.view != plain.view || opts.window_w != plain.window_w || opts.window_h != plain.window_h ||
      opts.window_x != plain.window_x || opts.window_y != plain.window_y) {
    const auto asked = run_building(opts);
    std::cout << "  просили картинку " << asked.side << "x" << asked.side << " клеток, окно ("
              << asked.view_x << ", " << asked.view_y << ") " << asked.view_w << "x" << asked.view_h
              << ": потеряно мест " << asked.lost << ", слиплось границ " << asked.merged << "\n";
    check(asked.zones == house.zones && asked.columns == house.columns,
          "картинка, заказанная ключом, не изменила ни одного места и ни одной колонны");
  }

  // КЛЕТКА ЛЕЖИТ В ГАБАРИТЕ СВОЕГО МЕСТА. Это проверка ПИСАТЕЛЯ: форма считается в двух местах —
  // в инструменте при разметке и нигде больше, — и единственное, что можно потребовать снаружи, это
  // чтобы она не вылезала за объявленный габарит.
  size_t outside_rect = 0;
  for (size_t i = 0; i < house.cells.size(); ++i) {
    const auto raw = house.cells[i];
    if (raw == 0) continue;
    const auto& zone = house.zones[raw - 1];
    const int64_t x = int64_t(i % house.side);
    const int64_t y = int64_t(i / house.side);
    outside_rect += size_t(x < zone.x || y < zone.y || x >= zone.x + zone.w || y >= zone.y + zone.h);
  }
  check(outside_rect == 0, "каждая клетка лежит в габарите своего места");

  // ФОРМА МЕНЯЕТ МЕСТО, А НЕ РАЗРЫВАЕТ ЕГО.
  check(zones_in_pieces(house) == 0, "каждое место — один связный кусок клеток, сколько бы его ни срезали");

  // 2. СТЕНА — ДОПОЛНЕНИЕ, И ОНА ОДНА. На этом стоит решение не держать стены списком: если раздел
  //    однажды разорвёт стеновую массу, «одно место» перестанет быть правдой, и увидеть это надо
  //    здесь, а не в игре.
  // ДО ДВЕРЕЙ У СТЕНЫ РОВНО СТОЛЬКО КУСКОВ, СКОЛЬКО ОБЪЯВЛЕНО: один массив плюс по куску на каждую
  //    колонну. Проверка сильнее прежней «стена связна» и ловит обе стороны: колонна, прилипшая к
  //    стене, перестаёт быть колонной (кусков станет меньше), а раздел, разорвавший стену, добавит
  //    кусок ниоткуда. После дверей масса распадается дальше, и это нормально: дверь режет не
  //    только проход.
  check(wall_pieces(house, door_kind, true) == 1 + house.columns.size(),
        "до дверей стена это один массив плюс по куску на колонну");

  // 3. ДВЕРЬ — ЗОНА, И ОНА В СТЕНЕ. Проверяется по растру: у клетки двери по обе стороны вдоль её
  //    короткой оси стоят ровно те зоны, которые названы в связи.
  size_t wrong_doors = 0;
  for (const auto& link : house.links) {
    if (link.door == 0) continue;
    const auto& door = house.zones[link.door - 1];
    // У двери в одну клетку ОРИЕНТАЦИИ НЕТ, и габарит о ней не говорит ничего: проверяются обе оси, а
    // совпасть обязана хотя бы одна. Угадывание по габариту прошло бы на широких дверях и провалилось
    // бы на однокле точных — то есть на самых обычных.
    const auto neighbours_match = [&](const int64_t dx, const int64_t dy) {
      const int64_t x = door.x - dx;
      const int64_t y = door.y - dy;
      const int64_t nx = door.x + (dx != 0 ? door.w : 0);
      const int64_t ny = door.y + (dy != 0 ? door.h : 0);
      if (x < 0 || y < 0 || nx >= int64_t(house.side) || ny >= int64_t(house.side)) {
        return false;
      }
      const auto first = house.cells[size_t(y) * house.side + size_t(x)];
      const auto second = house.cells[size_t(ny) * house.side + size_t(nx)];
      return (first == link.a + 1 && second == link.b + 1) || (first == link.b + 1 && second == link.a + 1);
    };
    wrong_doors += size_t(!neighbours_match(1, 0) && !neighbours_match(0, 1));
  }
  check(wrong_doors == 0, "у каждой двери по обе стороны стоят ровно те места, которые названы в связи");

  // 4. СВЯЗНОСТЬ ПО СВЯЗЯМ — обещание плана.
  check(link_components(house, {}, door_kind) == 1, "все проходимые места связаны в один кусок по связям");

  // 5. СВЯЗНОСТЬ ПО РАСТРУ — обещание геометрии.
  check(unreachable_cells(house, cell_class::passable) == 0, "всё проходимое достижимо по растру");

  // 6. ДВА ПРЕДСТАВЛЕНИЯ СОГЛАСНЫ, И ЭТО ОТДЕЛЬНОЕ УТВЕРЖДЕНИЕ. PF09 доказала, что связь без общего
  //    ребра и ребро без связи существуют; здесь они совпадать ОБЯЗАНЫ, и проверка требует именно
  //    совпадения: каждая пара, соединённая широким проёмом, касается на растре, и каждая пара,
  //    касающаяся на растре напрямую, соединена.
  const auto direct = touching_pairs(house, false);
  std::set<std::pair<uint32_t, uint32_t>> opened;
  for (const auto& link : house.links) {
    if (link.door != 0) continue;
    opened.insert({std::min(link.a, link.b), std::max(link.a, link.b)});
  }
  size_t missing = 0;
  for (const auto& pair : direct) {
    // Касание двери с её местом — это та же связь, она уже посчитана дверью.
    if (house.zones[pair.first].kind == door_kind || house.zones[pair.second].kind == door_kind) continue;
    missing += size_t(opened.find(pair) == opened.end());
  }
  check(missing == 0, "каждое прямое касание на растре названо связью");

  // 7. ПРАВИЛА ДВЕРЕЙ. Жёсткий локальный запрет: дверь из кухни в покои не появится ни ради
  //    связности, ни ради щедрости.
  size_t forbidden = 0;
  for (const auto& link : house.links) {
    forbidden += size_t(!house.door_allowed(house.zones[link.a].kind, house.zones[link.b].kind));
  }
  check(forbidden == 0, "ни одной связи между видами, которым связь запрещена");

  // 8. ПРЕДЕЛ ЧИСЛА СВЯЗЕЙ у вида соблюдён.
  std::vector<size_t> used(house.zones.size(), 0);
  for (const auto& link : house.links) {
    ++used[link.a];
    ++used[link.b];
  }
  size_t over = 0;
  for (size_t i = 0; i < house.zones.size(); ++i) {
    const auto limit = house.kinds[house.zones[i].kind].max_links;
    over += size_t(limit != 0 && used[i] > limit);
  }
  check(over == 0, "ни у одного места связей больше объявленного предела");

  // 9. ПРАВИЛА СМЕЖНОСТИ — другое утверждение, и проверяется оно по РАСТРУ, а не по тому CSR,
  //    который читал решатель: копия соседства сверяла бы решатель с самим собой.
  size_t bad_neighbours = 0;
  for (const auto& pair : touching_pairs(house, true)) {
    const auto ka = house.zones[pair.first].kind;
    const auto kb = house.zones[pair.second].kind;
    if (ka == door_kind || kb == door_kind) continue;
    bad_neighbours += size_t(!house.adjacent_allowed(ka, kb));
  }
  check(bad_neighbours == 0, "ни одной общей стены между видами, которым смежность запрещена");

  // 10. УСЛОВИЯ СОБЛЮДЕНЫ. Коридоры остались коридорами (их назначил раздел, а не решатель), зал —
  //     самое большое место, кухня с залом смежны. Это ровно те требования, которые решатель
  //     выразить не может: он покупает запрет, а не требование.
  const auto hall_kind = uint32_t(building_number("hall_kind"));
  const auto kitchen_kind = uint32_t(building_number("kitchen_kind"));
  const auto street_kind = uint32_t(building_number("street_kind"));
  const auto yard_kind = uint32_t(building_number("yard_kind"));
  const auto outside = [&](const uint32_t kind) { return kind == street_kind || kind == yard_kind; };

  size_t corridors = 0;
  size_t streets = 0;
  size_t yards = 0;
  for (size_t i = 0; i < house.planned; ++i) {
    corridors += size_t(house.zones[i].kind == corridor_kind);
    streets += size_t(house.zones[i].kind == street_kind);
    yards += size_t(house.zones[i].kind == yard_kind);
  }
  check(corridors > 0, "циркуляция осталась циркуляцией");
  check(streets == 1 && yards == 1, "улица и двор остались собой: участок объявлен площадкой, а не решателем");

  // САМОЕ БОЛЬШОЕ МЕСТО, ВЫХОДЯЩЕЕ НА УЛИЦУ, стало залом. Не просто самое большое: зал, до которого
  // нельзя дойти с улицы, залом быть перестаёт, и обязательная дверь улица-зал не нашла бы стыка.
  const auto walls = touching_pairs(house, true);
  const auto faces = [&](const size_t zone, const uint32_t kind) {
    for (const auto& pair : walls) {
      if (pair.first == zone && house.zones[pair.second].kind == kind) return true;
      if (pair.second == zone && house.zones[pair.first].kind == kind) return true;
    }
    return false;
  };

  size_t largest = house.zones.size();
  for (size_t i = 0; i < house.planned; ++i) {
    const auto kind = house.zones[i].kind;
    if (kind == corridor_kind || outside(kind) || !faces(i, street_kind)) {
      continue;
    }
    if (largest == house.zones.size() || house.zones[i].area() > house.zones[largest].area()) {
      largest = i;
    }
  }
  check(largest < house.zones.size(), "на участке есть место, выходящее на улицу");
  check(largest < house.zones.size() && house.zones[largest].kind == hall_kind,
        "самое большое место, выходящее на улицу, стало залом");

  size_t kitchens = 0;
  size_t kitchen = house.zones.size();
  for (size_t i = 0; i < house.zones.size(); ++i) {
    if (house.zones[i].kind != kitchen_kind) continue;
    ++kitchens;
    kitchen = i;
  }
  check(kitchens == 1, "кухня ровно одна — её поставили условием");
  check(kitchen < house.zones.size() && faces(kitchen, yard_kind),
        "кухня выходит во двор: служебный вход — то, что делает кухню кухней");

  // ТРЕБОВАНИЕ — НЕ ЗАПРЕТ, И ПРОВЕРЯЕТСЯ ОНО ОТДЕЛЬНО: каждый стык между обязательными видами обязан
  // стать дверью. Без этой проверки «парадный вход в зал» зависел бы от того, какой стык оказался
  // длиннее, и по карте это выглядело бы правдоподобно.
  std::set<std::pair<uint32_t, uint32_t>> linked;
  for (const auto& link : house.links) {
    linked.insert({std::min(link.a, link.b), std::max(link.a, link.b)});
  }
  size_t missing_required = 0;
  for (const auto& pair : walls) {
    const auto ka = house.zones[pair.first].kind;
    const auto kb = house.zones[pair.second].kind;
    if (!house.required_door(ka, kb)) continue;
    missing_required += size_t(linked.find(pair) == linked.end());
  }
  check(missing_required == 0, "каждый стык между обязательной парой видов стал дверью");

  // НАРУЖНЫЕ ДВЕРИ. Обе гарантированы остовом, и вот почему: улица и двор — РАЗНЫЕ места, между
  // собой не связанные (участок обрезан по краям растра), а достижимы обязаны быть оба. Значит
  // остову приходится открыть и парадный вход, и служебный.
  size_t from_street = 0;
  size_t from_yard = 0;
  size_t street_into_hall = 0;
  size_t yard_into_kitchen = 0;
  for (const auto& link : house.links) {
    const auto ka = house.zones[link.a].kind;
    const auto kb = house.zones[link.b].kind;
    const bool street_side = ka == street_kind || kb == street_kind;
    const bool yard_side = ka == yard_kind || kb == yard_kind;
    from_street += size_t(street_side);
    from_yard += size_t(yard_side);
    street_into_hall += size_t(street_side && (ka == hall_kind || kb == hall_kind));
    yard_into_kitchen += size_t(yard_side && (ka == kitchen_kind || kb == kitchen_kind));
  }
  check(from_street > 0 && from_yard > 0, "в дом можно войти и с улицы, и со двора");
  check(street_into_hall > 0, "парадный вход ведёт в зал");
  check(yard_into_kitchen > 0, "служебный вход ведёт в кухню");

  // 11. ЗАПИРАТЬ МОЖНО ТОЛЬКО ЛИШНЮЮ ДВЕРЬ. Правило PF09: запертая дверь, за которой остаётся кусок
  //     здания, — это не препятствие, а кусок города, куда игра никогда не попадёт.
  size_t breaking = 0;
  for (size_t i = 0; i < house.links.size(); ++i) {
    if (house.links[i].spare == 0) continue;
    breaking += size_t(link_components(house, {i}, door_kind) != 1);
  }
  check(breaking == 0, "закрытие любой лишней двери не отрезает ни одного места");

  // 12. ПОВТОРЯЕМОСТЬ.
  const auto again = run_building(plain);
  auto other_opts = plain;
  other_opts.seed += 1;
  const auto other = run_building(other_opts);
  check(again.zones == house.zones, "то же зерно даёт то же здание");
  check(other.zones != house.zones, "другое зерно даёт другое здание");

  // 13. ГЛУБИНА ОТ ЦИРКУЛЯЦИИ — то число, которым здание отличается от норы: до любого места
  //     дверь-другая от коридора.
  const auto depth = depth_from_corridors(house, corridor_kind);
  int64_t deepest = 0;
  bool reachable = true;
  for (size_t i = 0; i < house.zones.size(); ++i) {
    if (!house.passable_zone(i) || house.zones[i].kind == door_kind) continue;
    reachable = reachable && depth[i] >= 0;
    deepest = std::max(deepest, depth[i]);
  }
  std::cout << "  наибольшая глубина от коридора " << deepest << "\n";
  check(reachable, "до каждого места есть путь от циркуляции");
  // ТРИ — ЭТО НЕ ВКУС, А ДЛИНА ЦЕПОЧКИ ПРАВИЛ: самый дальний возможный путь от циркуляции ведёт
  // коридор -> зал -> кухня -> кладовая, и длиннее его правила дверей не разрешают. Поменяются
  // правила — проверка провалится, и это правильно: глубина здания задана правилами, а не удачей.
  check(deepest <= 3, "ни одно место не дальше трёх дверей от коридора — длиннее правила не позволяют");

  std::cout << "GN06 verify building: " << (check.checks - check.failures) << "/" << check.checks << "\n";
  return check.failures == 0 ? 0 : 1;
}

} // namespace

int main(const int argc, const char** argv) {
  try {
    const auto opts = parse_options(argc, argv);
    if (opts.mode == generator_mode::site) {
      if (opts.side_set || opts.view_set || opts.window_set || opts.rooms >= 0 ||
          opts.extra_links >= 0 || opts.corridor_width >= 0)
        utils::error{}("GN06 site: --size, --view, --window, --rooms, --loops and --corridor do not apply");
      const auto source = originator::read_generator_source(generator().resources, "generator/site");
      const auto motifs = originator::read_generator_source(generator().resources, "generator/site_motifs");
      if (opts.viewer) {
        if (opts.verify || opts.ascii || !opts.dump.empty())
          utils::error{}("GN06 site viewer: --verify, --ascii and --dump are separate headless views");
        return gn06::run_site_viewer(opts.seed, opts.motif_scale, opts.motif_entry_y,
                                     opts.surface_cut, source, motifs, opts.viewer_frames, opts.site_bounds);
      }
      if (opts.viewer_frames_set)
        utils::error{}("GN06 site: --frames requires --viewer");
      return gn06::run_site_graph(opts.seed, opts.motif_scale, opts.motif_entry_y,
                                  opts.surface_cut, opts.verify, opts.ascii, opts.dump, source, motifs, opts.site_bounds);
    }
    if (opts.site_bounds_set) {
      utils::error{}("GN06: --bounds belongs to --mode=site");
    }
    if (opts.mode != generator_mode::motifs &&
        (opts.motif_scale_set || opts.motif_entry_set || opts.surface_set || opts.viewer || opts.viewer_frames_set))
      utils::error{}("GN06: --scale, --entry-y, --surface, --viewer and --frames belong to --mode=motifs");
    if (opts.mode == generator_mode::cave) {
      return opts.verify ? run_verify(opts) : run_report(opts);
    }
    if (opts.mode == generator_mode::motifs) {
      if (opts.side_set && opts.motif_scale_set)
        utils::error{}("GN06 motifs: choose either --size or --scale, not both");
      if (opts.view_set || opts.window_set || opts.rooms >= 0 ||
          opts.extra_links >= 0 || opts.corridor_width >= 0)
        utils::error{}("GN06 motifs: --view, --window, --rooms, --loops and --corridor belong to the older modes");
      const auto catalogue = originator::read_generator_source(generator().resources, "generator/motifs");
      if (opts.viewer) {
        if (opts.verify || opts.ascii || !opts.dump.empty())
          utils::error{}("GN06 motifs viewer: --verify, --ascii and --dump are separate headless views");
        return gn06::run_motif_viewer(opts.seed, opts.motif_scale, opts.side_set ? opts.side : 0,
                                      opts.motif_entry_y, opts.surface_cut, catalogue, opts.viewer_frames);
      }
      if (opts.viewer_frames_set)
        utils::error{}("GN06 motifs: --frames requires --viewer");
      return gn06::run_motif_demo(opts.seed, opts.motif_scale, opts.side_set ? opts.side : 0,
                                  opts.motif_entry_y, opts.surface_cut,
                                  opts.verify, opts.ascii, opts.dump, catalogue);
    }
    return opts.verify ? run_building_verify(opts) : run_building_report(opts);
  } catch (const std::exception& error) {
    std::cerr << "GN06: " << error.what() << "\n";
    return 1;
  }
}
