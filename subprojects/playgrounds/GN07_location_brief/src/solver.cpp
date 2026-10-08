#include "solver.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <map>
#include <numeric>
#include <sstream>

#include <devils_engine/utils/hash.h>
#include <devils_engine/utils/string_id.h>

#include "mine_family.h"

namespace gn07 {

namespace {

using namespace devils_engine;

// Первый узел хода — тот, чей родитель уже не на ходу. Именно в него ведёт скрытый проход.
uint32_t way_start(const location_graph& g) {
  for (uint32_t i = 0; i < g.nodes.size(); ++i) {
    if (g.on_secret_way[i] != 0 && (g.nodes[i].parent == no_node || g.on_secret_way[g.nodes[i].parent] == 0)) {
      return i;
    }
  }
  return no_node;
}

int32_t along_of(const location_graph& g, const uint32_t node, const side s) {
  return (s == side::west || s == side::east) ? g.nodes[node].y : g.nodes[node].x;
}

int32_t side_length(const location_graph& g, const side s) {
  return (s == side::west || s == side::east) ? g.height : g.width;
}

// Проверка портов по вычисленным свойствам. Пустая строка — все порты выполнены.
std::string check_ports(const location_graph& g, const location_input& in, const archetype& arch,
                        const script_env& env, std::vector<port_outcome>& out) {
  out.clear();
  for (uint32_t i = 0; i < in.ports.size(); ++i) {
    const port_request& port = in.ports[i];
    port_outcome po;
    po.port = i;
    if (port.kind == link_kind::ordinary) {
      if (g.nodes.empty() || arch.kinds[g.nodes[0].kind].name != "entrance") {
        return std::format("link to '{}': the location has no ordinary entrance", port.neighbour);
      }
      if (g.boundary[0] != port.at || along_of(g, 0, port.at) != port.along) {
        return std::format("link to '{}': entrance at {}:{}, the neighbour faces {}:{}", port.neighbour,
          to_string(g.boundary[0]), along_of(g, 0, g.boundary[0]), to_string(port.at), port.along);
      }
      po.node = 0;
    } else {
      if (g.secret_entrance == no_node) {
        return std::format("link to '{}': no secret entrance in the graph", port.neighbour);
      }
      const uint32_t s = g.secret_entrance;
      const int32_t window = port_window(side_length(g, port.at));
      if (g.boundary[s] != port.at || std::abs(along_of(g, s, port.at) - port.along) > window) {
        return std::format("link to '{}': secret entrance at {}:{}, the neighbour faces {}:{} within {}",
          port.neighbour, to_string(g.boundary[s]), along_of(g, s, g.boundary[s]), to_string(port.at), port.along,
          window);
      }
      const uint32_t start = way_start(g);
      if (start == no_node || g.nodes[start].entry != passage::hidden) {
        return std::format("link to '{}': the secret way does not begin with a hidden passage", port.neighbour);
      }
      const place* where = arch.find_place(arch.find_port(link_kind::secret)->place);
      const location_view view{&g, &arch, &in.facts};
      po.attach = g.nodes[start].parent;
      if (!env.evaluate(where->where, view, po.attach)) {
        return std::format("link to '{}': the secret way leaves node {}, which is not a '{}'", port.neighbour,
          po.attach, where->name);
      }
      po.node = s;
      for (uint32_t n = 0; n < g.nodes.size(); ++n) {
        if (g.on_secret_way[n] != 0) {
          ++po.way_nodes;
          po.way_content += arch.kinds[g.nodes[n].kind].content;
        }
      }
    }
    out.push_back(po);
  }
  return {};
}

std::string check_allowance(const location_graph& g, const brief& task) {
  if (task.has_allowance && (g.total_content < task.content_min || g.total_content > task.content_max)) {
    return std::format("total content {} is outside the allowance [{}, {}]", g.total_content, task.content_min,
      task.content_max);
  }
  return {};
}

// Причины разных попыток отличаются числами, а по смыслу совпадают. Для подсчёта числа
// заменяются на '#'; в отказ попадает пример первой такой причины.
std::string cause_key(const std::string_view text) {
  std::string key;
  for (const char c : text) {
    const bool digit = c >= '0' && c <= '9';
    if (digit && (key.empty() || key.back() != '#')) {
      key += '#';
    } else if (!digit) {
      key += c;
    }
  }
  return key;
}

struct cause_count {
  uint32_t count = 0;
  std::string example;
};

void count_cause(std::map<std::string, cause_count>& causes, const std::string& text) {
  auto& entry = causes[cause_key(text)];
  if (entry.count++ == 0) {
    entry.example = text;
  }
}

// То, что не исправит никакой seed. Пустая строка — можно генерировать.
std::string refuse_before_generation(const brief& task, const location_input& in, const archetype& arch) {
  // Контент по пути к привязке — часть общего контента, так что бюджет больше допуска не
  // наберёт никакая попытка.
  if (task.has_allowance) {
    for (const auto& p : task.placements) {
      if (p.budget_min > task.content_max) {
        return std::format("placement '{}' needs at least {} content on its way, the whole location allows at most {}",
          p.name, p.budget_min, task.content_max);
      }
    }
  }
  size_t ordinary = 0;
  size_t secret = 0;
  for (const auto& p : in.ports) {
    if (arch.find_port(p.kind) == nullptr) {
      return std::format("link to '{}' is {}: archetype '{}' has no {} port", p.neighbour, to_string(p.kind),
        arch.name, to_string(p.kind));
    }
    (p.kind == link_kind::ordinary ? ordinary : secret) += 1;
  }
  if (ordinary == 0) {
    return std::format("location '{}' has no ordinary link: its entrance has nowhere to face", in.node->name);
  }
  if (ordinary > 1) {
    return std::format("location '{}' has {} ordinary links; one is supported", in.node->name, ordinary);
  }
  if (secret > 1) {
    return std::format("location '{}' has {} secret links; one is supported", in.node->name, secret);
  }
  if (arch.family != "mine") {
    return std::format("archetype '{}': unknown family '{}'", arch.name, arch.family);
  }
  return {};
}

} // namespace

solve_result solve(const brief& task, const location_input& input, const archetype& arch, const script_env& env,
                   const uint64_t seed, const uint32_t attempts) {
  solve_result result;
  result.attempts = attempts;
  result.refusal = refuse_before_generation(task, input, arch);
  if (!result.refusal.empty()) {
    return result;
  }

  // Самое требовательное размещение задаёт целевую глубину главной выработки.
  const placement* deepest = nullptr;
  for (const auto& p : task.placements) {
    if (deepest == nullptr || p.budget_min > deepest->budget_min) {
      deepest = &p;
    }
  }
  std::vector<uint32_t> order(task.placements.size());
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(), [&](const uint32_t a, const uint32_t b) {
    return task.placements[a].budget_max > task.placements[b].budget_max;
  });

  std::map<std::string, cause_count> causes;
  for (uint32_t k = 0; k < attempts; ++k) {
    const uint64_t attempt_seed = utils::splitmix(seed, k);
    const int64_t target = deepest == nullptr ? 0 :
      deepest->budget_min + int64_t(utils::splitmix(attempt_seed, 0x7a) %
        uint64_t(deepest->budget_max - deepest->budget_min + 1));

    family_request request;
    request.input = &input;
    request.arch = &arch;
    request.env = &env;
    request.seed = attempt_seed;
    request.target_depth = target;
    request.has_allowance = task.has_allowance;
    request.content_min = task.content_min;
    request.content_max = task.content_max;
    family_result built = build_mine(request);
    if (!built.valid()) {
      count_cause(causes, built.refusal);
      continue;
    }
    location_graph& g = built.graph;
    compute_properties(g, arch);

    std::vector<port_outcome> ports;
    std::string cause = check_ports(g, input, arch, env, ports);
    if (cause.empty()) {
      cause = check_allowance(g, task);
    }

    std::vector<binding> bindings;
    const location_view view{&g, &arch, &input.facts};
    for (const uint32_t pi : order) {
      if (!cause.empty()) {
        break;
      }
      const auto& p = task.placements[pi];
      const place* where = arch.find_place(p.place);
      std::vector<uint32_t> matching;
      size_t by_place = 0;
      for (uint32_t n = 0; n < g.nodes.size(); ++n) {
        if (!env.evaluate(where->where, view, n)) {
          continue;
        }
        ++by_place;
        if (g.depth[n] >= p.budget_min && g.depth[n] <= p.budget_max) {
          matching.push_back(n);
        }
      }
      if (matching.empty()) {
        cause = std::format("placement '{}': place '{}' matches {} nodes, none with content budget [{}, {}]",
          p.name, p.place, by_place, p.budget_min, p.budget_max);
        break;
      }
      const uint64_t pick = utils::splitmix(attempt_seed, utils::string_hash(p.name));
      bindings.push_back(binding{pi, matching[size_t(pick % matching.size())], matching.size()});
    }
    if (!cause.empty()) {
      count_cause(causes, cause);
      continue;
    }

    // Каркас — после привязок: он не меняет ни графа, ни свойств, а только воплощает их, поэтому
    // дорогой шаг делается для графа, который уже годен.
    frame_result framed = build_frame(g, arch, utils::splitmix(attempt_seed, 0xf4a3e));
    if (!framed.valid()) {
      count_cause(causes, framed.refusal);
      continue;
    }

    std::sort(bindings.begin(), bindings.end(), [](const binding& a, const binding& b) {
      return a.placement < b.placement;
    });
    result.ok = true;
    result.frame = std::move(framed.frame);
    result.attempt = k;
    result.attempt_seed = attempt_seed;
    result.target_depth = target;
    result.branches = built.branches_grown;
    result.graph = std::move(g);
    result.bindings = std::move(bindings);
    result.ports = std::move(ports);
    return result;
  }

  // Самая частая причина; при равенстве — первая по ключу, чтобы отказ был воспроизводим.
  const auto top = std::max_element(causes.begin(), causes.end(), [](const auto& a, const auto& b) {
    return a.second.count < b.second.count;
  });
  result.refusal = top == causes.end() ? "no attempts" :
    std::format("no attempt of {} satisfied the brief; most frequent cause ({}/{}): {}", attempts,
      top->second.count, attempts, top->second.example);
  return result;
}

std::vector<std::string> verify_result(const solve_result& result, const brief& task, const location_input& input,
                                       const archetype& arch, const script_env& env) {
  std::vector<std::string> problems;
  if (!result.ok) {
    problems.push_back("result is a refusal: " + result.refusal);
    return problems;
  }
  const auto problem = [&](std::string text) { problems.push_back(std::move(text)); };

  // Свойства пересчитываются на копии: проверка не доверяет тому, что сохранил решатель.
  location_graph g = result.graph;
  compute_properties(g, arch);
  const location_graph& stored = result.graph;
  if (g.depth != stored.depth || g.secret_depth != stored.secret_depth || g.subtree != stored.subtree ||
      g.on_secret_way != stored.on_secret_way || g.boundary != stored.boundary || g.max_depth != stored.max_depth ||
      g.total_content != stored.total_content) {
    problem("stored properties differ from recomputed ones");
  }
  if (g.width != input.width || g.height != input.height) {
    problem(std::format("lattice {}x{} differs from the location's {}x{}", g.width, g.height, input.width, input.height));
  }

  // Дерево на решётке: родитель старше номером, клетки различны, ребро — соседние клетки,
  // на кольце только входы.
  std::map<std::pair<int32_t, int32_t>, uint32_t> cells;
  for (uint32_t i = 0; i < g.nodes.size(); ++i) {
    const auto& n = g.nodes[i];
    if (!cells.emplace(std::make_pair(n.x, n.y), i).second) {
      problem(std::format("node {} shares a cell with node {}", i, cells[{n.x, n.y}]));
    }
    const bool ring = n.x == 0 || n.y == 0 || n.x == g.width - 1 || n.y == g.height - 1;
    const bool entrance = i == 0 || i == g.secret_entrance;
    if (ring != entrance) {
      problem(std::format("node {} {} the boundary ring", i, ring ? "lies on" : "should lie on"));
    }
    if (i == 0) {
      if (n.parent != no_node) problem("node 0 has a parent");
      continue;
    }
    if (n.parent >= i) {
      problem(std::format("node {} has parent {} that is not older", i, n.parent));
      continue;
    }
    const auto& p = g.nodes[n.parent];
    if (std::abs(p.x - n.x) + std::abs(p.y - n.y) != 1) {
      problem(std::format("node {} is not adjacent to its parent", i));
    }
  }

  // Глубина пересчитывается другим способом — по цепочке родителей, а не обходом соседей.
  const auto depth_by_parents = [&](uint32_t n) {
    int64_t sum = 0;
    for (; n != 0 && n != no_node; n = g.nodes[n].parent) {
      sum += arch.kinds[g.nodes[n].kind].content;
    }
    return sum;
  };

  if (result.bindings.size() != task.placements.size()) {
    problem(std::format("{} bindings for {} placements", result.bindings.size(), task.placements.size()));
  }
  const location_view view{&g, &arch, &input.facts};
  for (const auto& b : result.bindings) {
    const auto& p = task.placements[b.placement];
    const int64_t depth = depth_by_parents(b.node);
    if (depth != g.depth[b.node]) {
      problem(std::format("placement '{}': stored depth {} differs from recomputed {}", p.name, g.depth[b.node], depth));
    }
    if (depth < p.budget_min || depth > p.budget_max) {
      problem(std::format("placement '{}': depth {} outside budget [{}, {}]", p.name, depth, p.budget_min, p.budget_max));
    }
    if (!env.evaluate(arch.find_place(p.place)->where, view, b.node)) {
      problem(std::format("placement '{}': node {} does not satisfy place '{}'", p.name, b.node, p.place));
    }
  }

  std::vector<port_outcome> ports;
  if (const std::string cause = check_ports(g, input, arch, env, ports); !cause.empty()) {
    problem(cause);
  }
  if (const std::string cause = check_allowance(g, task); !cause.empty()) {
    problem(cause);
  }
  for (auto& p : verify_frame(result.frame, g, arch, input)) {
    problem(std::move(p));
  }
  return problems;
}

std::string format_report(const solve_result& result, const brief& task, const location_input& input,
                          const archetype& arch) {
  std::ostringstream out;
  if (!result.ok) {
    out << "refused: " << result.refusal << "\n";
    return out.str();
  }
  const location_graph& g = result.graph;
  std::string tags;
  for (const auto& t : arch.tags) {
    tags += tags.empty() ? t : "," + t;
  }
  out << std::format("brief={} location={} archetype={} tags=[{}] family={} attempt={}/{} target_depth={}\n",
    task.name, input.node->name, arch.name, tags, arch.family, result.attempt + 1, result.attempts,
    result.target_depth);
  out << std::format("graph: nodes={} lattice={}x{} cell={} max_depth={} branches={} total_content={}", g.nodes.size(),
    g.width, g.height, g.cell, g.max_depth, result.branches, g.total_content);
  if (task.has_allowance) {
    out << std::format(" allowance=[{},{}]", task.content_min, task.content_max);
  }
  out << "\n";

  out << "facts:";
  for (const auto& f : input.facts) {
    out << std::format(" {}={}", f.name, f.value);
  }
  out << "\n";

  out << "kinds:";
  for (uint32_t k = 0; k < arch.kinds.size(); ++k) {
    const auto count = std::count_if(g.nodes.begin(), g.nodes.end(), [&](const graph_node& n) { return n.kind == k; });
    out << std::format(" {}={}", arch.kinds[k].name, count);
  }
  out << "\n";

  const frame_analysis fa = analyse_frame(result.frame);
  const std::vector<int64_t> walk = frame_walk(result.frame, fa, g);
  {
    int64_t area = 0;
    for (const int64_t a : fa.zone_area) area += a;
    int32_t narrowest = 0;
    size_t hidden = 0;
    for (const auto& j : fa.joins) {
      narrowest = narrowest == 0 ? j.width : std::min(narrowest, j.width);
      const uint32_t child = g.nodes[j.b].parent == j.a ? j.b : j.a;
      hidden += g.nodes[child].entry == passage::hidden;
    }
    int64_t wall = 0;
    while ((wall + 1) * (wall + 1) <= fa.thinnest_wall_sq) ++wall;
    out << std::format("frame: plot={}x{} zones={} parts={} area={} m2 joins={} hidden={} narrowest_join={} "
                       "thinnest_wall={} (zones {}-{})\n", metres(result.frame.width), metres(result.frame.height),
      result.frame.zones.size(), fa.parts, area / (plan_scale * plan_scale), fa.joins.size(), hidden,
      metres(narrowest), metres(wall), fa.wall_a, fa.wall_b);
    for (const auto& o : fa.openings) {
      out << std::format("opening zone {} {} {}..{}\n", o.zone, to_string(o.at), metres(o.from), metres(o.to));
    }
  }

  for (const auto& po : result.ports) {
    const auto& port = input.ports[po.port];
    const auto& n = g.nodes[po.node];
    out << std::format("port {} to {} -> node {} at ({},{}) side={} facing={}:{}", to_string(port.kind),
      port.neighbour, po.node, n.x, n.y, to_string(g.boundary[po.node]), to_string(port.at), port.along);
    if (port.kind == link_kind::secret) {
      out << std::format(" from node {} {} depth={} way_nodes={} way_content={} way_length={}", po.attach,
        arch.kinds[g.nodes[po.attach].kind].name, g.depth[po.attach], po.way_nodes, po.way_content,
        po.way_nodes * g.cell);
    }
    out << "\n";
  }

  for (const auto& b : result.bindings) {
    const auto& p = task.placements[b.placement];
    const auto& n = g.nodes[b.node];
    out << std::format("placement {} ({} x{}) place={} budget=[{},{}] -> node {} {} at ({},{}) depth={} "
                       "route_length={} walk={} candidates={}", p.name, p.what, p.count, p.place, p.budget_min,
      p.budget_max, b.node, arch.kinds[n.kind].name, n.x, n.y, g.depth[b.node], g.route_steps[b.node] * g.cell,
      metres(walk[b.node]), b.candidates);
    if (g.secret_entrance != no_node) {
      out << std::format(" secret_depth={}", g.secret_depth[b.node]);
    }
    out << "\n";
  }
  for (const auto& l : task.lore) {
    out << std::format("lore {} -[{}]-> {}\n", l.from, l.kind, l.to);
  }
  // Преимущество тайного хода: сколько контента до каждой привязки от обычного и от особого входа.
  if (g.secret_entrance != no_node) {
    out << "advantage:";
    for (const auto& b : result.bindings) {
      out << std::format(" {}={}/{}", task.placements[b.placement].name, g.depth[b.node], g.secret_depth[b.node]);
    }
    out << " (content from ordinary/secret entrance)\n";
  }
  return out.str();
}

std::string format_frame_svg(const solve_result& result, const brief& task, const archetype& arch) {
  if (!result.ok) {
    return {};
  }
  std::vector<frame_mark> marks;
  for (const auto& b : result.bindings) {
    marks.push_back({b.node, std::format("{} {}", b.placement + 1, task.placements[b.placement].name)});
  }
  return format_svg(result.frame, result.graph, arch, marks);
}

std::string format_map(const solve_result& result, const archetype& arch) {
  if (!result.ok) {
    return {};
  }
  const location_graph& g = result.graph;
  std::vector<std::string> rows(size_t(g.height), std::string(size_t(g.width), '.'));
  for (const auto& n : g.nodes) {
    rows[size_t(n.y)][size_t(n.x)] = arch.kinds[n.kind].glyph;
  }
  // Привязки поверх глифов: цифра — номер размещения в задании.
  for (const auto& b : result.bindings) {
    const auto& n = g.nodes[b.node];
    rows[size_t(n.y)][size_t(n.x)] = char('1' + (b.placement % 9));
  }
  std::string out;
  for (const auto& row : rows) {
    out += row;
    out += '\n';
  }
  return out;
}

} // namespace gn07
