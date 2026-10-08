#include "mine_family.h"

#include <algorithm>
#include <array>
#include <deque>
#include <format>
#include <limits>
#include <stdexcept>

#include <devils_engine/utils/hash.h>

namespace gn07 {

namespace {

using namespace devils_engine;

// Счётчиковый поток: значение зависит только от (seed, номер вызова), а не от истории состояния.
struct draw {
  uint64_t seed = 0;
  uint64_t counter = 0;
  uint64_t next() noexcept { return utils::splitmix(seed, counter++); }
  int64_t range(const int64_t lo, const int64_t hi) noexcept {
    return lo + int64_t(next() % uint64_t(hi - lo + 1));
  }
  bool percent(const int64_t p) noexcept { return int64_t(next() % 100) < p; }
  template <typename C>
  void shuffle(C& values) noexcept {
    for (size_t i = values.size(); i > 1; --i) {
      std::swap(values[i - 1], values[size_t(next() % i)]);
    }
  }
};

// Направления: north, east, south, west. Индекс совпадает с (side - 1).
constexpr std::array<int32_t, 4> dir_x{0, 1, 0, -1};
constexpr std::array<int32_t, 4> dir_y{-1, 0, 1, 0};

int inward_direction(const side entrance) noexcept {
  switch (entrance) {
    case side::north: return 2;
    case side::east: return 3;
    case side::south: return 0;
    case side::west: return 1;
    case side::none: break;
  }
  return 1;
}

struct kinds_used {
  uint32_t entrance, camp, drift, abandoned, face, passage, secret;
};

uint32_t require_kind(const archetype& arch, const std::string_view name) {
  const uint32_t index = arch.kind_index(name);
  if (index == no_node) {
    throw std::runtime_error(std::format("archetype '{}': family 'mine' needs kind '{}'", arch.name, name));
  }
  return index;
}

class builder {
public:
  builder(const family_request& req, family_result& out)
    : req_(req), in_(*req.input), arch_(*req.arch), out_(out), g_(out.graph), rng_{req.seed} {
    g_.cell = in_.cell;
    g_.width = in_.width;
    g_.height = in_.height;
    k_ = kinds_used{
      require_kind(arch_, "entrance"), require_kind(arch_, "camp"), require_kind(arch_, "drift"),
      require_kind(arch_, "abandoned_drift"), require_kind(arch_, "face"),
      require_kind(arch_, "passage"), require_kind(arch_, "secret_entrance")};
  }

  void run() {
    if (g_.width < 5 || g_.height < 5) {
      refuse(std::format("bounds {}x{} give a lattice {}x{}; the family needs at least 5x5",
        in_.node->bounds_width, in_.node->bounds_height, g_.width, g_.height));
      return;
    }
    occupied_.assign(size_t(g_.width) * size_t(g_.height), no_node);
    const port_request* ordinary = nullptr;
    for (const auto& p : in_.ports) {
      if (p.kind == link_kind::ordinary) {
        ordinary = &p;
        break;
      }
    }
    if (ordinary == nullptr || !place_entrance(*ordinary) || !grow_main_line()) {
      if (ordinary == nullptr) {
        refuse("no ordinary link to place the entrance on");
      }
      return;
    }
    grow_branches();
    for (const auto& p : in_.ports) {
      if (p.kind == link_kind::secret && !grow_secret_way(p)) {
        return;
      }
    }
  }

private:
  void refuse(std::string reason) { out_.refusal = std::move(reason); }

  bool inside(const int32_t x, const int32_t y) const noexcept {
    return x >= 0 && y >= 0 && x < g_.width && y < g_.height;
  }
  bool interior(const int32_t x, const int32_t y) const noexcept {
    return x >= 1 && y >= 1 && x < g_.width - 1 && y < g_.height - 1;
  }
  uint32_t at(const int32_t x, const int32_t y) const noexcept {
    return inside(x, y) ? occupied_[size_t(y) * size_t(g_.width) + size_t(x)] : no_node;
  }

  // Клетка свободна, и сторонами она касается только своего будущего родителя.
  bool can_place(const int32_t x, const int32_t y, const uint32_t parent, const bool allow_ring = false) const noexcept {
    if (!(interior(x, y) || (allow_ring && inside(x, y))) || at(x, y) != no_node) {
      return false;
    }
    for (int d = 0; d < 4; ++d) {
      const uint32_t n = at(x + dir_x[d], y + dir_y[d]);
      if (n != no_node && n != parent) {
        return false;
      }
    }
    return true;
  }

  int64_t content(const uint32_t kind) const noexcept { return arch_.kinds[kind].content; }

  uint32_t add(const uint32_t kind, const uint32_t parent, const int32_t x, const int32_t y,
               const passage entry = passage::open) {
    const uint32_t index = uint32_t(g_.nodes.size());
    g_.nodes.push_back(graph_node{kind, parent, entry, x, y});
    occupied_[size_t(y) * size_t(g_.width) + size_t(x)] = index;
    total_ += content(kind);
    return index;
  }

  // Клетка кольца на стороне `s` в позиции `along` (вдоль стороны).
  void ring_cell(const side s, const int32_t along, int32_t& x, int32_t& y) const noexcept {
    x = s == side::west ? 0 : s == side::east ? g_.width - 1 : along;
    y = s == side::north ? 0 : s == side::south ? g_.height - 1 : along;
  }

  bool place_entrance(const port_request& port) {
    int32_t x = 0, y = 0;
    ring_cell(port.at, port.along, x, y);
    add(k_.entrance, no_node, x, y);
    main_dir_ = inward_direction(port.at);
    camp_ = add(k_.camp, 0, x + dir_x[main_dir_], y + dir_y[main_dir_]);
    return true;
  }

  // Шаг из узла: сначала, с заданной вероятностью, поворот, иначе прямо; назад — никогда.
  bool step(const uint32_t from, int& dir, int32_t& x, int32_t& y) {
    std::array<int, 3> order{dir, (dir + 1) % 4, (dir + 3) % 4};
    if (rng_.percent(arch_.growth.turn_percent)) {
      std::swap(order[0], order[rng_.percent(50) ? 1 : 2]);
    } else if (rng_.percent(50)) {
      std::swap(order[1], order[2]);
    }
    const auto& n = g_.nodes[from];
    for (const int d : order) {
      if (can_place(n.x + dir_x[d], n.y + dir_y[d], from)) {
        dir = d;
        x = n.x + dir_x[d];
        y = n.y + dir_y[d];
        return true;
      }
    }
    return false;
  }

  bool grow_main_line() {
    const int64_t target = std::max<int64_t>(req_.target_depth, content(k_.camp) + content(k_.face));
    uint32_t cur = camp_;
    int64_t depth = content(k_.camp);
    int dir = main_dir_;
    while (depth + content(k_.face) < target) {
      int32_t x = 0, y = 0;
      if (!step(cur, dir, x, y)) {
        refuse(std::format("main line blocked at content depth {} of target {}", depth, target));
        return false;
      }
      cur = add(k_.drift, cur, x, y);
      depth += content(k_.drift);
    }
    int32_t x = 0, y = 0;
    if (!step(cur, dir, x, y)) {
      refuse(std::format("main line has no room for its face at content depth {}", depth));
      return false;
    }
    add(k_.face, cur, x, y);
    return true;
  }

  // Тайный ход начинается в узле, который удовлетворяет месту порта из архетипа. Ход растёт
  // ПОСЛЕДНИМ: его узлы бедны контентом и висят на конце, поэтому ничьей глубины не меняют, и
  // предикат вычисляется на окончательной форме. Пока ход рос раньше боковых выработок, они
  // поднимали максимальную глубину, и место порта переставало выполняться уже после выбора.
  bool grow_secret_way(const port_request& port) {
    if (g_.secret_entrance != no_node) {
      refuse(std::format("link to '{}': only one secret entrance per location is supported", port.neighbour));
      return false;
    }
    const port_kind* declared = arch_.find_port(link_kind::secret);
    const place* where = arch_.find_place(declared->place);
    compute_properties(g_, arch_);
    const location_view view{&g_, &arch_, &in_.facts};
    std::vector<uint32_t> origins;
    for (uint32_t i = 0; i < g_.nodes.size(); ++i) {
      const uint32_t kind = g_.nodes[i].kind;
      if (kind == k_.entrance || kind == k_.passage || kind == k_.secret) {
        continue;
      }
      if (req_.env->evaluate(where->where, view, i)) {
        origins.push_back(i);
      }
    }
    if (origins.empty()) {
      refuse(std::format("link to '{}': no node satisfies the secret port place '{}'", port.neighbour, where->name));
      return false;
    }
    rng_.shuffle(origins);
    for (const uint32_t origin : origins) {
      if (try_secret_way(origin, port)) {
        return true;
      }
    }
    refuse(std::format("link to '{}': no free way from place '{}' to the {} side", port.neighbour, where->name,
      to_string(port.at)));
    return false;
  }

  // Клетка кольца на стороне соседа и в окне вокруг направления на него.
  bool port_target(const int32_t x, const int32_t y, const port_request& port) const noexcept {
    const bool corner = (x == 0 || x == g_.width - 1) && (y == 0 || y == g_.height - 1);
    if (corner) {
      return false;
    }
    const bool on_side = (port.at == side::north && y == 0) || (port.at == side::south && y == g_.height - 1) ||
      (port.at == side::west && x == 0) || (port.at == side::east && x == g_.width - 1);
    if (!on_side) {
      return false;
    }
    const bool vertical = port.at == side::west || port.at == side::east;
    const int32_t along = vertical ? y : x;
    const int32_t window = port_window(vertical ? g_.height : g_.width);
    return along >= port.along - window && along <= port.along + window;
  }

  // Кратчайший путь по решётке, который не касается чужих узлов. Кратчайший путь не касается
  // самого себя, иначе нашёлся бы короче, поэтому ход остаётся неразветвлённым коридором.
  bool try_secret_way(const uint32_t origin, const port_request& port) {
    const size_t cells = occupied_.size();
    std::vector<int32_t> came_from(cells, -1);
    const auto key = [&](const int32_t x, const int32_t y) { return size_t(y) * size_t(g_.width) + size_t(x); };
    const auto& o = g_.nodes[origin];
    std::deque<std::pair<int32_t, int32_t>> queue{{o.x, o.y}};
    came_from[key(o.x, o.y)] = int32_t(key(o.x, o.y));
    std::array<int, 4> order{0, 1, 2, 3};
    int32_t goal = -1;
    while (!queue.empty() && goal < 0) {
      const auto [cx, cy] = queue.front();
      queue.pop_front();
      rng_.shuffle(order);
      // Первый шаг хода может касаться своего узла выработки, остальные — только хода.
      const uint32_t parent = (cx == o.x && cy == o.y) ? origin : no_node;
      for (const int dir : order) {
        const int32_t nx = cx + dir_x[dir];
        const int32_t ny = cy + dir_y[dir];
        if (!inside(nx, ny) || came_from[key(nx, ny)] >= 0) {
          continue;
        }
        const bool target = port_target(nx, ny, port);
        if (!target && !interior(nx, ny)) {
          continue;
        }
        if (!can_place(nx, ny, parent, target)) {
          continue;
        }
        came_from[key(nx, ny)] = int32_t(key(cx, cy));
        if (target) {
          goal = int32_t(key(nx, ny));
          break;
        }
        queue.emplace_back(nx, ny);
      }
    }
    if (goal < 0) {
      return false;
    }
    std::vector<size_t> path;
    for (size_t c = size_t(goal); c != key(o.x, o.y); c = size_t(came_from[c])) {
      path.push_back(c);
    }
    std::reverse(path.begin(), path.end());
    uint32_t parent = origin;
    for (size_t i = 0; i < path.size(); ++i) {
      const int32_t x = int32_t(path[i] % size_t(g_.width));
      const int32_t y = int32_t(path[i] / size_t(g_.width));
      const bool last = i + 1 == path.size();
      parent = add(last ? k_.secret : k_.passage, parent, x, y, i == 0 ? passage::hidden : passage::open);
    }
    g_.secret_entrance = parent;
    return true;
  }

  // Без допуска — число веток из параметров роста. С допуском — ветки растут, пока общий
  // контент не дойдёт до цели внутри допуска; ни один узел не переводит его за верхнюю границу.
  void grow_branches() {
    const bool by_content = req_.has_allowance;
    const int64_t target = by_content ? rng_.range(req_.content_min, req_.content_max) : 0;
    const int64_t wanted = by_content ? std::numeric_limits<int64_t>::max() :
      rng_.range(arch_.growth.branches_min, arch_.growth.branches_max);
    int64_t failures = 0;
    for (int64_t b = 0; b < wanted && failures < 16; ++b) {
      if (by_content && total_ >= target) {
        break;
      }
      std::vector<uint32_t> origins;
      for (uint32_t i = 0; i < g_.nodes.size(); ++i) {
        const uint32_t kind = g_.nodes[i].kind;
        if (kind == k_.drift || kind == k_.abandoned || kind == k_.camp) {
          origins.push_back(i);
        }
      }
      rng_.shuffle(origins);
      const bool abandoned = rng_.percent(arch_.growth.abandoned_percent);
      const int64_t length = rng_.range(arch_.growth.branch_length_min, arch_.growth.branch_length_max);
      bool grown = false;
      for (size_t tries = 0; tries < std::min<size_t>(origins.size(), 8) && !grown; ++tries) {
        grown = grow_branch(origins[tries], abandoned, length);
      }
      if (grown) {
        ++out_.branches_grown;
      } else {
        ++failures;
      }
    }
  }

  // Узел вида `kind` помещается в допуск.
  bool fits(const uint32_t kind) const noexcept {
    return !req_.has_allowance || total_ + content(kind) <= req_.content_max;
  }

  bool grow_branch(const uint32_t origin, const bool abandoned, const int64_t length) {
    const uint32_t kind = abandoned ? k_.abandoned : k_.drift;
    int dir = int(rng_.range(0, 3));
    uint32_t cur = origin;
    int64_t grown = 0;
    for (; grown < length && fits(kind); ++grown) {
      int32_t x = 0, y = 0;
      if (!step(cur, dir, x, y)) {
        break;
      }
      cur = add(kind, cur, x, y);
    }
    if (grown == 0) {
      return false;
    }
    int32_t x = 0, y = 0;
    if (fits(k_.face) && rng_.percent(arch_.growth.face_percent) && step(cur, dir, x, y)) {
      add(k_.face, cur, x, y);
    }
    return true;
  }

  const family_request& req_;
  const location_input& in_;
  const archetype& arch_;
  family_result& out_;
  location_graph& g_;
  draw rng_;
  kinds_used k_{};
  std::vector<uint32_t> occupied_;
  uint32_t camp_ = no_node;
  int main_dir_ = 1;
  int64_t total_ = 0;
};

} // namespace

family_result build_mine(const family_request& request) {
  family_result out;
  builder(request, out).run();
  return out;
}

} // namespace gn07
