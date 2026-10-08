#include "resources.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "tavl_reader.h"

namespace gn07 {

namespace {

[[noreturn]] void unknown_field(tavl_reader& r, const std::string_view owner, const std::string_view field) {
  r.fail(std::format("unknown field '{}' in {}", field, owner));
}

link_kind read_link_kind(tavl_reader& r) {
  const std::string text = r.read_string();
  link_kind kind = link_kind::ordinary;
  if (!link_kind_from_string(text, kind)) {
    r.fail(std::format("'{}' is not a link kind (ordinary/secret)", text));
  }
  return kind;
}

std::vector<fact> read_facts(tavl_reader& r) {
  std::vector<fact> out;
  r.read_array([&] {
    fact f;
    r.read_object([&](const std::string_view name) {
      if (name == "name") f.name = r.read_string();
      else if (name == "value") f.value = r.read_string();
      else unknown_field(r, "fact", name);
    });
    if (f.name.empty()) {
      r.fail("fact needs a name");
    }
    out.push_back(std::move(f));
  });
  return out;
}

int32_t to_plan(const int64_t value) { return int32_t(value); }

void read_range32(tavl_reader& r, int32_t& min, int32_t& max) {
  int64_t lo = 0, hi = 0;
  r.read_fixed_range(plan_scale, lo, hi);
  min = to_plan(lo);
  max = to_plan(hi);
}

body_spec read_body(tavl_reader& r) {
  body_spec b;
  bool has_shape = false;
  r.read_object([&](const std::string_view field) {
    if (field == "shape") {
      const std::string text = r.read_string();
      if (text == "corridor") b.shape = body_shape::corridor;
      else if (text == "chamber") b.shape = body_shape::chamber;
      else r.fail(std::format("'{}' is not a body shape (corridor/chamber)", text));
      has_shape = true;
    } else if (field == "width") read_range32(r, b.width_min, b.width_max);
    else if (field == "size") read_range32(r, b.size_min, b.size_max);
    else if (field == "lobes") r.read_range(b.lobes_min, b.lobes_max);
    else unknown_field(r, "body", field);
  });
  if (!has_shape || b.width_min <= 0) {
    r.fail("body needs a shape and a positive width");
  }
  if (b.shape == body_shape::chamber && b.size_min < b.width_max) {
    r.fail("chamber body: size must be at least as large as its mouth width");
  }
  if (b.shape == body_shape::corridor && (b.size_max != 0 || b.lobes_max != 0)) {
    r.fail("corridor body has no size and no lobes");
  }
  return b;
}

void read_frame(tavl_reader& r, frame_rules& f) {
  r.read_object([&](const std::string_view field) {
    if (field == "wall") f.wall = to_plan(r.read_fixed(plan_scale));
    else if (field == "passage") f.passage = to_plan(r.read_fixed(plan_scale));
    else if (field == "hidden") read_range32(r, f.hidden_min, f.hidden_max);
    else if (field == "wander") f.wander = to_plan(r.read_fixed(plan_scale));
    else unknown_field(r, "frame", field);
  });
}

node_kind read_kind(tavl_reader& r) {
  node_kind kind;
  bool has_body = false;
  r.read_object([&](const std::string_view field) {
    if (field == "name") kind.name = r.read_string();
    else if (field == "function") kind.function = r.read_string();
    else if (field == "content") kind.content = r.read_int();
    else if (field == "tags") kind.tags = r.read_string_list();
    else if (field == "glyph") {
      const std::string glyph = r.read_string();
      if (glyph.size() != 1) {
        r.fail("glyph must be one character");
      }
      kind.glyph = glyph[0];
    } else if (field == "body") {
      kind.body = read_body(r);
      has_body = true;
    } else unknown_field(r, "kind", field);
  });
  if (kind.name.empty() || kind.function.empty() || !has_body) {
    r.fail("kind needs name, function and body");
  }
  if (kind.content < 0) {
    r.fail(std::format("kind '{}': content must not be negative", kind.name));
  }
  return kind;
}

place read_place(tavl_reader& r, const script_env& env) {
  place p;
  bool has_where = false;
  r.read_object([&](const std::string_view field) {
    if (field == "name") p.name = r.read_string();
    else if (field == "where") {
      // Блок `{ ... }` целиком съедает devils_script из того же парсера.
      p.where = env.compile_predicate(p.name.empty() ? std::string_view("place") : std::string_view(p.name),
        r.parser());
      has_where = true;
    } else unknown_field(r, "place", field);
  });
  if (p.name.empty() || !has_where) {
    r.fail("place needs name and where");
  }
  return p;
}

port_kind read_port(tavl_reader& r) {
  port_kind p;
  bool has_kind = false;
  r.read_object([&](const std::string_view field) {
    if (field == "kind") {
      p.kind = read_link_kind(r);
      has_kind = true;
    } else if (field == "place") p.place = r.read_string();
    else unknown_field(r, "port", field);
  });
  if (!has_kind) {
    r.fail("port needs a kind");
  }
  return p;
}

void read_growth(tavl_reader& r, mine_growth& g) {
  r.read_object([&](const std::string_view field) {
    if (field == "turn_percent") g.turn_percent = r.read_int();
    else if (field == "branches") r.read_range(g.branches_min, g.branches_max);
    else if (field == "branch_length") r.read_range(g.branch_length_min, g.branch_length_max);
    else if (field == "abandoned_percent") g.abandoned_percent = r.read_int();
    else if (field == "face_percent") g.face_percent = r.read_int();
    else unknown_field(r, "growth", field);
  });
  if (g.branch_length_min < 1) {
    r.fail("growth.branch_length must start at 1 or more");
  }
}

template <typename T, typename Name>
void refuse_duplicates(const std::vector<T>& items, const Name& name_of, const std::string& source,
                       const std::string_view what) {
  for (size_t i = 0; i < items.size(); ++i) {
    for (size_t j = i + 1; j < items.size(); ++j) {
      if (name_of(items[i]) == name_of(items[j])) {
        throw std::runtime_error(std::format("{}: {} '{}' declared twice", source, what, name_of(items[i])));
      }
    }
  }
}

// Числа каркаса, при которых воплощение по клеткам невозможно, видны из данных — это отказ
// чтения, а не попытки. Тело с полями толщиной в полстены обязано уместиться в клетку.
void validate_frame_rules(const archetype& arch, const bool has_frame, const std::string& source) {
  const frame_rules& f = arch.frame;
  const auto fail = [&](const std::string& message) {
    throw std::runtime_error(std::format("{}: frame: {}", source, message));
  };
  if (!has_frame) {
    fail("the archetype has no frame rules");
  }
  if (f.wall <= 0 || f.passage <= 0 || f.hidden_min <= 0 || f.wander < 0) {
    fail("wall, passage and hidden must be positive, wander must not be negative");
  }
  if (f.hidden_min < f.passage) {
    fail(std::format("a hidden passage of {} cm is narrower than the passage of {} cm the player needs", f.hidden_min,
      f.passage));
  }
  const int64_t cell = arch.cell * plan_scale;
  const int64_t room = cell - f.wall;
  if (f.hidden_max > room) {
    fail("hidden passage does not fit a cell");
  }
  for (const auto& k : arch.kinds) {
    const body_spec& b = k.body;
    // Открытый проход должен отличаться от скрытого по ширине: иначе «скрытость» в каркасе
    // ничем не выражена, и проверка не отличила бы спрятанный ход от явного.
    if (b.width_min <= f.hidden_max) {
      fail(std::format("kind '{}': width {} cm is not wider than a hidden passage ({} cm)", k.name, b.width_min,
        f.hidden_max));
    }
    if (b.width_min < f.passage) {
      fail(std::format("kind '{}': width {} cm is narrower than the passage", k.name, b.width_min));
    }
    const int64_t extent = b.shape == body_shape::chamber ? b.size_max : b.width_max;
    if (extent > room) {
      fail(std::format("kind '{}': {} cm does not fit a {} cm cell with a {} cm wall", k.name, extent, cell, f.wall));
    }
  }
}

void prepare_parser(tavl::parser& parser, const std::string_view text) {
  parser.add_default_operator();
  parser.flush(text);
  parser.finish();
}

} // namespace

std::string read_text_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error(std::format("cannot open '{}'", path));
  }
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}

archetype read_archetype(const std::string_view text, std::string source_name, const script_env& env) {
  tavl::parser parser;
  env.configure_parser(parser);
  parser.flush(text);
  parser.finish();
  tavl_reader r(parser, std::move(source_name));

  archetype arch;
  bool has_frame = false;
  r.read_document([&](const std::string_view field) {
    if (field == "name") arch.name = r.read_string();
    else if (field == "family") arch.family = r.read_string();
    else if (field == "cell") arch.cell = r.read_int();
    else if (field == "tags") arch.tags = r.read_string_list();
    else if (field == "kinds") r.read_array([&] { arch.kinds.push_back(read_kind(r)); });
    else if (field == "places") r.read_array([&] { arch.places.push_back(read_place(r, env)); });
    else if (field == "ports") r.read_array([&] { arch.ports.push_back(read_port(r)); });
    else if (field == "growth") read_growth(r, arch.growth);
    else if (field == "frame") {
      read_frame(r, arch.frame);
      has_frame = true;
    } else unknown_field(r, "archetype", field);
  });

  const std::string& source = r.source_name();
  if (arch.name.empty() || arch.family.empty() || arch.kinds.empty()) {
    throw std::runtime_error(std::format("{}: archetype needs name, family and kinds", source));
  }
  if (arch.cell <= 0) {
    throw std::runtime_error(std::format("{}: cell must be positive", source));
  }
  refuse_duplicates(arch.kinds, [](const node_kind& k) { return k.name; }, source, "kind");
  refuse_duplicates(arch.places, [](const place& p) { return p.name; }, source, "place");
  refuse_duplicates(arch.ports, [](const port_kind& p) { return std::string(to_string(p.kind)); }, source, "port");
  if (arch.find_port(link_kind::ordinary) == nullptr) {
    throw std::runtime_error(std::format("{}: archetype needs an ordinary port", source));
  }
  validate_frame_rules(arch, has_frame, source);
  for (const auto& p : arch.ports) {
    if (p.kind == link_kind::secret && arch.find_place(p.place) == nullptr) {
      throw std::runtime_error(std::format("{}: secret port refers to unknown place '{}'", source, p.place));
    }
  }
  return arch;
}

world_fragment read_world_fragment(const std::string_view text, std::string source_name) {
  tavl::parser parser;
  prepare_parser(parser, text);
  tavl_reader r(parser, std::move(source_name));

  world_fragment world;
  r.read_document([&](const std::string_view field) {
    if (field == "nodes") {
      r.read_array([&] {
        world_node n;
        r.read_object([&](const std::string_view f) {
          if (f == "name") n.name = r.read_string();
          else if (f == "parent") n.parent = r.read_string();
          else if (f == "archetype") n.archetype = r.read_string();
          else if (f == "at") {
            const auto values = r.read_int_list();
            if (values.size() != 2) {
              r.fail("at must be [x, y]");
            }
            n.at_x = values[0];
            n.at_y = values[1];
          } else if (f == "bounds") {
            const auto values = r.read_int_list();
            if (values.size() != 2 || values[0] <= 0 || values[1] <= 0) {
              r.fail("bounds must be [width, height] with positive values");
            }
            n.bounds_width = values[0];
            n.bounds_height = values[1];
          } else if (f == "facts") n.facts = read_facts(r);
          else unknown_field(r, "world node", f);
        });
        if (n.name.empty()) {
          r.fail("world node needs a name");
        }
        if (!n.archetype.empty() && n.bounds_width == 0) {
          r.fail(std::format("location '{}' needs bounds", n.name));
        }
        world.nodes.push_back(std::move(n));
      });
    } else if (field == "links") {
      r.read_array([&] {
        world_link l;
        r.read_object([&](const std::string_view f) {
          if (f == "between") {
            const auto names = r.read_string_list();
            if (names.size() != 2 || names[0] == names[1]) {
              r.fail("between must name two different nodes");
            }
            l.a = names[0];
            l.b = names[1];
          } else if (f == "kind") l.kind = read_link_kind(r);
          else unknown_field(r, "world link", f);
        });
        if (l.a.empty()) {
          r.fail("world link needs between");
        }
        world.links.push_back(std::move(l));
      });
    } else unknown_field(r, "world fragment", field);
  });

  const std::string& source = r.source_name();
  refuse_duplicates(world.nodes, [](const world_node& n) { return n.name; }, source, "node");
  for (const auto& n : world.nodes) {
    if (!n.parent.empty() && world.find(n.parent) == nullptr) {
      throw std::runtime_error(std::format("{}: node '{}' has unknown parent '{}'", source, n.name, n.parent));
    }
    // Цикл родителей: от любого узла до корня не дольше числа узлов.
    const world_node* cur = &n;
    for (size_t steps = 0; cur != nullptr && !cur->parent.empty(); ++steps) {
      if (steps > world.nodes.size()) {
        throw std::runtime_error(std::format("{}: parents of '{}' form a cycle", source, n.name));
      }
      cur = world.find(cur->parent);
    }
  }
  for (const auto& l : world.links) {
    if (world.find(l.a) == nullptr || world.find(l.b) == nullptr) {
      throw std::runtime_error(std::format("{}: link {}-{} names an unknown node", source, l.a, l.b));
    }
  }
  return world;
}

brief read_brief(const std::string_view text, std::string source_name) {
  tavl::parser parser;
  prepare_parser(parser, text);
  tavl_reader r(parser, std::move(source_name));

  brief b;
  r.read_document([&](const std::string_view field) {
    if (field == "name") b.name = r.read_string();
    else if (field == "location") b.location = r.read_string();
    else if (field == "content") {
      r.read_range(b.content_min, b.content_max);
      b.has_allowance = true;
    } else if (field == "placements") {
      r.read_array([&] {
        placement p;
        r.read_object([&](const std::string_view f) {
          if (f == "name") p.name = r.read_string();
          else if (f == "what") p.what = r.read_string();
          else if (f == "count") p.count = r.read_int();
          else if (f == "place") p.place = r.read_string();
          else if (f == "budget") r.read_range(p.budget_min, p.budget_max);
          else unknown_field(r, "placement", f);
        });
        if (p.name.empty() || p.place.empty()) {
          r.fail("placement needs name and place");
        }
        b.placements.push_back(std::move(p));
      });
    } else if (field == "lore") {
      r.read_array([&] {
        lore_link l;
        r.read_object([&](const std::string_view f) {
          if (f == "from") l.from = r.read_string();
          else if (f == "to") l.to = r.read_string();
          else if (f == "kind") l.kind = r.read_string();
          else unknown_field(r, "lore link", f);
        });
        if (l.from.empty() || l.to.empty() || l.kind.empty()) {
          r.fail("lore link needs from, to and kind");
        }
        b.lore.push_back(std::move(l));
      });
    } else unknown_field(r, "brief", field);
  });

  if (b.name.empty() || b.location.empty()) {
    throw std::runtime_error(std::format("{}: brief needs name and location", r.source_name()));
  }
  refuse_duplicates(b.placements, [](const placement& p) { return p.name; }, r.source_name(), "placement");
  return b;
}

void validate_brief(const brief& b, const world_fragment& world, const archetype& arch) {
  const world_node* node = world.find(b.location);
  if (node == nullptr) {
    throw std::runtime_error(std::format("brief '{}': location '{}' is not in the world fragment", b.name, b.location));
  }
  if (node->archetype != arch.name) {
    throw std::runtime_error(std::format("brief '{}': location '{}' is of archetype '{}', loaded '{}'",
      b.name, b.location, node->archetype, arch.name));
  }
  for (const auto& p : b.placements) {
    if (arch.find_place(p.place) == nullptr) {
      throw std::runtime_error(std::format("brief '{}': placement '{}' refers to unknown place '{}'",
        b.name, p.name, p.place));
    }
  }
  const auto known = [&](const std::string& name) {
    return std::any_of(b.placements.begin(), b.placements.end(), [&](const placement& p) { return p.name == name; });
  };
  for (const auto& l : b.lore) {
    if (!known(l.from) || !known(l.to)) {
      throw std::runtime_error(std::format("brief '{}': lore link {} -> {} names an unknown placement",
        b.name, l.from, l.to));
    }
  }
}

location_input derive_location_input(const world_fragment& world, const brief& b, const archetype& arch) {
  location_input in;
  in.node = world.find(b.location);
  in.cell = arch.cell;
  in.width = int32_t(in.node->bounds_width / arch.cell);
  in.height = int32_t(in.node->bounds_height / arch.cell);
  in.facts = world.facts_of(in.node->name);
  for (const auto& l : world.links) {
    if (l.a != in.node->name && l.b != in.node->name) {
      continue;
    }
    const world_node* other = world.find(l.a == in.node->name ? l.b : l.a);
    port_request port;
    port.neighbour = other->name;
    port.kind = l.kind;
    if (!facing(in.width, in.height, other->at_x - in.node->at_x, other->at_y - in.node->at_y, port.at, port.along)) {
      throw std::runtime_error(std::format("location '{}': neighbour '{}' stands at the same point, its direction is undefined",
        in.node->name, other->name));
    }
    in.ports.push_back(std::move(port));
  }
  return in;
}

} // namespace gn07
