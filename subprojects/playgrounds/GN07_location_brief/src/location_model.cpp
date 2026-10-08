#include "location_model.h"

#include <algorithm>
#include <deque>

namespace gn07 {

std::string_view to_string(const side value) noexcept {
  switch (value) {
    case side::north: return "north";
    case side::east: return "east";
    case side::south: return "south";
    case side::west: return "west";
    case side::none: break;
  }
  return "none";
}

side side_from_string(const std::string_view text) noexcept {
  if (text == "north") return side::north;
  if (text == "east") return side::east;
  if (text == "south") return side::south;
  if (text == "west") return side::west;
  return side::none;
}

std::string_view to_string(const passage value) noexcept {
  return value == passage::hidden ? "hidden" : "open";
}

std::string_view to_string(const body_shape value) noexcept {
  return value == body_shape::chamber ? "chamber" : "corridor";
}

std::string_view to_string(const link_kind value) noexcept {
  return value == link_kind::secret ? "secret" : "ordinary";
}

bool link_kind_from_string(const std::string_view text, link_kind& out) noexcept {
  if (text == "ordinary") {
    out = link_kind::ordinary;
    return true;
  }
  if (text == "secret") {
    out = link_kind::secret;
    return true;
  }
  return false;
}

const port_kind* archetype::find_port(const link_kind kind) const noexcept {
  for (const auto& p : ports) {
    if (p.kind == kind) {
      return &p;
    }
  }
  return nullptr;
}

const world_node* world_fragment::find(const std::string_view name) const noexcept {
  for (const auto& n : nodes) {
    if (n.name == name) {
      return &n;
    }
  }
  return nullptr;
}

std::vector<fact> world_fragment::facts_of(const std::string_view name) const {
  // Цепочка от узла к корню, затем факты от корня вниз: потомок перекрывает предка.
  std::vector<const world_node*> chain;
  for (const world_node* n = find(name); n != nullptr; n = n->parent.empty() ? nullptr : find(n->parent)) {
    if (std::find(chain.begin(), chain.end(), n) != chain.end()) {
      break; // цикл родителей ловит чтение фрагмента; здесь только не зациклиться
    }
    chain.push_back(n);
  }
  std::vector<fact> out;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    for (const auto& f : (*it)->facts) {
      const auto same = std::find_if(out.begin(), out.end(), [&](const fact& o) { return o.name == f.name; });
      if (same != out.end()) {
        same->value = f.value;
      } else {
        out.push_back(f);
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const fact& a, const fact& b) { return a.name < b.name; });
  return out;
}

int32_t port_window(const int32_t side_length) noexcept {
  return std::max(2, side_length / 5);
}

bool facing(const int32_t width, const int32_t height, const int64_t dx_east, const int64_t dy_north, side& at,
            int32_t& along) noexcept {
  // В решётке y растёт на юг, в мире — на север.
  const int64_t lx = dx_east;
  const int64_t ly = -dy_north;
  if (lx == 0 && ly == 0) {
    return false;
  }
  const int64_t ax = lx < 0 ? -lx : lx;
  const int64_t ay = ly < 0 ? -ly : ly;
  const int64_t w = width;
  const int64_t h = height;
  int64_t position = 0;
  int32_t length = 0;
  if (ax * h >= ay * w) {
    at = lx > 0 ? side::east : side::west;
    position = (h * ax + ly * w) / (2 * ax);
    length = height;
  } else {
    at = ly > 0 ? side::south : side::north;
    position = (w * ay + lx * h) / (2 * ay);
    length = width;
  }
  along = int32_t(std::clamp<int64_t>(position, 2, length - 3));
  return true;
}

uint32_t archetype::kind_index(const std::string_view kind) const noexcept {
  for (uint32_t i = 0; i < kinds.size(); ++i) {
    if (kinds[i].name == kind) {
      return i;
    }
  }
  return no_node;
}

const place* archetype::find_place(const std::string_view place_name) const noexcept {
  for (const auto& p : places) {
    if (p.name == place_name) {
      return &p;
    }
  }
  return nullptr;
}

bool archetype::knows_tag(const std::string_view tag) const noexcept {
  for (const auto& k : kinds) {
    if (std::find(k.tags.begin(), k.tags.end(), tag) != k.tags.end()) {
      return true;
    }
  }
  return false;
}

bool archetype::knows_function(const std::string_view function) const noexcept {
  for (const auto& k : kinds) {
    if (k.function == function) {
      return true;
    }
  }
  return false;
}

std::vector<int64_t> content_distances(const location_graph& graph, const archetype& arch, const uint32_t from) {
  std::vector<int64_t> out(graph.nodes.size(), -1);
  if (from == no_node) {
    return out;
  }
  std::deque<uint32_t> queue{from};
  out[from] = 0;
  while (!queue.empty()) {
    const uint32_t cur = queue.front();
    queue.pop_front();
    for (const uint32_t next : graph.neighbours[cur]) {
      if (out[next] >= 0) {
        continue;
      }
      out[next] = out[cur] + arch.kinds[graph.nodes[next].kind].content;
      queue.push_back(next);
    }
  }
  return out;
}

void compute_properties(location_graph& graph, const archetype& arch) {
  const size_t count = graph.nodes.size();
  graph.neighbours.assign(count, {});
  for (uint32_t i = 1; i < count; ++i) {
    const uint32_t parent = graph.nodes[i].parent;
    graph.neighbours[i].push_back(parent);
    graph.neighbours[parent].push_back(i);
  }

  graph.depth = content_distances(graph, arch, 0);
  graph.secret_depth = content_distances(graph, arch, graph.secret_entrance);
  graph.max_depth = count == 0 ? 0 : *std::max_element(graph.depth.begin(), graph.depth.end());
  graph.total_content = 0;
  for (const auto& n : graph.nodes) {
    graph.total_content += arch.kinds[n.kind].content;
  }

  // Шаги решётки: у дерева рёбра только между соседними клетками, поэтому шаг = ребро.
  graph.route_steps.assign(count, 0);
  for (uint32_t i = 1; i < count; ++i) {
    // Родитель всегда создан раньше ребёнка, так что его шаги уже посчитаны.
    graph.route_steps[i] = graph.route_steps[graph.nodes[i].parent] + 1;
  }

  // Поддерево: обход с конца, потому что ребёнок всегда старше номером, чем родитель.
  graph.subtree.assign(count, 0);
  for (uint32_t i = uint32_t(count); i-- > 1;) {
    graph.subtree[graph.nodes[i].parent] += graph.subtree[i] + 1;
  }

  // Путь особого входа — всё, что за скрытым проходом: от особого входа к корню, включая
  // первый узел, в который ведёт скрытый проход. Определение опирается на отметку прохода,
  // а не на форму: «до первой развилки» захватывало часть шахты, если ход прицеплен к узлу
  // посреди неразветвлённого штрека. Нет скрытого прохода на пути — пути нет вовсе.
  graph.on_secret_way.assign(count, 0);
  if (graph.secret_entrance != no_node) {
    std::vector<uint32_t> chain;
    for (uint32_t cur = graph.secret_entrance; cur != no_node && cur != 0; cur = graph.nodes[cur].parent) {
      chain.push_back(cur);
      if (graph.nodes[cur].entry == passage::hidden) {
        for (const uint32_t n : chain) {
          graph.on_secret_way[n] = 1;
        }
        break;
      }
    }
  }

  graph.boundary.assign(count, side::none);
  for (uint32_t i = 0; i < count; ++i) {
    const auto& n = graph.nodes[i];
    if (n.y == 0) graph.boundary[i] = side::north;
    else if (n.y == graph.height - 1) graph.boundary[i] = side::south;
    else if (n.x == 0) graph.boundary[i] = side::west;
    else if (n.x == graph.width - 1) graph.boundary[i] = side::east;
  }
}

} // namespace gn07
