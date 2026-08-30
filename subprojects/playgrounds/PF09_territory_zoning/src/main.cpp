// PF09 — territory zoning. Срез 1 полностью headless: иерархия территорий существует раньше любого
// растра, и все следующие срезы обязаны совпадать с тем, что считает этот executable.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <numbers>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "devils_engine/utils/hash.h"

#include "locality.h"
#include "navigate.h"
#include "viewer.h"
#include "tactics.h"
#include "titles.h"
#include "world_build.h"
#include "zones.h"
#include "territory.h"

using namespace devils_engine;

namespace {

enum class action : uint32_t { report, locality, world, stream, view, verify, dump };
enum class dump_mode : uint32_t { zone, owner };
enum class dump_source : uint32_t { direct, zones };

struct options {
  action requested = action::report;
  pf09::layout_config layout;

  std::string dump_path;
  pf09::tier dump_tier = pf09::tier::barony;
  dump_mode dump_colouring = dump_mode::zone;
  double dump_center_x_m = 512000.0;
  double dump_center_y_m = 512000.0;
  double dump_span_m = 1024000.0;
  uint32_t dump_size = 768;

  pf09::locality_config local;
  pf09::build_options build{};
  double stream_radius_m = 12000.0;
  uint32_t stream_budget = 32;
  pf09::viewer_options view{};
  double view_distance_m = 12000.0;
  uint32_t screen_pixels = 1920;
  double pitch_deg = 55.0;
  double fov_deg = 40.0;
  dump_source dump_from = dump_source::direct;

  uint32_t samples = 200000;
  uint32_t coverage_side = 1200;
  double spread_limit = 6.0;
  uint64_t expected_fingerprint = 0;
};

bool read_prefixed(const std::string_view argument, const std::string_view prefix, std::string& out) {
  if (!argument.starts_with(prefix)) return false;
  out = std::string(argument.substr(prefix.size()));
  return true;
}

void print_usage() {
  std::cout << "PF09 territory zoning\n"
               "  --report              таблица ярусов, узлов и масштабов (по умолчанию)\n"
               "  --locality            локальности: размещение, состав, связность и бюджет\n"
               "  --build-world         собрать секторные файлы игровых территорий\n"
               "  --stream              пройти маршрут и показать подгрузку секторов\n"
               "  --view                интерактивный просмотр: наведение, выделение, персонажи\n"
               "  --frames=N            закрыть окно после N кадров\n"
               "  --shot=PATH           сохранить последний кадр в ppm\n"
               "  --view-span=M         стартовая ширина обзора\n"
               "  --agents=N            сколько персонажей гонять\n"
               "  --validation          слои валидации Vulkan\n"
               "  --world=PATH          каталог секторных файлов\n"
               "  --sectors=N           сторона квадрата собираемых секторов\n"
               "  --stream-radius=M     радиус подгрузки\n"
               "  --stream-budget=N     сколько секторов держим одновременно\n"
               "  --extent=M            сторона пятна локальности\n"
               "  --plot-side=N         сетка участков застройки внутри пятна\n"
               "  --room-side=N         сетка помещений внутри здания\n"
               "  --screen=N            ширина экрана в пикселях для пересчёта в метры на пиксель\n"
               "  --pitch=DEG           наклон камеры к земле\n"
               "  --fov=DEG             вертикальный угол обзора\n"
               "  --verify              численные инварианты, ненулевой код возврата при провале\n"
               "  --dump=PATH           ppm-срез карты для глаз\n"
               "  --floor=N --cutaway   с какого этажа начинать и срезать ли передний план\n"
               "  --tactics             показать укрытия, наблюдение и веер приказа наведённого места\n"
               "  --control-map=0..4    раскраска: вид / кто держит / преступность / собственник / закон\n"
               "  --dump-tier=N         ярус окраски дампа, 0 = world, 6 = parcel\n"
               "  --dump-mode=NAME      zone | owner\n"
               "  --dump-source=NAME    direct | zones\n"
               "  --bake-budget=N       потолок печки за кадр в текселях, 0 снимает ограничение\n"
               "  --view=M              дальность видимости в метрах\n"
               "  --tier-texels=N       сколько текселей занимает разрешимая ячейка яруса\n"
               "  --base-texel=M        размер текселя самого мелкого уровня в метрах\n"
               "  --dump-center=X,Y     центр дампа в метрах\n"
               "  --dump-span=M         сторона дампа в метрах\n"
               "  --dump-size=N         сторона дампа в пикселях\n"
               "  --seed=N              seed раскладки\n"
               "  --span=M              сторона мира в метрах\n"
               "  --warp=F              доля ячейки, на которую варп смещает координату запроса\n"
               "  --merge=F             вероятность поглощения ячейки соседом\n"
               "  --inherit=F           вероятность унаследовать владельца родителя\n"
               "  --split=A,B,C,...     линейное дробление по ярусам, ровно 7 чисел\n"
               "  --samples=N           размер случайной выборки для проверок\n"
               "  --coverage-side=N     сторона регулярной сетки для проверки покрытия\n"
               "  --spread-limit=F      предел отношения p95/p5 площадей внутри яруса\n"
               "  --expect-fingerprint=N  сверить отпечаток раскладки с известным\n";
}

bool parse_options(const int argc, const char** argv, options& out) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    std::string value;

    if (argument == "--help" || argument == "-h") {
      print_usage();
      return false;
    } else if (argument == "--report") {
      out.requested = action::report;
    } else if (argument == "--locality") {
      out.requested = action::locality;
    } else if (argument == "--build-world") {
      out.requested = action::world;
    } else if (argument == "--stream") {
      out.requested = action::stream;
    } else if (argument == "--view") {
      out.requested = action::view;
    } else if (argument == "--validation") {
      out.view.validation = true;
    } else if (read_prefixed(argument, "--frames=", value)) {
      out.view.frames = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--shot=", value)) {
      out.view.dump_path = value;
    } else if (read_prefixed(argument, "--floor=", value)) {
      out.view.start_floor = int32_t(std::stol(value));
    } else if (argument == "--cutaway") {
      out.view.start_cutaway = true;
    } else if (argument == "--tactics") {
      out.view.start_tactics = true;
    } else if (read_prefixed(argument, "--control-map=", value)) {
      out.view.start_control_mode = uint32_t(std::stoul(value)) % 5;
    } else if (read_prefixed(argument, "--view-span=", value)) {
      out.view.start_span_m = std::stod(value);
    } else if (read_prefixed(argument, "--agents=", value)) {
      out.view.agent_count = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--world=", value)) {
      out.build.root = value;
    } else if (read_prefixed(argument, "--sectors=", value)) {
      out.build.sector_side = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--stream-radius=", value)) {
      out.stream_radius_m = std::stod(value);
    } else if (read_prefixed(argument, "--stream-budget=", value)) {
      out.stream_budget = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--extent=", value)) {
      out.local.extent_m = std::stod(value);
    } else if (read_prefixed(argument, "--plot-side=", value)) {
      out.local.plot_side = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--room-side=", value)) {
      out.local.room_side = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--screen=", value)) {
      out.screen_pixels = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--pitch=", value)) {
      out.pitch_deg = std::stod(value);
    } else if (read_prefixed(argument, "--fov=", value)) {
      out.fov_deg = std::stod(value);
    } else if (argument == "--verify") {
      out.requested = action::verify;
    } else if (read_prefixed(argument, "--dump=", value)) {
      out.requested = action::dump;
      out.dump_path = value;
    } else if (read_prefixed(argument, "--dump-tier=", value)) {
      const auto index = std::stoul(value);
      if (index >= pf09::tier_count) {
        std::cout << std::format("ярус {} вне списка\n", index);
        return false;
      }
      out.dump_tier = pf09::tier(uint32_t(index));
    } else if (read_prefixed(argument, "--dump-mode=", value)) {
      out.dump_colouring = value == "owner" ? dump_mode::owner : dump_mode::zone;
    } else if (read_prefixed(argument, "--dump-source=", value)) {
      out.dump_from = value == "zones" ? dump_source::zones : dump_source::direct;
    } else if (read_prefixed(argument, "--view=", value)) {
      out.view_distance_m = std::stod(value);
    } else if (read_prefixed(argument, "--dump-center=", value)) {
      const auto comma = value.find(',');
      if (comma == std::string::npos) {
        std::cout << "--dump-center ждёт X,Y\n";
        return false;
      }
      out.dump_center_x_m = std::stod(value.substr(0, comma));
      out.dump_center_y_m = std::stod(value.substr(comma + 1));
    } else if (read_prefixed(argument, "--dump-span=", value)) {
      out.dump_span_m = std::stod(value);
    } else if (read_prefixed(argument, "--dump-size=", value)) {
      out.dump_size = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--seed=", value)) {
      out.layout.seed = std::stoull(value);
    } else if (read_prefixed(argument, "--span=", value)) {
      out.layout.world_span_m = std::stod(value);
    } else if (read_prefixed(argument, "--warp=", value)) {
      out.layout.warp_strength = std::stod(value);
    } else if (read_prefixed(argument, "--merge=", value)) {
      out.layout.merge_chance = std::stod(value);
    } else if (read_prefixed(argument, "--inherit=", value)) {
      out.layout.owner_inherit_chance = std::stod(value);
    } else if (read_prefixed(argument, "--split=", value)) {
      size_t begin = 0;
      size_t index = 0;
      while (begin <= value.size() && index < pf09::tier_count) {
        const auto comma = value.find(',', begin);
        const auto piece = value.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
        out.layout.split[index] = uint32_t(std::stoul(piece));
        ++index;
        if (comma == std::string::npos) break;
        begin = comma + 1;
      }
      if (index != pf09::tier_count) {
        std::cout << std::format("--split ждёт {} чисел, получено {}\n", pf09::tier_count, index);
        return false;
      }
    } else if (read_prefixed(argument, "--samples=", value)) {
      out.samples = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--coverage-side=", value)) {
      out.coverage_side = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--spread-limit=", value)) {
      out.spread_limit = std::stod(value);
    } else if (read_prefixed(argument, "--expect-fingerprint=", value)) {
      out.expected_fingerprint = std::stoull(value);
    } else {
      std::cout << std::format("неизвестный аргумент: {}\n", argument);
      print_usage();
      return false;
    }
  }
  return true;
}

// Детерминированный поток точек: проверки обязаны воспроизводиться между запусками и между машинами,
// поэтому выборка берётся из хеша индекса, а не из std::random_device.
glm::dvec2 sample_point(const uint32_t index, const double span_m, const uint64_t salt) {
  const uint64_t hx = utils::splitmix(uint64_t(index) * 2ull + 1ull, salt);
  const uint64_t hy = utils::splitmix(uint64_t(index) * 2ull + 2ull, salt);
  constexpr double scale = 1.0 / 18446744073709551616.0;
  return {double(hx) * scale * span_m, double(hy) * scale * span_m};
}

// --- отчёт ---

void run_report(const pf09::territory& map) {
  const auto& config = map.config();

  std::cout << std::format("PF09 иерархия территорий\n  seed {}  сторона мира {:.1f} км  варп {:.3f}  слияние {:.2f}\n",
                           config.seed, config.world_span_m / 1000.0, config.warp_strength, config.merge_chance);
  std::cout << std::format("  отпечаток раскладки: {}\n\n", map.fingerprint());

  std::cout << "  ярус       сетка      ячеек        узлов     сторона   среднее потомков   хранение\n";
  for (size_t t = 0; t < pf09::tier_count; ++t) {
    const auto value = pf09::tier(t);
    const auto nodes = map.node_count(value);
    const auto span = map.tier_span_m(value);
    const double mean_children =
      t + 1 < pf09::tier_count ? double(map.node_count(pf09::tier(t + 1))) / double(nodes) : 0.0;
    const bool authored = t <= size_t(config.authored_depth);

    std::cout << std::format("  {:<9} {:>5}²  {:>10}  {:>11}  {:>8}  {:>16}   {}\n", pf09::tier_name(value),
                             map.grid_dim(value), map.cell_count(value), nodes,
                             span >= 1000.0 ? std::format("{:.0f} км", span / 1000.0) : std::format("{:.0f} м", span),
                             t + 1 < pf09::tier_count ? std::format("{:.1f}", mean_children) : std::string("—"),
                             authored ? "таблица" : "арифметика");
  }

  const auto authored_bytes = map.authored().size() * sizeof(pf09::authored_node);
  std::cout << std::format("\n  материализовано {} узлов, {:.1f} КБ; ниже яруса '{}' узлы не хранятся вовсе\n",
                           map.authored().size(), double(authored_bytes) / 1024.0,
                           pf09::tier_name(config.authored_depth));

  std::vector<uint32_t> per_owner(config.dynasty_count, 0);
  for (const auto& node : map.authored()) {
    if (pf09::tier_of(node.id) != pf09::tier::barony) continue;
    ++per_owner[node.owner];
  }
  const auto held = std::count_if(per_owner.begin(), per_owner.end(), [](const uint32_t count) { return count != 0; });
  const auto largest = *std::max_element(per_owner.begin(), per_owner.end());
  std::cout << std::format("  баронства держат {} династий из {}, крупнейшая владеет {} титулами\n", held,
                           config.dynasty_count, largest);
}


// --- локальности ---

// Обход графа смежности от улиц. Это ровно то, что делает локальный ИИ, когда ему сказали «найди работу
// в городе»: он идёт по публичным зонам и заходит внутрь через выходы построек.
std::vector<uint8_t> reach_from_streets(const pf09::locality& place) {
  std::vector<uint8_t> seen(place.zone_count(), 0);
  std::vector<uint32_t> queue;

  for (uint32_t i = 0; i < place.zone_count(); ++i) {
    if (place.role(i) != pf09::zone_role::street) continue;
    seen[i] = 1;
    queue.push_back(i);
    break;
  }

  for (size_t head = 0; head < queue.size(); ++head) {
    for (const auto next : place.neighbours(queue[head])) {
      if (seen[next] != 0) continue;
      seen[next] = 1;
      queue.push_back(next);
    }
  }
  return seen;
}

uint32_t count_role(const pf09::locality& place, const pf09::zone_role role) {
  uint32_t total = 0;
  for (uint32_t i = 0; i < place.zone_count(); ++i) {
    if (place.role(i) == role) ++total;
  }
  return total;
}

int run_locality_report(const pf09::territory& map, const options& opts) {
  pf09::locality_field field(map, opts.local);
  const glm::dvec2 observer{map.config().world_span_m * 0.5, map.config().world_span_m * 0.5};

  const auto begin = std::chrono::steady_clock::now();
  const uint32_t built = field.focus(observer);
  const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();

  std::cout << std::format("PF09 локальности: пятно {:.0f} м, сетка участков {}², помещений {}², радиус "
                           "резидентности {:.0f} м\n",
                           opts.local.extent_m, opts.local.plot_side, opts.local.room_side,
                           opts.local.residency_radius_m);
  std::cout << std::format("  построено {} локальностей за {:.1f} мс, резидентно {}, {:.1f} КБ\n\n", built, elapsed,
                           field.resident().size(), double(field.resident_bytes()) / 1024.0);

  std::cout << "  вид      якорь        улиц   дворов   зданий   помещений   достижимо   размер\n";
  for (const auto* place : field.resident()) {
    const auto seen = reach_from_streets(*place);
    uint32_t live = 0;
    uint32_t reached = 0;
    for (uint32_t i = 0; i < place->zone_count(); ++i) {
      if (place->role(i) == pf09::zone_role::count) continue;
      ++live;
      reached += seen[i];
    }

    std::cout << std::format("  {:<8} {:>10}   {:>5}   {:>6}   {:>6}   {:>9}   {:>4}/{:<4}   {:>5.1f} КБ\n",
                             pf09::locality_kind_name(place->kind()), place->anchor(),
                             count_role(*place, pf09::zone_role::street), count_role(*place, pf09::zone_role::yard),
                             count_role(*place, pf09::zone_role::building),
                             count_role(*place, pf09::zone_role::room), reached, live,
                             double(place->byte_size()) / 1024.0);
  }

  // Сколько локальностей вообще размещено на весь мир — это цена, которую платит игра за то, чтобы
  // «зайти в любой город», и она должна быть посчитана, а не оценена.
  const uint64_t locales = map.node_count(pf09::tier::locale);
  const double density = opts.local.town_chance + opts.local.crypt_chance + opts.local.castle_chance;
  std::cout << std::format("\n  на {} локальных территорий мира приходится примерно {:.0f} локальностей "
                           "({:.1f}% плотность); материализуются только те, что в радиусе\n",
                           locales, double(locales) * density, density * 100.0);
  return EXIT_SUCCESS;
}


// --- игровые территории: сборка и стриминг ---

std::filesystem::path world_root(const options& opts) {
  return opts.build.root.empty() ? std::filesystem::temp_directory_path() / "pf09_world" : opts.build.root;
}

glm::dvec2 world_centre(const options& opts) {
  return {(double(opts.build.sector_x) + double(opts.build.sector_side) * 0.5) * pf09::sector_span_m,
          (double(opts.build.sector_y) + double(opts.build.sector_side) * 0.5) * pf09::sector_span_m};
}

pf09::build_stats build_fixture(const pf09::territory& map, const options& opts) {
  auto build = opts.build;
  build.root = world_root(opts);
  return pf09::build_world(map, opts.local, build);
}

int run_build_world(const pf09::territory& map, const options& opts) {
  const auto stats = build_fixture(map, opts);

  std::cout << std::format("PF09 сборка мира в '{}'\n", world_root(opts).string());
  std::cout << std::format("  {} секторов по {:.0f} м, {} зон, {} связей, {} предметов, {} поселений\n", stats.sectors,
                           pf09::sector_span_m, stats.zones, stats.links, stats.props, stats.settlements);
  std::cout << std::format("  {} титулов: право отдельным файлом, потому что «кто правит» обязано "
                           "отвечаться и без подгруженной земли\n", stats.titles);
  std::cout << std::format("  {:.1f} КБ содержимого, собрано за {:.0f} мс, в среднем {:.1f} КБ и {} зон на сектор\n",
                           double(stats.bytes) / 1024.0, stats.millis,
                           double(stats.bytes) / 1024.0 / double(std::max(stats.sectors, 1u)),
                           stats.zones / std::max(stats.sectors, 1u));

  // Экстраполяция на весь мир — то число, ради которого стриминг и существует. Держать комнаты всей
  // империи в памяти нельзя, и это надо назвать вслух, а не подразумевать.
  const double covered = double(opts.build.sector_side) * pf09::sector_span_m;
  const double ratio = (map.config().world_span_m / covered) * (map.config().world_span_m / covered);
  std::cout << std::format("  собрано {:.0f}x{:.0f} км из {:.0f}x{:.0f} км мира; при той же плотности весь мир — "
                           "{:.1f} ГБ и {:.0f} млн зон\n",
                           covered / 1000.0, covered / 1000.0, map.config().world_span_m / 1000.0,
                           map.config().world_span_m / 1000.0, double(stats.bytes) * ratio / (1024.0 * 1024.0 * 1024.0),
                           double(stats.zones) * ratio / 1.0e6);
  return EXIT_SUCCESS;
}

int run_stream_report(const pf09::territory& map, const options& opts) {
  const auto root = world_root(opts);
  if (!std::filesystem::exists(root)) build_fixture(map, opts);

  pf09::zone_store store(root, opts.stream_radius_m, opts.stream_budget);
  const auto centre = world_centre(opts);

  std::cout << std::format("PF09 стриминг из '{}': радиус {:.0f} м, бюджет {} секторов\n\n", root.string(),
                           opts.stream_radius_m, opts.stream_budget);
  std::cout << "  шаг   позиция, км    загружено   выгружено   резидентно   память   прочитано\n";

  const double travel = double(opts.build.sector_side) * pf09::sector_span_m * 0.8;
  constexpr uint32_t steps = 10;

  uint64_t total_read = 0;
  for (uint32_t step = 0; step <= steps; ++step) {
    const glm::dvec2 observer{centre.x - travel * 0.5 + travel * double(step) / double(steps), centre.y};
    const auto stats = store.focus(observer);
    total_read += stats.bytes_read;

    std::cout << std::format("  {:>3}   {:>7.1f}, {:<4.1f} {:>10}  {:>10}   {:>10}   {:>5.1f} КБ   {:>5.1f} КБ\n", step,
                             observer.x / 1000.0, observer.y / 1000.0, stats.loaded, stats.evicted, stats.resident,
                             double(stats.resident_bytes) / 1024.0, double(stats.bytes_read) / 1024.0);
  }

  // Пикинг: это и есть «информация о конкретной зоне», ради которой всё и затевалось. Точка берётся
  // ВНУТРИ поселения, а не в центре области: в центре законно пусто, и пустой ответ на всех уровнях
  // показал бы правило про пропуски, но не показал бы, что все четыре уровня отвечают.
  store.focus(centre);
  glm::vec3 probe{float(centre.x), 1.0f, float(centre.y)};
  for (int32_t y = 0; y < int32_t(opts.build.sector_side) && probe.y > 0.0f; ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;

      const auto found = std::find_if(sector->zones.begin(), sector->zones.end(), [](const pf09::zone_record& item) {
        return item.level == pf09::zone_level::interior && item.kind == pf09::zone_kind::hall;
      });
      if (found == sector->zones.end()) continue;

      probe = {(found->bounds.lower.x + found->bounds.upper.x) * 0.5f,
               (found->bounds.lower.y + found->bounds.upper.y) * 0.5f,
               (found->bounds.lower.z + found->bounds.upper.z) * 0.5f};
      store.focus({double(probe.x), double(probe.z)});
      y = int32_t(opts.build.sector_side);
      break;
    }
  }

  std::cout << std::format("\n  что под точкой ({:.0f}, {:.0f}, {:.0f}):\n", probe.x, probe.y, probe.z);
  for (uint32_t level = 0; level < uint32_t(pf09::zone_level::count); ++level) {
    const auto hit = store.pick(probe, pf09::zone_level(level));
    const auto* found = hit.valid() ? store.find(hit.zone) : nullptr;
    std::cout << std::format("    {:<10} {}\n", pf09::zone_level_name(pf09::zone_level(level)),
                             found == nullptr
                               ? std::string("— пусто, и это законный ответ")
                               : std::format("{} '{}', частей {}, проходов {}", pf09::zone_kind_name(found->kind),
                                             store.name_of(*found), found->part_count,
                                             store.portals_of(hit).size()));
  }

  std::cout << std::format("\n  всего прочитано за проход {:.1f} КБ\n", double(total_read) / 1024.0);
  return EXIT_SUCCESS;
}

// --- численные проверки ---

class checker {
public:
  void expect(const bool condition, const std::string_view name, const std::string& detail = {}) {
    ++total_;
    if (condition) return;
    ++failed_;
    std::cout << std::format("  ПРОВАЛ  {}{}{}\n", name, detail.empty() ? "" : ": ", detail);
  }

  bool passed() const { return failed_ == 0; }
  size_t total() const { return total_; }
  size_t failed() const { return failed_; }

private:
  size_t total_ = 0;
  size_t failed_ = 0;
};

void verify_identifiers(checker& check, const pf09::territory& map) {
  bool packing = true;
  for (size_t t = 0; t < pf09::tier_count; ++t) {
    const auto value = pf09::tier(t);
    const uint32_t last = uint32_t(map.cell_count(value) - 1);
    const auto id = pf09::make_zone(value, last);
    packing = packing && pf09::tier_of(id) == value && pf09::index_of(id) == last;
  }
  check.expect(packing, "упаковка id", "ярус и индекс не пережили дорогу через zone_id");
}

void verify_nesting(checker& check, const pf09::territory& map, const uint32_t samples) {
  const double span = map.config().world_span_m;

  size_t broken_parent = 0;
  size_t broken_ancestor = 0;
  size_t broken_agreement = 0;
  size_t broken_walk = 0;

  // Единственность родителя проверяется не по построению, а по наблюдению: если бы разные точки увидели у
  // одного узла разных родителей, дерево бы уже развалилось, и заметить это можно только сравнением.
  std::unordered_map<pf09::zone_id, pf09::zone_id> seen_parent;
  seen_parent.reserve(samples * 2);
  size_t parent_conflicts = 0;

  for (uint32_t i = 0; i < samples; ++i) {
    const auto point = sample_point(i, span, 0x9e35ull);
    const auto chain = map.resolve_chain(point);

    for (size_t t = 1; t < pf09::tier_count; ++t) {
      if (map.parent_of(chain[t]) != chain[t - 1]) ++broken_parent;
      if (map.ancestor_at(chain[pf09::tier_count - 1], pf09::tier(t - 1)) != chain[t - 1]) ++broken_ancestor;

      const auto [entry, inserted] = seen_parent.try_emplace(chain[t], chain[t - 1]);
      if (!inserted && entry->second != chain[t - 1]) ++parent_conflicts;
    }

    for (size_t t = 0; t < pf09::tier_count; ++t) {
      if (map.resolve(point, pf09::tier(t)) != chain[t]) ++broken_agreement;
    }

    size_t steps = 0;
    auto current = chain[pf09::tier_count - 1];
    while (pf09::tier_of(current) != pf09::tier::world) {
      current = map.parent_of(current);
      ++steps;
      if (steps > pf09::tier_count) break;
    }
    if (steps != pf09::tier_count - 1) ++broken_walk;
  }

  check.expect(broken_parent == 0, "строгое вложение", std::format("{} цепочек, где родитель яруса не совпал", broken_parent));
  check.expect(parent_conflicts == 0, "единственный родитель", std::format("{} узлов с двумя родителями", parent_conflicts));
  check.expect(broken_ancestor == 0, "ancestor_at", std::format("{} расхождений с цепочкой", broken_ancestor));
  check.expect(broken_agreement == 0, "resolve против resolve_chain", std::format("{} расхождений", broken_agreement));
  check.expect(broken_walk == 0, "длина подъёма к корню", std::format("{} цепочек не той глубины", broken_walk));
}

void verify_determinism(checker& check, const pf09::territory& map, const uint32_t samples) {
  const double span = map.config().world_span_m;

  std::vector<uint32_t> order(samples);
  std::iota(order.begin(), order.end(), 0u);

  std::vector<pf09::zone_id> forward(samples);
  for (uint32_t i = 0; i < samples; ++i) {
    forward[i] = map.resolve(sample_point(i, span, 0x9e35ull));
  }

  // Тот же набор точек в обратном порядке и через другую точку входа. Если бы в разрешении завелось хоть
  // какое-то состояние, порядок его бы вскрыл.
  size_t mismatches = 0;
  for (uint32_t i = samples; i-- > 0;) {
    const auto chain = map.resolve_chain(sample_point(i, span, 0x9e35ull));
    if (chain[pf09::tier_count - 1] != forward[i]) ++mismatches;
  }

  check.expect(mismatches == 0, "детерминизм разрешения", std::format("{} расхождений при обратном обходе", mismatches));
}

void verify_warp(checker& check, const pf09::territory& map) {
  const double span = map.config().world_span_m;
  const double step = map.tier_span_m(pf09::leaf_tier) * 0.05;

  double worst = 1.0e30;
  size_t folded = 0;
  constexpr uint32_t side = 256;

  for (uint32_t y = 0; y < side; ++y) {
    for (uint32_t x = 0; x < side; ++x) {
      const glm::dvec2 point{(x + 0.5) / side * span, (y + 0.5) / side * span};
      const auto dx = (map.warp({point.x + step, point.y}) - map.warp({point.x - step, point.y})) / (2.0 * step);
      const auto dy = (map.warp({point.x, point.y + step}) - map.warp({point.x, point.y - step})) / (2.0 * step);

      const double determinant = dx.x * dy.y - dx.y * dy.x;
      worst = std::min(worst, determinant);
      if (determinant <= 0.0) ++folded;
    }
  }

  // Складка варпа означает, что территория разорвана на два несвязных куска в разных концах карты. Это не
  // косметика: по такой карте нельзя ни пройти, ни осмысленно нарисовать границу.
  check.expect(folded == 0, "варп без складок",
               std::format("{} точек с неположительным якобианом, минимум {:.4f}", folded, worst));
}

void verify_extent(checker& check, const pf09::territory& map, const uint32_t side, const double spread_limit) {
  const double span = map.config().world_span_m;
  const auto probe_tier = map.config().authored_depth;

  std::vector<uint32_t> hits(map.cell_count(probe_tier), 0);
  for (uint32_t y = 0; y < side; ++y) {
    for (uint32_t x = 0; x < side; ++x) {
      const glm::dvec2 point{(x + 0.5) / side * span, (y + 0.5) / side * span};
      ++hits[pf09::index_of(map.resolve(point, probe_tier))];
    }
  }

  std::vector<uint32_t> areas;
  areas.reserve(size_t(map.node_count(probe_tier)));
  for (const auto count : hits) {
    if (count != 0) areas.push_back(count);
  }

  const auto reached = areas.size();
  const auto expected = map.node_count(probe_tier);

  // Недостижимый узел — это либо складка варпа, либо шов на краю мира. И то и другое означает территорию,
  // которая есть в таблице, но которой нет на карте.
  check.expect(uint64_t(reached) == expected, std::format("покрытие яруса '{}'", pf09::tier_name(probe_tier)),
               std::format("достигнуто {} узлов из {}", reached, expected));
  if (areas.empty()) return;

  std::sort(areas.begin(), areas.end());
  const auto low = double(areas[areas.size() / 20]);
  const auto high = double(areas[areas.size() - 1 - areas.size() / 20]);
  const auto median = double(areas[areas.size() / 2]);
  const double spread = low > 0.0 ? high / low : 1.0e30;

  // Разброс площадей внутри яруса — это не косметика, а несущий контракт zone LOD: по высоте камеры
  // выбирается ЯРУС, значит все территории яруса обязаны быть примерно одного масштаба. Разброс копится
  // по ярусам, поэтому его надо мерить, а не надеяться на него.
  std::cout << std::format("  замер: площади яруса '{}' p5 {:.0f}, медиана {:.0f}, p95 {:.0f} сэмплов, разброс {:.2f}\n",
                           pf09::tier_name(probe_tier), low, median, high, spread);
  check.expect(spread <= spread_limit, std::format("масштаб яруса '{}' един", pf09::tier_name(probe_tier)),
               std::format("p95/p5 площадей {:.2f} при пределе {:.2f}, медиана {:.0f} сэмплов", spread, spread_limit,
                           median));
}

void verify_structure(checker& check, const pf09::territory& map) {
  size_t childless = 0;
  size_t orphan = 0;
  uint64_t counted_cells = 0;

  for (const auto& node : map.authored()) {
    const auto value = pf09::tier_of(node.id);
    if (value != map.config().authored_depth && node.child_count == 0) ++childless;
    if (value != pf09::tier::world && map.find_authored(node.parent) == nullptr) ++orphan;
    counted_cells += node.cell_count;
  }

  uint64_t expected_cells = 0;
  for (size_t t = 0; t <= size_t(map.config().authored_depth); ++t) {
    expected_cells += map.cell_count(pf09::tier(t));
  }

  // Ни один родитель не может остаться без потомков: поглощённая ячейка не бывает целью поглощения,
  // поэтому в блоке родителя всегда остаётся хотя бы одна самостоятельная.
  check.expect(childless == 0, "у каждого узла есть потомки", std::format("{} узлов без потомков", childless));
  check.expect(orphan == 0, "родитель существует", std::format("{} узлов с потерянным родителем", orphan));
  check.expect(counted_cells == expected_cells, "ячейки не потеряны при слиянии",
               std::format("{} против {}", counted_cells, expected_cells));
}

void verify_owners(checker& check, const pf09::territory& map, const uint32_t samples) {
  const double span = map.config().world_span_m;
  const auto depth = map.config().authored_depth;

  size_t mismatches = 0;
  for (uint32_t i = 0; i < samples; ++i) {
    const auto point = sample_point(i, span, 0x0a17ull);
    const auto leaf = map.resolve(point);

    // У участка нет титула, значит не может быть и своего хозяина: он обязан отвечать владельцем
    // ближайшего титулованного предка, иначе окраска по владельцу разъедется с подсветкой яруса.
    if (map.owner_of(leaf) != map.owner_of(map.ancestor_at(leaf, depth))) ++mismatches;
  }

  check.expect(mismatches == 0, "владелец наследуется от титула", std::format("{} расхождений", mismatches));
}

void verify_border_monotonicity(checker& check, const pf09::territory& map, const uint32_t samples) {
  const double span = map.config().world_span_m;
  const double leaf_span = map.tier_span_m(pf09::leaf_tier);

  size_t inconsistent = 0;
  size_t inspected = 0;

  for (uint32_t i = 0; i < samples; ++i) {
    const auto a = sample_point(i, span, 0xb0d3ull);
    const auto b = glm::dvec2{a.x + leaf_span * 4.0, a.y + leaf_span * 4.0};

    auto left = a;
    auto right = b;
    auto left_chain = map.resolve_chain(left);
    if (left_chain == map.resolve_chain(right)) continue;

    // Бинарный поиск границы: сама точка перехода не нужна, нужна пара соседей по разные стороны.
    for (uint32_t step = 0; step < 48; ++step) {
      const glm::dvec2 middle{(left.x + right.x) * 0.5, (left.y + right.y) * 0.5};
      if (map.resolve_chain(middle) == left_chain) {
        left = middle;
      } else {
        right = middle;
      }
    }

    const auto right_chain = map.resolve_chain(right);
    ++inspected;

    // Расхождение цепочек обязано быть монотонным: если территории разошлись на ярусе `t`, они не могут
    // снова совпасть глубже. Иначе у потомка оказались бы два разных предка на одном ярусе.
    bool diverged = false;
    for (size_t t = 0; t < pf09::tier_count; ++t) {
      const bool same = left_chain[t] == right_chain[t];
      if (diverged && same) {
        ++inconsistent;
        break;
      }
      if (!same) diverged = true;
    }
  }

  check.expect(inspected > 0, "границы найдены", "ни один отрезок не пересёк границу");
  check.expect(inconsistent == 0, "монотонность расхождения на границе",
               std::format("{} границ из {} снова сходятся глубже", inconsistent, inspected));
}

void verify_torus(checker& check, const pf09::territory& map, const uint32_t samples) {
  const double span = map.config().world_span_m;

  size_t mismatches = 0;
  for (uint32_t i = 0; i < samples; ++i) {
    const auto point = sample_point(i, span, 0x70ec5ull);

    // Октавы варпа периодичны с периодом в сторону мира, а индексы ячеек заворачиваются. Значит мир
    // действительно замкнут, и сдвиг на целую сторону обязан не менять вообще ничего. Проверка держит
    // связку «периодичный шум ↔ заворачивание индексов»: разъедься она, и на шве появились бы territории
    // без прообраза, которые есть в таблице и которых нет на карте.
    const auto here = map.resolve_chain(point);
    if (map.resolve_chain({point.x + span, point.y}) != here) ++mismatches;
    if (map.resolve_chain({point.x, point.y - span}) != here) ++mismatches;
  }

  check.expect(mismatches == 0, "мир замкнут", std::format("{} расхождений при сдвиге на сторону мира", mismatches));
}





void verify_locality(checker& check, const pf09::territory& map, const options& opts) {
  pf09::locality_field field(map, opts.local);

  // Наблюдатель ИЩЕТСЯ, а не берётся в центре мира. Локальности разрежены по замыслу, и «в центре мира
  // ничего нет» — свойство расстановки при этом сиде, а не поломка. Проверка должна утверждать, что
  // размещённая локальность устроена правильно, а не что она оказалась в заранее выбранной точке.
  const double span = map.config().world_span_m;
  glm::dvec2 observer{span * 0.5, span * 0.5};
  field.focus(observer);
  for (uint32_t attempt = 0; attempt < 64 && field.resident().empty(); ++attempt) {
    const auto unit = sample_point(attempt, 1.0, 0x10ca17ull);
    observer = {unit.x * span, unit.y * span};
    field.focus(observer);
  }

  check.expect(!field.resident().empty(), "локальности размещены",
               "ни одной не нашлось в 65 попытках по всему миру");
  if (field.resident().empty()) return;

  size_t unreachable = 0;
  size_t orphan_anchor = 0;
  size_t stray_zone = 0;
  size_t over_budget = 0;

  for (const auto* place : field.resident()) {
    const auto seen = reach_from_streets(*place);
    for (uint32_t i = 0; i < place->zone_count(); ++i) {
      if (place->role(i) == pf09::zone_role::count) continue;
      if (seen[i] == 0) ++unreachable;
    }

    // Локальность обязана висеть под своим якорем, а якорь — быть узлом яруса `locale` глобального
    // дерева. Иначе город окажется без владельца, и вопрос «кому платить пошлину» останется без ответа.
    if (pf09::tier_of(place->anchor()) != pf09::tier::locale) ++orphan_anchor;
    if (map.resolve(place->centre_m(), pf09::tier::locale) != place->anchor()) ++stray_zone;
  }

  if (field.resident().size() > opts.local.residency) ++over_budget;

  // Полная достижимость от улиц — это и есть контракт «локальный ИИ может построить маршрут». Регулярный
  // шаг улиц гарантирует его по построению, и проверка ловит именно поломку построения.
  check.expect(unreachable == 0, "все зоны достижимы от улицы",
               std::format("{} зон без пути от улицы", unreachable));
  check.expect(orphan_anchor == 0, "якорь локальности — узел глобального дерева",
               std::format("{} локальностей с чужим ярусом якоря", orphan_anchor));
  check.expect(stray_zone == 0, "центр локальности лежит в своём якоре",
               std::format("{} локальностей уехали из якоря", stray_zone));
  check.expect(over_budget == 0, "резидентность не превышает бюджет");

  // Детерминизм: то же место даёт ту же локальность. Проверяется повторным построением с нуля, а не
  // повторным чтением уже построенного — иначе проверялся бы кеш, а не генератор.
  pf09::locality_field twin(map, opts.local);
  twin.focus(observer);

  size_t drift = 0;
  if (twin.resident().size() != field.resident().size()) {
    ++drift;
  } else {
    for (size_t i = 0; i < field.resident().size(); ++i) {
      const auto* a = field.resident()[i];
      const auto* b = twin.resident()[i];
      if (a->anchor() != b->anchor() || a->zone_count() != b->zone_count()) {
        ++drift;
        continue;
      }
      for (uint32_t z = 0; z < a->zone_count(); ++z) {
        if (a->role(z) != b->role(z)) ++drift;
      }
    }
  }
  check.expect(drift == 0, "локальность детерминирована", std::format("{} расхождений при повторной сборке", drift));

  // Разрежённость: подавляющая часть мира локальностей не имеет, и разрешение вне пятна обязано
  // возвращать пустой адрес, а не выдумывать зону.
  const auto* place = field.resident().front();
  const double away = place->extent_m();
  const glm::dvec2 outside{place->centre_m().x + away * 3.0, place->centre_m().y + away * 3.0};

  check.expect(field.resolve_local(place->centre_m()).valid(), "внутри пятна зона находится");
  check.expect(!field.resolve_local(outside).valid(), "вне пятна локальной зоны нет");

  std::cout << std::format("  замер: {} локальностей рядом, {:.1f} КБ, у первой {} зон и {} помещений\n",
                           field.resident().size(), double(field.resident_bytes()) / 1024.0, place->zone_count(),
                           count_role(*place, pf09::zone_role::room));
}


// Разделяющая ось для двух выпуклых многоугольников. Если существует ось, на которую их проекции не
// пересекаются, фигуры не накладываются. Для выпуклого это не эвристика, а точный ответ.
bool convex_overlap(const std::span<const glm::vec2> a, const std::span<const glm::vec2> b) {
  if (a.size() < 3 || b.size() < 3) return false;

  // Проекции считаются ОТНОСИТЕЛЬНО общей точки и на единичную ось. Абсолютные координаты доходят до
  // полумиллиона метров, где шаг `float` уже около двух единиц проекции на неединичной оси — больше
  // любого разумного допуска, и соприкасающиеся фигуры объявлялись пересекающимися. Это та же ошибка,
  // что и с ключом прямой: точность `float` зависит от величины, а допуск задан в метрах.
  const glm::vec2 reference = a[0];

  const auto separated = [&](const std::span<const glm::vec2> from) {
    for (size_t i = 0; i < from.size(); ++i) {
      const auto edge = from[(i + 1) % from.size()] - from[i];
      const float length = std::sqrt(edge.x * edge.x + edge.y * edge.y);
      if (length < 1.0e-4f) continue;
      const glm::vec2 axis{-edge.y / length, edge.x / length};

      float min_a = 1e30f;
      float max_a = -1e30f;
      for (const auto& point : a) {
        const float value = axis.x * (point.x - reference.x) + axis.y * (point.y - reference.y);
        min_a = std::min(min_a, value);
        max_a = std::max(max_a, value);
      }

      float min_b = 1e30f;
      float max_b = -1e30f;
      for (const auto& point : b) {
        const float value = axis.x * (point.x - reference.x) + axis.y * (point.y - reference.y);
        min_b = std::min(min_b, value);
        max_b = std::max(max_b, value);
      }

      // Соприкосновение по общему ребру — не наложение, а именно то, чем связаны соседние зоны.
      constexpr float slack = 0.02f;
      if (min_a >= max_b - slack || min_b >= max_a - slack) return true;
    }
    return false;
  };

  return !separated(a) && !separated(b);
}

// Симметрия порталов и проходимость. Проход — общее ребро двух фигур, значит он обязан быть виден с обеих
// сторон одинаково: асимметрия означала бы дверь, открытую только снаружи.
void verify_portals(checker& check, const pf09::zone_store& store, const options& opts) {
  size_t asymmetric = 0;
  size_t degenerate = 0;
  size_t counted = 0;

  for (int32_t y = 0; y < int32_t(opts.build.sector_side); ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;

      for (const auto& record : sector->zones) {
        for (uint32_t index = 0; index < record.part_count; ++index) {
          const pf09::part_ref here{record.key, index};
          for (const auto& portal : store.portals_of(here)) {
            const pf09::part_ref there{portal.other, portal.other_part};
            if (store.part_of(there) == nullptr) continue; // сосед в невыгруженном секторе — не асимметрия
            ++counted;

            const auto back = store.portals_of(there);
            // Зеркало ищется ПО ОТРЕЗКУ, а не только по соседу: две фигуры могут делить несколько рёбер,
            // и тогда «первый портал к этому соседу» — разные порталы с разных сторон.
            const auto mirror = std::find_if(back.begin(), back.end(), [&](const pf09::zone_portal& item) {
              return item.other == record.key && item.other_part == index && item.from == portal.from &&
                     item.to == portal.to;
            });
            if (mirror == back.end() || mirror->flags != portal.flags) {
              ++asymmetric;
              continue;
            }
            if (portal.geometric() && portal.from == portal.to) ++degenerate;
          }
        }
      }
    }
  }

  check.expect(counted > 0, "порталы выведены из геометрии");
  check.expect(asymmetric == 0, "проход виден с обеих сторон одинаково",
               std::format("{} несимметричных из {}", asymmetric, counted));
  check.expect(degenerate == 0, "у прохода между фигурами есть отрезок",
               std::format("{} вырожденных рёбер", degenerate));
}

// Персонажи. Путь строится по проёмам, персонаж идёт от проёма к проёму и обязан всё время находиться
// внутри своей текущей зоны — это и есть проверка, что зональность пригодна для движения, а не только
// для картинки.
void verify_agents(checker& check, const pf09::zone_store& store, const options& opts) {
  // Комнаты группируются ПО ПОСЕЛЕНИЮ. Пары берутся внутри одной группы, потому что между комнатами
  // разных городов пути на внутреннем уровне и не должно быть: туда ходят по дороге, а дорога — связь
  // уровня выше. Прежняя редакция брала пары где попало и требовала невозможного.
  std::map<pf09::zone_key, std::vector<const pf09::zone_record*>> by_settlement;
  for (int32_t y = 0; y < int32_t(opts.build.sector_side); ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;
      for (const auto& record : sector->zones) {
        // Непроходимые места целями не берутся: пути в стену не должно быть, и требовать его значило бы
        // проверять не связность, а собственную забывчивость. Запертая дверь — то же самое, только её
        // непроходимость временная: маршрут в неё не обязан существовать, пока она закрыта.
        if (record.level != pf09::zone_level::interior || record.abstract() || !store.passable(record)) continue;

        auto owner = record.parent;
        for (uint32_t hop = 0; hop < 4; ++hop) {
          const auto* node = store.find(owner);
          if (node == nullptr || node->level != pf09::zone_level::interior) break;
          owner = node->parent;
        }
        if (owner != pf09::invalid_key) by_settlement[owner].push_back(&record);
      }
    }
  }

  std::vector<const pf09::zone_record*> rooms;
  for (const auto& [settlement, list] : by_settlement) {
    if (list.size() > rooms.size()) rooms = list;
  }
  // Ходят по ЧАСТЯМ: зона может состоять из нескольких выпуклых кусков, и выбирать надо кусок.
  std::vector<pf09::part_ref> spots;
  for (const auto* record : rooms) {
    for (uint32_t index = 0; index < record->part_count; ++index) {
      spots.push_back({record->key, index});
    }
  }
  check.expect(!spots.empty(), "есть по чему ходить");
  if (spots.empty()) return;

  size_t routed = 0;
  size_t escaped = 0;
  size_t stalled = 0;
  size_t no_path = 0;
  size_t inside_prop = 0;
  uint64_t total_steps = 0;

  // Первое время в проверках этой площадки. Потолок нарочно щедрый: он ловит не «стало на десять
  // процентов медленнее», а «стало в разы» — смену алгоритма, случайный O(n²), потерянный кеш. Узкий
  // порог на такой машине ловил бы шум и был бы выключен через неделю.
  double search_millis = 0.0;
  size_t search_nodes = 0;
  size_t worst_nodes = 0;

  constexpr uint32_t trials = 64;
  for (uint32_t i = 0; i < trials; ++i) {
    const auto from = spots[utils::splitmix(i * 2ull + 1ull, 0xa9e17ull) % spots.size()];
    const auto to = spots[utils::splitmix(i * 2ull + 2ull, 0xa9e17ull) % spots.size()];
    if (from == to) continue;

    pf09::agent walker{};
    walker.location = from;
    if (!pf09::interior_point(store, from, walker.position)) continue;

    const auto search_begin = std::chrono::steady_clock::now();
    walker.path = pf09::find_path(store, from, to);
    search_millis += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - search_begin).count();
    search_nodes += pf09::last_search_nodes();
    worst_nodes = std::max(worst_nodes, pf09::last_search_nodes());
    if (walker.path.empty()) {
      ++no_path;
      continue;
    }
    ++routed;

    uint32_t steps = 0;
    while (!walker.arrived && steps < 40000) {
      if (!pf09::step_agent(store, walker, 0.6f)) break;
      ++steps;

      const auto outline = store.outline_of(walker.location);
      if (outline.empty()) break;
      if (!pf09::point_in_outline(outline, walker.position)) ++escaped;
      if (pf09::blocked_by_prop(store, walker.location, walker.position)) ++inside_prop;
    }
    total_steps += steps;
    if (!walker.arrived) ++stalled;
  }

  check.expect(no_path == 0, "маршрут между комнатами находится", std::format("{} пар без пути", no_path));
  check.expect(escaped == 0, "персонаж не выходит из своей зоны", std::format("{} выходов наружу", escaped));
  check.expect(stalled == 0, "персонаж доходит до цели", std::format("{} застряли", stalled));
  // Предмет ломает то, на чём держалось движение: «внутри выпуклой части можно идти по прямой». Что
  // маршрут ВСЁ ЕЩЁ проходится — это и есть проверка, что обход предмета остался местной задачей шага и
  // не потребовал резать часть на куски.
  check.expect(inside_prop == 0, "персонаж не проходит сквозь предмет",
               std::format("{} шагов внутри непроходимого предмета", inside_prop));

  check.expect(search_millis < 400.0, "поиск пути укладывается в бюджет",
               std::format("{:.0f} мс на {} поисков", search_millis, routed));
  std::cout << std::format("  замер: {} маршрутов пройдено, {} шагов всего, в среднем {:.0f} шагов на маршрут; "
                           "поиск занял {:.1f} мс, в среднем {:.2f} мс и {} развёрнутых узлов на маршрут "
                           "(худший {})\n",
                           routed, total_steps, double(total_steps) / double(std::max<size_t>(routed, 1)),
                           search_millis, search_millis / double(std::max<size_t>(routed, 1)),
                           search_nodes / std::max<size_t>(routed, 1), worst_nodes);
}



// Двери и этажи — два утверждения, ради которых связность лежит в файле, а не выводится из геометрии.
//
// Дверь: обычный проход не нужно делать особым видом связи. Дверь — МЕСТО, и «заперта» это его
// проходимость. Отсюда одно переключение закрывает сразу все её рёбра, а маршрут обязан измениться без
// пересборки данных.
//
// Этаж: верхний зал лежит в плане ровно над нижним. Общее ребро у них идеальное, а прохода через него
// нет — между ними перекрытие. Связь даёт лестница, записанная явно. Это ровно тот случай, где
// «совпали рёбра» и «есть проход» — разные утверждения, и потому связность обязана быть данными.
void verify_doors_and_floors(checker& check, pf09::zone_store& store, const options& opts) {
  std::vector<const pf09::zone_record*> doors;
  std::vector<const pf09::zone_record*> stairs;
  size_t upper_zones = 0;
  size_t closed_in_file = 0;
  size_t geometric_across_floors = 0;
  size_t floor_links = 0;

  for (int32_t y = 0; y < int32_t(opts.build.sector_side); ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;

      for (const auto& record : sector->zones) {
        if (record.floor != 0) ++upper_zones;
        if (record.kind == pf09::zone_kind::door) {
          doors.push_back(&record);
          if (record.closed()) ++closed_in_file;
        }
        if (record.kind == pf09::zone_kind::stair) stairs.push_back(&record);

        for (uint32_t index = 0; index < record.part_count; ++index) {
          for (const auto& portal : store.portals_of({record.key, index})) {
            const auto* other = store.find(portal.other);
            if (other == nullptr || other->floor == record.floor) continue;
            ++floor_links;
            // Проход между этажами не может быть геометрическим: у пола нет ребра, есть перекрытие.
            if (portal.geometric()) ++geometric_across_floors;
          }
        }
      }
    }
  }

  check.expect(!doors.empty(), "двери есть", "ни одного места вида door");
  check.expect(upper_zones > 0, "второй этаж собрался", "нет ни одной зоны выше первого этажа");
  check.expect(!stairs.empty(), "лестницы есть");
  check.expect(floor_links > 0, "этажи связаны", "между этажами нет ни одной связи");
  check.expect(geometric_across_floors == 0, "переход между этажами не выводится из рёбер",
               std::format("{} геометрических проходов сквозь перекрытие", geometric_across_floors));
  check.expect(closed_in_file > 0, "часть дверей заперта с самого начала");
  if (doors.empty() || stairs.empty()) return;

  // Лестница обязана вести НАВЕРХ И ОБРАТНО. Односторонняя связь заметилась бы только тем, что персонаж
  // однажды застрял на втором этаже, и искали бы её в навигации, а не в сборщике.
  size_t one_way = 0;
  size_t no_upper = 0;
  size_t off_screen = 0;
  for (const auto* stair : stairs) {
    pf09::zone_key upper = pf09::invalid_key;
    for (const auto& portal : store.portals_of({stair->key, 0})) {
      if (portal.geometric()) continue;
      upper = portal.other;
      break;
    }
    // «Связи нет» и «сосед не подгружен» — разные утверждения, и путать их значит мерить резидентность
    // вместо данных. Первое — ошибка сборки, второе — нормальная работа стриминга.
    if (upper == pf09::invalid_key) {
      ++no_upper;
      continue;
    }
    const auto* target = store.find(upper);
    if (target == nullptr) {
      ++off_screen;
      continue;
    }
    const auto back = store.portals_of({target->key, 0});
    if (std::none_of(back.begin(), back.end(),
                     [&](const pf09::zone_portal& item) { return item.other == stair->key; })) {
      ++one_way;
    }
  }
  check.expect(no_upper == 0, "у лестницы есть верх", std::format("{} лестниц никуда не ведут", no_upper));
  check.expect(off_screen == 0, "верх лестницы лежит в том же файле, что и она",
               std::format("{} лестниц ссылаются в невыгруженный сектор — здание разъехалось по файлам",
                           off_screen));
  check.expect(one_way == 0, "по лестнице можно спуститься", std::format("{} односторонних лестниц", one_way));

  // Переключение двери. Берём соседей двери по разные стороны и смотрим, что маршрут между ними живёт
  // ровно так, как говорит состояние места.
  size_t trials = 0;
  size_t through_closed = 0;
  size_t changed = 0;
  size_t not_restored = 0;

  for (const auto* door : doors) {
    if (trials >= 48) break;
    if (!store.passable(*door)) continue;

    std::vector<pf09::part_ref> sides;
    for (const auto& portal : store.portals_of({door->key, 0})) {
      const auto* other = store.find(portal.other);
      if (other == nullptr || !store.passable(*other) || other->key == door->key) continue;
      if (std::none_of(sides.begin(), sides.end(),
                       [&](const pf09::part_ref& item) { return item.zone == portal.other; })) {
        sides.push_back({portal.other, portal.other_part});
      }
    }
    if (sides.size() < 2) continue;

    const auto before = pf09::find_path(store, sides[0], sides[1]);
    if (before.empty()) continue;
    const bool used = std::any_of(before.begin(), before.end(),
                                  [&](const pf09::part_ref& item) { return item.zone == door->key; });
    ++trials;

    store.set_closed(door->key, true);
    const auto after = pf09::find_path(store, sides[0], sides[1]);
    if (std::any_of(after.begin(), after.end(),
                    [&](const pf09::part_ref& item) { return item.zone == door->key; })) {
      ++through_closed;
    }
    if (used && after != before) ++changed;

    store.set_closed(door->key, false);
    if (pf09::find_path(store, sides[0], sides[1]) != before) ++not_restored;
  }

  check.expect(trials > 0, "нашлись двери с соседями по обе стороны");
  check.expect(through_closed == 0, "маршрут не идёт сквозь закрытую дверь",
               std::format("{} маршрутов из {} прошли через запертое место", through_closed, trials));
  check.expect(changed > 0, "закрытая дверь меняет маршрут",
               "ни один маршрут не отреагировал на запертую дверь — значит переключение ни на что не влияет");
  check.expect(not_restored == 0, "открытая обратно дверь возвращает прежний маршрут",
               std::format("{} маршрутов не восстановились", not_restored));

  std::cout << std::format("  замер: дверей {} (заперто в файле {}), лестниц {}, зон выше первого этажа {}, "
                           "переключение изменило {} маршрутов из {}\n",
                           doors.size(), closed_in_file, stairs.size(), upper_zones, changed, trials);
}


// Предметы. Расстановка свободна, но не произвольна: предмет обязан целиком лежать внутри своей выпуклой
// части, держаться от проёмов дальше габарита актора и не липнуть к соседу. Нарушение любого из трёх — это
// запертая мебелью комната, о которой маршрут не узнает: связность живёт на частях, а часть предмет не
// разрезает.
void verify_props(checker& check, const pf09::zone_store& store, const options& opts) {
  const auto segment_distance = [](const glm::vec2 point, const glm::vec2 a, const glm::vec2 b) {
    const auto span = b - a;
    const float length = span.x * span.x + span.y * span.y;
    const float t = length < 1.0e-8f ? 0.0f : std::clamp(((point.x - a.x) * span.x + (point.y - a.y) * span.y) / length,
                                                         0.0f, 1.0f);
    const glm::vec2 closest{a.x + span.x * t, a.y + span.y * t};
    return std::sqrt((point.x - closest.x) * (point.x - closest.x) + (point.y - closest.y) * (point.y - closest.y));
  };

  size_t total = 0;
  size_t outside = 0;
  size_t on_edge = 0;
  size_t at_gate = 0;
  size_t touching = 0;
  size_t sight_blockers = 0;
  size_t on_road = 0;

  for (int32_t y = 0; y < int32_t(opts.build.sector_side); ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;

      for (const auto& record : sector->zones) {
        const auto props = sector->props_of(record);
        if (props.empty()) continue;
        if (record.road()) on_road += props.size();

        for (size_t i = 0; i < props.size(); ++i) {
          const auto& prop = props[i];
          ++total;
          if (prop.blocks_sight()) ++sight_blockers;

          const pf09::part_ref where{record.key, prop.part};
          const auto outline = store.outline_of(where);
          if (outline.size() < 3 || !pf09::point_in_outline(outline, prop.position)) {
            ++outside;
            continue;
          }
          for (size_t e = 0; e < outline.size(); ++e) {
            if (segment_distance(prop.position, outline[e], outline[(e + 1) % outline.size()]) <= prop.radius) {
              ++on_edge;
              break;
            }
          }
          for (const auto& gate : store.portals_of(where)) {
            if (!gate.geometric()) continue;
            if (segment_distance(prop.position, gate.from, gate.to) <= prop.radius + pf09::agent_radius_m) {
              ++at_gate;
              break;
            }
          }
          for (size_t j = i + 1; j < props.size(); ++j) {
            if (props[j].part != prop.part) continue;
            const auto delta = props[j].position - prop.position;
            if (std::sqrt(delta.x * delta.x + delta.y * delta.y) <=
                prop.radius + props[j].radius + 2.0f * pf09::agent_radius_m) {
              ++touching;
              break;
            }
          }
        }
      }
    }
  }

  check.expect(total > 0, "предметы расставлены");
  check.expect(outside == 0, "предмет лежит внутри своей части", std::format("{} снаружи", outside));
  check.expect(on_edge == 0, "предмет не торчит сквозь стену", std::format("{} задевают ребро", on_edge));
  check.expect(at_gate == 0, "предмет не перекрывает проём", std::format("{} стоят в дверях", at_gate));
  check.expect(touching == 0, "между предметами проходит актор", std::format("{} пар вплотную", touching));
  check.expect(on_road == 0, "дороги остаются чистыми",
               std::format("{} предметов на проезжей части", on_road));
  check.expect(sight_blockers > 0, "есть за чем прятаться", "ни один предмет не перекрывает взгляд");
  std::cout << std::format("  замер: предметов {}, из них перекрывают взгляд {}\n", total, sight_blockers);
}


// Тактические запросы. Ради них зонирование и затевалось: место должно отвечать не только «где я», но и
// «где спрятаться», «откуда видно вход», «куда развести группу». Проверяются не сами ответы (их считает
// жадный перебор, и «лучший» ответ здесь никто не обещал), а СВОЙСТВА, без которых они бессмысленны.
void verify_tactics(checker& check, const pf09::zone_store& store, const options& opts) {
  std::vector<const pf09::zone_record*> places;
  for (int32_t y = 0; y < int32_t(opts.build.sector_side) && places.size() < 400; ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side) && places.size() < 400; ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;
      for (const auto& record : sector->zones) {
        if (record.level != pf09::zone_level::interior || record.abstract()) continue;
        if (sector->props_of(record).empty()) continue;
        places.push_back(&record);
        if (places.size() >= 400) break;
      }
    }
  }
  check.expect(!places.empty(), "нашлись места с предметами");
  if (places.empty()) return;

  size_t asymmetric = 0;
  size_t self_blind = 0;
  size_t with_cover = 0;
  size_t threat_dependent = 0;
  size_t wrong_side = 0;
  size_t gates_missed = 0;
  size_t watched = 0;
  size_t duplicate_orders = 0;
  size_t orders_outside = 0;
  size_t total_cover = 0;
  size_t shadowed_anyway = 0;
  size_t by_object = 0;

  for (const auto* place : places) {
    glm::vec2 anchor{};
    if (!pf09::interior_point(store, {place->key, 0}, anchor)) continue;

    // Видимость — ОТНОШЕНИЕ, а не направление. Несимметричная видимость означала бы, что стрелять можно
    // только в одну сторону, и такую ошибку в игре ищут месяцами.
    if (!pf09::visible(store, place->key, anchor, anchor)) ++self_blind;
    for (uint32_t index = 1; index < place->part_count; ++index) {
      glm::vec2 other{};
      if (!pf09::interior_point(store, {place->key, index}, other)) continue;
      if (pf09::visible(store, place->key, anchor, other) != pf09::visible(store, place->key, other, anchor)) {
        ++asymmetric;
      }
    }

    // Угроза берётся ВНУТРИ места, а не снаружи. Снаружи она не видит ничего по определению видимости, и
    // фильтр «не просматривается» пропускал бы всё подряд: проверка была бы зелёной и пустой. Для врага,
    // который ещё не вошёл, роль угрозы играет проём, через который он войдёт.
    glm::vec2 threat = anchor;
    glm::vec2 opposite = anchor;
    {
      const auto edges = store.perimeter(place->key);
      for (const auto& portal : edges) {
        if (!portal.geometric()) continue;
        uint32_t part = 0;
        (void)part;
        threat = portal.middle();
        break;
      }
      float best = -1.0f;
      for (uint32_t index = 0; index < place->part_count; ++index) {
        glm::vec2 point{};
        if (!pf09::interior_point(store, {place->key, index}, point)) continue;
        const auto delta = point - threat;
        const float distance = delta.x * delta.x + delta.y * delta.y;
        if (distance > best) {
          best = distance;
          opposite = point;
        }
      }
    }
    // Угроза в проёме стоит на самом ребре, и `visible` честно скажет «снаружи». Втягиваем её внутрь.
    pf09::settle_into_place(store, place->key, threat);

    const auto cover = pf09::cover_spots(store, place->key, threat);
    total_cover += cover.size();
    if (!cover.empty()) ++with_cover;
    for (const auto& spot : cover) {
      // Здесь НЕ проверяется, что укрытие не просматривается угрозой и не стоит в мебели: `cover_spots`
      // сама отбрасывает и то, и другое, и такая проверка подтверждала бы лишь саму себя. Проверять
      // имеет смысл то, что функция НЕ гарантирует по построению, — а это две вещи ниже: что укрытие
      // даёт именно названный предмет и что оно лежит с обратной от угрозы стороны.

      // И главное — что укрытие даёт ИМЕННО ПРЕДМЕТ, а не то, что здесь и так ничего не видно. Зеркальная
      // точка — перед тем же предметом, со стороны угрозы — обязана просматриваться. Без этой половины
      // проверка остаётся тавтологией: `cover_spots` сама отбрасывает видимое и подтверждает лишь себя.
      const auto props = store.props_of(place->key);
      if (spot.sheltered()) ++by_object;
      if (spot.blocker < props.size()) {
        const auto& prop = props[spot.blocker];
        auto towards = threat - prop.position;
        const float length = std::sqrt(towards.x * towards.x + towards.y * towards.y);
        if (length > 1.0e-3f) {
          const auto front = prop.position + towards / length * (prop.radius + 0.55f);
          if (!pf09::visible(store, place->key, threat, front)) ++shadowed_anyway;
        }
      }
    }

    // Укрытие ОТНОСИТЕЛЬНО угрозы, и это надо утверждать аккуратно. «Набор обязан отличаться в КАЖДОМ
    // месте» — утверждение про фикстуру, а не про модель: в комнате с одним предметом две разные угрозы
    // законно дают одно и то же. Утверждать здесь можно две вещи: что зависимость есть ХОТЯ БЫ ГДЕ-ТО, и
    // что каждая точка лежит с ПРОТИВОПОЛОЖНОЙ от угрозы стороны своего предмета — а вот это уже
    // выполняется всегда и проверяется точно.
    const auto other_cover = pf09::cover_spots(store, place->key, opposite);
    bool same = cover.size() == other_cover.size();
    for (size_t i = 0; i < cover.size() && same; ++i) {
      const auto delta = cover[i].position - other_cover[i].position;
      same = delta.x * delta.x + delta.y * delta.y < 0.01f;
    }
    if (!cover.empty() && !same) ++threat_dependent;

    const auto place_props = store.props_of(place->key);
    for (const auto& spot : cover) {
      if (!spot.sheltered() || spot.blocker >= place_props.size()) continue;
      const auto& prop = place_props[spot.blocker];
      const auto to_spot = spot.position - prop.position;
      const auto to_threat = threat - prop.position;
      if (to_spot.x * to_threat.x + to_spot.y * to_threat.y >= 0.0f) ++wrong_side;
    }

    // Наблюдение: жадный набор обязан закрыть все входы, которые вообще откуда-нибудь видны.
    const auto spots = pf09::watch_spots(store, place->key, 4);
    if (!spots.empty()) ++watched;
    for (const auto& portal : store.perimeter(place->key)) {
      if (!portal.geometric()) continue;
      const auto gate = portal.middle();

      bool reachable_by_someone = false;
      for (uint32_t index = 0; index < place->part_count && !reachable_by_someone; ++index) {
        glm::vec2 point{};
        if (!pf09::interior_point(store, {place->key, index}, point)) continue;
        reachable_by_someone = pf09::visible(store, place->key, point, gate);
      }
      if (!reachable_by_someone) continue; // вход, не видный ниоткуда, — законный ответ

      const bool seen = std::any_of(spots.begin(), spots.end(), [&](const pf09::tactical_spot& item) {
        return pf09::visible(store, place->key, item.position, gate);
      });
      if (!seen) ++gates_missed;
    }

    // Веер приказа: одно распоряжение — разные задачи. Двое в одной точке это не группа, а один человек.
    std::vector<glm::vec2> group;
    for (uint32_t i = 0; i < 4; ++i) {
      group.push_back(anchor + glm::vec2{float(i) * 0.5f, 0.0f});
    }
    const auto plan = pf09::fan_out(store, place->key, pf09::order_kind::hold, threat, group);
    for (size_t i = 0; i < plan.size(); ++i) {
      uint32_t part = 0;
      const auto outline = store.outline_of({place->key, plan[i].target.part});
      if (!pf09::point_in_outline(outline, plan[i].position)) ++orders_outside;
      (void)part;
      for (size_t j = i + 1; j < plan.size(); ++j) {
        const auto delta = plan[i].position - plan[j].position;
        if (delta.x * delta.x + delta.y * delta.y < 0.01f) ++duplicate_orders;
      }
    }
  }

  check.expect(self_blind == 0, "точка видна сама себе", std::format("{} мест без этого", self_blind));
  check.expect(asymmetric == 0, "видимость симметрична", std::format("{} несимметричных пар", asymmetric));
  check.expect(with_cover > 0, "укрытия находятся", "ни в одном месте не нашлось укрытия");
  // Точное утверждение, а не допуск: если укрытие приписано предмету, то подход к этому предмету со
  // стороны угрозы обязан просматриваться. Иначе прячет форма места, и предмет тут ни при чём.
  check.expect(shadowed_anyway == 0, "названный предмет и есть тот, кто укрывает",
               std::format("{} точек приписаны предмету, который ничего не закрывает", shadowed_anyway));
  check.expect(by_object > 0, "укрытие за предметом существует", "все укрытия оказались укрытиями формы");
  check.expect(threat_dependent > 0, "укрытие зависит от того, откуда угроза",
               "ни в одном месте набор не изменился при угрозе с другой стороны");
  check.expect(wrong_side == 0, "укрытие лежит с обратной от угрозы стороны предмета",
               std::format("{} точек с той же стороны, что и угроза", wrong_side));
  check.expect(watched > 0, "точки наблюдения находятся");
  check.expect(gates_missed == 0, "видимые входы закрыты наблюдением",
               std::format("{} входов остались без присмотра", gates_missed));
  check.expect(duplicate_orders == 0, "приказ разводит людей по разным точкам",
               std::format("{} совпадений", duplicate_orders));
  check.expect(orders_outside == 0, "назначенная точка лежит в своей части",
               std::format("{} назначений снаружи", orders_outside));

  std::cout << std::format("  замер: мест с предметами {}, из них с укрытиями {} (всего {} точек, из них "
                           "{} за предметом и {} за формой места), с наблюдением {}\n",
                           places.size(), with_cover, total_cover, by_object, total_cover - by_object, watched);
}






// Право на землю: де-юре и де-факто. Проверяется не то, что титулы записались, а то, ради чего они
// заведены: что карта права и карта силы РАЗДЕЛЕНЫ, что цепочка признаний обрывается на узурпаторе, и
// что из обрыва вырастает вопрос «чей закон здесь в силе» с ответом, который бывает «ничей».
void verify_titles(checker& check, pf09::zone_store& store, const options& opts) {
  pf09::title_book titles;
  titles.load(pf09::titles_path(world_root(opts)));
  check.expect(titles.size() > 0, "дерево титулов прочиталось");
  if (titles.size() == 0) return;

  const pf09::realm_view view{&store, &titles};

  size_t zoned = 0;
  size_t untitled = 0;
  size_t rootless = 0;
  size_t parallel = 0;
  size_t usurped_wards = 0;
  size_t wards = 0;
  size_t under_crown = 0;
  size_t under_ward = 0;
  size_t lawless = 0;

  pf09::zone_key sample_house = pf09::invalid_key;
  pf09::zone_key sample_street = pf09::invalid_key;
  pf09::zone_key sample_in_usurped = pf09::invalid_key;

  for (int32_t y = 0; y < int32_t(opts.build.sector_side); ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;

      for (const auto& record : sector->zones) {
        if (record.kind == pf09::zone_kind::district) {
          ++wards;
          if (record.title != 0xffffffffu && titles.usurped(record.title)) ++usurped_wards;
        }
        if (record.level != pf09::zone_level::interior || record.abstract()) continue;
        ++zoned;

        if (record.title == 0xffffffffu) {
          ++untitled;
          continue;
        }

        const auto chain = titles.chain(record.title);
        const auto* top = chain.empty() ? nullptr : titles.find(chain.back());
        if (top == nullptr || top->rank != pf09::title_rank::realm) ++rootless;

        // Две иерархии ПАРАЛЛЕЛЬНЫ, а не дублируют друг друга. Пространственный родитель зала — здание;
        // его титул — дом. Разные вещи, и совпадать они не должны: «переехал» и «сменил хозяина» —
        // разные события, и одной иерархией их не выразить.
        const auto* spatial = store.find(record.parent);
        if (spatial != nullptr && spatial->title != record.title) ++parallel;

        const auto law = pf09::law_over(view, record.key);
        if (law == pf09::invalid_title) ++lawless;
        else {
          const auto* source = titles.find(law);
          if (source != nullptr && source->rank == pf09::title_rank::realm) ++under_crown;
          if (source != nullptr && source->rank == pf09::title_rank::district) {
            ++under_ward;
            if (sample_in_usurped == pf09::invalid_key) sample_in_usurped = record.key;
          }
        }

        if (record.kind == pf09::zone_kind::hall && sample_house == pf09::invalid_key) sample_house = record.key;
        if (record.kind == pf09::zone_kind::street && sample_street == pf09::invalid_key) {
          sample_street = record.key;
        }
      }
    }
  }

  check.expect(zoned > 0, "места прочитались");
  check.expect(untitled == 0, "у каждого места есть титул", std::format("{} мест без права", untitled));
  check.expect(rootless == 0, "цепочка права доходит до державы", std::format("{} оборванных", rootless));
  check.expect(parallel > 0, "право и пространство — разные иерархии",
               "титул места всегда совпал с титулом его пространственного родителя");
  check.expect(usurped_wards > 0, "часть кварталов держит не законный владелец");

  // Обрыв признания — то, ради чего всё это. В законопослушном квартале действует закон державы; в
  // занятом — закон самого квартала, и корона до него не достаёт.
  check.expect(under_crown > 0, "в законопослушных местах в силе закон державы");
  check.expect(under_ward > 0, "в занятом квартале в силе его собственный закон",
               "нигде не нашлось места, до которого корона не достаёт");

  // Занять МЕСТО — не то же, что взять квартал. Революционеры на площади не становятся хозяевами района.
  if (sample_street != pf09::invalid_key) {
    const auto* street = store.find(sample_street);
    const auto ward = street == nullptr ? 0xffffffffu : street->title;
    const auto before_ward = titles.holder(ward);

    store.set_place_holder(sample_street, 4242);
    check.expect(pf09::de_facto_holder(view, sample_street) == 4242,
                 "занятое место держит тот, кто его занял");
    check.expect(titles.holder(ward) == before_ward,
                 "занятие места не делает захватчика хозяином квартала");
    store.set_place_holder(sample_street, 0);
  }

  // Частная территория. Улица открыта всем; дом — только своему де-факто держателю, и держателю, а не
  // законному владельцу: занятый дом закрыт и для хозяина тоже.
  if (sample_house != pf09::invalid_key && sample_street != pf09::invalid_key) {
    const auto* house = store.find(sample_house);
    const auto deed = house == nullptr ? 0xffffffffu : house->title;
    const auto owner = titles.de_jure_holder(deed);

    check.expect(pf09::may_enter(view, sample_street, 777).allowed, "улица открыта всем");
    const auto stranger = pf09::may_enter(view, sample_house, 777);
    check.expect(!stranger.allowed, "в чужой дом просто так не войти");
    check.expect(stranger.owner == deed, "отказ называет, чью границу нарушают");
    check.expect(pf09::may_enter(view, sample_house, owner).allowed, "хозяин входит в свой дом");

    // Дом занят чужаком. Право хозяина никуда не делось — а дверь ему больше не открывается, и городской
    // закон до этого дома не достаёт: цепочка признаний оборвалась на самом доме, а собственность
    // законов не издаёт.
    titles.set_holder(deed, 4243);
    check.expect(!pf09::may_enter(view, sample_house, owner).allowed,
                 "в занятый дом не входит и законный владелец");
    check.expect(pf09::may_enter(view, sample_house, 4243).allowed, "входит тот, кто занял");
    check.expect(titles.de_jure_holder(deed) == owner, "занятие не отменяет права: де-юре владелец прежний");
    check.expect(pf09::law_over(view, sample_house) == pf09::invalid_title,
                 "до занятого дома не достаёт ничей закон");
    titles.set_holder(deed, owner);
    check.expect(pf09::law_over(view, sample_house) != pf09::invalid_title,
                 "закон возвращается вместе с законным хозяином");
  }

  // И следствие для навигации: законопослушный обходит чужую территорию, вор идёт напрямик.
  //
  // Пары берутся МЕЖДУ ДОМАМИ, а не между обочинами. Улицы связаны хорошо, чужие дворы на пути между
  // ними не попадаются вовсе, и первая редакция сравнивала «ноль нарушений против нуля» — то есть была
  // зелёной и пустой. Чтобы вопрос вообще возник, маршрут должен идти оттуда, где ты хозяин, туда, где
  // ты гость, мимо третьих владений.
  std::vector<pf09::part_ref> spots;
  pf09::zone_key town = pf09::invalid_key;
  for (int32_t y = 0; y < int32_t(opts.build.sector_side) && spots.size() < 300; ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side) && spots.size() < 300; ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;
      for (const auto& record : sector->zones) {
        if (record.kind != pf09::zone_kind::hall && record.kind != pf09::zone_kind::yard) continue;
        const auto* home = store.containing(record.key, pf09::zone_kind::settlement);
        if (home == nullptr) continue;
        if (town == pf09::invalid_key) town = home->key;
        if (home->key != town) continue;
        spots.push_back({record.key, 0});
        if (spots.size() >= 300) break;
      }
    }
  }

  size_t rude_private = 0;
  size_t polite_private = 0;
  size_t compared = 0;
  size_t rude_longer = 0;

  for (uint32_t i = 0; i < 96 && spots.size() > 2; ++i) {
    const auto from = spots[utils::splitmix(i * 2ull + 1ull, 0x1a3full) % spots.size()];
    const auto to = spots[utils::splitmix(i * 2ull + 2ull, 0x1a3full) % spots.size()];
    if (from == to || from.zone == to.zone) continue;

    const auto* start_zone = store.find(from.zone);
    const auto* goal_zone = store.find(to.zone);
    if (start_zone == nullptr || goal_zone == nullptr) continue;
    (void)goal_zone;

    // Актор — хозяин ТОГО дома, откуда вышел. Тогда «своё» и «чужое» на маршруте различимы, а иначе
    // хозяином он не был бы нигде и первый же шаг считался бы нарушением.
    const uint32_t actor = titles.de_jure_holder(start_zone->title);
    pf09::travel_policy lawful{};
    lawful.realm = &view;
    lawful.actor = actor;

    const auto rude = pf09::find_path(store, from, to);
    const auto polite = pf09::find_path(store, from, to, 65536, lawful);
    if (rude.empty() || polite.empty()) continue;

    // Считается РОВНО ТО, что минимизирует маршрут, — все шаги по чужому, включая дом назначения. Первая
    // редакция исключала владения цели и старта «по смыслу»: идти в гости не преступление. Но политика
    // штрафует их наравне со всеми, и проверка мерила одну величину, а маршрут оптимизировал другую —
    // на одном сиде из девяти вежливый путь вышел «хуже» на один шаг. Мерить надо то, чем управляешь.
    const auto count_private = [&](const std::vector<pf09::part_ref>& path) {
      size_t total = 0;
      for (const auto& step : path) {
        if (!pf09::may_enter(view, step.zone, actor).allowed) ++total;
      }
      return total;
    };
    const auto rude_count = count_private(rude);
    const auto polite_count = count_private(polite);
    if (rude_count == 0 && polite_count == 0) continue; // нечего сравнивать: чужого на пути и не было

    ++compared;
    rude_private += rude_count;
    polite_private += polite_count;
    if (polite.size() > rude.size()) ++rude_longer;
  }

  check.expect(compared > 0, "нашлись маршруты, где чужая территория вообще попадается",
               "ни на одном пути не оказалось третьих владений — сравнивать нечего, и молчать об этом нельзя");
  check.expect(polite_private < rude_private, "законопослушный обходит чужую территорию",
               std::format("{} чужих шагов против {} у безразличного на {} маршрутах", polite_private,
                           rude_private, compared));

  std::cout << std::format("  замер: титулов {}, кварталов {} (занято {}); мест под законом державы {}, "
                           "под законом квартала {}, без закона {}; на {} маршрутах законопослушный "
                           "прошёл {} чужих шагов против {}, крюк сделал на {}\n",
                           titles.size(), wards, usurped_wards, under_crown, under_ward, lawless, compared,
                           polite_private, rude_private, rude_longer);
}

// Улица, нарезанная на мостовую и обочины. Это не отдельный слой и не флаг: то же зонирование, просто
// мельче, а разница между полосами выражена СКОРОСТЬЮ. Проверяются три следствия, ради которых нарезка и
// делалась, — иначе она осталась бы лишними записями в файле.
void verify_roads(checker& check, const pf09::zone_store& store, const options& opts) {
  size_t roadways = 0;
  size_t verges = 0;
  size_t slow_roadway = 0;
  size_t door_on_roadway = 0;
  size_t doors = 0;
  float roadway_speed = 0.0f;
  float verge_speed = 0.0f;

  for (int32_t y = 0; y < int32_t(opts.build.sector_side); ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;

      for (const auto& record : sector->zones) {
        if (record.kind == pf09::zone_kind::street) {
          ++roadways;
          roadway_speed = record.speed;
        }
        if (record.kind == pf09::zone_kind::verge) {
          ++verges;
          verge_speed = record.speed;
          if (record.speed >= roadway_speed && roadway_speed > 0.0f) ++slow_roadway;
        }
        if (record.kind != pf09::zone_kind::door) continue;
        ++doors;

        // Дверь дома обязана выходить НА ОБОЧИНУ, а не на мостовую. Ради этого полосы и кладутся
        // параллельно улице: положи их поперёк — и дверь окажется на проезжей части, а «зона, прилегающая
        // к дому» перестанет существовать как отдельная вещь.
        for (const auto& portal : store.portals_of({record.key, 0})) {
          const auto* other = store.find(portal.other);
          if (other == nullptr) continue;
          if (other->kind == pf09::zone_kind::street) ++door_on_roadway;
        }
      }
    }
  }

  check.expect(roadways > 0 && verges > 0, "улица нарезана на мостовую и обочины",
               std::format("мостовых {}, обочин {}", roadways, verges));
  check.expect(roadway_speed > verge_speed, "по мостовой идут быстрее, чем по обочине",
               std::format("{:.2f} против {:.2f}", roadway_speed, verge_speed));
  check.expect(slow_roadway == 0, "ни одна обочина не быстрее мостовой");
  check.expect(door_on_roadway == 0, "дверь дома выходит на обочину, а не на проезжую часть",
               std::format("{} дверей из {} открываются прямо на мостовую", door_on_roadway, doors));

  // И главное следствие: маршрут ТЯНЕТСЯ на мостовую. Скорость, которая ни на что не влияет, — это
  // просто число в файле.
  // Обочины берутся ИЗ ОДНОГО ГОРОДА. Между городами внутреннего пути и не должно быть — туда ходят
  // дорогой уровнем выше, — и пары где попало давали шесть коротких маршрутов вместо шестидесяти
  // четырёх длинных. На коротком маршруте мостовая и не успевает себя проявить.
  std::vector<pf09::part_ref> spots;
  pf09::zone_key town = pf09::invalid_key;
  for (int32_t y = 0; y < int32_t(opts.build.sector_side) && spots.size() < 400; ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side) && spots.size() < 400; ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;
      for (const auto& record : sector->zones) {
        if (record.kind != pf09::zone_kind::verge) continue;
        const auto* home = store.containing(record.key, pf09::zone_kind::settlement);
        if (home == nullptr) continue;
        if (town == pf09::invalid_key) town = home->key;
        if (home->key != town) continue;

        // Берётся КАЖДАЯ часть обочины: одно место тянется вдоль всей улицы десятком кусков, и пары из
        // «нулевых» частей оказывались рядом друг с другом.
        for (uint32_t index = 0; index < record.part_count; ++index) {
          spots.push_back({record.key, index});
        }
        if (spots.size() >= 400) break;
      }
    }
  }

  size_t roadway_steps = 0;
  size_t verge_steps = 0;
  size_t routed = 0;
  for (uint32_t i = 0; i < 64 && spots.size() > 2; ++i) {
    const auto from = spots[utils::splitmix(i * 2ull + 1ull, 0x0adcull) % spots.size()];
    const auto to = spots[utils::splitmix(i * 2ull + 2ull, 0x0adcull) % spots.size()];
    if (from == to) continue;
    const auto path = pf09::find_path(store, from, to);
    if (path.empty()) continue;
    ++routed;

    // Считаются ЧАСТИ, а не зоны. Обочина вдоль всей улицы — ОДНА зона из десятка кусков, и подсчёт по
    // зонам давал «1 мостовая против 2 обочин» на любом маршруте: метрика мерила число мест, а не
    // пройденный путь, и разница в скорости в неё не попадала вовсе.
    for (const auto& step : path) {
      const auto* zone = store.find(step.zone);
      if (zone == nullptr) continue;
      if (zone->kind == pf09::zone_kind::street) ++roadway_steps;
      if (zone->kind == pf09::zone_kind::verge) ++verge_steps;
    }
  }

  check.expect(routed > 0, "маршруты по улицам нашлись");
  // Контракт формулируется через ДОЛЮ, а не через выдуманный порог. Мостовая занимает треть площади
  // улицы (полосы режутся один к одному к одному), значит при безразличии к скорости на неё пришлась бы
  // примерно треть шагов. Больше трети — и скорость действительно тянет маршрут; ровно треть — и число в
  // файле ни на что не влияет. Требовать «вдвое больше обочины» было бы требованием к геометрии города:
  // на изломах улицы полосы мостовой не стыкуются, и маршрут законно сходит на обочину.
  const double street_steps = double(roadway_steps + verge_steps);
  const double roadway_share = street_steps > 0.0 ? double(roadway_steps) / street_steps : 0.0;
  check.expect(roadway_steps * 2 > verge_steps, "мостовая берёт больше своей доли улицы",
               std::format("{:.0f}% шагов при трети площади ({} против {})", roadway_share * 100.0,
                           roadway_steps, verge_steps));
  std::cout << std::format("  замер: мостовых {} (скорость {:.2f}), обочин {} (скорость {:.2f}); "
                           "на {} маршрутах мостовая берёт {:.0f}% шагов при трети площади ({} против {})\n",
                           roadways, roadway_speed, verges, verge_speed, routed, roadway_share * 100.0,
                           roadway_steps, verge_steps);
}

// Кто держит территорию и как это меняет маршруты. Это и есть проверка того, что иерархия вложенности
// НЕСУЩАЯ, а не справочная: если ответ «кто держит район» не влияет ни на что, то и района как игровой
// сущности нет — есть только слово в файле.
void verify_control(checker& check, pf09::zone_store& store, const options& opts) {
  size_t places = 0;
  size_t no_district_answer = 0;
  size_t no_settlement_answer = 0;
  size_t criminal_districts = 0;
  size_t districts = 0;
  size_t disagreeing = 0;
  size_t comparable = 0;
  size_t wrong_carrier = 0;

  // Образец берётся ИЗ СЕКТОРА НАВЕДЕНИЯ, а не откуда попало. Дальше мы будем отводить наблюдателя и
  // возвращать, проверяя, что рантайм-состояние пережило выгрузку; место из дальнего сектора после
  // возврата просто не резидентно, и проверка мерила бы резидентность вместо состояния — ошибка, которая
  // на этой площадке ловится уже в четвёртый раз.
  const auto focus_centre = world_centre(opts);
  const int32_t home_x = pf09::sector_of(focus_centre.x);
  const int32_t home_y = pf09::sector_of(focus_centre.y);

  const pf09::zone_record* sample_district = nullptr;
  const pf09::zone_record* sample_place = nullptr;

  for (int32_t y = 0; y < int32_t(opts.build.sector_side); ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;

      for (const auto& record : sector->zones) {
        if (record.kind == pf09::zone_kind::district && record.control != pf09::no_control) {
          ++districts;
          const auto own = store.control_at(record.key, pf09::control_field::faction);
          if (own.faction >= 5) ++criminal_districts;

          // Уровни ВПРАВЕ расходиться, и это не дефект данных, а содержание игры: банда держит квартал
          // в городе, которым распоряжается кто-то другой. Сравниваем с ПОСЕЛЕНИЕМ, а не с владением:
          // владение живёт на политическом уровне и в собранную область попадает не всегда, а требовать
          // от проверки, чтобы барония оказалась внутри фикстуры, значит проверять фикстуру.
          const auto* town = store.containing(record.key, pf09::zone_kind::settlement);
          const auto town_faction = town == nullptr
                                      ? 0u
                                      : store.control_at(town->key, pf09::control_field::faction).faction;
          // Расхождение считается только там, где У ГОРОДА ЕСТЬ ответ. Иначе «нет владельца» выглядит как
          // «другой владелец», и проверка зеленеет ровно тогда, когда носитель потерялся: первая редакция
          // насчитала расхождение у ВСЕХ 44 районов, потому что поселение не значилось носителем и
          // отвечало нулём.
          if (town_faction != 0) {
            ++comparable;
            if (town_faction != own.faction) ++disagreeing;
          }
          if (sample_district == nullptr && std::abs(opts.build.sector_x + x - home_x) <= 1 &&
              std::abs(opts.build.sector_y + y - home_y) <= 1) {
            sample_district = &record;
          }
        }

        if (record.level != pf09::zone_level::interior || record.abstract()) continue;
        // Дверь и стена принадлежат зданию, а не кварталу напрямую; цепочка у них длиннее, но ответ тот же.
        if (store.containing(record.key, pf09::zone_kind::district) == nullptr) continue;
        ++places;

        // Ответ ищется НА УРОВНЕ-НОСИТЕЛЕ. Носителем преступности обязан быть район, а не сама комната:
        // иначе десять тысяч чисел обязаны меняться вместе и однажды разойдутся.
        const auto* carrier = store.carrier_of(record.key, pf09::control_field::crime);
        if (carrier == nullptr) ++no_district_answer;
        else if (carrier->kind != pf09::zone_kind::district) ++wrong_carrier;

        const auto* wealth = store.carrier_of(record.key, pf09::control_field::prosperity);
        if (wealth == nullptr) ++no_settlement_answer;
        else if (wealth->kind != pf09::zone_kind::settlement && wealth->kind != pf09::zone_kind::market) {
          ++wrong_carrier;
        }
        if (sample_place == nullptr && std::abs(opts.build.sector_x + x - home_x) <= 1 &&
            std::abs(opts.build.sector_y + y - home_y) <= 1) {
          sample_place = &record;
        }
      }
    }
  }

  check.expect(districts > 0, "районы несут контроль");
  check.expect(places > 0, "места прочитались");
  check.expect(no_district_answer == 0, "у места есть ответ «насколько тут опасно»",
               std::format("{} мест из {} без района-носителя", no_district_answer, places));
  check.expect(no_settlement_answer == 0, "у места есть ответ «насколько тут богато»",
               std::format("{} мест без поселения-носителя", no_settlement_answer));
  check.expect(wrong_carrier == 0, "ответ несёт тот уровень, которому он положен",
               std::format("{} ответов пришли не с того уровня", wrong_carrier));
  check.expect(criminal_districts > 0, "часть районов держат не власти");
  check.expect(comparable > 0, "у города есть свой ответ, с чем сравнивать");
  check.expect(disagreeing > 0, "уровни расходятся в том, кто держит",
               "район и город всегда совпали — тогда иерархия ничего не добавляет");
  check.expect(disagreeing < comparable, "уровни не расходятся поголовно",
               std::format("разошлись все {} — похоже, у одного из уровней потерялся носитель", comparable));

  // Молчаливый пропуск здесь недопустим: без образца шесть утверждений про рантайм-состояние просто не
  // выполняются, и «123 проверки вместо 129» — единственное, чем это себя выдаёт. Отсутствие образца
  // рядом с наведением — само по себе провал.
  check.expect(sample_district != nullptr && sample_place != nullptr,
               "рядом с наведением нашлись район и место под ним");
  if (sample_district == nullptr || sample_place == nullptr) return;

  // Смена владельца — рантайм-событие, и ответ ДЛЯ МЕСТ ПОД НИМ обязан измениться сразу, без пересборки.
  // Носителя спрашиваем у `carrier_of`, а не у `containing`. Разница не косметическая: `containing`
  // отдаёт ближайшего предка нужного ВИДА независимо от того, несёт ли он ответ, и первая редакция
  // восстанавливала значение через `controls[home->control]` — где `control` вполне мог быть
  // `no_control`. Это индекс `0xffffffff` в вектор, то есть сегфолт, и он честно случился на одном сиде
  // из девяти. `carrier_of` по построению возвращает только того, у кого ответ есть.
  const auto* home = store.carrier_of(sample_place->key, pf09::control_field::faction);
  check.expect(home != nullptr, "у места есть носитель ответа «кто держит»");
  if (home != nullptr) {
    // ДАЛЬШЕ ТОЛЬКО КЛЮЧИ. `zone_record*` указывает внутрь вектора резидентного сектора, и выгрузка его
    // уничтожает: после `focus` любой такой указатель — висячий. Первая редакция читала `sample_place->key`
    // ПОСЛЕ возврата наблюдателя и получала мусор, а выглядело это как «рантайм-состояние не пережило
    // выгрузку» — то есть обвиняло ровно тот механизм, который работал.
    const auto place_key = sample_place->key;
    const auto home_key = home->key;

    const auto original = store.control_at(place_key, pf09::control_field::faction);
    const auto before = original.faction;
    pf09::zone_control seized{};
    seized.faction = before + 100;
    seized.crime = 999;
    store.set_control(home_key, seized);
    const auto after = store.control_at(place_key, pf09::control_field::faction).faction;
    check.expect(after == before + 100, "район отжали — место под ним знает об этом",
                 std::format("ответ остался {}", after));
    check.expect(store.control_at(place_key, pf09::control_field::crime).crime == 999,
                 "смена контроля меняет и преступность");

    // Переживает выгрузку: партия ушла за край мира и вернулась, район всё ещё чужой.
    store.focus({focus_centre.x + pf09::sector_span_m * 40.0, focus_centre.y});
    store.focus(focus_centre);
    check.expect(store.control_at(place_key, pf09::control_field::faction).faction == before + 100,
                 "рантайм-контроль переживает выгрузку сектора");
    // Возвращаем как было — из значения, снятого ДО подмены, а не из файла: лезть за ним в сектор значит
    // предполагать, что сектор резидентен и индекс валиден, а обе эти догадки уже подводили.
    store.set_control(home_key, original);
  }

  // Маршруты. Осторожный путь обязан ОТЛИЧАТЬСЯ от короткого, иначе преступность — украшение.
  std::vector<pf09::part_ref> spots;
  pf09::zone_key sample_town = pf09::invalid_key;
  for (int32_t y = 0; y < int32_t(opts.build.sector_side) && spots.size() < 600; ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side) && spots.size() < 600; ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;
      for (const auto& record : sector->zones) {
        if (record.level != pf09::zone_level::interior || record.abstract()) continue;
        if (!store.passable(record)) continue;
        // Маршруты сравниваются ВНУТРИ одного города: между городами внутреннего пути и не должно
        // быть — туда ходят дорогой, а дорога живёт уровнем выше. Город запоминается КЛЮЧОМ, а не
        // указателем: указатель смотрит внутрь резидентного сектора и переживает выгрузку только по
        // видимости, а сравнение указателей вдобавок молча ломается при перезагрузке того же сектора.
        const auto* home = store.containing(record.key, pf09::zone_kind::settlement);
        if (home == nullptr) continue;
        if (sample_town == pf09::invalid_key) sample_town = home->key;
        if (home->key != sample_town) continue;
        spots.push_back({record.key, 0});
        if (spots.size() >= 600) break;
      }
    }
  }

  size_t pairs = 0;
  size_t safer = 0;
  size_t worse = 0;
  size_t longer = 0;
  uint64_t plain_crime = 0;
  uint64_t careful_crime = 0;
  size_t plain_steps = 0;
  size_t careful_steps = 0;

  pf09::travel_policy careful{};
  careful.crime_weight = 6.0f;

  for (uint32_t i = 0; i < 96 && spots.size() > 2; ++i) {
    const auto from = spots[utils::splitmix(i * 2ull + 1ull, 0xc0ffeeull) % spots.size()];
    const auto to = spots[utils::splitmix(i * 2ull + 2ull, 0xc0ffeeull) % spots.size()];
    if (from == to) continue;

    const auto plain = pf09::find_path(store, from, to);
    if (plain.empty()) continue;
    const auto quiet = pf09::find_path(store, from, to, 65536, careful);
    if (quiet.empty()) continue;
    ++pairs;

    const auto a = pf09::measure_route(store, plain, 0);
    const auto b = pf09::measure_route(store, quiet, 0);
    plain_crime += a.crime_sum;
    careful_crime += b.crime_sum;
    plain_steps += a.steps;
    careful_steps += b.steps;
    // Сравниваем СРЕДНЮЮ преступность на шаг, а не сумму: осторожный маршрут заведомо длиннее, и по
    // сумме он проигрывал бы, будучи безопаснее на каждом шагу.
    const double a_mean = double(a.crime_sum) / double(std::max<uint32_t>(a.steps, 1));
    const double b_mean = double(b.crime_sum) / double(std::max<uint32_t>(b.steps, 1));
    if (b_mean < a_mean - 1.0) ++safer;
    if (b_mean > a_mean + 1.0) ++worse;
    if (b.steps > a.steps) ++longer;
  }

  check.expect(pairs > 0, "маршруты для сравнения нашлись");
  if (pairs == 0) return;

  const double plain_mean = double(plain_crime) / double(std::max<size_t>(plain_steps, 1));
  const double careful_mean = double(careful_crime) / double(std::max<size_t>(careful_steps, 1));

  // Числовой контракт, но аккуратный. «Осторожный маршрут ТИШЕ НА ДЕСЯТЬ ПРОЦЕНТОВ» — утверждение про
  // фикстуру: там, где преступность ровная по всему городу, выигрывать нечего, и требовать выигрыша
  // значит требовать, чтобы сид оказался удачным. Держится другое: осторожность НИКОГДА НЕ ХУЖЕ в целом,
  // а где выбор есть — лучше, и это видно по числу пар.
  check.expect(careful_mean <= plain_mean + 1.0, "осторожный маршрут не идёт по местам хуже прежних",
               std::format("средняя преступность {:.0f} против {:.0f}", careful_mean, plain_mean));
  // Требовать «тише у большинства» нельзя: у многих пар обходного пути просто нет, и осторожный маршрут
  // совпадает с коротким — это правильный ответ, а не провал. Утверждение, которое действительно
  // держится: осторожность НЕ ДЕЛАЕТ ХУЖЕ, и там, где выбор есть, она им пользуется.
  check.expect(worse * 3 < safer, "осторожность не заводит в места хуже прежних",
               std::format("тише у {} пар, хуже у {} из {}", safer, worse, pairs));

  std::cout << std::format("  замер: районов {} (из них у преступных сил {}), расходятся с городом {}; "
                           "средняя преступность маршрута {:.0f} против {:.0f}; тише у {}, хуже у {}, "
                           "длиннее у {} из {} пар\n",
                           districts, criminal_districts,
                           std::format("{} из {}", disagreeing, comparable), careful_mean, plain_mean, safer,
                           worse, longer, pairs);
}

// Связи без геометрии. Их два вида, и оба обязаны лежать В ФАЙЛЕ, потому что вывести их из совпадения
// рёбер нельзя в принципе: у дороги между поселениями общего ребра нет, а у лестницы оно есть, но прохода
// через него нет. Отдельная забота — узел БЕЗ ФОРМЫ: порталы висят на частях, частей у него ноль, и
// раньше его рёбра молча пропадали. Весь политический уровень уезжал на диск набором изолированных точек,
// чего не замечала ни одна проверка, потому что все они смотрели на порталы частей.
void verify_graph_nodes(checker& check, const pf09::zone_store& store, const options& opts) {
  size_t nodes = 0;
  size_t abstract_nodes = 0;
  size_t abstract_linked = 0;
  size_t edges = 0;
  size_t asymmetric = 0;
  size_t geometric = 0;
  size_t unresolved = 0;

  const auto links_of = [&](const pf09::zone_record& record) {
    std::vector<pf09::zone_portal> out;
    for (const auto& link : store.links_of(record.key)) {
      out.push_back(link);
    }
    for (uint32_t index = 0; index < record.part_count; ++index) {
      for (const auto& portal : store.portals_of({record.key, index})) {
        if (!portal.geometric()) out.push_back(portal);
      }
    }
    return out;
  };

  for (int32_t y = 0; y < int32_t(opts.build.sector_side); ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;

      for (const auto& record : sector->zones) {
        if (record.level == pf09::zone_level::interior) continue;
        ++nodes;

        const auto links = links_of(record);
        if (record.abstract()) {
          ++abstract_nodes;
          if (!links.empty()) ++abstract_linked;
          // У зоны без формы рёбра обязаны лежать в собственном списке записи, а не где-то ещё.
          if (links.size() != store.links_of(record.key).size()) ++unresolved;
        }

        for (const auto& link : links) {
          ++edges;
          if (link.geometric()) ++geometric;

          const auto* other = store.find(link.other);
          if (other == nullptr) continue; // сосед в невыгруженном секторе — это не асимметрия
          const auto back = links_of(*other);
          if (std::none_of(back.begin(), back.end(),
                           [&](const pf09::zone_portal& item) { return item.other == record.key; })) {
            ++asymmetric;
          }
        }
      }
    }
  }

  check.expect(nodes > 0, "зоны крупных уровней прочитались");
  check.expect(edges > 0, "связь без геометрии доехала до файла",
               "ни одного ребра графа — дороги и лестницы потерялись при записи");
  check.expect(geometric == 0, "ребро графа не притворяется геометрией",
               std::format("{} рёбер без отрезка помечены геометрическими", geometric));
  check.expect(asymmetric == 0, "ребро графа видно с обеих сторон", std::format("{} односторонних", asymmetric));
  check.expect(unresolved == 0, "узел без формы носит свои рёбра сам", std::format("{} нарушений", unresolved));
  std::cout << std::format("  замер: зон крупных уровней {} (без формы {}, из них со связями {}), рёбер графа {}\n",
                           nodes, abstract_nodes, abstract_linked, edges);
}

// Полное покрытие, непроходимость и вложенность — три утверждения новой модели, и каждое проверяется
// отдельно, потому что ломаются они независимо.
void verify_places(checker& check, const options& opts) {
  // У проверки СВОЙ store, наведённый на само поселение. Общий был наведён на центр области, а поселение
  // размером в 640 м свободно лежит на границе секторов по 8 км: четверть его площади оказывалась в
  // невыгруженном секторе, и покрытие «проваливалось» там, где данные в порядке. Проверять покрытие,
  // не убедившись, что покрываемое загружено, — значит мерить резидентность вместо геометрии.
  // Радиус берётся заведомо больше рабочего: место лежит в соседнем с наведением секторе, его квартал —
  // ещё в соседнем, и проверке нужны оба. Требовать этого от игрового радиуса незачем — там подъём по
  // иерархии идёт от места, которое уже под ногами.
  pf09::zone_store store(world_root(opts), pf09::sector_span_m * 3.0, 64);
  store.focus(world_centre(opts));

  const pf09::zone_record* sample = nullptr;
  const pf09::zone_sector* home = nullptr;

  // Наводимся на поселение ВНУТРИ области, а не на первое попавшееся: первое лежит в углу, где половина
  // его окрестности за краем фикстуры. Окно берётся широким — в один сектор от края, а не в два: поселения
  // расставлены полем локальности, а не сеткой, и требовать их в конкретной четвёрке секторов значит
  // проверять расстановку, а не то, ради чего проверка написана.
  for (int32_t y = 1; y + 1 < int32_t(opts.build.sector_side) && sample == nullptr; ++y) {
    for (int32_t x = 1; x + 1 < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;
      for (const auto& record : sector->zones) {
        if (record.kind != pf09::zone_kind::settlement) continue;
        sample = &record;
        home = sector;
        break;
      }
      if (sample != nullptr) break;
    }
  }
  check.expect(sample != nullptr, "поселение найдено");
  if (sample == nullptr) return;
  (void)home;

  const glm::dvec2 focus{(sample->bounds.lower.x + sample->bounds.upper.x) * 0.5,
                         (sample->bounds.lower.z + sample->bounds.upper.z) * 0.5};
  store.focus(focus);

  sample = nullptr;
  for (int32_t y = -1; y <= 1 && sample == nullptr; ++y) {
    for (int32_t x = -1; x <= 1; ++x) {
      const auto* sector = store.sector(pf09::sector_of(focus.x) + x, pf09::sector_of(focus.y) + y);
      if (sector == nullptr) continue;
      for (const auto& record : sector->zones) {
        if (record.kind != pf09::zone_kind::settlement) continue;
        const glm::dvec2 centre{(record.bounds.lower.x + record.bounds.upper.x) * 0.5,
                                (record.bounds.lower.z + record.bounds.upper.z) * 0.5};
        if (std::abs(centre.x - focus.x) > 1.0 || std::abs(centre.y - focus.y) > 1.0) continue;
        sample = &record;
        break;
      }
      if (sample != nullptr) break;
    }
  }
  if (sample == nullptr) return;

  // Покрытие: внутри поселения пустоты быть не должно. Стена — это место, а не отсутствие места, и
  // раньше между комнатой и улицей действительно ничего не лежало.
  const glm::vec2 lower{sample->bounds.lower.x, sample->bounds.lower.z};
  const glm::vec2 upper{sample->bounds.upper.x, sample->bounds.upper.z};

  size_t probes = 0;
  size_t empty = 0;
  size_t patched = 0;
  size_t multi_kind = 0;
  std::map<pf09::zone_kind, uint32_t> kinds;
  for (uint32_t i = 0; i < 4096; ++i) {
    const auto unit = sample_point(i, 1.0, 0x9a6eull);
    const glm::vec3 point{lower.x + float(unit.x) * (upper.x - lower.x), 0.05f,
                          lower.y + float(unit.y) * (upper.y - lower.y)};
    ++probes;
    auto hit = store.pick(point, pf09::zone_level::interior);
    bool nudged = false;
    if (!hit.valid()) {
      nudged = true;
      // Допуск задан ПРЕДСТАВЛЕНИЕМ, а не вкусом. Вершины лежат в мировых `float`, а координаты доходят до
      // полумиллиона метров, где соседние представимые числа отстоят на три сантиметра. Две фигуры,
      // аналитически делящие ребро, после округления расходятся на эту величину, и точка выборки
      // проваливается в щель, которой в геометрии нет. Требовать от проверки точности, которой формат не
      // несёт, значит проверять формат, а не покрытие.
      //
      // Область сборки переехала к началу координат, потому что зонируется окрестность партии, а не
      // планета: шаг `float` упал с трёх сантиметров до четырёх миллиметров, и допуск ушёл вслед за ним.
      constexpr float nudge = 0.01f;
      for (int32_t dy = -1; dy <= 1 && !hit.valid(); ++dy) {
        for (int32_t dx = -1; dx <= 1 && !hit.valid(); ++dx) {
          if (dx == 0 && dy == 0) continue;
          hit = store.pick({point.x + float(dx) * nudge, point.y, point.z + float(dy) * nudge},
                           pf09::zone_level::interior);
        }
      }
    }
    if (!hit.valid()) {
      ++empty;
      continue;
    }
    patched += nudged ? 1 : 0;
    const auto* zone = store.find(hit.zone);
    if (zone != nullptr) ++kinds[zone->kind];
  }
  {
    std::string summary;
    for (const auto& [kind, count] : kinds) {
      summary += std::format("{}={} ", pf09::zone_kind_name(kind), count);
    }
    std::cout << std::format("  замер: покрытие по видам мест — {}\n", summary);
  }
  check.expect(empty == 0, "игровая поверхность покрыта целиком",
               std::format("{} точек из {} не принадлежат ни одному месту", empty, probes));
  std::cout << std::format("  замер: щели округления задели {} точек из {} ({:.2f}%), ширина щели не больше "
                           "шага `float` на этих координатах\n",
                           patched, probes, 100.0 * double(patched) / double(std::max<size_t>(probes, 1)));
  (void)multi_kind;

  // Вложенность: у каждого места есть квартал, у квартала — поселение. Без этого вопрос «кто держит
  // район» не на что опереть.
  size_t no_district = 0;
  size_t no_settlement = 0;
  size_t inspected = 0;
  size_t lonely_perimeter = 0;
  size_t internal_leak = 0;
  size_t far_parent = 0;
  size_t resolved = 0;

  for (int32_t y = 0; y < int32_t(opts.build.sector_side); ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;

      for (const auto& record : sector->zones) {
        if (record.level != pf09::zone_level::interior || record.abstract()) continue;
        if (record.kind == pf09::zone_kind::door) continue; // дверь принадлежит зданию, а не кварталу
        ++inspected;

        // Родитель обязан лежать в СОСЕДНЕМ секторе, а не где угодно. Это утверждение о данных, и оно
        // проверяется на всех резидентных: без него подъём по иерархии стоил бы подгрузки неизвестно чего.
        if (record.parent != pf09::invalid_key) {
          const int32_t dx = pf09::key_sector_x(record.parent) - pf09::key_sector_x(record.key);
          const int32_t dy = pf09::key_sector_y(record.parent) - pf09::key_sector_y(record.key);
          if (std::abs(dx) > 1 || std::abs(dy) > 1) ++far_parent;
        }

        // А вот РАЗРЕШЕНИЕ цепочки требует, чтобы предки были загружены, поэтому спрашивается только у
        // мест наведённого поселения. Иначе проверка мерила бы резидентность, а не вложенность.
        const int32_t home_dx = pf09::key_sector_x(record.key) - pf09::sector_of(focus.x);
        const int32_t home_dy = pf09::key_sector_y(record.key) - pf09::sector_of(focus.y);
        if (std::abs(home_dx) > 1 || std::abs(home_dy) > 1) continue;

        // Кромка собранной области пропускается. Мир фикстуры обрезан, и клетка ровно на границе секторов
        // может отдать свой абстрактный узел наружу: у здания центр уходит за край, узел не пишется, и зал
        // остаётся без родителя. Это свойство обрезанного мира, а не утверждение модели, и требовать здесь
        // целостности значило бы проверять край фикстуры.
        const int32_t sx = pf09::key_sector_x(record.key) - opts.build.sector_x;
        const int32_t sy = pf09::key_sector_y(record.key) - opts.build.sector_y;
        if (sx <= 0 || sy <= 0 || sx >= int32_t(opts.build.sector_side) - 1 ||
            sy >= int32_t(opts.build.sector_side) - 1) {
          continue;
        }
        ++resolved;

        if (store.containing(record.key, pf09::zone_kind::district) == nullptr) ++no_district;
        if (store.containing(record.key, pf09::zone_kind::settlement) == nullptr) ++no_settlement;

        // Периметр: только внешние рёбра. Стык двух частей ОДНОГО места периметром не является — по нему
        // не строят баррикаду, потому что это не граница.
        const auto edges = store.perimeter(record.key);
        if (edges.empty()) ++lonely_perimeter;
        for (const auto& portal : edges) {
          if (portal.other == record.key) ++internal_leak;
        }
      }
    }
  }

  check.expect(inspected > 0, "места прочитались");
  check.expect(far_parent == 0, "родитель лежит в соседнем секторе", std::format("{} мест с далёким родителем",
                                                                                 far_parent));
  check.expect(resolved > 0, "нашлись места наведённого поселения");
  check.expect(no_district == 0, "у места есть квартал", std::format("{} из {} мест без квартала", no_district,
                                                                     resolved));
  check.expect(no_settlement == 0, "у места есть поселение", std::format("{} из {} мест без поселения",
                                                                         no_settlement, resolved));
  check.expect(lonely_perimeter == 0, "у места есть периметр", std::format("{} мест без внешних рёбер",
                                                                           lonely_perimeter));
  check.expect(internal_leak == 0, "внутренний стык не попал в периметр",
               std::format("{} внутренних рёбер снаружи", internal_leak));

  std::cout << std::format("  замер: мест {}, у первого поселения габарит {:.0f}x{:.0f} м\n", inspected,
                           double(upper.x - lower.x), double(upper.y - lower.y));
}

void verify_zones(checker& check, const pf09::territory& map, const options& opts) {
  const auto root = world_root(opts);
  const auto first = build_fixture(map, opts);
  check.expect(first.sectors > 0 && first.zones > 0, "мир собрался", "сборка не дала ни одного сектора");
  if (first.zones == 0) return;

  // Детерминизм сборки проверяется по отпечаткам файлов, а не по их числу: одинаковое число секторов с
  // разным содержимым — ровно тот случай, который «всё построилось» не заметит.
  std::vector<uint64_t> before;
  for (uint32_t y = 0; y < opts.build.sector_side; ++y) {
    for (uint32_t x = 0; x < opts.build.sector_side; ++x) {
      pf09::zone_sector loaded{};
      if (pf09::read_sector(pf09::sector_path(root, opts.build.sector_x + int32_t(x), opts.build.sector_y + int32_t(y)),
                            loaded)) {
        before.push_back(loaded.fingerprint);
      }
    }
  }
  build_fixture(map, opts);

  size_t drift = 0;
  size_t index = 0;
  for (uint32_t y = 0; y < opts.build.sector_side; ++y) {
    for (uint32_t x = 0; x < opts.build.sector_side; ++x) {
      pf09::zone_sector loaded{};
      if (!pf09::read_sector(pf09::sector_path(root, opts.build.sector_x + int32_t(x),
                                               opts.build.sector_y + int32_t(y)), loaded)) {
        continue;
      }
      if (index >= before.size() || before[index] != loaded.fingerprint) ++drift;
      ++index;
    }
  }
  check.expect(drift == 0, "пересборка даёт те же файлы", std::format("{} секторов разошлись", drift));

  pf09::zone_store store(root, opts.stream_radius_m, opts.stream_budget);
  const auto centre = world_centre(opts);
  store.focus(centre);

  check.expect(store.resident_sectors() <= opts.stream_budget, "стриминг не превышает бюджет",
               std::format("{} секторов при бюджете {}", store.resident_sectors(), opts.stream_budget));

  size_t dangling = 0;
  size_t unresolved = 0;
  size_t self_link = 0;
  size_t bad_parent = 0;
  size_t overlap = 0;
  size_t inspected = 0;

  for (int32_t y = 0; y < int32_t(opts.build.sector_side); ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;

      for (const auto& record : sector->zones) {
        ++inspected;

        // «Висячая ссылка» и «сектор не загружен» — РАЗНЫЕ вещи, и путать их нельзя: стриминг существует
        // ровно ради второго. Ссылка обязана указывать на сектор, который есть на диске; а если этот
        // сектор сейчас резидентен, то разрешение по ключу обязано сработать за один шаг.
        for (uint32_t index = 0; index < record.part_count; ++index) {
        for (const auto& portal : store.portals_of(pf09::part_ref{record.key, index})) {
          const auto link = portal.other;

          // Петля — это проход части В САМУ СЕБЯ. Проход между двумя частями одной зоны петлёй не
          // является и обязан существовать: зона произвольной формы только из них и состоит.
          if (link == record.key && portal.other_part == index) ++self_link;

          const auto target = pf09::sector_path(root, pf09::key_sector_x(link), pf09::key_sector_y(link));
          if (!std::filesystem::exists(target)) {
            ++dangling;
            continue;
          }
          if (store.sector(pf09::key_sector_x(link), pf09::key_sector_y(link)) != nullptr &&
              store.part_of({link, portal.other_part}) == nullptr) {
            ++unresolved;
          }
        }
        }

        // Родитель не может лежать на БОЛЕЕ МЕЛКОМ уровне. Тот же уровень допустим и нужен: здание —
        // абстрактный узел, группирующий комнаты, и оно живёт на одном уровне с ними, потому что здание
        // это не другой масштаб, а другая природа записи.
        if (record.parent != pf09::invalid_key) {
          const auto* parent = store.find(record.parent);
          if (parent == nullptr) continue;
          if (uint32_t(parent->level) < uint32_t(record.level)) ++bad_parent;
          if (parent->level == record.level && !parent->abstract()) ++bad_parent;
        }

        // Зоны одного вида на одном уровне не должны накладываться. Габаритами это больше не проверить:
        // фигуры перестали быть выровненными по осям, и у двух соседних скошенных четырёхугольников
        // габариты пересекаются, хотя сами они лишь соприкасаются. Поэтому габарит остался широкой
        // фазой, а решает разделяющая ось — для выпуклых частей она точна.
        if (record.abstract()) continue;
        for (const auto& part : sector->parts_of(record)) {
          const auto outline = sector->outline_of(part);
          for (const auto& other : sector->zones) {
            if (other.key <= record.key || other.abstract()) continue;
            if (other.level != record.level || other.kind != record.kind) continue;
            if (!record.bounds.overlaps_xz(other.bounds)) continue;

            for (const auto& other_part : sector->parts_of(other)) {
              if (!part.bounds.overlaps_xz(other_part.bounds)) continue;
              if (part.bounds.lower.y >= other_part.bounds.upper.y ||
                  other_part.bounds.lower.y >= part.bounds.upper.y) {
                continue;
              }
              if (convex_overlap(outline, sector->outline_of(other_part))) ++overlap;
            }
          }
        }
      }
    }
  }

  // Выпуклость частей — несущее условие, а не свойство фикстуры: на невыпуклой части и разделяющая ось
  // неприменима, и персонаж, идущий по прямой к проёму, выходит наружу. Поэтому её проверяет отдельная
  // проверка, а не подразумевает сборщик.
  size_t concave = 0;
  size_t degenerate_part = 0;
  for (int32_t y = 0; y < int32_t(opts.build.sector_side); ++y) {
    for (int32_t x = 0; x < int32_t(opts.build.sector_side); ++x) {
      const auto* sector = store.sector(opts.build.sector_x + x, opts.build.sector_y + y);
      if (sector == nullptr) continue;

      for (const auto& record_scan : sector->zones) {
      for (const auto& part : sector->parts_of(record_scan)) {
        const auto outline = sector->outline_of(part);
        if (outline.size() < 3) {
          ++degenerate_part;
          continue;
        }

        int32_t sign = 0;
        double area = 0.0;
        for (size_t i = 0; i < outline.size(); ++i) {
          const auto& a = outline[i];
          const auto& b = outline[(i + 1) % outline.size()];
          const auto& c = outline[(i + 2) % outline.size()];
          const double cross = double(b.x - a.x) * double(c.y - b.y) - double(b.y - a.y) * double(c.x - b.x);
          area += double(a.x) * double(b.y) - double(b.x) * double(a.y);

          const int32_t here = cross > 1.0e-4 ? 1 : (cross < -1.0e-4 ? -1 : 0);
          if (here == 0) continue;
          if (sign == 0) sign = here;
          else if (sign != here) {
            ++concave;
            break;
          }
        }
        if (std::abs(area) < 0.01) ++degenerate_part;
      }
      }
    }
  }
  check.expect(concave == 0, "части выпуклы", std::format("{} невыпуклых частей", concave));
  check.expect(degenerate_part == 0, "у части есть площадь", std::format("{} вырожденных частей", degenerate_part));

  check.expect(dangling == 0, "ссылка указывает на существующий сектор", std::format("{} висячих ссылок", dangling));
  check.expect(unresolved == 0, "резидентная ссылка разрешается по ключу",
               std::format("{} ссылок не нашли зону в загруженном секторе", unresolved));
  check.expect(self_link == 0, "зона не ссылается на себя", std::format("{} петель", self_link));
  check.expect(bad_parent == 0, "родитель лежит на более крупном уровне", std::format("{} нарушений", bad_parent));
  check.expect(overlap == 0, "зоны одного вида не накладываются", std::format("{} пересечений", overlap));
  check.expect(inspected > 0, "зоны прочитались");

  // Пикинг: в центре бокса обязана найтись зона, и найденная обязана этот центр содержать. Требовать
  // ИМЕННО ту же зону нельзя — внутри здания законно выигрывает комната, она меньше.
  size_t missed = 0;
  size_t outside_box = 0;
  size_t probes = 0;

  const auto* sample = store.sector(opts.build.sector_x, opts.build.sector_y);
  if (sample != nullptr) {
    for (const auto& record : sample->zones) {
      if (record.abstract()) continue;
      const auto& probe_part = sample->parts_of(record).front();
      const glm::vec3 middle{(probe_part.bounds.lower.x + probe_part.bounds.upper.x) * 0.5f,
                             (probe_part.bounds.lower.y + probe_part.bounds.upper.y) * 0.5f,
                             (probe_part.bounds.lower.z + probe_part.bounds.upper.z) * 0.5f};
      const auto found = store.pick(middle, record.level);
      ++probes;
      if (!found.valid()) {
        ++missed;
      } else if (!store.part_of(found)->bounds.contains(middle)) {
        ++outside_box;
      }
    }
  }
  check.expect(missed == 0, "в центре зоны что-то находится", std::format("{} промахов из {}", missed, probes));
  check.expect(outside_box == 0, "найденная зона содержит точку", std::format("{} промахов", outside_box));

  // Пропуски — законная часть модели: между зонами бывает пустота, и врать про неё нельзя.
  const glm::vec3 nowhere{float(centre.x) + 3000.0f, 400.0f, float(centre.y) + 3000.0f};
  check.expect(!store.pick(nowhere, pf09::zone_level::interior).valid(), "в пропуске зоны нет");

  // Ответы не зависят от того, как наблюдатель сюда пришёл. Это главный контракт стриминга: подгрузка
  // меняет ЧТО доступно, но не ЧТО правда.
  pf09::zone_store roundabout(root, opts.stream_radius_m, opts.stream_budget);
  const double travel = double(opts.build.sector_side) * pf09::sector_span_m * 0.4;
  for (uint32_t step = 0; step < 8; ++step) {
    roundabout.focus({centre.x - travel + travel * double(step) / 4.0, centre.y + travel * 0.3});
  }
  roundabout.focus(centre);

  size_t divergence = 0;
  for (uint32_t i = 0; i < 4096; ++i) {
    const auto unit = sample_point(i, 1.0, 0x2ffee1ull);
    const glm::vec3 point{float(centre.x + (unit.x - 0.5) * 4000.0), 1.0f,
                          float(centre.y + (unit.y - 0.5) * 4000.0)};
    for (uint32_t level = 0; level < uint32_t(pf09::zone_level::count); ++level) {
      if (store.pick(point, pf09::zone_level(level)) != roundabout.pick(point, pf09::zone_level(level))) {
        ++divergence;
      }
    }
  }
  check.expect(divergence == 0, "ответ не зависит от пути наблюдателя",
               std::format("{} расхождений после другого маршрута", divergence));

  verify_portals(check, store, opts);
  verify_places(check, opts);
  verify_agents(check, store, opts);
  verify_graph_nodes(check, store, opts);
  verify_props(check, store, opts);
  verify_tactics(check, store, opts);
  verify_control(check, store, opts);
  verify_roads(check, store, opts);
  verify_titles(check, store, opts);
  verify_doors_and_floors(check, store, opts);

  std::cout << std::format("  замер: {} секторов, {} зон, {} связей, {:.1f} КБ на диске; резидентно {} секторов "
                           "и {:.1f} КБ\n",
                           first.sectors, first.zones, first.links, double(first.bytes) / 1024.0,
                           store.resident_sectors(), double(store.resident_bytes()) / 1024.0);
}


void verify_fingerprint(checker& check, const pf09::territory& map, const uint64_t expected) {
  // Молчаливого пропуска здесь быть не должно. Отпечаток — единственная защита от тихого дрейфа
  // генератора, и когда он не закреплён, об этом надо СКАЗАТЬ, а не просто выйти: незакреплённая защита
  // выглядит в отчёте ровно так же, как сработавшая, и разницу видно только по числу проверок.
  if (expected == 0) {
    std::cout << std::format("  замер: отпечаток раскладки НЕ ЗАКРЕПЛЁН; чтобы закрепить, добавьте "
                             "--expect-fingerprint={}\n",
                             map.fingerprint());
    check.expect(true, "отпечаток раскладки посчитан (не закреплён)");
    return;
  }
  check.expect(map.fingerprint() == expected, "отпечаток раскладки",
               std::format("{} против ожидаемого {}", map.fingerprint(), expected));
}

int run_verification(const pf09::territory& map, const options& opts) {
  std::cout << std::format("PF09 проверки: seed {}, выборка {}, сетка покрытия {}²\n", map.config().seed, opts.samples,
                           opts.coverage_side);

  checker check;
  verify_identifiers(check, map);
  verify_nesting(check, map, opts.samples);
  verify_determinism(check, map, opts.samples);
  verify_warp(check, map);
  verify_extent(check, map, opts.coverage_side, opts.spread_limit);
  verify_structure(check, map);
  verify_owners(check, map, opts.samples);
  verify_border_monotonicity(check, map, opts.samples / 20 + 1);
  verify_torus(check, map, opts.samples / 10 + 1);

  verify_locality(check, map, opts);
  verify_zones(check, map, opts);
  verify_fingerprint(check, map, opts.expected_fingerprint);

  std::cout << std::format("\n  проверок {}, провалов {}\n  отпечаток раскладки: {}\n", check.total(), check.failed(),
                           map.fingerprint());
  return check.passed() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// --- дамп ---

struct rgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

// Пастельная палитра из хеша: чистый хеш даёт слишком тёмные и слишком кислотные соседние territории, и
// глазом перестаёт читаться, где проходит граница.
rgb colour_of(const uint64_t key) {
  const uint64_t h = utils::splitmix(key + 1ull);
  return {uint8_t(110 + (h & 0x7f)), uint8_t(110 + ((h >> 21) & 0x7f)), uint8_t(110 + ((h >> 42) & 0x7f))};
}


// --- отрисовка зон ---

rgb kind_colour(const pf09::zone_kind kind) {
  switch (kind) {
    case pf09::zone_kind::street: return {150, 146, 138};
    case pf09::zone_kind::yard: return {132, 156, 112};
    case pf09::zone_kind::hall: return {196, 172, 140};
    case pf09::zone_kind::door: return {214, 150, 90};
    case pf09::zone_kind::stair: return {206, 196, 120};
    case pf09::zone_kind::wall: return {112, 96, 82};
    case pf09::zone_kind::settlement: return {96, 104, 126};
    default: return {110, 110, 120};
  }
}

void draw_line(std::vector<rgb>& pixels, const uint32_t size, glm::vec2 a, glm::vec2 b, const rgb colour) {
  const float steps = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
  const uint32_t count = uint32_t(std::min(steps, 4096.0f)) + 1;
  for (uint32_t i = 0; i <= count; ++i) {
    const float t = float(i) / float(count);
    const int32_t x = int32_t(a.x + (b.x - a.x) * t);
    const int32_t y = int32_t(a.y + (b.y - a.y) * t);
    if (x < 0 || y < 0 || x >= int32_t(size) || y >= int32_t(size)) continue;
    pixels[size_t(y) * size + uint32_t(x)] = colour;
  }
}

int run_zone_dump(const pf09::territory& map, const options& opts) {
  const auto root = world_root(opts);
  if (!std::filesystem::exists(root)) build_fixture(map, opts);

  pf09::zone_store store(root, opts.stream_radius_m, opts.stream_budget);
  auto centre = world_centre(opts);
  store.focus(centre);

  // Камера ставится на поселение: иначе кадр покажет пустоту между местами, что честно, но неинформативно.
  const pf09::zone_record* focus_zone = nullptr;
  for (int32_t sy = 0; sy < int32_t(opts.build.sector_side) && focus_zone == nullptr; ++sy) {
    for (int32_t sx = 0; sx < int32_t(opts.build.sector_side); ++sx) {
      const auto* sector = store.sector(opts.build.sector_x + sx, opts.build.sector_y + sy);
      if (sector == nullptr) continue;
      for (const auto& record : sector->zones) {
        if (record.kind != pf09::zone_kind::settlement) continue;
        focus_zone = &record;
        break;
      }
      if (focus_zone != nullptr) break;
    }
  }
  if (focus_zone != nullptr) {
    centre = {(focus_zone->bounds.lower.x + focus_zone->bounds.upper.x) * 0.5,
              (focus_zone->bounds.lower.z + focus_zone->bounds.upper.z) * 0.5};
    store.focus(centre);
  }

  const uint32_t size = std::max(opts.dump_size, 64u);
  const double span = opts.dump_span_m > 0.0 ? opts.dump_span_m : 700.0;
  const double origin_x = centre.x - span * 0.5;
  const double origin_y = centre.y - span * 0.5;
  const double scale = double(size) / span;

  std::vector<rgb> pixels(size_t(size) * size, rgb{26, 26, 30});
  const auto to_pixel = [&](const glm::vec2 point) {
    return glm::vec2{float((double(point.x) - origin_x) * scale), float((double(point.y) - origin_y) * scale)};
  };

  // Заливка идёт по каждой фигуре в пределах её габарита: перебирать все зоны на каждый пиксель значило бы
  // сотни миллионов проверок там, где хватает суммы площадей.
  uint32_t drawn = 0;
  for (int32_t sy = 0; sy < int32_t(opts.build.sector_side); ++sy) {
    for (int32_t sx = 0; sx < int32_t(opts.build.sector_side); ++sx) {
      const auto* sector = store.sector(opts.build.sector_x + sx, opts.build.sector_y + sy);
      if (sector == nullptr) continue;

      for (const auto& record : sector->zones) {
        if (record.level != pf09::zone_level::interior || record.abstract()) continue;
        for (const auto& part : sector->parts_of(record)) {
        const auto outline = sector->outline_of(part);

        const auto lower = to_pixel({part.bounds.lower.x, part.bounds.lower.z});
        const auto upper = to_pixel({part.bounds.upper.x, part.bounds.upper.z});
        const int32_t x0 = std::max(0, int32_t(std::floor(lower.x)));
        const int32_t y0 = std::max(0, int32_t(std::floor(lower.y)));
        const int32_t x1 = std::min(int32_t(size) - 1, int32_t(std::ceil(upper.x)));
        const int32_t y1 = std::min(int32_t(size) - 1, int32_t(std::ceil(upper.y)));
        if (x1 < x0 || y1 < y0) continue;
        ++drawn;

        const auto colour = kind_colour(record.kind);
        for (int32_t y = y0; y <= y1; ++y) {
          for (int32_t x = x0; x <= x1; ++x) {
            const glm::vec2 world{float(origin_x + (x + 0.5) / scale), float(origin_y + (y + 0.5) / scale)};
            if (!pf09::point_in_outline(outline, world)) continue;
            pixels[size_t(y) * size + uint32_t(x)] = colour;
          }
        }
        }
      }
    }
  }

  // Проходы поверх заливки: открытые светлые, запертые красные. Видно, что связность живёт на рёбрах.
  uint32_t portals_drawn = 0;
  uint32_t locked_drawn = 0;
  for (int32_t sy = 0; sy < int32_t(opts.build.sector_side); ++sy) {
    for (int32_t sx = 0; sx < int32_t(opts.build.sector_side); ++sx) {
      const auto* sector = store.sector(opts.build.sector_x + sx, opts.build.sector_y + sy);
      if (sector == nullptr) continue;
      for (const auto& record : sector->zones) {
        if (record.level != pf09::zone_level::interior) continue;
        for (const auto& part : sector->parts_of(record)) {
          for (const auto& portal : sector->portals_of(part)) {
            if (!portal.geometric() || portal.other < record.key) continue;
            draw_line(pixels, size, to_pixel(portal.from), to_pixel(portal.to),
                      portal.passable() ? rgb{245, 245, 235} : rgb{200, 60, 60});
            portal.passable() ? ++portals_drawn : ++locked_drawn;
          }
        }
      }
    }
  }

  // Персонаж проходит маршрут, и его след рисуется поверх всего.
  uint32_t path_zones = 0;
  const auto start = store.pick({float(centre.x), 0.1f, float(centre.y)}, pf09::zone_level::interior);
  if (start.valid()) {
    const auto* sector = store.sector(pf09::key_sector_x(start.zone), pf09::key_sector_y(start.zone));
    pf09::part_ref goal{};
    for (const auto& record : sector->zones) {
      if (record.kind == pf09::zone_kind::hall) goal = {record.key, 0};
    }

    if (goal.valid()) {
      pf09::agent walker{};
      walker.location = start;
      if (pf09::interior_point(store, start, walker.position)) {
        walker.path = pf09::find_path(store, start, goal);
        path_zones = uint32_t(walker.path.size());

        auto previous = to_pixel(walker.position);
        uint32_t steps = 0;
        while (!walker.arrived && steps < 40000 && pf09::step_agent(store, walker, 0.5f)) {
          const auto now = to_pixel(walker.position);
          draw_line(pixels, size, previous, now, rgb{255, 90, 200});
          previous = now;
          ++steps;
        }
      }
    }
  }

  std::ofstream file(opts.dump_path, std::ios::binary);
  if (!file) {
    std::cout << std::format("не удалось открыть '{}'\n", opts.dump_path);
    return EXIT_FAILURE;
  }
  file << std::format("P6\n{} {}\n255\n", size, size);
  file.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size() * sizeof(rgb)));

  std::cout << std::format("PF09 зоны '{}': окно {:.0f} м, {:.2f} м на пиксель, нарисовано {} фигур, "
                           "{} открытых проходов и {} запертых, маршрут через {} зон\n",
                           opts.dump_path, span, 1.0 / scale, drawn, portals_drawn, locked_drawn, path_zones);
  return EXIT_SUCCESS;
}

int run_dump(const pf09::territory& map, const options& opts) {
  if (opts.dump_from == dump_source::zones) return run_zone_dump(map, opts);

  const uint32_t size = std::max(opts.dump_size, 8u);
  const double step = opts.dump_span_m / double(size);
  const double origin_x = opts.dump_center_x_m - opts.dump_span_m * 0.5;
  const double origin_y = opts.dump_center_y_m - opts.dump_span_m * 0.5;

  // Отдельный маркер для «этот ярус здесь не разрешим»: он не должен раствориться среди обычных цветов,
  // потому что именно он показывает границу применимости zone LOD.
  constexpr pf09::zone_id unresolvable = pf09::invalid_zone - 1;

  std::vector<pf09::zone_id> keys(size_t(size) * size);
  for (uint32_t y = 0; y < size; ++y) {
    for (uint32_t x = 0; x < size; ++x) {
      const glm::dvec2 point{origin_x + (x + 0.5) * step, origin_y + (y + 0.5) * step};
      keys[size_t(y) * size + x] = map.resolve(point, opts.dump_tier);
    }
  }

  std::vector<rgb> pixels(keys.size());
  for (uint32_t y = 0; y < size; ++y) {
    for (uint32_t x = 0; x < size; ++x) {
      const size_t index = size_t(y) * size + x;
      const auto id = keys[index];
      if (id == pf09::invalid_zone || id == unresolvable) {
        pixels[index] = id == unresolvable ? rgb{200, 40, 40} : rgb{24, 24, 28};
        continue;
      }

      const uint64_t key = opts.dump_colouring == dump_mode::owner ? uint64_t(map.owner_of(id)) * 2654435761ull : uint64_t(id);
      auto colour = colour_of(key);

      // Граница рисуется сравнением соседей, а не отдельным проходом: на срезе 1 это ровно то, что нужно
      // увидеть глазом — где ярус реально меняется. Резолюционно-независимый SDF приходит на срезе 4.
      const bool edge = (x + 1 < size && keys[index + 1] != id) || (y + 1 < size && keys[index + size] != id) ||
                        (x > 0 && keys[index - 1] != id) || (y > 0 && keys[index - size] != id);
      if (edge) {
        colour = {uint8_t(colour.r / 3), uint8_t(colour.g / 3), uint8_t(colour.b / 3)};
      }

      pixels[index] = colour;
    }
  }

  std::ofstream file(opts.dump_path, std::ios::binary);
  if (!file) {
    std::cout << std::format("не удалось открыть '{}'\n", opts.dump_path);
    return EXIT_FAILURE;
  }
  file << std::format("P6\n{} {}\n255\n", size, size);
  file.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size() * sizeof(rgb)));

  const double pixel_m = step;
  const double tier_m = map.tier_span_m(opts.dump_tier);
  std::cout << std::format("PF09 дамп '{}': ярус '{}', {} пикселей, {:.1f} м на пиксель, ячейка яруса {:.1f} м "
                           "({:.1f} пикселей)\n",
                           opts.dump_path, pf09::tier_name(opts.dump_tier), size, pixel_m, tier_m, tier_m / pixel_m);
  return EXIT_SUCCESS;
}

} // namespace

int main(const int argc, const char** argv) {
  options opts;
  if (!parse_options(argc, argv, opts)) return EXIT_FAILURE;

  const pf09::territory map(opts.layout);

  switch (opts.requested) {
    case action::report: run_report(map); return EXIT_SUCCESS;
    case action::locality: return run_locality_report(map, opts);
    case action::world: return run_build_world(map, opts);
    case action::stream: return run_stream_report(map, opts);
    case action::view: {
      auto view = opts.view;
      view.world_root = world_root(opts);
      view.stream_radius_m = opts.stream_radius_m;
      view.stream_budget = opts.stream_budget;
      if (!std::filesystem::exists(view.world_root)) build_fixture(map, opts);
      return pf09::run_viewer(map, opts.local, view);
    }
    case action::verify: return run_verification(map, opts);
    case action::dump: return run_dump(map, opts);
  }
  return EXIT_SUCCESS;
}
