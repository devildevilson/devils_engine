#include "frame.h"

#include <algorithm>
#include <deque>
#include <format>
#include <map>
#include <numeric>
#include <sstream>

#include <devils_engine/utils/hash.h>

namespace gn07 {

namespace {

using namespace devils_engine;

// Поток узла: значение зависит от (seed, узел, номер вызова). Сколько раз тянул соседний узел,
// на этот узел не влияет, поэтому правка одного вида тел не сдвигает весь план.
struct node_draw {
  uint64_t seed = 0;
  uint64_t node = 0;
  uint64_t slot = 0;
  uint64_t next() noexcept { return utils::splitmix(seed, node * 64 + slot++); }
  int32_t range(const int32_t lo, const int32_t hi) noexcept {
    return hi <= lo ? lo : lo + int32_t(next() % uint64_t(int64_t(hi) - lo + 1));
  }
  int64_t range(const int64_t lo, const int64_t hi) noexcept {
    return hi <= lo ? lo : lo + int64_t(next() % uint64_t(hi - lo + 1));
  }
  int32_t jitter(const int32_t amplitude) noexcept { return amplitude <= 0 ? 0 : range(-amplitude, amplitude); }
};

int32_t clamp(const int32_t v, const int32_t lo, const int32_t hi) noexcept {
  return hi < lo ? (lo + hi) / 2 : std::clamp(v, lo, hi);
}

// Отрезок [c - w/2, c - w/2 + w): сечение ширины w вокруг c. Округление одинаковое везде, чтобы
// два полухода одной ширины совпадали до сантиметра.
int32_t lo_of(const int32_t c, const int32_t w) noexcept { return c - w / 2; }
int32_t hi_of(const int32_t c, const int32_t w) noexcept { return c - w / 2 + w; }

struct body_draw {
  int32_t width = 0;            // сечение / устье
  int32_t size_x = 0;           // протяжённость ядра по осям
  int32_t size_y = 0;
  int64_t lobes = 0;
};

class frame_builder {
public:
  frame_builder(const location_graph& g, const archetype& arch, const uint64_t seed, frame_result& out)
    : g_(g), arch_(arch), rules_(arch.frame), seed_(seed), out_(out), f_(out.frame) {
    cell_ = int32_t(g.cell) * plan_scale;
    inset_ = (rules_.wall + 1) / 2;
    f_.width = g.width * cell_;
    f_.height = g.height * cell_;
  }

  void run() {
    const size_t n = g_.nodes.size();
    bodies_.resize(n);
    mouth_.assign(n, std::vector<int32_t>(4, 0));
    edge_width_.assign(n, 0);
    f_.zones.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      draw_body(i);
    }
    for (uint32_t i = 0; i < n; ++i) {
      if (!place(i)) {
        return;
      }
    }
    for (uint32_t i = 0; i < n; ++i) {
      emit_parts(i);
    }
  }

private:
  const body_spec& spec(const uint32_t node) const noexcept { return arch_.kinds[g_.nodes[node].kind].body; }
  bool chamber(const uint32_t node) const noexcept { return spec(node).shape == body_shape::chamber; }

  node_draw stream(const uint32_t node, const uint64_t purpose) const noexcept {
    return node_draw{utils::splitmix(seed_, purpose), node, 0};
  }

  void draw_body(const uint32_t i) {
    node_draw d = stream(i, 0xb0d1);
    const body_spec& b = spec(i);
    body_draw& out = bodies_[i];
    out.width = d.range(b.width_min, b.width_max);
    if (b.shape == body_shape::chamber) {
      out.size_x = d.range(b.size_min, b.size_max);
      out.size_y = d.range(b.size_min, b.size_max);
      out.lobes = d.range(b.lobes_min, b.lobes_max);
    } else {
      out.size_x = out.size_y = out.width;
    }
  }

  // Допустимые положения точки зоны по оси: ядро целиком в клетке с полем в полстены.
  void anchor_range(const uint32_t i, const bool x_axis, int32_t& lo, int32_t& hi) const noexcept {
    const auto& node = g_.nodes[i];
    const int32_t cell_lo = (x_axis ? node.x : node.y) * cell_;
    const int32_t size = x_axis ? bodies_[i].size_x : bodies_[i].size_y;
    lo = cell_lo + inset_ + size / 2;
    hi = cell_lo + cell_ - inset_ - (size - size / 2);
  }

  int32_t& anchor(const uint32_t i, const bool x_axis) noexcept {
    return x_axis ? f_.zones[i].anchor_x : f_.zones[i].anchor_y;
  }

  // Ядро зоны по оси.
  void core_span(const uint32_t i, const bool x_axis, int32_t& lo, int32_t& hi) const noexcept {
    const int32_t a = x_axis ? f_.zones[i].anchor_x : f_.zones[i].anchor_y;
    const int32_t size = x_axis ? bodies_[i].size_x : bodies_[i].size_y;
    lo = lo_of(a, size);
    hi = hi_of(a, size);
  }

  // Направление от p к n: 0 north, 1 east, 2 south, 3 west.
  int direction(const uint32_t p, const uint32_t n) const noexcept {
    const auto& a = g_.nodes[p];
    const auto& b = g_.nodes[n];
    return b.y < a.y ? 0 : b.x > a.x ? 1 : b.y > a.y ? 2 : 3;
  }

  // Устье камеры: где-то на её стороне, целиком в пределах ядра.
  int32_t chamber_mouth(const uint32_t i, const bool lateral_x, const int32_t want, const int32_t w) const noexcept {
    int32_t lo = 0, hi = 0;
    core_span(i, lateral_x, lo, hi);
    return clamp(want, lo + w / 2, hi - w + w / 2);
  }

  bool place(const uint32_t i) {
    node_draw d = stream(i, 0xa7c4);
    const auto& node = g_.nodes[i];
    int32_t lo_x, hi_x, lo_y, hi_y;
    anchor_range(i, true, lo_x, hi_x);
    anchor_range(i, false, lo_y, hi_y);
    if (node.parent == no_node) {
      anchor(i, true) = clamp(node.x * cell_ + cell_ / 2 + d.jitter(rules_.wander), lo_x, hi_x);
      anchor(i, false) = clamp(node.y * cell_ + cell_ / 2 + d.jitter(rules_.wander), lo_y, hi_y);
      return true;
    }
    const uint32_t p = node.parent;
    const int dir = direction(p, i);
    const bool lateral_x = dir == 0 || dir == 2;  // ребро вертикально — сдвиг вбок по x
    const bool hidden = node.entry == passage::hidden;
    const int32_t w = hidden ? d.range(rules_.hidden_min, rules_.hidden_max) :
      std::min(bodies_[p].width, bodies_[i].width);
    edge_width_[i] = w;

    // Устье родителя к этому узлу. Коридор не выбирает: его устье — его ось. Камера ставит
    // устье где-то на стороне, но так, чтобы и сечение узла-коридора за ней поместилось.
    int32_t mp = 0;
    if (chamber(p)) {
      const int32_t room = chamber(i) ? w : std::max(w, bodies_[i].width);
      int32_t clo = 0, chi = 0;
      core_span(p, lateral_x, clo, chi);
      mp = chamber_mouth(p, lateral_x, (clo + chi) / 2 + d.jitter((chi - clo) / 4), room);
    } else {
      mp = anchor(p, lateral_x);
    }
    mouth_[p][size_t(dir)] = mp;

    // Узел уходит вбок от устья родителя: на столько, сколько позволяют блуждание и проём,
    // который должен остаться не уже прохода игрока.
    const int32_t along_lo = lateral_x ? lo_y : lo_x;
    const int32_t along_hi = lateral_x ? hi_y : hi_x;
    const int32_t lat_lo = lateral_x ? lo_x : lo_y;
    const int32_t lat_hi = lateral_x ? hi_x : hi_y;
    const int32_t along_cell = (lateral_x ? node.y : node.x) * cell_ + cell_ / 2;
    anchor(i, !lateral_x) = clamp(along_cell + d.jitter(rules_.wander), along_lo, along_hi);
    int32_t mn = 0;
    if (chamber(i)) {
      const int32_t size = lateral_x ? bodies_[i].size_x : bodies_[i].size_y;
      anchor(i, lateral_x) = clamp(mp + d.jitter(std::max(0, size / 2 - w)), lat_lo, lat_hi);
      mn = chamber_mouth(i, lateral_x, mp, w);
    } else {
      // Открытый проём обязан остаться шире любого скрытого, а не только шире прохода игрока:
      // иначе узкий ход с блужданием сам собой выглядит спрятанным.
      const int32_t narrowest = hidden ? rules_.passage : rules_.hidden_max + 1;
      const int32_t slack = std::max(0, std::min(rules_.wander, w - narrowest));
      anchor(i, lateral_x) = clamp(mp + d.jitter(slack), lat_lo, lat_hi);
      mn = anchor(i, lateral_x);
    }
    mouth_[i][size_t((dir + 2) % 4)] = mn;

    const int32_t opening = w - std::abs(mp - mn);
    const int32_t needed = hidden ? rules_.passage : rules_.hidden_max + 1;
    if (opening < needed) {
      out_.refusal = std::format("frame: the {} opening between nodes {} and {} is {} cm, it needs {} cm",
        to_string(node.entry), p, i, opening, needed);
      return false;
    }
    return true;
  }

  // Полуход от точки зоны до линии границы, сечением w вокруг mouth.
  plan_rect half_way(const uint32_t i, const int dir, const int32_t mouth, const int32_t w) const noexcept {
    const auto& node = g_.nodes[i];
    const int32_t ax = f_.zones[i].anchor_x;
    const int32_t ay = f_.zones[i].anchor_y;
    switch (dir) {
      case 0: return {lo_of(mouth, w), node.y * cell_, hi_of(mouth, w), ay};
      case 1: return {ax, lo_of(mouth, w), (node.x + 1) * cell_, hi_of(mouth, w)};
      case 2: return {lo_of(mouth, w), ay, hi_of(mouth, w), (node.y + 1) * cell_};
      default: return {node.x * cell_, lo_of(mouth, w), ax, hi_of(mouth, w)};
    }
  }

  void emit_parts(const uint32_t i) {
    frame_zone& z = f_.zones[i];
    z.node = i;
    const body_draw& b = bodies_[i];
    const plan_rect core{lo_of(z.anchor_x, b.size_x), lo_of(z.anchor_y, b.size_y), hi_of(z.anchor_x, b.size_x),
      hi_of(z.anchor_y, b.size_y)};
    z.parts.push_back({core, part_role::core, no_node});

    if (chamber(i)) {
      // Наросты: камера разрасталась неровно. Центр нароста — внутри ядра, поэтому нарост с ним
      // пересекается; сдвиг внутрь клетки сохраняет поле в полстены.
      node_draw d = stream(i, 0x10be);
      const auto& node = g_.nodes[i];
      const int32_t box_lo_x = node.x * cell_ + inset_;
      const int32_t box_hi_x = (node.x + 1) * cell_ - inset_;
      const int32_t box_lo_y = node.y * cell_ + inset_;
      const int32_t box_hi_y = (node.y + 1) * cell_ - inset_;
      for (int64_t l = 0; l < b.lobes; ++l) {
        const int32_t lx = d.range(b.size_x / 2, b.size_x * 4 / 5);
        const int32_t ly = d.range(b.size_y / 2, b.size_y * 4 / 5);
        const int32_t cx = z.anchor_x + d.jitter(b.size_x / 2);
        const int32_t cy = z.anchor_y + d.jitter(b.size_y / 2);
        const int32_t x0 = clamp(lo_of(cx, lx), box_lo_x, box_hi_x - lx);
        const int32_t y0 = clamp(lo_of(cy, ly), box_lo_y, box_hi_y - ly);
        const plan_rect lobe{x0, y0, x0 + lx, y0 + ly};
        const bool touches_core = std::min(lobe.x1, core.x1) > std::max(lobe.x0, core.x0) &&
          std::min(lobe.y1, core.y1) > std::max(lobe.y0, core.y0);
        if (touches_core) {
          z.parts.push_back({lobe, part_role::lobe, no_node});
        }
      }
    }

    // Устья ко всем соседям по графу: к родителю и к детям.
    for (const uint32_t other : g_.neighbours[i]) {
      const bool to_parent = g_.nodes[i].parent == other;
      const uint32_t child = to_parent ? i : other;
      const int dir = direction(i, other);
      z.parts.push_back({half_way(i, dir, mouth_[i][size_t(dir)], edge_width_[child]), part_role::mouth, other});
    }

    // Портал входа — до края участка, на стороне, к которой узел прилегает.
    if (i == 0 || i == g_.secret_entrance) {
      const side s = g_.boundary[i];
      const int dir = s == side::north ? 0 : s == side::east ? 1 : s == side::south ? 2 : 3;
      const bool lateral_x = dir == 0 || dir == 2;
      const int32_t w = b.width;
      const int32_t m = chamber(i) ? chamber_mouth(i, lateral_x, anchor(i, lateral_x), w) : anchor(i, lateral_x);
      z.parts.push_back({half_way(i, dir, m, w), part_role::portal, no_node});
    }
  }

  const location_graph& g_;
  const archetype& arch_;
  const frame_rules& rules_;
  uint64_t seed_ = 0;
  frame_result& out_;
  location_frame& f_;
  int32_t cell_ = 0;
  int32_t inset_ = 0;
  std::vector<body_draw> bodies_;
  std::vector<std::vector<int32_t>> mouth_;   // [узел][направление] — поперечная координата устья
  std::vector<int32_t> edge_width_;           // ширина проёма к родителю
};

bool interiors_overlap(const plan_rect& a, const plan_rect& b) noexcept {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

// Касание двух прямоугольников отрезком положительной длины.
struct contact {
  bool vertical = false;
  int32_t line = 0;
  int32_t from = 0;
  int32_t to = 0;
};

bool touch(const plan_rect& a, const plan_rect& b, contact& out) noexcept {
  if (a.x1 == b.x0 || b.x1 == a.x0) {
    const int32_t from = std::max(a.y0, b.y0);
    const int32_t to = std::min(a.y1, b.y1);
    if (to > from) {
      out = {true, a.x1 == b.x0 ? a.x1 : a.x0, from, to};
      return true;
    }
  }
  if (a.y1 == b.y0 || b.y1 == a.y0) {
    const int32_t from = std::max(a.x0, b.x0);
    const int32_t to = std::min(a.x1, b.x1);
    if (to > from) {
      out = {false, a.y1 == b.y0 ? a.y1 : a.y0, from, to};
      return true;
    }
  }
  return false;
}

int64_t distance_sq(const plan_rect& a, const plan_rect& b) noexcept {
  const int64_t gx = std::max<int64_t>({0, int64_t(b.x0) - a.x1, int64_t(a.x0) - b.x1});
  const int64_t gy = std::max<int64_t>({0, int64_t(b.y0) - a.y1, int64_t(a.y0) - b.y1});
  return gx * gx + gy * gy;
}

// Объединение отрезков на одной линии: самый длинный сплошной кусок.
void longest_run(std::vector<std::pair<int32_t, int32_t>>& spans, int32_t& from, int32_t& to) {
  std::sort(spans.begin(), spans.end());
  from = to = 0;
  int32_t cur_from = spans.front().first;
  int32_t cur_to = spans.front().second;
  const auto take = [&] {
    if (cur_to - cur_from > to - from) {
      from = cur_from;
      to = cur_to;
    }
  };
  for (size_t i = 1; i < spans.size(); ++i) {
    if (spans[i].first <= cur_to) {
      cur_to = std::max(cur_to, spans[i].second);
    } else {
      take();
      cur_from = spans[i].first;
      cur_to = spans[i].second;
    }
  }
  take();
}

int64_t union_area(const std::vector<frame_part>& parts) {
  std::vector<int32_t> xs, ys;
  for (const auto& p : parts) {
    xs.push_back(p.box.x0);
    xs.push_back(p.box.x1);
    ys.push_back(p.box.y0);
    ys.push_back(p.box.y1);
  }
  std::sort(xs.begin(), xs.end());
  xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
  std::sort(ys.begin(), ys.end());
  ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
  int64_t area = 0;
  for (size_t i = 0; i + 1 < xs.size(); ++i) {
    for (size_t j = 0; j + 1 < ys.size(); ++j) {
      for (const auto& p : parts) {
        if (p.box.x0 <= xs[i] && xs[i + 1] <= p.box.x1 && p.box.y0 <= ys[j] && ys[j + 1] <= p.box.y1) {
          area += int64_t(xs[i + 1] - xs[i]) * (ys[j + 1] - ys[j]);
          break;
        }
      }
    }
  }
  return area;
}

} // namespace

frame_result build_frame(const location_graph& graph, const archetype& arch, const uint64_t seed) {
  frame_result out;
  frame_builder(graph, arch, seed, out).run();
  return out;
}

frame_analysis analyse_frame(const location_frame& frame) {
  frame_analysis out;
  const auto problem = [&](std::string text) { out.problems.push_back(std::move(text)); };

  struct flat_part {
    plan_rect box;
    uint32_t zone;
  };
  std::vector<flat_part> parts;
  for (uint32_t z = 0; z < frame.zones.size(); ++z) {
    if (frame.zones[z].parts.empty()) {
      problem(std::format("zone {} has no parts", z));
    }
    for (const auto& p : frame.zones[z].parts) {
      const plan_rect& b = p.box;
      if (b.x0 >= b.x1 || b.y0 >= b.y1) {
        problem(std::format("zone {} has an empty part", z));
      }
      if (b.x0 < 0 || b.y0 < 0 || b.x1 > frame.width || b.y1 > frame.height) {
        problem(std::format("zone {} has a part outside the plot", z));
      }
      parts.push_back({b, z});
    }
  }
  out.parts = parts.size();

  // Связность внутри зоны и стыки между зонами — одним проходом по парам частей.
  std::vector<uint32_t> group(parts.size());
  std::iota(group.begin(), group.end(), 0u);
  const auto find = [&](uint32_t i) {
    while (group[i] != i) {
      i = group[i] = group[group[i]];
    }
    return i;
  };
  struct line_key {
    uint32_t a, b;
    bool vertical;
    int32_t line;
    auto operator<=>(const line_key&) const = default;
  };
  std::map<line_key, std::vector<std::pair<int32_t, int32_t>>> lines;
  std::map<std::pair<uint32_t, uint32_t>, int64_t> nearest;   // несвязанные пары — квадрат расстояния
  for (size_t i = 0; i < parts.size(); ++i) {
    for (size_t j = i + 1; j < parts.size(); ++j) {
      const auto& a = parts[i];
      const auto& b = parts[j];
      contact c;
      const bool overlap = interiors_overlap(a.box, b.box);
      const bool touching = !overlap && touch(a.box, b.box, c);
      if (a.zone == b.zone) {
        if (overlap || touching) {
          group[find(uint32_t(i))] = find(uint32_t(j));
        }
        continue;
      }
      const uint32_t za = std::min(a.zone, b.zone);
      const uint32_t zb = std::max(a.zone, b.zone);
      if (overlap) {
        problem(std::format("zones {} and {} overlap", za, zb));
        continue;
      }
      if (touching) {
        lines[{za, zb, c.vertical, c.line}].emplace_back(c.from, c.to);
        continue;
      }
      const int64_t d = distance_sq(a.box, b.box);
      auto [it, fresh] = nearest.try_emplace({za, zb}, d);
      if (!fresh) {
        it->second = std::min(it->second, d);
      }
    }
  }
  std::vector<uint8_t> split(frame.zones.size(), 0);
  std::vector<uint32_t> root(frame.zones.size(), no_node);
  for (size_t i = 0; i < parts.size(); ++i) {
    const uint32_t z = parts[i].zone;
    const uint32_t r = find(uint32_t(i));
    if (root[z] == no_node) {
      root[z] = r;
    } else if (root[z] != r) {
      split[z] = 1;
    }
  }
  for (uint32_t z = 0; z < frame.zones.size(); ++z) {
    if (split[z] != 0) {
      problem(std::format("zone {} falls into separate pieces", z));
    }
  }

  for (auto& [key, spans] : lines) {
    frame_join j{key.a, key.b, 0, key.vertical, key.line, 0};
    int32_t from = 0, to = 0;
    longest_run(spans, from, to);
    j.width = to - from;
    j.mid = from + (to - from) / 2;
    if (!out.joins.empty() && out.joins.back().a == j.a && out.joins.back().b == j.b) {
      if (j.width > out.joins.back().width) {
        out.joins.back() = j;
      }
      continue;
    }
    out.joins.push_back(j);
  }
  for (const auto& [pair, d] : nearest) {
    const bool joined = std::any_of(out.joins.begin(), out.joins.end(), [&](const frame_join& j) {
      return j.a == pair.first && j.b == pair.second;
    });
    if (joined) {
      continue;   // у связанных зон части рядом — законно
    }
    out.gaps.push_back({pair.first, pair.second, d});
    if (out.thinnest_wall_sq < 0 || d < out.thinnest_wall_sq) {
      out.thinnest_wall_sq = d;
      out.wall_a = pair.first;
      out.wall_b = pair.second;
    }
  }

  // Проёмы на краю участка.
  std::map<std::pair<uint32_t, side>, std::vector<std::pair<int32_t, int32_t>>> edges;
  for (const auto& p : parts) {
    const plan_rect& b = p.box;
    if (b.y0 == 0) edges[{p.zone, side::north}].emplace_back(b.x0, b.x1);
    if (b.x1 == frame.width) edges[{p.zone, side::east}].emplace_back(b.y0, b.y1);
    if (b.y1 == frame.height) edges[{p.zone, side::south}].emplace_back(b.x0, b.x1);
    if (b.x0 == 0) edges[{p.zone, side::west}].emplace_back(b.y0, b.y1);
  }
  for (auto& [key, spans] : edges) {
    int32_t from = 0, to = 0;
    longest_run(spans, from, to);
    out.openings.push_back({key.first, key.second, from, to});
  }

  out.zone_area.reserve(frame.zones.size());
  for (const auto& z : frame.zones) {
    out.zone_area.push_back(z.parts.empty() ? 0 : union_area(z.parts));
  }
  return out;
}

std::vector<std::string> verify_frame(const location_frame& frame, const location_graph& g, const archetype& arch,
                                      const location_input& input) {
  std::vector<std::string> problems;
  const auto problem = [&](std::string text) { problems.push_back("frame: " + std::move(text)); };
  const frame_rules& rules = arch.frame;
  const int32_t cell = int32_t(g.cell) * plan_scale;

  if (frame.zones.size() != g.nodes.size()) {
    problem(std::format("{} zones for {} graph nodes", frame.zones.size(), g.nodes.size()));
    return problems;
  }
  if (frame.width != g.width * cell || frame.height != g.height * cell) {
    problem(std::format("plot {}x{} cm differs from the lattice {}x{} of {} cm", frame.width, frame.height, g.width,
      g.height, cell));
  }
  const frame_analysis a = analyse_frame(frame);
  for (const auto& p : a.problems) {
    problem(p);
  }

  // Стыки плана против рёбер графа: каждое ребро — проём не уже прохода, и других стыков нет.
  // Скрытость прохода выражена шириной: скрытый — в своём коридоре, открытый — шире него.
  std::map<std::pair<uint32_t, uint32_t>, const frame_join*> joins;
  for (const auto& j : a.joins) {
    joins[{j.a, j.b}] = &j;
  }
  size_t edges = 0;
  for (uint32_t i = 0; i < g.nodes.size(); ++i) {
    const uint32_t p = g.nodes[i].parent;
    if (p == no_node) {
      continue;
    }
    ++edges;
    const auto it = joins.find({std::min(p, i), std::max(p, i)});
    if (it == joins.end()) {
      problem(std::format("graph edge {}-{} has no opening in the plan", p, i));
      continue;
    }
    const int32_t w = it->second->width;
    if (w < rules.passage) {
      problem(std::format("opening {}-{} is {} cm, narrower than the passage of {} cm", p, i, w, rules.passage));
    }
    const bool hidden = g.nodes[i].entry == passage::hidden;
    if (hidden && w > rules.hidden_max) {
      problem(std::format("hidden passage {}-{} is {} cm wide, an obvious opening", p, i, w));
    }
    if (!hidden && w <= rules.hidden_max) {
      problem(std::format("open passage {}-{} is only {} cm, as narrow as a hidden one", p, i, w));
    }
  }
  for (const auto& j : a.joins) {
    if (j.b >= g.nodes.size() || (g.nodes[j.b].parent != j.a && g.nodes[j.a].parent != j.b)) {
      problem(std::format("zones {} and {} touch, but the graph has no edge between them", j.a, j.b));
    }
  }
  (void)edges;
  for (const auto& gap : a.gaps) {
    if (gap.distance_sq < int64_t(rules.wall) * rules.wall) {
      problem(std::format("rock between unrelated zones {} and {} is thinner than the wall of {} cm", gap.a, gap.b,
        rules.wall));
    }
  }

  // Достижимость по стыкам плана, независимо от графа.
  std::vector<std::vector<uint32_t>> adjacent(frame.zones.size());
  for (const auto& j : a.joins) {
    adjacent[j.a].push_back(j.b);
    adjacent[j.b].push_back(j.a);
  }
  std::vector<uint8_t> seen(frame.zones.size(), 0);
  std::deque<uint32_t> queue{0};
  seen[0] = 1;
  size_t reached = 1;
  while (!queue.empty()) {
    const uint32_t z = queue.front();
    queue.pop_front();
    for (const uint32_t n : adjacent[z]) {
      if (seen[n] == 0) {
        seen[n] = 1;
        ++reached;
        queue.push_back(n);
      }
    }
  }
  if (reached != frame.zones.size()) {
    problem(std::format("only {} of {} zones are reachable from the entrance through the plan", reached,
      frame.zones.size()));
  }

  // Проёмы на краю участка: только у входов, по одному, там, куда смотрит сосед.
  std::map<uint32_t, std::vector<const frame_opening*>> by_zone;
  for (const auto& o : a.openings) {
    by_zone[o.zone].push_back(&o);
  }
  for (const auto& [zone, list] : by_zone) {
    if (zone != 0 && zone != g.secret_entrance) {
      problem(std::format("zone {} opens onto the edge of the plot, but it is not an entrance", zone));
    }
    if (list.size() != 1) {
      problem(std::format("entrance zone {} has {} openings onto the edge", zone, list.size()));
    }
  }
  for (const auto& port : input.ports) {
    const uint32_t zone = port.kind == link_kind::ordinary ? 0 : g.secret_entrance;
    const auto it = by_zone.find(zone);
    if (zone == no_node || it == by_zone.end() || it->second.size() != 1) {
      problem(std::format("link to '{}': its entrance has no single opening onto the edge", port.neighbour));
      continue;
    }
    const frame_opening& o = *it->second.front();
    const int32_t window = port.kind == link_kind::ordinary ? 0 :
      port_window((port.at == side::west || port.at == side::east) ? g.height : g.width);
    const int32_t from = (port.along - window) * cell;
    const int32_t to = (port.along + window + 1) * cell;
    if (o.at != port.at || o.from < from || o.to > to) {
      problem(std::format("link to '{}': opening on the {} side at {}..{} cm, the neighbour faces the {} side at {}..{} cm",
        port.neighbour, to_string(o.at), o.from, o.to, to_string(port.at), from, to));
    }
    if (o.to - o.from < rules.passage) {
      problem(std::format("link to '{}': opening of {} cm is narrower than the passage", port.neighbour, o.to - o.from));
    }
  }
  return problems;
}

std::vector<int64_t> frame_walk(const location_frame& frame, const frame_analysis& analysis,
                                const location_graph& graph) {
  std::vector<int64_t> walk(frame.zones.size(), -1);
  if (frame.zones.empty()) {
    return walk;
  }
  std::map<std::pair<uint32_t, uint32_t>, const frame_join*> joins;
  for (const auto& j : analysis.joins) {
    joins[{j.a, j.b}] = &j;
  }
  // Начало — середина проёма обычного входа на краю участка.
  const frame_zone& entry = frame.zones[0];
  int64_t start = 0;
  for (const auto& o : analysis.openings) {
    if (o.zone != 0) {
      continue;
    }
    const int32_t mid = o.from + (o.to - o.from) / 2;
    const bool vertical_edge = o.at == side::west || o.at == side::east;
    const int32_t edge = o.at == side::west ? 0 : o.at == side::east ? frame.width : o.at == side::north ? 0 : frame.height;
    start = vertical_edge ? std::abs(entry.anchor_x - edge) + std::abs(entry.anchor_y - mid) :
      std::abs(entry.anchor_y - edge) + std::abs(entry.anchor_x - mid);
  }
  walk[0] = start;
  for (uint32_t i = 1; i < graph.nodes.size(); ++i) {
    const uint32_t p = graph.nodes[i].parent;
    const auto it = joins.find({std::min(p, i), std::max(p, i)});
    if (p == no_node || walk[p] < 0 || it == joins.end()) {
      continue;
    }
    const frame_join& j = *it->second;
    const int32_t jx = j.vertical ? j.line : j.mid;
    const int32_t jy = j.vertical ? j.mid : j.line;
    const auto leg = [&](const frame_zone& z) { return int64_t(std::abs(z.anchor_x - jx)) + std::abs(z.anchor_y - jy); };
    walk[i] = walk[p] + leg(frame.zones[p]) + leg(frame.zones[i]);
  }
  return walk;
}

std::string metres(const int64_t cm) {
  const int64_t a = cm < 0 ? -cm : cm;
  return std::format("{}{}.{:02}", cm < 0 ? "-" : "", a / plan_scale, a % plan_scale);
}

std::string format_svg(const location_frame& frame, const location_graph& graph, const archetype& arch,
                       const std::vector<frame_mark>& marks) {
  std::ostringstream out;
  const int32_t margin = 400;
  const int32_t cell = int32_t(graph.cell) * plan_scale;
  out << std::format("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"{} {} {} {}\" width=\"{}\" height=\"{}\">\n",
    -margin, -margin, frame.width + 2 * margin, frame.height + 2 * margin, (frame.width + 2 * margin) / 5,
    (frame.height + 2 * margin) / 5);
  out << std::format("<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" fill=\"#2b2622\"/>\n", -margin, -margin,
    frame.width + 2 * margin, frame.height + 2 * margin);
  out << std::format("<rect x=\"0\" y=\"0\" width=\"{}\" height=\"{}\" fill=\"#3a332d\" stroke=\"#6a6058\" "
                     "stroke-width=\"20\"/>\n", frame.width, frame.height);
  for (int32_t x = cell; x < frame.width; x += cell) {
    out << std::format("<line x1=\"{0}\" y1=\"0\" x2=\"{0}\" y2=\"{1}\" stroke=\"#453d36\" stroke-width=\"6\"/>\n", x,
      frame.height);
  }
  for (int32_t y = cell; y < frame.height; y += cell) {
    out << std::format("<line x1=\"0\" y1=\"{0}\" x2=\"{1}\" y2=\"{0}\" stroke=\"#453d36\" stroke-width=\"6\"/>\n", y,
      frame.width);
  }
  // Цвет — по функции вида; заброшенное темнее.
  const auto colour = [&](const uint32_t node) -> std::string_view {
    const node_kind& k = arch.kinds[graph.nodes[node].kind];
    if (std::find(k.tags.begin(), k.tags.end(), "abandoned") != k.tags.end()) return "#7a6a58";
    if (k.function == "living") return "#d9a05b";
    if (k.function == "extraction") return "#c0705a";
    if (k.function == "natural") return "#6d8aa0";
    if (k.function == "access") return "#d8cf8a";
    return "#a8a198";
  };
  for (uint32_t z = 0; z < frame.zones.size(); ++z) {
    for (const auto& p : frame.zones[z].parts) {
      const plan_rect& b = p.box;
      out << std::format("<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" fill=\"{}\"/>\n", b.x0, b.y0, b.x1 - b.x0,
        b.y1 - b.y0, colour(z));
    }
  }
  // Скрытый проход — красная черта поперёк проёма.
  for (uint32_t i = 1; i < graph.nodes.size(); ++i) {
    if (graph.nodes[i].entry != passage::hidden) {
      continue;
    }
    for (const auto& p : frame.zones[i].parts) {
      if (p.role != part_role::mouth || p.toward != graph.nodes[i].parent) {
        continue;
      }
      const plan_rect& b = p.box;
      out << std::format("<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" fill=\"none\" stroke=\"#e04040\" "
                         "stroke-width=\"30\" stroke-dasharray=\"60 40\"/>\n", b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
    }
  }
  // Два размещения вправе занять один узел — их подписи идут столбиком.
  std::map<uint32_t, int32_t> stacked;
  for (const auto& m : marks) {
    const frame_zone& z = frame.zones[m.node];
    const int32_t row = stacked[m.node]++;
    if (row == 0) {
      out << std::format("<circle cx=\"{}\" cy=\"{}\" r=\"110\" fill=\"#f4f0e6\" stroke=\"#202020\" "
                         "stroke-width=\"20\"/>\n", z.anchor_x, z.anchor_y);
    }
    out << std::format("<text x=\"{}\" y=\"{}\" font-family=\"sans-serif\" font-size=\"260\" fill=\"#f4f0e6\">{}</text>\n",
      z.anchor_x + 160, z.anchor_y - 140 - row * 300, m.label);
  }
  out << "</svg>\n";
  return out.str();
}

} // namespace gn07
