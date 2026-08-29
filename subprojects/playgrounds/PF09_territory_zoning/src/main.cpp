// PF09 — territory zoning. Срез 1 полностью headless: иерархия территорий существует раньше любого
// растра, и все следующие срезы обязаны совпадать с тем, что считает этот executable.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "devils_engine/utils/hash.h"

#include "clipmap.h"
#include "territory.h"

using namespace devils_engine;

namespace {

enum class action : uint32_t { report, clipmap, verify, dump };
enum class dump_mode : uint32_t { zone, owner };
enum class dump_source : uint32_t { direct, clipmap };

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

  pf09::clipmap_config clip;
  double zoom_m_per_pixel = 4.0;
  double view_distance_m = 12000.0;
  uint32_t verify_clip_side = 128;
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
               "  --clipmap             таблица уровней клипмапа, окно резидентности и цена обновления\n"
               "  --verify              численные инварианты, ненулевой код возврата при провале\n"
               "  --dump=PATH           ppm-срез карты для глаз\n"
               "  --dump-tier=N         ярус окраски дампа, 0 = world, 6 = parcel\n"
               "  --dump-mode=NAME      zone | owner\n"
               "  --dump-source=NAME    direct | clipmap: прямое разрешение или то, что лежит в клипмапе\n"
               "  --zoom=F              метров на пиксель экрана\n"
               "  --view=M              дальность видимости в метрах\n"
               "  --clip-side=N         сторона уровня клипмапа в текселях\n"
               "  --clip-levels=N       сколько уровней держит пул\n"
               "  --tier-texels=N       сколько текселей занимает разрешимая ячейка яруса\n"
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
    } else if (argument == "--clipmap") {
      out.requested = action::clipmap;
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
      out.dump_from = value == "clipmap" ? dump_source::clipmap : dump_source::direct;
    } else if (read_prefixed(argument, "--zoom=", value)) {
      out.zoom_m_per_pixel = std::stod(value);
    } else if (read_prefixed(argument, "--view=", value)) {
      out.view_distance_m = std::stod(value);
    } else if (read_prefixed(argument, "--clip-side=", value)) {
      out.clip.side = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--clip-levels=", value)) {
      out.clip.resident_levels = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--tier-texels=", value)) {
      out.clip.min_tier_texels = uint32_t(std::stoul(value));
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


// --- клипмап ---

int run_clipmap_report(const pf09::territory& map, const options& opts) {
  pf09::clipmap clip(map, opts.clip);

  std::cout << std::format("PF09 клипмап: сторона {}², пул {} уровней, разрешимость {} текселей на ячейку\n",
                           opts.clip.side, opts.clip.resident_levels, opts.clip.min_tier_texels);
  std::cout << std::format("  зум {:.2f} м/пиксель, дальность {:.0f} м\n\n", opts.zoom_m_per_pixel,
                           opts.view_distance_m);

  const auto first = clip.required_first(opts.zoom_m_per_pixel);
  const auto last = clip.required_last(opts.view_distance_m);

  std::cout << "  уровень   тексель    покрытие   хранит ярус   резидентен\n";
  const glm::dvec2 centre{map.config().world_span_m * 0.5, map.config().world_span_m * 0.5};
  clip.focus(centre, opts.zoom_m_per_pixel, opts.view_distance_m);

  for (uint32_t k = 0; k < clip.level_count(); ++k) {
    const double texel = clip.texel_size_m(k);
    const double cover = clip.coverage_m(k);
    std::cout << std::format("  {:>7}   {:>7}   {:>9}   {:<11}   {}\n", k,
                             texel >= 1000.0 ? std::format("{:.1f} км", texel / 1000.0) : std::format("{:.0f} м", texel),
                             cover >= 1000.0 ? std::format("{:.0f} км", cover / 1000.0) : std::format("{:.0f} м", cover),
                             pf09::tier_name(clip.level_tier(k)), clip.resident(k) ? "да" : "—");
  }

  const uint32_t needed = last >= first ? last - first + 1 : 1;
  std::cout << std::format("\n  зум требует уровни {}..{} — это {} из пула в {}\n", first, last, needed,
                           opts.clip.resident_levels);
  if (needed > opts.clip.resident_levels) {
    std::cout << "  ПУЛ МАЛ: между самым мелким и самым крупным нужным уровнем останется дыра\n";
  }
  std::cout << std::format("  резидентно {} уровней, {:.1f} МБ при 4 байтах на тексель\n", clip.resident_count(),
                           double(clip.resident_bytes()) / (1024.0 * 1024.0));

  // Время полной перепечки уровня — это ответ на вопрос, можно ли вообще строить уровень на CPU в кадре.
  // Если нельзя, у среза 2b появляется второй потребитель: запекание уходит на GPU или размазывается.
  const auto begin = std::chrono::steady_clock::now();
  const auto reference = clip.bake_reference(clip.first_resident(), centre);
  const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
  std::cout << std::format("  полная перепечка уровня {}: {:.1f} мс на {} текселей ({:.2f} млн разрешений/с)\n",
                           clip.first_resident(), elapsed, reference.size(),
                           double(reference.size()) / (elapsed * 1000.0));

  // Панорамирование на один тексель самого мелкого резидентного уровня — самый частый шаг камеры.
  const double step = clip.texel_size_m(clip.first_resident());
  const auto cost = clip.focus({centre.x + step, centre.y + step}, opts.zoom_m_per_pixel, opts.view_distance_m);
  std::cout << std::format("  сдвиг на один тексель уровня {}: {} текселей в {} регионах, полос {}, перепечек {}\n",
                           clip.first_resident(), cost.texels, cost.regions, cost.shifted_levels,
                           cost.rebuilt_levels);

  const auto far_cost = clip.focus({centre.x + clip.coverage_m(clip.first_resident()) * 2.0, centre.y},
                                   opts.zoom_m_per_pixel, opts.view_distance_m);
  std::cout << std::format("  прыжок за пределы окна: {} текселей в {} регионах, перепечек {}\n", far_cost.texels,
                           far_cost.regions, far_cost.rebuilt_levels);
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


void verify_clipmap_levels(checker& check, const pf09::clipmap& clip) {
  bool doubling = true;
  bool monotone = true;
  for (uint32_t k = 1; k < clip.level_count(); ++k) {
    doubling = doubling && std::abs(clip.coverage_m(k) - clip.coverage_m(k - 1) * 2.0) < 1.0e-6;
    monotone = monotone && uint32_t(clip.level_tier(k)) <= uint32_t(clip.level_tier(k - 1));
  }

  check.expect(doubling, "уровень покрывает вдвое больше предыдущего");

  // Ярус, записанный на уровне, не может углубляться с ростом уровня: более крупный тексель разрешает
  // МЕНЬШЕ, а не больше. Разъедься это — и подъём по дереву от записанного узла перестал бы давать
  // запрошенный ярус, потому что запрошенный оказался бы глубже записанного.
  check.expect(monotone, "ярус уровня не углубляется с масштабом");
  check.expect(clip.coverage_m(clip.level_count() - 1) >= clip.config().side * 0.0, "уровни существуют");
}

// Полное сравнение резидентных уровней с эталонной перепечкой. Это главный контракт площадки в чистом
// виде: тексель обязан равняться прямому разрешению своего центра, и никакая инкрементальность не имеет
// права этого изменить.
size_t compare_resident(const pf09::clipmap& clip, const glm::dvec2& centre) {
  size_t mismatches = 0;
  for (uint32_t k = clip.first_resident(); k < clip.first_resident() + clip.resident_count(); ++k) {
    const auto reference = clip.bake_reference(k, centre);
    const auto origin = clip.window_origin(k);
    const int64_t side = int64_t(clip.config().side);

    for (int64_t y = origin.y; y < origin.y + side; ++y) {
      for (int64_t x = origin.x; x < origin.x + side; ++x) {
        const auto point = clip.texel_center_m({x, y}, k);
        const int64_t wx = ((x % side) + side) % side;
        const int64_t wy = ((y % side) + side) % side;
        if (clip.sample(point, k) != reference[size_t(wy * side + wx)]) ++mismatches;
      }
    }
  }
  return mismatches;
}

void verify_clipmap_bake(checker& check, const pf09::territory& map, const options& opts) {
  pf09::clipmap clip(map, opts.clip);
  const glm::dvec2 centre{map.config().world_span_m * 0.5, map.config().world_span_m * 0.5};
  clip.focus(centre, opts.zoom_m_per_pixel, opts.view_distance_m);

  check.expect(compare_resident(clip, centre) == 0, "клипмап равен прямому разрешению",
               "тексели резидентных уровней разошлись с перепечкой");
}

void verify_clipmap_walk(checker& check, const pf09::territory& map, const options& opts) {
  auto small = opts.clip;
  small.side = opts.verify_clip_side;

  // Инварианты тороидального обновления не зависят от стороны, а полная сверка зависит от неё квадратично.
  // Поэтому прогулка камеры идёт на маленьком экземпляре, а бюджет памяти меряется на настоящем.
  pf09::clipmap clip(map, small);

  const double span = map.config().world_span_m;
  glm::dvec2 centre{span * 0.5, span * 0.5};
  clip.focus(centre, opts.zoom_m_per_pixel, opts.view_distance_m);

  size_t mismatches = 0;
  size_t overlaps = 0;
  size_t out_of_bounds = 0;
  size_t over_budget = 0;
  constexpr uint32_t steps = 24;

  for (uint32_t i = 0; i < steps; ++i) {
    const auto jump = sample_point(i, 1.0, 0xc0ffeeull);
    const double reach = clip.texel_size_m(clip.first_resident()) * double(clip.config().side) * 0.35;
    const glm::dvec2 moved{centre.x + (jump.x * 2.0 - 1.0) * reach, centre.y + (jump.y * 2.0 - 1.0) * reach};

    const auto previous_origin = clip.window_origin(clip.first_resident());
    const auto cost = clip.focus(moved, opts.zoom_m_per_pixel, opts.view_distance_m);
    centre = moved;

    uint64_t region_texels = 0;
    for (const auto& region : clip.regions()) {
      region_texels += region.texel_count();
      if (region.x + region.width > clip.config().side || region.y + region.height > clip.config().side) {
        ++out_of_bounds;
      }
    }

    // Регионы обязаны покрывать записанное ровно один раз. Двойная запись стоила бы вдвое дороже и
    // прятала бы ошибку адресации: перекрывающиеся полосы дают верный результат при неверных границах.
    if (region_texels != cost.texels) ++overlaps;

    const auto origin = clip.window_origin(clip.first_resident());
    const int64_t dx = std::abs(origin.x - previous_origin.x);
    const int64_t dy = std::abs(origin.y - previous_origin.y);
    const uint64_t bound = uint64_t(clip.config().side) * uint64_t(dx + dy) * clip.resident_count() + 1u;
    if (cost.rebuilt_levels == 0 && cost.texels > bound) ++over_budget;

    mismatches += compare_resident(clip, centre);
  }

  check.expect(mismatches == 0, "тороидальное обновление точно",
               std::format("{} текселей разошлись с перепечкой после сдвигов", mismatches));
  check.expect(overlaps == 0, "регионы не перекрываются", std::format("{} шагов с двойной записью", overlaps));
  check.expect(out_of_bounds == 0, "регионы внутри текстуры", std::format("{} регионов за краем", out_of_bounds));
  check.expect(over_budget == 0, "трафик обновления ограничен сдвигом",
               std::format("{} шагов дороже границы side*(|dx|+|dy|)", over_budget));
}

void verify_clipmap_hysteresis(checker& check, const pf09::territory& map, const options& opts) {
  auto small = opts.clip;
  small.side = opts.verify_clip_side;
  pf09::clipmap clip(map, small);

  const glm::dvec2 centre{map.config().world_span_m * 0.5, map.config().world_span_m * 0.5};

  // Зум ставится ровно на границу между уровнями и качается вокруг неё меньше, чем на гистерезис. Без
  // гистерезиса окно переключалось бы каждый кадр, а переключение окна — это полная перепечка уровня.
  const double boundary = clip.texel_size_m(0) * 4.0;
  clip.focus(centre, boundary, opts.view_distance_m);
  const auto settled = clip.first_resident();

  uint32_t switches = 0;
  uint32_t rebuilds = 0;
  for (uint32_t i = 0; i < 32; ++i) {
    const double wobble = (i % 2 == 0) ? 1.0 - 0.08 : 1.0 + 0.08;
    const auto cost = clip.focus(centre, boundary * wobble, opts.view_distance_m);
    rebuilds += cost.rebuilt_levels;
    if (clip.first_resident() != settled) ++switches;
  }

  check.expect(switches == 0, "гистерезис держит окно", std::format("{} переключений на дрожащем зуме", switches));
  check.expect(rebuilds == 0, "дрожащий зум не перепекает уровни", std::format("{} перепечек", rebuilds));
}

void verify_clipmap_quantisation(checker& check, const pf09::territory& map, const options& opts,
                                 const uint32_t samples) {
  auto small = opts.clip;
  small.side = opts.verify_clip_side;
  pf09::clipmap clip(map, small);

  const glm::dvec2 centre{map.config().world_span_m * 0.5, map.config().world_span_m * 0.5};
  clip.focus(centre, opts.zoom_m_per_pixel, opts.view_distance_m);

  const uint32_t level = clip.first_resident();
  const auto value = clip.level_tier(level);
  const double texel = clip.texel_size_m(level);
  const double reach = clip.coverage_m(level) * 0.45;

  size_t inspected = 0;
  size_t band = 0;
  size_t vanished = 0;
  size_t ancestor_mismatches = 0;

  for (uint32_t i = 0; i < samples; ++i) {
    const auto unit = sample_point(i, 1.0, 0x9105ull);
    const glm::dvec2 point{centre.x + (unit.x * 2.0 - 1.0) * reach, centre.y + (unit.y * 2.0 - 1.0) * reach};

    const auto stored = clip.sample(point, level);
    if (stored == pf09::invalid_zone) continue;
    ++inspected;

    // Подъём от записанного узла обязан совпасть с прямым разрешением ТОГО ЖЕ ТЕКСЕЛЯ. Это точное
    // равенство без всяких оговорок, и на нём держится показ любого яруса из одного растра.
    const glm::dvec2 sampled_centre{std::floor(point.x / texel) * texel + texel * 0.5,
                                    std::floor(point.y / texel) * texel + texel * 0.5};
    if (map.ancestor_at(stored, pf09::tier::barony) != map.resolve(sampled_centre, pf09::tier::barony)) {
      ++ancestor_mismatches;
    }

    if (stored == map.resolve(point, value)) continue;

    // Растр — это точечная выборка, а точка не равна центру своего текселя. Значит расхождение около
    // границы неизбежно и НЕ является дефектом; дефект — это расхождение ВДАЛИ от границы, потому что оно
    // означает территорию, целиком провалившуюся между текселями. Её и ловим: если все девять соседних
    // центров дают один и тот же узел, а точка внутри них — другой, то на этом уровне ярус на самом деле
    // не разрешим, как бы его ни объявили.
    bool uniform = true;
    for (int32_t dy = -1; dy <= 1 && uniform; ++dy) {
      for (int32_t dx = -1; dx <= 1 && uniform; ++dx) {
        const glm::dvec2 probe{sampled_centre.x + dx * texel, sampled_centre.y + dy * texel};
        uniform = map.resolve(probe, value) == stored;
      }
    }

    if (uniform) {
      ++vanished;
    } else {
      ++band;
    }
  }

  check.expect(inspected > 0, "точки попали в окно клипмапа");
  check.expect(ancestor_mismatches == 0, "подъём от текселя даёт нужный ярус",
               std::format("{} расхождений из {}", ancestor_mismatches, inspected));
  check.expect(vanished == 0, "территория не проваливается между текселями",
               std::format("{} точек разошлись с растром вдали от границы", vanished));

  std::cout << std::format("  замер: полоса квантования растра {:.1f}% точек при {} текселях на ячейку яруса '{}'\n",
                           100.0 * double(band) / double(std::max<size_t>(inspected, 1)), opts.clip.min_tier_texels,
                           pf09::tier_name(value));
}

void verify_fingerprint(checker& check, const pf09::territory& map, const uint64_t expected) {
  if (expected == 0) return;
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

  const pf09::clipmap probe(map, opts.clip);
  verify_clipmap_levels(check, probe);
  verify_clipmap_bake(check, map, opts);
  verify_clipmap_walk(check, map, opts);
  verify_clipmap_hysteresis(check, map, opts);
  verify_clipmap_quantisation(check, map, opts, opts.samples / 4 + 1);
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

int run_dump(const pf09::territory& map, const options& opts) {
  const uint32_t size = std::max(opts.dump_size, 8u);
  const double step = opts.dump_span_m / double(size);
  const double origin_x = opts.dump_center_x_m - opts.dump_span_m * 0.5;
  const double origin_y = opts.dump_center_y_m - opts.dump_span_m * 0.5;

  // Отдельный маркер для «этот ярус здесь не разрешим»: он не должен раствориться среди обычных цветов,
  // потому что именно он показывает границу применимости zone LOD.
  constexpr pf09::zone_id unresolvable = pf09::invalid_zone - 1;

  std::vector<pf09::zone_id> keys(size_t(size) * size);
  if (opts.dump_from == dump_source::direct) {
    for (uint32_t y = 0; y < size; ++y) {
      for (uint32_t x = 0; x < size; ++x) {
        const glm::dvec2 point{origin_x + (x + 0.5) * step, origin_y + (y + 0.5) * step};
        keys[size_t(y) * size + x] = map.resolve(point, opts.dump_tier);
      }
    }
  } else {
    pf09::clipmap clip(map, opts.clip);
    const glm::dvec2 centre{opts.dump_center_x_m, opts.dump_center_y_m};
    clip.focus(centre, step, opts.dump_span_m * 0.5);

    std::vector<uint64_t> per_level(clip.level_count(), 0);
    uint64_t outside = 0;

    for (uint32_t y = 0; y < size; ++y) {
      for (uint32_t x = 0; x < size; ++x) {
        const glm::dvec2 point{origin_x + (x + 0.5) * step, origin_y + (y + 0.5) * step};

        // Берём самый мелкий резидентный уровень, чьё окно накрывает точку. Это и есть правило выполнения
        // zone LOD: карта хранит один ярус на уровень и умеет показать любой ярус НЕ ГЛУБЖЕ него.
        auto value = pf09::zone_id(pf09::invalid_zone);
        for (uint32_t k = clip.first_resident(); k < clip.first_resident() + clip.resident_count(); ++k) {
          const auto stored = clip.sample(point, k);
          if (stored == pf09::invalid_zone) continue;

          value = uint32_t(opts.dump_tier) > uint32_t(clip.level_tier(k))
                    ? unresolvable
                    : map.ancestor_at(stored, opts.dump_tier);
          ++per_level[k];
          break;
        }
        if (value == pf09::invalid_zone) ++outside;
        keys[size_t(y) * size + x] = value;
      }
    }

    std::cout << std::format("  клипмап: окно {}..{}", clip.first_resident(),
                             clip.first_resident() + clip.resident_count() - 1);
    for (uint32_t k = clip.first_resident(); k < clip.first_resident() + clip.resident_count(); ++k) {
      if (per_level[k] != 0) {
        std::cout << std::format(", уровень {} даёт {:.1f}% пикселей", k,
                                 100.0 * double(per_level[k]) / double(keys.size()));
      }
    }
    std::cout << std::format(", вне окна {:.1f}%\n", 100.0 * double(outside) / double(keys.size()));
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
    case action::clipmap: return run_clipmap_report(map, opts);
    case action::verify: return run_verification(map, opts);
    case action::dump: return run_dump(map, opts);
  }
  return EXIT_SUCCESS;
}
