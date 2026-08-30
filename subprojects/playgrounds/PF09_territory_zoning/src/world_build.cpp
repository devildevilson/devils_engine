#include "world_build.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <vector>

#include "devils_engine/utils/hash.h"

namespace devils_engine::pf09 {

namespace {

constexpr float edge_epsilon = 0.05f; // короче этого общий отрезок проходом не считается
constexpr double wall_inset_m = 1.2;  // насколько стены здания отступают от границы участка
constexpr double door_width_m = 2.4;

// Черновая зона до раздачи ключей. Списка соседей здесь НЕТ: связность выводится из геометрии позже, и
// это главное отличие новой модели — проход это общее ребро, а не строка в списке.
struct draft {
  std::vector<glm::vec2> outline;
  float low = 0.0f;
  float high = 0.0f;
  zone_level level = zone_level::interior;
  zone_kind kind = zone_kind::room;
  std::string name;
  uint64_t identity = 0;
  uint64_t parent_identity = 0;
  std::vector<uint64_t> graph_links; // явные рёбра для зон без формы
  int32_t sector_x = 0;
  int32_t sector_y = 0;
};

std::vector<glm::vec2> rectangle(const double x0, const double y0, const double x1, const double y1) {
  return {{float(x0), float(y0)}, {float(x1), float(y0)}, {float(x1), float(y1)}, {float(x0), float(y1)}};
}

zone_bounds bounds_of(const std::vector<glm::vec2>& outline, const float low, const float high) {
  zone_bounds out{{1e30f, low, 1e30f}, {-1e30f, high, -1e30f}};
  for (const auto& point : outline) {
    out.lower.x = std::min(out.lower.x, point.x);
    out.lower.z = std::min(out.lower.z, point.y);
    out.upper.x = std::max(out.upper.x, point.x);
    out.upper.z = std::max(out.upper.z, point.y);
  }
  return out;
}

uint64_t identity_of(const uint32_t domain, const uint64_t a, const uint64_t b = 0) {
  return (uint64_t(domain) << 56) | ((a & 0xffffffffull) << 24) | (b & 0xffffffull);
}

// Ребро, приведённое к своей прямой. Прямая — ключ корзины: делить отрезок могут только рёбра на одной
// прямой, и сравнивать имеет смысл лишь внутри корзины. Дальше остаётся одномерная задача о пересечении
// интервалов, а не перебор всех пар зон.
struct edge_ref {
  uint32_t zone = 0;
  float begin = 0.0f;
  float end = 0.0f;
};

int64_t quantise(const float value) { return int64_t(std::llround(double(value) * 64.0)); }

} // namespace

build_stats build_world(const territory& map, const locality_config& local, const build_options& options) {
  const auto begin_time = std::chrono::steady_clock::now();

  const double low_x = double(options.sector_x) * sector_span_m;
  const double low_y = double(options.sector_y) * sector_span_m;
  const double high_x = low_x + double(options.sector_side) * sector_span_m;
  const double high_y = low_y + double(options.sector_side) * sector_span_m;

  const auto inside = [&](const glm::dvec2& point) {
    return point.x >= low_x && point.x < high_x && point.y >= low_y && point.y < high_y;
  };

  std::vector<draft> drafts;

  // --- крупные уровни: узлы графа без формы ---
  //
  // На политическом уровне достаточно графа между владениями. Форма там всё равно не квадратная, а на
  // вопрос «кому принадлежит эта земля» отвечает разбиение из `territory`, которое стоит 244 КБ на весь
  // мир. Поэтому зона здесь АБСТРАКТНАЯ: узел без многоугольника, связанный явными рёбрами.
  struct placed_node {
    zone_id node = invalid_zone;
    glm::dvec2 centre{};
  };

  const auto gather = [&](const tier value) {
    std::vector<placed_node> out;
    for (uint64_t index = 0; index < map.cell_count(value); ++index) {
      const auto id = make_zone(value, uint32_t(index));
      const auto* node = map.find_authored(id);
      if (node == nullptr || node->id != id) continue;

      const auto centre = map.node_centre_m(id);
      if (!inside(centre)) continue;
      out.push_back({id, centre});
    }
    return out;
  };

  const auto baronies = gather(tier::barony);
  const auto districts = gather(tier::district);

  const auto emit_graph = [&](const std::vector<placed_node>& nodes, const double reach, const zone_level level,
                              const zone_kind kind, const uint32_t domain, const auto& parent_identity) {
    for (const auto& item : nodes) {
      draft entry{};
      entry.level = level;
      entry.kind = kind;
      entry.identity = identity_of(domain, item.node);
      entry.parent_identity = parent_identity(item.node);
      entry.name = std::format("{} {}", zone_kind_name(kind), index_of(item.node));
      entry.sector_x = sector_of(item.centre.x);
      entry.sector_y = sector_of(item.centre.y);

      for (const auto& other : nodes) {
        if (other.node == item.node) continue;
        const double dx = other.centre.x - item.centre.x;
        const double dy = other.centre.y - item.centre.y;
        if (dx * dx + dy * dy > reach * reach) continue;
        entry.graph_links.push_back(identity_of(domain, other.node));
      }
      drafts.push_back(std::move(entry));
    }
  };

  emit_graph(baronies, map.tier_span_m(tier::barony) * 1.6, zone_level::political, zone_kind::holding, 1,
             [](const zone_id) { return uint64_t(0); });
  emit_graph(districts, map.tier_span_m(tier::district) * 1.6, zone_level::regional, zone_kind::market, 2,
             [&](const zone_id node) { return identity_of(1, map.ancestor_at(node, tier::barony)); });

  // --- поселения: фигура на карте, дороги между ними — рёбра без геометрии ---

  locality_field field(map, local);
  std::vector<placed_node> settlements;

  const double locale_span = map.tier_span_m(tier::locale);
  for (int64_t y = int64_t(low_y / locale_span) - 1; y <= int64_t(high_y / locale_span) + 1; ++y) {
    for (int64_t x = int64_t(low_x / locale_span) - 1; x <= int64_t(high_x / locale_span) + 1; ++x) {
      const glm::dvec2 probe{(double(x) + 0.5) * locale_span, (double(y) + 0.5) * locale_span};
      const auto node = map.resolve(probe, tier::locale);
      if (field.placed_at(node) == locality_kind::none) continue;

      const auto centre = map.node_centre_m(node);
      if (!inside(centre)) continue;

      // Два поселения не могут занимать одну землю. Порядок обхода детерминирован, поэтому и отбор тоже.
      const bool collides = std::any_of(settlements.begin(), settlements.end(), [&](const placed_node& item) {
        return item.node == node || (std::abs(item.centre.x - centre.x) < local.extent_m &&
                                     std::abs(item.centre.y - centre.y) < local.extent_m);
      });
      if (collides) continue;
      settlements.push_back({node, centre});
    }
  }

  const auto emit_shape = [&](draft entry) {
    const auto box = bounds_of(entry.outline, entry.low, entry.high);
    entry.sector_x = sector_of(double(box.lower.x + box.upper.x) * 0.5);
    entry.sector_y = sector_of(double(box.lower.z + box.upper.z) * 0.5);
    drafts.push_back(std::move(entry));
  };

  for (const auto& item : settlements) {
    const double half = local.extent_m * 0.5;
    draft entry{};
    entry.outline = rectangle(item.centre.x - half, item.centre.y - half, item.centre.x + half, item.centre.y + half);
    entry.high = 40.0f;
    entry.level = zone_level::local;
    entry.kind = zone_kind::settlement;
    entry.identity = identity_of(3, item.node);
    entry.parent_identity = identity_of(2, map.ancestor_at(item.node, tier::district));
    entry.name = std::format("settlement {}", index_of(item.node));

    for (const auto& other : settlements) {
      if (other.node == item.node) continue;
      const double dx = other.centre.x - item.centre.x;
      const double dy = other.centre.y - item.centre.y;
      if (dx * dx + dy * dy > 12000.0 * 12000.0) continue;
      entry.graph_links.push_back(identity_of(3, other.node));
    }
    emit_shape(std::move(entry));
  }

  // --- внутренний уровень: улицы, дворы, комнаты и двери ---
  //
  // Здание САМО зоной-фигурой не является: оно абстрактный узел, а форму имеют комнаты внутри него. Так
  // снимается прежний вопрос «здание и комната накладываются»: они и не должны, потому что здание — это
  // не место, а группа мест. Дверь — отдельная маленькая фигура, чьи рёбра лежат и на комнате, и на
  // улице; она и делает вход входом, ровно как проём в navmesh.

  const double plot_cell = local.extent_m / double(local.plot_side);
  const double room_cell = (plot_cell - 2.0 * wall_inset_m) / double(local.room_side);
  const uint32_t rooms_per_plot = local.room_side * local.room_side;
  const uint32_t stride = rooms_per_plot + 1;

  for (const auto& item : settlements) {
    locality place(map, local, item.node, field.placed_at(item.node));
    const double half = local.extent_m * 0.5;
    const auto settlement_identity = identity_of(3, item.node);

    for (uint32_t plot = 0; plot < local.plot_side * local.plot_side; ++plot) {
      const uint32_t zone = plot * stride;
      const auto role = place.role(zone);
      const uint32_t px = plot % local.plot_side;
      const uint32_t py = plot / local.plot_side;
      const double x0 = item.centre.x - half + double(px) * plot_cell;
      const double y0 = item.centre.y - half + double(py) * plot_cell;

      if (role == zone_role::street || role == zone_role::yard) {
        draft entry{};
        entry.outline = rectangle(x0, y0, x0 + plot_cell, y0 + plot_cell);
        entry.high = 0.2f;
        entry.kind = role == zone_role::street ? zone_kind::street : zone_kind::yard;
        entry.identity = identity_of(4, item.node, zone);
        entry.parent_identity = settlement_identity;
        entry.name = std::format("{} {},{}", zone_kind_name(entry.kind), px, py);
        emit_shape(std::move(entry));
        continue;
      }
      if (role != zone_role::building) continue;

      draft shell{};
      shell.kind = zone_kind::building;
      shell.identity = identity_of(4, item.node, zone);
      shell.parent_identity = settlement_identity;
      shell.name = std::format("building {},{}", px, py);
      shell.sector_x = sector_of(x0);
      shell.sector_y = sector_of(y0);
      drafts.push_back(std::move(shell));

      for (uint32_t room = 0; room < rooms_per_plot; ++room) {
        const uint32_t rx = room % local.room_side;
        const uint32_t ry = room / local.room_side;
        const double rx0 = x0 + wall_inset_m + double(rx) * room_cell;
        const double ry0 = y0 + wall_inset_m + double(ry) * room_cell;

        draft entry{};
        entry.outline = rectangle(rx0, ry0, rx0 + room_cell, ry0 + room_cell);
        entry.high = 3.0f;
        entry.kind = zone_kind::room;
        entry.identity = identity_of(4, item.node, zone * 64 + room + 1);
        entry.parent_identity = identity_of(4, item.node, zone);
        entry.name = std::format("room {} of {},{}", room, px, py);
        emit_shape(std::move(entry));
      }

      // Дверь ставится с КАЖДОЙ стороны, где снаружи улица или двор. Одной двери на здание мало, и это
      // выяснилось проверкой достижимости: замкнутый внутри квартала двор соприкасается только со
      // зданиями, у которых форма есть лишь у комнат внутри. Если у такого здания единственная дверь
      // выходит во двор, то двор и его комнаты образуют остров. Второй выход на улицу — это не украшение,
      // а то, что делает квартал проходимым.
      constexpr int32_t offsets[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
      for (uint32_t d = 0; d < 4; ++d) {
        const int64_t nx = int64_t(px) + offsets[d][0];
        const int64_t ny = int64_t(py) + offsets[d][1];
        if (nx < 0 || ny < 0 || nx >= int64_t(local.plot_side) || ny >= int64_t(local.plot_side)) continue;

        const uint32_t neighbour = uint32_t(ny) * local.plot_side + uint32_t(nx);
        const auto neighbour_role = place.role(neighbour * stride);
        if (neighbour_role != zone_role::street && neighbour_role != zone_role::yard) continue;

        const double cx = x0 + plot_cell * 0.5;
        const double cy = y0 + plot_cell * 0.5;
        draft entry{};
        if (offsets[d][0] == 0) {
          const double outer = offsets[d][1] < 0 ? y0 : y0 + plot_cell;
          const double inner = offsets[d][1] < 0 ? y0 + wall_inset_m : y0 + plot_cell - wall_inset_m;
          entry.outline = rectangle(cx - door_width_m * 0.5, std::min(outer, inner), cx + door_width_m * 0.5,
                                    std::max(outer, inner));
        } else {
          const double outer = offsets[d][0] < 0 ? x0 : x0 + plot_cell;
          const double inner = offsets[d][0] < 0 ? x0 + wall_inset_m : x0 + plot_cell - wall_inset_m;
          entry.outline = rectangle(std::min(outer, inner), cy - door_width_m * 0.5, std::max(outer, inner),
                                    cy + door_width_m * 0.5);
        }
        entry.high = 3.0f;
        entry.kind = zone_kind::landmark; // дверной проём как отдельное маленькое место
        entry.identity = identity_of(4, item.node, zone * 64 + 63 - d);
        entry.parent_identity = identity_of(4, item.node, zone);
        entry.name = std::format("door of {},{} side {}", px, py, d);
        emit_shape(std::move(entry));
      }
    }
  }

  // --- связность выводится из геометрии ---

  std::map<int64_t, std::vector<edge_ref>> vertical;
  std::map<int64_t, std::vector<edge_ref>> horizontal;

  for (uint32_t index = 0; index < drafts.size(); ++index) {
    const auto& outline = drafts[index].outline;
    for (size_t i = 0; i < outline.size(); ++i) {
      const auto& a = outline[i];
      const auto& b = outline[(i + 1) % outline.size()];

      if (std::abs(a.x - b.x) < 1.0e-4f) {
        vertical[quantise(a.x)].push_back({index, std::min(a.y, b.y), std::max(a.y, b.y)});
      } else if (std::abs(a.y - b.y) < 1.0e-4f) {
        horizontal[quantise(a.y)].push_back({index, std::min(a.x, b.x), std::max(a.x, b.x)});
      }
    }
  }

  struct raw_portal {
    uint32_t a = 0;
    uint32_t b = 0;
    glm::vec2 from{};
    glm::vec2 to{};
  };
  std::vector<raw_portal> raw;

  const auto sweep = [&](const std::map<int64_t, std::vector<edge_ref>>& buckets, const bool vertical_axis) {
    for (const auto& [key, list] : buckets) {
      const float line = float(double(key) / 64.0);
      for (size_t i = 0; i < list.size(); ++i) {
        for (size_t j = i + 1; j < list.size(); ++j) {
          if (list[i].zone == list[j].zone) continue;

          const float lower = std::max(list[i].begin, list[j].begin);
          const float upper = std::min(list[i].end, list[j].end);
          if (upper - lower < edge_epsilon) continue;

          // Высоты обязаны пересекаться: две комнаты на разных этажах делят проекцию, но не проход.
          const auto& first = drafts[list[i].zone];
          const auto& second = drafts[list[j].zone];
          if (first.high <= second.low || second.high <= first.low) continue;

          raw.push_back({list[i].zone, list[j].zone,
                         vertical_axis ? glm::vec2{line, lower} : glm::vec2{lower, line},
                         vertical_axis ? glm::vec2{line, upper} : glm::vec2{upper, line}});
        }
      }
    }
  };
  sweep(vertical, true);
  sweep(horizontal, false);

  // --- раздача ключей и запись ---

  std::vector<std::vector<uint32_t>> per_sector(size_t(options.sector_side) * options.sector_side);
  const auto slot_of = [&](const draft& entry) -> int64_t {
    const int64_t sx = entry.sector_x - options.sector_x;
    const int64_t sy = entry.sector_y - options.sector_y;
    if (sx < 0 || sy < 0 || sx >= int64_t(options.sector_side) || sy >= int64_t(options.sector_side)) return -1;
    return sy * int64_t(options.sector_side) + sx;
  };

  for (uint32_t index = 0; index < drafts.size(); ++index) {
    const auto slot = slot_of(drafts[index]);
    if (slot >= 0) per_sector[size_t(slot)].push_back(index);
  }

  std::vector<zone_key> keys(drafts.size(), invalid_key);
  std::map<uint64_t, zone_key> by_identity;
  for (uint32_t y = 0; y < options.sector_side; ++y) {
    for (uint32_t x = 0; x < options.sector_side; ++x) {
      const auto& list = per_sector[size_t(y) * options.sector_side + x];
      for (uint32_t local_index = 0; local_index < list.size(); ++local_index) {
        const auto key = make_key(options.sector_x + int32_t(x), options.sector_y + int32_t(y), local_index);
        keys[list[local_index]] = key;
        by_identity[drafts[list[local_index]].identity] = key;
      }
    }
  }

  // Запираются только те проходы, без которых граф остаётся связным: сначала остовное дерево, потом замки
  // на лишних рёбрах. Иначе запертая дверь могла бы отрезать кусок города, и проверка достижимости падала
  // бы на случайных сидах вместо настоящих ошибок. Считается это ПОСЛЕ раздачи ключей, потому что остов
  // должен строиться ровно на тех рёбрах, которые попадут в файлы.
  std::vector<uint32_t> parent(drafts.size());
  for (uint32_t i = 0; i < parent.size(); ++i) {
    parent[i] = i;
  }
  const auto root = [&](uint32_t node) {
    while (parent[node] != node) {
      parent[node] = parent[parent[node]];
      node = parent[node];
    }
    return node;
  };

  std::vector<uint32_t> flags(raw.size(), uint32_t(portal_flags::open));
  for (uint32_t i = 0; i < raw.size(); ++i) {
    // Рёбра к зонам за краем собранной области в остове НЕ участвуют. Раньше участвовали, и это была
    // настоящая ошибка: такое ребро считалось деревом, потом выбрасывалось при записи, и город оставался
    // разрезанным — а замки при этом ставились так, будто он связен.
    if (keys[raw[i].a] == invalid_key || keys[raw[i].b] == invalid_key) continue;

    if (drafts[raw[i].a].kind == zone_kind::landmark || drafts[raw[i].b].kind == zone_kind::landmark) {
      flags[i] |= uint32_t(portal_flags::door);
    }

    const uint32_t ra = root(raw[i].a);
    const uint32_t rb = root(raw[i].b);
    if (ra != rb) {
      parent[ra] = rb;
      continue;
    }
    if ((utils::splitmix(uint64_t(i) + 1ull, 0x10c6edull) & 0xffffull) < 0x2000ull) {
      flags[i] |= uint32_t(portal_flags::locked);
    }
  }

  std::vector<std::vector<zone_portal>> outgoing(drafts.size());
  for (uint32_t i = 0; i < raw.size(); ++i) {
    if (keys[raw[i].a] == invalid_key || keys[raw[i].b] == invalid_key) continue;
    outgoing[raw[i].a].push_back({keys[raw[i].b], raw[i].from, raw[i].to, flags[i]});
    outgoing[raw[i].b].push_back({keys[raw[i].a], raw[i].from, raw[i].to, flags[i]});
  }
  for (uint32_t index = 0; index < drafts.size(); ++index) {
    for (const auto identity : drafts[index].graph_links) {
      const auto found = by_identity.find(identity);
      if (found == by_identity.end()) continue;
      outgoing[index].push_back({found->second, {}, {}, uint32_t(portal_flags::graph)});
    }
  }

  build_stats stats{};
  stats.settlements = uint32_t(settlements.size());

  for (uint32_t y = 0; y < options.sector_side; ++y) {
    for (uint32_t x = 0; x < options.sector_side; ++x) {
      const auto& list = per_sector[size_t(y) * options.sector_side + x];

      zone_sector out{};
      out.x = options.sector_x + int32_t(x);
      out.y = options.sector_y + int32_t(y);
      out.names.push_back('\0');

      for (const auto index : list) {
        const auto& item = drafts[index];
        zone_record record{};
        record.key = keys[index];
        record.level = item.level;
        record.kind = item.kind;
        record.bounds = item.outline.empty() ? zone_bounds{} : bounds_of(item.outline, item.low, item.high);

        const auto parent_key = by_identity.find(item.parent_identity);
        record.parent = parent_key == by_identity.end() ? invalid_key : parent_key->second;

        record.vertex_begin = uint32_t(out.vertices.size());
        record.vertex_count = uint32_t(item.outline.size());
        out.vertices.insert(out.vertices.end(), item.outline.begin(), item.outline.end());

        record.name_offset = uint32_t(out.names.size());
        out.names.insert(out.names.end(), item.name.begin(), item.name.end());
        out.names.push_back('\0');

        record.portal_begin = uint32_t(out.portals.size());
        out.portals.insert(out.portals.end(), outgoing[index].begin(), outgoing[index].end());
        record.portal_count = uint32_t(out.portals.size()) - record.portal_begin;

        out.zones.push_back(record);
      }

      out.fingerprint = compute_fingerprint(out);
      write_sector(sector_path(options.root, out.x, out.y), out);

      ++stats.sectors;
      stats.zones += uint32_t(out.zones.size());
      stats.links += uint32_t(out.portals.size());
      stats.bytes += out.byte_size();
    }
  }

  stats.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin_time).count();
  return stats;
}

} // namespace devils_engine::pf09
