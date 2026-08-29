// PF09 — territory zoning. Срез 1 полностью headless: иерархия территорий существует раньше любого
// растра, и все следующие срезы обязаны совпадать с тем, что считает этот executable.

#include <algorithm>
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

#include "territory.h"

using namespace devils_engine;

namespace {

enum class action : uint32_t { report, verify, dump };
enum class dump_mode : uint32_t { zone, owner };

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
               "  --verify              численные инварианты, ненулевой код возврата при провале\n"
               "  --dump=PATH           ppm-срез карты для глаз\n"
               "  --dump-tier=N         ярус окраски дампа, 0 = world, 6 = parcel\n"
               "  --dump-mode=NAME      zone | owner\n"
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
    case action::verify: return run_verification(map, opts);
    case action::dump: return run_dump(map, opts);
  }
  return EXIT_SUCCESS;
}
