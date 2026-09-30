#include "site_graph.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <format>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <tavl/tavl.h>

#include "devils_engine/originator/motif_layout.h"
#include "devils_engine/utils/shared.h"

#include "site_motif_resource.h"
#include "motif_viewer.h"

namespace gn06 {
namespace {

struct ending_decl {
  std::string name;
  std::string motif;
  std::vector<uint32_t> depth;
  uint32_t frontage_extra = 4;
};

struct kind_decl {
  std::string name;
  std::string category;
  std::string function;
  std::string motif;
  std::vector<uint32_t> width;
  std::vector<uint32_t> width_max;
  std::vector<uint32_t> height;
  std::vector<uint32_t> height_max;
  std::string geometry_operation;
  std::vector<devils_engine::originator::motif_geometry_parameter> geometry_parameters;
  site_motif_placement placement;
  std::string height_policy;
  uint32_t frontage_margin = 0;
  uint32_t frontage_gap = 0;
  std::string glyph;
  std::vector<uint32_t> colour;
  std::vector<part_decl> parts;
  path_decl path;
};

struct rule_decl {
  std::string kind;
  std::vector<std::string> anchors;
  std::vector<uint32_t> min_count;
  std::vector<uint32_t> max_count;
  uint32_t per_anchor = 0;
  uint32_t unique_port = 0;
  uint32_t passage_inset = 0;
  std::string via;
  std::string appearance_from;
  std::string anchor_part;
  std::string passage_align;
  std::string port;
  std::vector<std::string> sides;
  std::string attachment; // wall, cap, bend — геометрическое назначение, не лорный port
  std::string leaf_policy; // omit: переходный путь без продолжения не нужен композиции
  uint32_t open_percent = 0;
  std::vector<std::string> turns;
};

struct perception_decl {
  std::string from;
  std::string to;
  std::string witness;
  std::string via;
  std::string dialogue_template;
  uint32_t max_distance = 0;
};

struct area_decl {
  std::string name;
  std::string category;
  std::string function;
  std::string motif;
};

struct site_document {
  uint32_t version = 0;
  std::string name;
  std::string theme;
  std::vector<std::string> scales;
  std::vector<uint32_t> footprints;
  std::vector<area_decl> kinds;
  std::vector<rule_decl> rules;
  std::vector<ending_decl> endings;
  perception_decl perception;
};

struct site_decl {
  uint32_t version = 0;
  std::string name;
  std::string theme;
  std::vector<std::string> scales;
  std::vector<uint32_t> footprints;
  std::vector<kind_decl> kinds;
  std::vector<rule_decl> rules;
  std::vector<ending_decl> endings;
  perception_decl perception;
  site_motif_catalogue motifs;
  site_bounds bounds;
};

struct site_motif_binding {
  uint32_t id = 0;
  uint32_t colour = 0;
};

std::vector<site_motif_binding> motif_bindings(const site_decl& site) {
  std::vector<site_motif_binding> result;
  const auto append = [&](const std::string_view name) {
    const auto& motif = find_site_motif(site.motifs, name);
    const auto& colour = motif.presentation.colour;
    result.push_back({uint32_t(&motif - site.motifs.motifs.data()) + 1,
      (colour[0] << 16) | (colour[1] << 8) | colour[2]});
  };
  for (const auto& kind : site.kinds) { append(kind.motif); }
  for (const auto& ending : site.endings) { append(ending.motif); }
  return result;
}

site_bounds resolve_bounds(const site_decl& site, const size_t profile, const site_bounds requested) {
  if (requested.width == 0 && requested.height == 0) {
    return {site.footprints[profile], site.footprints[profile]};
  }
  if (requested.width < 32 || requested.height < 32 || requested.width > 4096 || requested.height > 4096) {
    throw std::runtime_error("site: hard bounds need width and height in 32..4096; they are not clamped");
  }
  return requested;
}

uint32_t kind_id(const site_decl& site, const std::string_view name) {
  const auto it = std::find_if(site.kinds.begin(), site.kinds.end(),
    [&](const kind_decl& kind) { return kind.name == name; });
  if (it == site.kinds.end())
    throw std::runtime_error(std::format("site: unknown area kind '{}'", name));
  return uint32_t(it - site.kinds.begin());
}

site_decl read_site(const std::string_view source, const std::string_view motifs_source) {
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(std::string(source));
  parser.finish();
  tavl::ct_context context;
  site_document document;
  if (!tavl::deserialize_next(parser, context, document))
    throw std::runtime_error("site: empty declaration");
  for (const auto& diagnostic : context.diagnostics)
    if (diagnostic.error.is_critical())
      throw std::runtime_error(std::format("site: parse error '{}' at {}:{} field '{}'",
        tavl::to_string(diagnostic.error.type), diagnostic.error.span.line,
        diagnostic.error.span.column, diagnostic.field));
  site_decl site;
  site.version = document.version;
  site.name = std::move(document.name);
  site.theme = std::move(document.theme);
  site.scales = std::move(document.scales);
  site.footprints = std::move(document.footprints);
  site.rules = std::move(document.rules);
  site.endings = std::move(document.endings);
  site.perception = std::move(document.perception);
  site.motifs = read_site_motifs(motifs_source);
  if (site.scales != site.motifs.profiles) {
    throw std::runtime_error("site: content profiles do not match the motif catalogue");
  }
  for (const auto& area : document.kinds) {
    const auto& motif = find_site_motif(site.motifs, area.motif);
    const auto& geometry = motif.geometry;
    kind_decl kind;
    kind.name = area.name;
    kind.category = area.category;
    kind.function = area.function;
    kind.motif = area.motif;
    kind.width = geometry.width;
    kind.width_max = geometry.width_max;
    kind.height = geometry.height;
    kind.height_max = geometry.height_max;
    kind.geometry_operation = geometry.operation;
    kind.geometry_parameters = geometry.parameters;
    kind.placement = motif.placement;
    kind.height_policy = geometry.height_policy;
    kind.frontage_margin = geometry.frontage_margin;
    kind.frontage_gap = geometry.frontage_gap;
    kind.parts = geometry.parts;
    kind.path = geometry.path;
    kind.glyph = motif.presentation.glyph;
    kind.colour = motif.presentation.colour;
    site.kinds.push_back(std::move(kind));
  }
  if (site.version != 11 || site.name.empty() || site.theme.empty() || site.scales.empty() ||
      site.kinds.empty() || site.rules.empty() || site.footprints.size() != site.scales.size())
    throw std::runtime_error("site: version 11 needs a name, theme, scales, kinds and rules");
  for (size_t i = 0; i < site.scales.size(); ++i)
    if (site.scales[i].empty() ||
        std::find(site.scales.begin(), site.scales.begin() + i, site.scales[i]) != site.scales.begin() + i ||
        site.footprints[i] < 32 || site.footprints[i] > 4096 ||
        (i > 0 && site.footprints[i] <= site.footprints[i - 1]))
      throw std::runtime_error("site: scales need unique names and increasing footprints in 32..4096");
  for (size_t i = 0; i < site.endings.size(); ++i) {
    const auto& ending = site.endings[i];
    if (ending.name.empty() || ending.motif.empty() || ending.motif == "none" ||
        ending.depth.size() != site.scales.size() || ending.frontage_extra > 32 ||
        std::any_of(ending.depth.begin(), ending.depth.end(), [](const auto v) { return v < 3 || v > 32; }) ||
        std::any_of(site.endings.begin(), site.endings.begin() + i,
          [&](const ending_decl& prior) { return prior.name == ending.name; })) {
      throw std::runtime_error("site: invalid or duplicate authored path ending");
    }
  }
  for (size_t i = 0; i < site.kinds.size(); ++i) {
    const auto& kind = site.kinds[i];
    if (kind.name.empty() || kind.function.empty() ||
        (kind.category != "functional" && kind.category != "technical") ||
        std::any_of(site.kinds.begin(), site.kinds.begin() + i,
          [&](const kind_decl& prior) { return prior.name == kind.name; })) {
      throw std::runtime_error(std::format("site: invalid or duplicate area type '{}'", kind.name));
    }
    if (!kind.path.mode.empty()) {
      for (const auto* ending : {&kind.path.start, &kind.path.end}) {
        if (ending->policy != "trim" && std::none_of(site.endings.begin(), site.endings.end(),
              [&](const ending_decl& accent) { return accent.name == ending->accent; })) {
          throw std::runtime_error(std::format("site: motif '{}' needs missing accent '{}'",
            kind.motif, ending->accent));
        }
      }
    }
  }
  std::vector<uint32_t> earlier;
  for (size_t i = 0; i < site.rules.size(); ++i) {
    const auto& rule = site.rules[i];
    const auto id = kind_id(site, rule.kind);
    if (std::any_of(site.kinds[id].height.begin(), site.kinds[id].height.end(),
          [](const auto height) { return height < 3; }) ||
        std::any_of(site.kinds[id].width.begin(), site.kinds[id].width.end(),
          [](const auto width) { return width < 3; }))
      throw std::runtime_error(std::format("site: spawned area '{}' needs at least 3x3 geometry", rule.kind));
    if (std::find(earlier.begin(), earlier.end(), id) != earlier.end())
      throw std::runtime_error(std::format("site: kind '{}' has two spawn rules", rule.kind));
    if (rule.min_count.size() != site.scales.size() || rule.max_count.size() != site.scales.size() ||
        rule.per_anchor > 1 || rule.unique_port > 1 || (i == 0) != rule.anchors.empty())
      throw std::runtime_error(std::format("site: invalid declaration of rule '{}'", rule.kind));
    for (size_t scale = 0; scale < site.scales.size(); ++scale)
      if (rule.min_count[scale] == 0 || rule.max_count[scale] < rule.min_count[scale] ||
          rule.max_count[scale] > 32 || (i == 0 && rule.max_count[scale] != 1))
        throw std::runtime_error(std::format("site: invalid count for '{}' at scale '{}'",
          rule.kind, site.scales[scale]));
    for (const auto& anchor : rule.anchors) {
      const auto anchor_id = kind_id(site, anchor);
      if (std::find(earlier.begin(), earlier.end(), anchor_id) == earlier.end())
        throw std::runtime_error(std::format("site: anchor '{}' for '{}' must be spawned earlier",
          anchor, rule.kind));
    }
    if (site.kinds[id].placement.strategy == "parent_wrap") {
      if (rule.anchors.size() != 1 || rule.sides != std::vector<std::string>{"east"} ||
          rule.port != "centre" || rule.via.empty() ||
          site.kinds[kind_id(site, rule.anchors.front())].geometry_operation != "rounded_east")
        throw std::runtime_error("site: wrap_east needs one rounded parent, east side and centred connector");
      const auto& parent = site.kinds[kind_id(site, rule.anchors.front())];
      for (size_t scale = 0; scale < site.scales.size(); ++scale)
        if (site.kinds[id].height[scale] != parent.height[scale] ||
            !parent.height_max.empty() || !site.kinds[id].height_max.empty())
          throw std::runtime_error("site: wrap_east and its rounded parent need the same fixed height");
    }
    if (!rule.via.empty()) {
      const auto& connector = site.kinds[kind_id(site, rule.via)];
      if (connector.category != "technical")
        throw std::runtime_error(std::format("site: connector '{}' must be a technical area", rule.via));
      if (rule.appearance_from != "parent" && rule.appearance_from != "child" &&
          rule.appearance_from != "vary")
        throw std::runtime_error(std::format("site: '{}' needs appearance_from = parent, child or vary", rule.kind));
    } else if (!rule.appearance_from.empty()) {
      throw std::runtime_error(std::format("site: '{}' declares connector appearance without a connector", rule.kind));
    }
    if (i == 0 && !rule.anchor_part.empty())
      throw std::runtime_error("site: entry cannot have an anchor_part");
    if (!rule.anchor_part.empty() && rule.anchor_part != "last" && rule.anchor_part != "main")
      for (const auto& anchor : rule.anchors) {
        const auto& parent = site.kinds[kind_id(site, anchor)];
        if (!parent.path.mode.empty() && rule.anchor_part == "bend") continue;
        const auto& parts = parent.parts;
        if (std::none_of(parts.begin(), parts.end(), [&](const part_decl& part) {
              return part.name == rule.anchor_part;
            }))
          throw std::runtime_error(std::format(
            "site: '{}' asks for missing part '{}' on anchor '{}'",
            rule.kind, rule.anchor_part, anchor));
      }
    if (!rule.passage_align.empty() && rule.passage_align != "start" &&
        rule.passage_align != "centre" && rule.passage_align != "end")
      throw std::runtime_error(std::format("site: '{}' has an unknown passage_align", rule.kind));
    if (i > 0 && rule.port.empty())
      throw std::runtime_error(std::format("site: '{}' needs a named attachment port", rule.kind));
    if (i > 0 && rule.sides.empty())
      throw std::runtime_error(std::format("site: '{}' needs geometric attachment sides", rule.kind));
    for (const auto& side : rule.sides)
      if (side != "east" && side != "west" && side != "north" && side != "south")
        throw std::runtime_error(std::format("site: unknown side '{}'", side));
    if (rule.open_percent > 100 || (!rule.attachment.empty() && rule.attachment != "wall" &&
        rule.attachment != "cap" && rule.attachment != "bend" && rule.attachment != "paired_wall")) {
      throw std::runtime_error("site: invalid aperture policy or attachment kind");
    }
    if (!rule.leaf_policy.empty() &&
        (rule.leaf_policy != "omit" || site.kinds[id].category != "functional" ||
         site.kinds[id].path.mode.empty() || !rule.via.empty())) {
      throw std::runtime_error("site: leaf_policy=omit needs a functional path without a connector");
    }
    if (rule.attachment == "paired_wall" && (rule.per_anchor != 1 || rule.unique_port ||
        !site.kinds[id].path.mode.empty() ||
        std::any_of(rule.min_count.begin(), rule.min_count.end(), [](const auto count) { return count != 2; }) ||
        rule.min_count != rule.max_count)) {
      throw std::runtime_error("site: paired_wall needs exactly two non-path children per anchor");
    }
    for (const auto& turn : rule.turns) {
      if (turn != "straight" && turn != "left" && turn != "right") {
        throw std::runtime_error("site: unknown path turn");
      }
    }
    earlier.push_back(id);
  }
  const auto& p = site.perception;
  if (p.dialogue_template.empty() || p.from.empty() || p.to.empty() || p.witness.empty() || p.via.empty() ||
      p.max_distance == 0 || p.max_distance > 32)
    throw std::runtime_error("site: perception needs from, to, witness, via, a distance of 1..32 and dialogue_template");
  for (const auto& name : {p.from, p.to, p.witness, p.via})
    if (std::find(earlier.begin(), earlier.end(), kind_id(site, name)) == earlier.end())
      throw std::runtime_error(std::format("site: perception kind '{}' is never spawned", name));
  if (site.kinds[kind_id(site, p.from)].category != "functional" ||
      site.kinds[kind_id(site, p.to)].category != "functional" ||
      site.kinds[kind_id(site, p.witness)].category != "functional" ||
      site.kinds[kind_id(site, p.via)].category != "functional")
    throw std::runtime_error("site: perception requires functional endpoints, witness and route area");
  return site;
}

struct node {
  uint32_t kind = 0;
  bool connector = false;
  bool operator==(const node&) const = default;
};

struct edge {
  uint32_t a = 0;
  uint32_t b = 0;
  std::string port;
  bool operator==(const edge&) const = default;
};

struct cue {
  uint32_t witness = 0;
  uint32_t route_via = 0;
  std::string token;
  bool operator==(const cue&) const = default;
};

struct graph {
  std::vector<node> nodes;
  std::vector<edge> edges;
  std::vector<uint32_t> route;
  std::vector<cue> cues;
  bool operator==(const graph&) const = default;
};

uint32_t roll(const uint64_t seed, const uint32_t rule, const uint32_t instance,
              const uint32_t choice) {
  const auto base = devils_engine::utils::shared::prng2(uint32_t(seed) ^ rule,
                                                         uint32_t(seed >> 32) ^ instance);
  return devils_engine::utils::shared::prng2(base, choice + 0x9e3779b9u);
}

std::vector<uint32_t> neighbours(const graph& graph, const uint32_t id) {
  std::vector<uint32_t> result;
  for (const auto& edge : graph.edges) {
    if (edge.a == id) result.push_back(edge.b);
    if (edge.b == id) result.push_back(edge.a);
  }
  return result;
}

std::vector<uint32_t> path(const graph& graph, const uint32_t from, const uint32_t to) {
  std::vector<uint32_t> previous(graph.nodes.size(), UINT32_MAX);
  std::queue<uint32_t> pending;
  previous[from] = from;
  pending.push(from);
  while (!pending.empty() && previous[to] == UINT32_MAX) {
    const auto here = pending.front();
    pending.pop();
    for (const auto next : neighbours(graph, here))
      if (previous[next] == UINT32_MAX) {
        previous[next] = here;
        pending.push(next);
      }
  }
  if (previous[to] == UINT32_MAX) return {};
  std::vector<uint32_t> result;
  for (auto at = to; at != from; at = previous[at]) result.push_back(at);
  result.push_back(from);
  std::reverse(result.begin(), result.end());
  return result;
}

graph assemble(const site_decl& site, const size_t scale, const uint64_t seed) {
  graph result;
  for (uint32_t rule_index = 0; rule_index < site.rules.size(); ++rule_index) {
    const auto& rule = site.rules[rule_index];
    const auto kind = kind_id(site, rule.kind);
    std::vector<uint32_t> anchors;
    for (uint32_t id = 0; id < result.nodes.size(); ++id)
      if (!result.nodes[id].connector &&
          std::find(rule.anchors.begin(), rule.anchors.end(), site.kinds[result.nodes[id].kind].name) != rule.anchors.end())
        anchors.push_back(id);
    if (rule_index > 0 && anchors.empty())
      throw std::runtime_error(std::format("site: '{}' has no anchor", rule.kind));
    const auto groups = rule.per_anchor ? uint32_t(anchors.size()) : 1u;
    for (uint32_t group = 0; group < groups; ++group) {
      const auto amount = rule.min_count[scale] +
        roll(seed, rule_index, group, 0) % (rule.max_count[scale] - rule.min_count[scale] + 1);
      for (uint32_t instance = 0; instance < amount; ++instance) {
        const auto id = uint32_t(result.nodes.size());
        result.nodes.push_back({kind, false});
        if (rule_index == 0) continue;
        auto available = anchors;
        if (rule.per_anchor) available = {anchors[group]};
        if (rule.unique_port)
          std::erase_if(available, [&](const uint32_t candidate) {
            return std::any_of(result.edges.begin(), result.edges.end(), [&](const edge& link) {
              return link.a == candidate && link.port == rule.port;
            });
          });
        if (available.empty())
          throw std::runtime_error(std::format("site: '{}' exhausted port '{}'", rule.kind, rule.port));
        const auto anchor = available[roll(seed, rule_index, instance, 1) % available.size()];
        if (rule.via.empty() || roll(seed, rule_index,
            rule.attachment == "paired_wall" ? anchor : id, 0x0aeu) % 100 < rule.open_percent) {
          result.edges.push_back({anchor, id, rule.port});
        } else {
          const auto connector = uint32_t(result.nodes.size());
          result.nodes.push_back({kind_id(site, rule.via), true});
          result.edges.push_back({anchor, connector, rule.port});
          result.edges.push_back({connector, id, rule.port});
        }
      }
    }
  }
  // Правила собираются по порядку: будущий ребёнок ещё не известен в момент
  // появления переходного пути. После сборки графа убираем только явно
  // объявленные пути-листья. Родительский торец затем завершит finish_paths.
  std::vector<bool> omit_kind(site.kinds.size(), false);
  for (const auto& rule : site.rules)
    if (rule.leaf_policy == "omit") omit_kind[kind_id(site, rule.kind)] = true;
  std::vector<bool> omitted(result.nodes.size(), false);
  for (uint32_t id = 1; id < result.nodes.size(); ++id)
    if (omit_kind[result.nodes[id].kind] &&
        std::none_of(result.edges.begin(), result.edges.end(),
          [&](const edge& link) { return link.a == id; }))
      omitted[id] = true;
  if (std::any_of(omitted.begin(), omitted.end(), [](const bool value) { return value; })) {
    std::vector<uint32_t> remap(result.nodes.size(), UINT32_MAX);
    graph compact;
    for (uint32_t id = 0; id < result.nodes.size(); ++id)
      if (!omitted[id]) {
        remap[id] = uint32_t(compact.nodes.size());
        compact.nodes.push_back(result.nodes[id]);
      }
    for (const auto& link : result.edges)
      if (!omitted[link.b]) compact.edges.push_back({remap[link.a], remap[link.b], link.port});
    result = std::move(compact);
  }
  const auto& p = site.perception;
  std::vector<uint32_t> starts, ends;
  for (uint32_t id = 0; id < result.nodes.size(); ++id) {
    if (result.nodes[id].kind == kind_id(site, p.from)) starts.push_back(id);
    if (result.nodes[id].kind == kind_id(site, p.to)) ends.push_back(id);
  }
  if (starts.empty() || ends.empty())
    throw std::runtime_error("site: perception route has no endpoint");
  result.route = path(result, starts[roll(seed, 0xf001u, 0, 0) % starts.size()],
                      ends[roll(seed, 0xf002u, 0, 0) % ends.size()]);
  if (result.route.empty()) throw std::runtime_error("site: perception route is disconnected");
  const auto witness_kind = kind_id(site, p.witness);
  const auto via_kind = kind_id(site, p.via);
  for (uint32_t witness = 0; witness < result.nodes.size(); ++witness) {
    if (result.nodes[witness].kind != witness_kind) continue;
    for (const auto connector : neighbours(result, witness))
      for (const auto via : neighbours(result, connector))
        if (result.nodes[via].kind == via_kind &&
            std::find(result.route.begin(), result.route.end(), via) != result.route.end())
          result.cues.push_back({witness, via, p.dialogue_template});
  }
  return result;
}

void check(const site_decl& site, const graph& graph) {
  if (graph.nodes.empty() || graph.edges.size() + 1 != graph.nodes.size())
    throw std::runtime_error("site verify: graph is not a tree");
  for (uint32_t id = 0; id < graph.nodes.size(); ++id)
    if (path(graph, 0, id).empty())
      throw std::runtime_error(std::format("site verify: area #{} cannot be reached from the entrance", id));
  for (uint32_t id = 0; id < graph.nodes.size(); ++id)
    if (graph.nodes[id].connector &&
        (site.kinds[graph.nodes[id].kind].category != "technical" || neighbours(graph, id).size() != 2))
      throw std::runtime_error(std::format("site verify: connector #{} is not a two-sided technical area", id));
  for (const auto& rule : site.rules)
    if (rule.leaf_policy == "omit") {
      const auto kind = kind_id(site, rule.kind);
      for (uint32_t id = 0; id < graph.nodes.size(); ++id)
        if (graph.nodes[id].kind == kind &&
            std::none_of(graph.edges.begin(), graph.edges.end(),
              [&](const edge& link) { return link.a == id; }))
          throw std::runtime_error(std::format("site verify: unused transition #{} was not omitted", id));
    }
  const auto& p = site.perception;
  if (graph.route.empty() || graph.nodes[graph.route.front()].kind != kind_id(site, p.from) ||
      graph.nodes[graph.route.back()].kind != kind_id(site, p.to) || graph.cues.empty())
    throw std::runtime_error("site verify: route or perception cue missing");
  for (size_t i = 1; i < graph.route.size(); ++i) {
    const auto adjacent = neighbours(graph, graph.route[i - 1]);
    if (std::find(adjacent.begin(), adjacent.end(), graph.route[i]) == adjacent.end())
      throw std::runtime_error("site verify: route contains a non-edge");
  }
  for (const auto& cue : graph.cues) {
    if (cue.token != p.dialogue_template || graph.nodes[cue.witness].kind != kind_id(site, p.witness) ||
        graph.nodes[cue.route_via].kind != kind_id(site, p.via) ||
        std::find(graph.route.begin(), graph.route.end(), cue.route_via) == graph.route.end() ||
        path(graph, cue.witness, cue.route_via).size() != 3)
      throw std::runtime_error("site verify: perception cue is not beside its route");
  }
}

struct connector_visual {
  uint32_t colour = 0;
  uint32_t inherited_from = UINT32_MAX;
  std::string_view source;
};

connector_visual appearance_of_connector(const site_decl& site, const graph& graph,
                                         const uint32_t id, const uint64_t seed) {
  const auto inbound = std::find_if(graph.edges.begin(), graph.edges.end(),
    [&](const edge& link) { return link.b == id; });
  const auto outbound = std::find_if(graph.edges.begin(), graph.edges.end(),
    [&](const edge& link) { return link.a == id; });
  if (inbound == graph.edges.end() || outbound == graph.edges.end())
    throw std::runtime_error(std::format("site: connector #{} has no two sides", id));
  const auto child_kind_id = graph.nodes[outbound->b].kind;
  const auto& child_kind = site.kinds[child_kind_id];
  const auto child_rule = std::find_if(site.rules.begin(), site.rules.end(),
    [&](const rule_decl& rule) { return rule.kind == child_kind.name; });
  if (child_rule == site.rules.end())
    throw std::runtime_error(std::format("site: connector #{} has no appearance rule", id));
  const std::string_view source = child_rule->appearance_from == "vary" ?
    (roll(seed, id, child_kind_id, 0xd00u) & 1 ? "parent" : "child") :
    std::string_view(child_rule->appearance_from);
  const auto source_id = source == "parent" ? inbound->a : outbound->b;
  const auto& inherited = site.kinds[graph.nodes[source_id].kind].colour;
  const auto& metal = site.kinds[graph.nodes[id].kind].colour;
  const auto colour = ((2 * inherited[0] + metal[0]) / 3 << 16) |
                      ((2 * inherited[1] + metal[1]) / 3 << 8) |
                      ((2 * inherited[2] + metal[2]) / 3);
  return {colour, source_id, source};
}

struct parent_edge {
  uint32_t parent = UINT32_MAX;
  uint32_t connector = UINT32_MAX;
  std::string port;
};

parent_edge parent_of(const graph& graph, const uint32_t child) {
  const auto inbound = std::find_if(graph.edges.begin(), graph.edges.end(),
    [&](const edge& link) { return link.b == child; });
  if (inbound == graph.edges.end())
    throw std::runtime_error(std::format("site layout: area #{} has no parent", child));
  if (!graph.nodes[inbound->a].connector) return {inbound->a, UINT32_MAX, inbound->port};
  const auto into_connector = std::find_if(graph.edges.begin(), graph.edges.end(),
    [&](const edge& link) { return link.b == inbound->a; });
  if (into_connector == graph.edges.end())
    throw std::runtime_error(std::format("site layout: door #{} has no parent", inbound->a));
  return {into_connector->a, inbound->a, inbound->port};
}

uint8_t side_mask(const std::vector<std::string>& sides) {
  uint8_t result = 0;
  for (const auto& side : sides) {
    if (side == "east") result |= devils_engine::originator::motif_east;
    if (side == "west") result |= devils_engine::originator::motif_west;
    if (side == "south") result |= devils_engine::originator::motif_south;
    if (side == "north") result |= devils_engine::originator::motif_north;
  }
  return result;
}

site_view_part precise_part(const site_part_ref ref, std::vector<site_point> outline,
                            const bool seam = false) {
  if (outline.size() < 3)
    throw std::runtime_error("site shape: a convex part needs at least three vertices");
  int32_t left = INT32_MAX, top = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
  for (const auto point : outline) {
    left = std::min(left, point.x);
    top = std::min(top, point.y);
    right = std::max(right, point.x);
    bottom = std::max(bottom, point.y);
  }
  const auto floor_unit = [](const int32_t v) {
    return v >= 0 ? v / site_precision : -((-v + site_precision - 1) / site_precision);
  };
  const auto x = floor_unit(left);
  const auto y = floor_unit(top);
  const auto r = -floor_unit(-right);
  const auto b = -floor_unit(-bottom);
  return {ref, {x, y, r - x, b - y}, std::move(outline), seam};
}

site_view_part polygon_part(const site_part_ref ref, std::vector<site_point> outline,
                            const bool seam = false) {
  for (auto& point : outline) {
    point.x *= site_precision;
    point.y *= site_precision;
  }
  return precise_part(ref, std::move(outline), seam);
}

site_view_part rectangular_part(const site_part_ref ref, const devils_engine::originator::motif_rect r,
                                const bool seam = false) {
  return polygon_part(ref, {{r.x, r.y}, {r.x + r.w, r.y},
    {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}}, seam);
}

struct east_rounding {
  int32_t shoulder = 0;
  int32_t centre_top = 0;
  int32_t centre_bottom = 0;
};

east_rounding rounding_of(const devils_engine::originator::motif_rect r) {
  if (r.w < 8 || r.h < 8) throw std::runtime_error("site shape: rounded side needs at least 8x8");
  return {std::clamp(r.w / 6, 2, 4), r.y + r.h / 2 - 1, r.y + (r.h + 1) / 2 + 1};
}

std::vector<site_view_part> wrap_east_parts(const site_part_ref first,
                                            const devils_engine::originator::motif_rect parent,
                                            const devils_engine::originator::motif_rect reserved) {
  const auto profile = rounding_of(parent);
  const auto right = parent.x + parent.w;
  if (reserved.x != right + 1 || reserved.y != parent.y || reserved.h != parent.h)
    throw std::runtime_error("site shape: wrap reservation is not centred beside its rounded parent");
  const auto outer = reserved.x + reserved.w;
  const auto shoulder = right - profile.shoulder + 1;
  const auto middle = right + 1;
  return {
    polygon_part(first, {{shoulder, parent.y}, {outer, parent.y},
      {outer, profile.centre_top}, {middle, profile.centre_top}}),
    polygon_part({first.area, first.part + 1}, {{middle, profile.centre_top},
      {outer, profile.centre_top}, {outer, profile.centre_bottom}, {middle, profile.centre_bottom}}),
    polygon_part({first.area, first.part + 2}, {{middle, profile.centre_bottom},
      {outer, profile.centre_bottom}, {outer, parent.y + parent.h},
      {shoulder, parent.y + parent.h}})
  };
}

int64_t cross(const site_point a, const site_point b, const site_point c) noexcept {
  return int64_t(b.x - a.x) * (c.y - a.y) - int64_t(b.y - a.y) * (c.x - a.x);
}

bool convex_outline(const site_view_part& part) noexcept {
  if (part.outline.size() < 3) return false;
  int32_t winding = 0;
  for (size_t i = 0; i < part.outline.size(); ++i) {
    const auto turn = cross(part.outline[i], part.outline[(i + 1) % part.outline.size()],
                            part.outline[(i + 2) % part.outline.size()]);
    if (turn == 0) continue;
    const auto sign = turn > 0 ? 1 : -1;
    if (winding != 0 && winding != sign) return false;
    winding = sign;
  }
  return winding > 0;
}

// Окно задано в единицах плана, density — числом пикселей на единицу.
// Дробные центры пикселей проверяются целочисленно, без округления координат портов.
bool inside_part_sample(const site_view_part& part, const int32_t x, const int32_t y,
                        const int32_t origin_x, const int32_t origin_y,
                        const uint32_t density) noexcept {
  for (size_t i = 0; i < part.outline.size(); ++i) {
    const auto a = part.outline[i], b = part.outline[(i + 1) % part.outline.size()];
    const auto sample_y = (int64_t(2) * origin_y * density + 2 * y + 1) * site_precision;
    const auto sample_x = (int64_t(2) * origin_x * density + 2 * x + 1) * site_precision;
    const auto turn = int64_t(b.x - a.x) * (sample_y - int64_t(2) * density * a.y) -
                      int64_t(b.y - a.y) * (sample_x - int64_t(2) * density * a.x);
    if (turn < 0) return false;
    // Полуоткрытая кромка: центр на общем miter-сечении принадлежит ровно одной части.
    if (turn == 0 && (b.y > a.y || (b.y == a.y && b.x < a.x))) return false;
  }
  return true;
}

struct projection {
  devils_engine::originator::motif_layout layout;
  std::vector<uint32_t> instance_nodes;
  std::vector<site_part_ref> instance_parts;
  std::vector<site_view_part> parts;
  std::vector<devils_engine::originator::motif_path_shape> paths; // исходные рецепты; после обрезки истина в parts
  std::vector<site_view_join> joins;
  std::vector<site_path_ending> endings;
  struct route_crossing {
    site_part_ref from;
    site_part_ref to;
    site_point a;
    site_point b;
    bool operator==(const route_crossing&) const = default;
  };
  std::vector<route_crossing> plan_route;
  std::vector<site_route_span> route_spans;
  site_projection_quality quality;
  std::vector<uint32_t> owner; // 0 wall, otherwise graph node id + 1
  std::vector<site_part_ref> part_owner;
  std::vector<uint8_t> exposed;
  struct perceived {
    cue candidate;
    site_point eye;
    site_point target;
    uint32_t door = UINT32_MAX;
    bool operator==(const perceived&) const = default;
  };
  std::vector<perceived> perceptions;
};

struct site_sample {
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint32_t> owner;
  std::vector<site_part_ref> part_owner;
  size_t lost_parts = 0;
};

site_sample sample_site_plan(const projection& plan,
                             const devils_engine::originator::motif_rect window,
                             const uint32_t density) {
  if (density == 0 || density > 16 || window.x < 0 || window.y < 0 ||
      window.w <= 0 || window.h <= 0 ||
      window.x + window.w > plan.layout.width || window.y + window.h > plan.layout.height)
    throw std::runtime_error("site projection: invalid window or density");
  site_sample sampled;
  sampled.width = window.w * int32_t(density);
  sampled.height = window.h * int32_t(density);
  if (int64_t(sampled.width) * sampled.height > 16'000'000)
    throw std::runtime_error("site projection: requested image exceeds 16 million samples");
  sampled.owner.resize(size_t(sampled.width) * sampled.height, 0);
  sampled.part_owner.resize(sampled.owner.size());
  for (const auto& part : plan.parts) {
    const auto left = std::max(window.x, part.bounds.x);
    const auto top = std::max(window.y, part.bounds.y);
    const auto right = std::min(window.x + window.w, part.bounds.x + part.bounds.w);
    const auto bottom = std::min(window.y + window.h, part.bounds.y + part.bounds.h);
    if (left >= right || top >= bottom) continue;
    bool represented = false;
    for (int32_t y = (top - window.y) * int32_t(density);
         y < (bottom - window.y) * int32_t(density); ++y) {
      for (int32_t x = (left - window.x) * int32_t(density);
           x < (right - window.x) * int32_t(density); ++x) {
        if (!inside_part_sample(part, x, y, window.x, window.y, density)) continue;
        const auto cell = size_t(y) * sampled.width + x;
        if (sampled.owner[cell] != 0)
          throw std::runtime_error("site projection: samples from distinct parts overlap");
        sampled.owner[cell] = part.ref.area + 1;
        sampled.part_owner[cell] = part.ref;
        represented = true;
      }
    }
    sampled.lost_parts += !represented;
  }
  return sampled;
}

void check_site_plan(const graph& graph, const projection& image, const bool require_endings = true);
std::vector<projection::route_crossing> trace_plan_route(const graph& graph, const projection& plan);
std::vector<site_route_span> make_route_spans(const projection& plan, const graph& graph);
void trace_plan_perceptions(const site_decl& site, const graph& graph, projection& plan);
void check_plan_facts(const site_decl& site, const graph& graph, const projection& plan);

std::vector<uint8_t> example_surface(const site_bounds bounds, const bool cut) {
  std::vector<uint8_t> surface(size_t(bounds.width) * bounds.height, 0);
  if (!cut) return surface;
  // Не мир и не чанковый генератор, а неизменяемый входной образец окружения.
  for (uint32_t y = 1; y + 1 < bounds.height; ++y) {
    for (uint32_t x = bounds.width / 3; x < bounds.width / 3 + 2; ++x) {
      surface[size_t(y) * bounds.width + x] = 1;
    }
  }
  return surface;
}

struct site_frame {
  site_point origin;
  uint32_t turn = 0;
};

site_point rotate_point(site_point point, const uint32_t turn) {
  for (uint32_t i = 0; i < turn; ++i) {
    point = {-point.y, point.x};
  }
  return point;
}

site_view_part transform_part(const site_view_part& part, const site_frame frame) {
  auto outline = part.outline;
  for (auto& point : outline) {
    point = rotate_point(point, frame.turn);
    point.x += frame.origin.x;
    point.y += frame.origin.y;
  }
  return precise_part(part.ref, std::move(outline), part.seam);
}

uint32_t sample_size(const std::vector<uint32_t>& low, const std::vector<uint32_t>& high,
                     const size_t scale, const uint32_t word) {
  return low[scale] + word % ((high.empty() ? low[scale] : high[scale]) - low[scale] + 1);
}

std::vector<site_view_part> local_parts(const kind_decl& kind, const uint32_t id,
                                        const int32_t w, const int32_t h,
                                        const devils_engine::originator::motif_path_shape& path_shape,
                                        const size_t scale, const uint64_t seed) {
  std::vector<site_view_part> result;
  if (!path_shape.pieces.empty()) {
    for (uint32_t piece = 0; piece < path_shape.pieces.size(); ++piece) {
      result.push_back(polygon_part({id, piece}, path_shape.pieces[piece].outline));
    }
  } else {
    const auto geometry = site_geometry_operations().build(kind.geometry_operation,
      {uint32_t(w), uint32_t(h), seed, kind.geometry_parameters});
    if (!geometry.valid()) { throw std::runtime_error(geometry.refusal); }
    if ((geometry.pieces.size() != 1 && !kind.parts.empty()) ||
        kind.placement.entrance_edge >= geometry.pieces.front().size() ||
        std::any_of(kind.placement.wall_edges.begin(), kind.placement.wall_edges.end(),
          [&](const auto edge) { return edge >= geometry.pieces.front().size(); })) {
      throw std::runtime_error(std::format("site motif '{}': generated pieces do not match the port/extra-part contract",
        kind.motif));
    }
    for (uint32_t piece = 0; piece < geometry.pieces.size(); ++piece) {
      result.push_back(polygon_part({id, piece}, geometry.pieces[piece]));
    }
  }
  std::vector<site_view_part> seams;
  for (uint32_t i = 0; i < kind.parts.size(); ++i) {
    const auto& declaration = kind.parts[i];
    const auto parent = result[declaration.attach_to == "previous" ? i : 0].bounds;
    const auto width = int32_t(sample_size(declaration.width, declaration.width_max, scale, roll(seed, id, i, 1)));
    const auto height = int32_t(sample_size(declaration.height, declaration.height_max, scale, roll(seed, id, i, 2)));
    const auto& side = declaration.sides[roll(seed, id, i, 3) % declaration.sides.size()];
    const auto horizontal = side == "north" || side == "south";
    const auto start = horizontal ? parent.x : parent.y;
    const auto extent = horizontal ? parent.w : parent.h;
    const auto frontage = horizontal ? width : height;
    const auto offset = declaration.align == "start" ? start : declaration.align == "end"    ? start + extent - frontage
                                                             : declaration.align == "centre" ? start + (extent - frontage) / 2
                                                                                             : start - frontage + 1 + int32_t(roll(seed, id, i, 4) % uint32_t(extent + frontage - 1));
    const auto gap = declaration.join == "direct" ? 0 : 1;
    devils_engine::originator::motif_rect rect;
    if (side == "east") rect = {parent.x + parent.w + gap, offset, width, height};
    if (side == "west") rect = {parent.x - width - gap, offset, width, height};
    if (side == "south") rect = {offset, parent.y + parent.h + gap, width, height};
    if (side == "north") rect = {offset, parent.y - height - gap, width, height};
    const auto geometry = site_geometry_operations().build(declaration.shape.empty() ? "rectangle" : declaration.shape,
      {uint32_t(rect.w), uint32_t(rect.h), seed, {}});
    if (!geometry.valid()) { throw std::runtime_error(geometry.refusal); }
    auto outline = geometry.pieces.front();
    for (auto& point : outline) {
      point.x += rect.x;
      point.y += rect.y;
    }
    result.push_back(polygon_part({id, i + 1}, std::move(outline)));
    if (gap == 0) continue;
    const auto common_start = std::max(start, offset);
    const auto common_length = std::min(start + extent, offset + frontage) - common_start;
    const auto position = common_start + (declaration.passage_align == "start" ? 0 : declaration.passage_align == "end" ? common_length - 1
                                                                                                                        : common_length / 2);
    const auto x = horizontal ? position : side == "east" ? parent.x + parent.w
                                                          : parent.x - 1;
    const auto y = horizontal ? (side == "south" ? parent.y + parent.h : parent.y - 1) : position;
    seams.push_back(rectangular_part({id, uint32_t(kind.parts.size() + 1 + seams.size())}, {x, y, 1, 1}, true));
  }
  result.insert(result.end(), seams.begin(), seams.end());
  return result;
}

bool parts_fit(const std::vector<site_view_part>& trial, const projection& image) {
  using devils_engine::originator::convex_interiors_overlap;
  for (size_t i = 0; i < trial.size(); ++i) {
    const auto& part = trial[i];
    if (!convex_outline(part) || part.bounds.x < 1 || part.bounds.y < 1 ||
        part.bounds.x + part.bounds.w >= image.layout.width ||
        part.bounds.y + part.bounds.h >= image.layout.height) return false;
    for (const auto& prior : image.parts) {
      if (convex_interiors_overlap(part.outline, prior.outline)) return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (convex_interiors_overlap(part.outline, trial[j].outline)) return false;
    }
  }
  return true;
}

void append_site_instance(projection& image, const graph& graph, const site_view_part& part,
                          const uint32_t motif, const uint32_t parent) {
  image.instance_nodes.push_back(part.ref.area);
  image.instance_parts.push_back(part.ref);
  image.layout.instances.push_back({part.bounds, motif, graph.nodes[part.ref.area].kind + 1,
    uint32_t(image.layout.instances.size()), parent, true});
}

// После укладки всех соседей, но ДО проекции и перцепции. Защищаются общие отрезки,
// а не только центры дверей: при повороте открытый стык входит в боковину начала пути.
void finish_paths(const site_decl& site, const graph& graph, projection& image,
                   const size_t scale, const uint64_t seed,
                   const std::span<const site_motif_binding> motifs) {
  using namespace devils_engine::originator;
  for (uint32_t id = 0; id < graph.nodes.size(); ++id) {
    const auto& recipe = site.kinds[graph.nodes[id].kind].path;
    if (recipe.mode.empty()) continue;
    for (const auto start : {true, false}) {
      const auto ref = site_part_ref{id, start ? 0u : uint32_t(image.paths[id].pieces.size() - 1)};
      const auto found = std::find_if(image.parts.begin(), image.parts.end(),
        [&](const site_view_part& part) { return part.ref == ref; });
      const auto part_index = size_t(found - image.parts.begin());
      const auto outline = found->outline;
      const auto edge = start ? 3u : 1u;
      const auto& policy = start ? recipe.start : recipe.end;
      site_path_ending ending;
      ending.part = ref;
      ending.start = start;
      ending.policy = policy.policy;
      ending.a = outline[edge];
      ending.b = outline[(edge + 1) % 4];
      const auto on_cap = [&](const site_point p) {
        return ending.a.x == ending.b.x ? p.x == ending.a.x : p.y == ending.a.y;
      };
      std::vector<site_point> protected_points;
      bool connected = false;
      for (const auto& other : image.parts) {
        if (other.ref == ref) continue;
        for (const auto& segment : shared_motif_boundary(outline, other.outline)) {
          protected_points.push_back(segment.a);
          protected_points.push_back(segment.b);
          connected |= on_cap(segment.a) && on_cap(segment.b);
        }
      }
      if (connected) {
        ending.outcome = "connected";
        image.endings.push_back(std::move(ending));
        continue;
      }

      // Даже авторский акцент не должен удерживать длинный незанятый хвост.
      // Сначала сохраняем все использованные боковые стыки и минимальную длину
      // последней части, затем завершаем уже новый свободный торец.
      const auto trimmed = trim_motif_path_end(outline, edge, protected_points,
        policy.clearance * site_precision, policy.minimum_length * site_precision);
      if (!trimmed.valid()) throw std::runtime_error("site ending: " + trimmed.refusal);
      image.parts[part_index] = precise_part(ref, trimmed.outline);
      const auto instance = std::find(image.instance_parts.begin(), image.instance_parts.end(), ref);
      image.layout.instances[size_t(instance - image.instance_parts.begin())].rect = image.parts[part_index].bounds;
      ending.removed = trimmed.removed;
      ending.a = trimmed.outline[edge];
      ending.b = trimmed.outline[(edge + 1) % 4];

      const auto try_accent = policy.policy == "accent" || (policy.policy == "vary" &&
        roll(seed, id, uint32_t(start), 0xe0du) % 100 >= policy.trim_percent);
      if (try_accent) {
        const auto accent = std::find_if(site.endings.begin(), site.endings.end(),
          [&](const ending_decl& candidate) { return candidate.name == policy.accent; });
        const auto width = uint32_t(std::abs(ending.a.x - ending.b.x) + std::abs(ending.a.y - ending.b.y));
        const auto port = make_motif_edge_port(trimmed.outline, edge, width);
        const auto body = attach_motif_rect(port, width + accent->frontage_extra * site_precision,
                                            accent->depth[scale] * site_precision, 0);
        const auto next_part = uint32_t(std::count_if(image.parts.begin(), image.parts.end(),
          [&](const site_view_part& part) { return part.ref.area == id; }));
        const auto accent_ref = site_part_ref{id, next_part};
        bool fits = body.valid();
        site_view_part candidate;
        if (fits) {
          candidate = precise_part(accent_ref, body.body);
          fits = parts_fit({candidate}, image) &&
            std::none_of(image.parts.begin(), image.parts.end(), [&](const site_view_part& prior) {
              return prior.ref != ref && share_motif_boundary(prior.outline, candidate.outline);
            });
        }
        if (fits) {
          const auto motif_index = site.kinds.size() + size_t(accent - site.endings.begin());
          append_site_instance(image, graph, candidate, motifs[motif_index].id,
                               uint32_t(instance - image.instance_parts.begin()));
          image.parts.push_back(std::move(candidate));
          ending.accent_part = accent_ref;
          ending.accent = accent->name;
          ending.colour = motifs[motif_index].colour;
          ending.outcome = "accent";
          image.endings.push_back(std::move(ending));
          continue;
        }
        ending.reason = body.valid() ? "accent collides, touches a neighbour or leaves plot" : body.refusal;
        if (policy.policy == "accent") {
          throw std::runtime_error(std::format("site ending: area #{} {} accent refused: {}",
            id, start ? "start" : "end", ending.reason));
        }
      }

      ending.outcome = "trim";
      image.endings.push_back(std::move(ending));
    }
  }
}

// Решение живёт в полигонах. Габариты экземпляров нужны старому контейнеру и сводкам,
// но не пристыковке. При столкновении перебираются порты, размеры и профили пути, не клетки.
projection place_once(const site_decl& site, const graph& graph, const size_t scale,
                      const int32_t entry_y, const uint64_t layout_seed,
                      const std::span<const site_motif_binding> motifs) {
  using namespace devils_engine::originator;
  projection result;
  result.layout.width = int32_t(site.bounds.width);
  result.layout.height = int32_t(site.bounds.height);
  result.layout.entry_y = entry_y;
  if (entry_y < 1 || entry_y >= result.layout.height - 1) throw std::runtime_error("site: entrance outside plot");
  result.paths.resize(graph.nodes.size());
  std::vector<site_frame> frames(graph.nodes.size());
  std::vector<motif_rect> dimensions(graph.nodes.size());
  std::vector<uint32_t> main_instance(graph.nodes.size(), UINT32_MAX);
  for (uint32_t id = 0; id < graph.nodes.size(); ++id) {
    if (graph.nodes[id].connector) continue;
    const auto& kind = site.kinds[graph.nodes[id].kind];
    const auto rule_it = std::find_if(site.rules.begin(), site.rules.end(),
                                      [&](const rule_decl& rule) {
                                        return rule.kind == kind.name;
                                      });
    const auto& rule = *rule_it;
    bool placed = false;
    std::string last_refusal;
    for (uint32_t attempt = 0; attempt < 96 && !placed; ++attempt) {
      ++result.layout.attempts;
      const auto random = [&](const uint32_t choice) {
        return roll(layout_seed, id, attempt, choice);
      };
      auto w = int32_t(sample_size(kind.width, kind.width_max, scale, random(1)));
      auto h = int32_t(sample_size(kind.height, kind.height_max, scale, random(2)));
      const site_view_join* paired_first = nullptr;
      if (id != 0 && rule.attachment == "paired_wall") {
        const auto parent = parent_of(graph, id);
        for (const auto& join : result.joins) {
          if (join.parent.area == parent.parent && graph.nodes[join.child.area].kind == graph.nodes[id].kind) {
            paired_first = &join;
            w = dimensions[join.child.area].w;
            h = dimensions[join.child.area].h;
            break;
          }
        }
      }
      if (kind.height_policy == "frontage") {
        uint32_t count = 0;
        uint32_t frontage = 0;
        for (uint32_t child = id + 1; child < graph.nodes.size(); ++child) {
          if (graph.nodes[child].connector || parent_of(graph, child).parent != id) continue;
          frontage += site.kinds[graph.nodes[child].kind].height[scale];
          ++count;
        }
        h = std::max(h, int32_t(2 * kind.frontage_margin + frontage +
                                (count == 0 ? 0 : count - 1) * kind.frontage_gap));
      }
      motif_path_shape shape;
      if (!kind.path.mode.empty()) {
        const auto& recipe = kind.path;
        auto bend = recipe.bend[scale];
        if (!recipe.bend_max.empty()) {
          bend += int32_t(random(5) % uint32_t(recipe.bend_max[scale] - bend + 1));
        }
        if (recipe.mirror && (random(6) & 1u)) bend = -bend;
        shape = make_motif_path({uint32_t(w), uint32_t(h),
                                 sample_size(recipe.l1_per_mille, recipe.l1_max, scale, random(3)),
                                 sample_size(recipe.l2_per_mille, recipe.l2_max, scale, random(4)), bend,
                                 recipe.mode == "corner" ? motif_path_mode::corner : motif_path_mode::offset,
                                 recipe.min_bend_length});
        if (!shape.valid()) {
          last_refusal = shape.refusal;
          continue;
        }
      }
      auto local = local_parts(kind, id, w, h, shape, scale, layout_seed ^ attempt);
      site_frame frame;
      std::vector<site_view_part> trial;
      site_view_join join;
      uint32_t attached_instance = no_motif_parent;
      if (id == 0) {
        frame.origin = {2 * site_precision, (entry_y - h / 2) * site_precision};
        for (const auto& part : local) {
          trial.push_back(transform_part(part, frame));
        }
      } else {
        const auto parent = parent_of(graph, id);
        const auto& parent_kind = site.kinds[graph.nodes[parent.parent].kind];
        const auto& parent_path = result.paths[parent.parent];
        struct edge_choice {
          site_part_ref ref;
          uint32_t edge;
          bool cap;
        };
        std::vector<edge_choice> choices;
        if (!parent_path.pieces.empty()) {
          for (const auto& port : parent_path.ports) {
            const auto cap = port.kind != motif_path_port_kind::wall;
            if (rule.attachment == "cap") {
              if (port.kind != motif_path_port_kind::end) continue;
            } else {
              if (cap) continue;
              const auto part = rule.attachment == "bend" || rule.anchor_part == "bend" ? uint32_t(parent_path.pieces.size() > 1) : rule.anchor_part == "last" ? uint32_t(parent_path.pieces.size() - 1)
                                                                                                                                                               : 0u;
              if (port.piece != (rule.attachment == "paired_wall" ? uint32_t(parent_path.pieces.size() - 1) : part)) continue;
              if ((port.edge == 0 && (side_mask(rule.sides) & motif_north) == 0) ||
                  (port.edge == 2 && (side_mask(rule.sides) & motif_south) == 0)) continue;
            }
            choices.push_back({{parent.parent, port.piece}, port.edge, cap});
          }
        } else {

          uint32_t anchor_part = 0;
          if (rule.anchor_part == "last") {
            for (const auto& part : result.parts) {
              if (part.ref.area == parent.parent && !part.seam) {
                anchor_part = std::max(anchor_part, part.ref.part);
              }
            }
          } else if (!rule.anchor_part.empty() && rule.anchor_part != "main") {
            const auto part = std::find_if(parent_kind.parts.begin(), parent_kind.parts.end(),
                                           [&](const part_decl& declaration) {
                                             return declaration.name == rule.anchor_part;
                                           });
            anchor_part = uint32_t(part - parent_kind.parts.begin()) + 1;
          }
          for (const auto& side_name : rule.sides) {
            const auto side_index = side_name == "north" ? 0u : side_name == "east" ? 1u :
              side_name == "south" ? 2u : 3u;
            const auto edge = anchor_part == 0 ? parent_kind.placement.wall_edges[side_index] : side_index;
            choices.push_back({{parent.parent, anchor_part}, edge, false});
          }
        }
        if (choices.empty()) throw std::runtime_error("site: no boundary port matches the attachment rule");
        if (paired_first) {
          std::erase_if(choices, [&](const edge_choice& choice) {
            return choice.edge == paired_first->edge;
          });
        }
        if (choices.empty()) throw std::runtime_error("site: paired port has no opposite wall");
        const auto choice = choices[random(7) % choices.size()];
        const auto found = std::find_if(result.parts.begin(), result.parts.end(),
                                        [&](const site_view_part& part) {
                                          return part.ref == choice.ref;
                                        });
        const auto& anchor = *found;
        const auto instance = std::find(result.instance_parts.begin(), result.instance_parts.end(), choice.ref);
        attached_instance = uint32_t(instance - result.instance_parts.begin());
        uint32_t aperture = uint32_t(h) * site_precision;
        uint32_t gap = 0;
        if (parent.connector != UINT32_MAX) {
          const auto& connector = site.kinds[graph.nodes[parent.connector].kind];
          aperture = sample_size(connector.width, connector.width_max, scale, random(8)) * site_precision;
          gap = sample_size(connector.height, connector.height_max, scale, random(9)) * site_precision;
          if (connector.function == "open_passage" && !shape.pieces.empty()) aperture = h * site_precision;
        } else if (shape.pieces.empty() && !parent_path.pieces.empty()) {
          aperture = parent_path.width * site_precision;
        }
        if (paired_first) {
          aperture = paired_first->width;
          gap = paired_first->depth;
        }
        uint32_t position = random(10) % 1001;
        if (choice.cap || rule.port == "centre" || kind.placement.strategy == "parent_wrap" || rule.attachment == "bend") {
          position = 500;
        } else if (rule.port == "start" || rule.port == "end" || rule.attachment == "paired_wall") {
          // Для боковины центр маленького блока отстоит от конца примерно на половину его фасада.
          const auto a = anchor.outline[choice.edge];
          const auto b = anchor.outline[(choice.edge + 1) % anchor.outline.size()];
          const auto extent = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
          const auto inset = uint32_t(std::min(450, h * site_precision * 500 / std::max(1, extent)));
          const auto start = rule.attachment != "paired_wall" && rule.port == "start";
          position = (start != (choice.edge == 2)) ? inset : 1000 - inset;
        }
        auto port = make_motif_edge_port(anchor.outline, choice.edge, aperture, position,
                                         choice.cap || kind.placement.strategy == "parent_wrap" ? 0 : std::max(1u, rule.passage_inset) * site_precision);
        if (paired_first && port.valid()) {
          const auto vertical = anchor.outline[0].x == anchor.outline[1].x;
          const auto centre_sum = vertical ? anchor.outline[0].x + anchor.outline[3].x : anchor.outline[0].y + anchor.outline[3].y;
          const auto mirror = [&](site_point point) {
            if (vertical)
              point.x = centre_sum - point.x;
            else
              point.y = centre_sum - point.y;
            return point;
          };
          port.a = mirror(paired_first->b);
          port.b = mirror(paired_first->a);
        }
        if (!port.valid()) {
          last_refusal = port.refusal;
          continue;
        }
        join = {choice.ref, {id, kind.placement.strategy == "parent_wrap" ? 1u : 0u}, port.a, port.b, parent.connector, aperture, gap, choice.edge};
        if (kind.placement.strategy == "edge_parallel") {
          const auto attached = attach_motif_rect(port, uint32_t(w) * site_precision,
                                                  uint32_t(h) * site_precision, gap);
          if (!attached.valid()) {
            last_refusal = attached.refusal;
            continue;
          }
          trial.push_back(precise_part({id, 0}, attached.body));
          if (!attached.passage.empty()) trial.push_back(precise_part({parent.connector, 0}, attached.passage));
        } else if (kind.placement.strategy == "parent_wrap") {
          const auto parent_rect = dimensions[parent.parent];
          const auto wrapped = wrap_east_parts({id, 0}, parent_rect,
                                               {parent_rect.w + 1, 0, w, parent_rect.h});
          for (const auto& part : wrapped) {
            trial.push_back(transform_part(part, frames[parent.parent]));
          }
          const auto attached = attach_motif_rect(port, aperture, site_precision, gap);
          if (!attached.valid()) continue;
          trial.push_back(precise_part({parent.connector, 0}, attached.passage));
          frame = frames[parent.parent];
        } else {
          uint32_t entrance_edge = kind.placement.entrance_edge;
          uint32_t entrance_position = 500;
          if (!shape.pieces.empty() && !rule.turns.empty()) {
            const auto& turn = rule.turns[random(11) % rule.turns.size()];
            if (turn == "left") {
              entrance_edge = 0;
              entrance_position = 0;
            } else if (turn == "right") {
              entrance_edge = 2;
              entrance_position = 1000;
            }
          }
          const auto pa = port.a;
          const auto pb = port.b;
          const auto ca = local.front().outline[entrance_edge];
          const auto cb = local.front().outline[(entrance_edge + 1) % local.front().outline.size()];
          const auto dx = pb.x - pa.x;
          const auto dy = pb.y - pa.y;
          bool aligned = false;
          for (uint32_t turn = 0; turn < 4; ++turn) {
            const auto edge = rotate_point({cb.x - ca.x, cb.y - ca.y}, turn);
            if (int64_t(dx) * edge.y == int64_t(dy) * edge.x && int64_t(dx) * edge.x + int64_t(dy) * edge.y < 0) {
              frame.turn = turn;
              aligned = true;
              break;
            }
          }
          if (!aligned) {
            last_refusal = "child template needs an oblique entrance wall";
            continue;
          }
          const auto child_port = make_motif_edge_port(local.front().outline, entrance_edge,
                                                       aperture, entrance_position);
          if (!child_port.valid()) {
            last_refusal = child_port.refusal;
            continue;
          }
          const auto attached = attach_motif_rect(port, aperture, site_precision, gap);
          if (!attached.valid()) continue;
          const auto target = gap == 0 ? port.a : attached.passage[2];
          const auto source = rotate_point(child_port.b, frame.turn);
          frame.origin = {target.x - source.x, target.y - source.y};
          for (const auto& part : local) {
            trial.push_back(transform_part(part, frame));
          }
          if (gap != 0) trial.push_back(precise_part({parent.connector, 0}, attached.passage));
        }
      }
      if (!parts_fit(trial, result)) {
        last_refusal = "convex geometry collides or leaves the plot";
        continue;
      }
      result.paths[id] = std::move(shape);
      if (id != 0) result.joins.push_back(join);
      frames[id] = frame;
      dimensions[id] = {0, 0, w, h};
      main_instance[id] = uint32_t(result.layout.instances.size());
      for (const auto& part : trial) {
        if (part.ref.area == id) {
          append_site_instance(result, graph, part, motifs[graph.nodes[id].kind].id,
                               part.ref.part == 0 ? attached_instance : main_instance[id]);
        }
        result.parts.push_back(part);
      }
      placed = true;
    }
    if (!placed) throw std::runtime_error(std::format("site ports: area #{} ({}) exhausted candidates: {}",
                                                      id, kind.name, last_refusal));
  }
  const auto root_parts = uint32_t(std::count_if(result.parts.begin(), result.parts.end(),
                                                 [](const site_view_part& part) {
                                                   return part.ref.area == 0;
                                                 }));
  result.parts.push_back(rectangular_part({0, root_parts}, {0, entry_y, 2, 1}, true));
  check_site_plan(graph, result, false);
  finish_paths(site, graph, result, scale, layout_seed, motifs);
  check_site_plan(graph, result);
  result.plan_route = trace_plan_route(graph, result);
  result.route_spans = make_route_spans(result, graph);
  trace_plan_perceptions(site, graph, result);
  check_plan_facts(site, graph, result);
  return result;
}

// Проекция не участвует в выборе укладки: она лишь показывает уже принятую геометрию.
void project_site(projection& result, const std::vector<uint8_t>& surface) {
  if (!result.owner.empty() || !result.part_owner.empty() || !result.exposed.empty())
    throw std::runtime_error("site projection: plan was already projected");
  if (surface.size() != size_t(result.layout.width) * result.layout.height)
    throw std::runtime_error("site projection: surface extent differs from plan bounds");
  auto sampled = sample_site_plan(result, {0, 0, result.layout.width, result.layout.height}, 1);
  result.owner = std::move(sampled.owner);
  result.part_owner = std::move(sampled.part_owner);
  result.exposed.resize(result.owner.size(), 0);
  for (size_t cell = 0; cell < result.owner.size(); ++cell) {
    result.exposed[cell] = result.owner[cell] != 0 && surface[cell] != 0;
  }
}

std::pair<uint32_t, uint32_t> pair_key(const uint32_t a, const uint32_t b) {
  return {std::min(a, b), std::max(a, b)};
}

void check_site_plan(const graph& graph, const projection& image, const bool require_endings) {
  using namespace devils_engine::originator;
  if (image.parts.empty()) throw std::runtime_error("site plan: no parts");
  std::set<std::pair<uint32_t, uint32_t>> actual, expected;
  std::set<site_part_ref> refs;
  std::vector<std::vector<size_t>> adjacency(image.parts.size());
  for (const auto& link : graph.edges) {
    expected.insert(pair_key(link.a, link.b));
  }
  for (size_t i = 0; i < image.parts.size(); ++i) {
    const auto& a = image.parts[i];
    if (!convex_outline(a) || a.ref.area >= graph.nodes.size() || !refs.insert(a.ref).second ||
        a.bounds.x < 0 || a.bounds.y < 0 || a.bounds.x + a.bounds.w > image.layout.width ||
        a.bounds.y + a.bounds.h > image.layout.height) {
      throw std::runtime_error("site plan: invalid, duplicate or out-of-bounds part");
    }
    for (size_t j = 0; j < i; ++j) {
      const auto& b = image.parts[j];
      if (convex_interiors_overlap(a.outline, b.outline)) {
        throw std::runtime_error("site plan: convex interiors overlap");
      }
      if (!share_motif_boundary(a.outline, b.outline)) continue;
      adjacency[i].push_back(j);
      adjacency[j].push_back(i);
      if (a.ref.area != b.ref.area) actual.insert(pair_key(a.ref.area, b.ref.area));
    }
  }
  if (actual != expected) {
    for (const auto& pair : expected) {
      if (!actual.contains(pair)) {
        throw std::runtime_error(std::format("site plan: #{}--#{} has no shared boundary segment",
                                             pair.first, pair.second));
      }
    }
    throw std::runtime_error("site plan: an undeclared boundary connection appears");
  }
  std::vector<uint8_t> reached(image.parts.size(), 0);
  std::queue<size_t> pending;
  reached[0] = 1;
  pending.push(0);
  while (!pending.empty()) {
    const auto here = pending.front();
    pending.pop();
    for (const auto next : adjacency[here]) {
      if (reached[next]) continue;
      reached[next] = 1;
      pending.push(next);
    }
  }
  if (std::find(reached.begin(), reached.end(), uint8_t(0)) != reached.end()) {
    throw std::runtime_error("site plan: a convex part cannot be reached from the entrance");
  }
  for (uint32_t area = 0; area < graph.nodes.size(); ++area) {
    const auto first = std::find_if(image.parts.begin(), image.parts.end(),
                                    [&](const site_view_part& part) {
                                      return part.ref.area == area;
                                    });
    if (first == image.parts.end()) throw std::runtime_error("site plan: an area has no geometry");
    reached.assign(image.parts.size(), 0);
    const auto start = size_t(first - image.parts.begin());
    reached[start] = 1;
    pending.push(start);
    while (!pending.empty()) {
      const auto here = pending.front();
      pending.pop();
      for (const auto next : adjacency[here]) {
        if (image.parts[next].ref.area != area || reached[next]) continue;
        reached[next] = 1;
        pending.push(next);
      }
    }
    uint32_t count = 0;
    for (size_t i = 0; i < image.parts.size(); ++i) {
      if (image.parts[i].ref.area != area) continue;
      ++count;
      if (!reached[i]) throw std::runtime_error(std::format("site plan: area #{} has disconnected parts", area));
    }
    if (graph.nodes[area].connector && count != 1) throw std::runtime_error("site plan: a door needs one convex part");
    for (uint32_t part = 0; part < count; ++part) {
      if (!refs.contains({area, part})) throw std::runtime_error("site plan: part_ref sequence has a gap");
    }
  }
  const auto part_at = [&](const site_part_ref ref) -> const site_view_part& {
    const auto found = std::find_if(image.parts.begin(), image.parts.end(),
                                    [&](const site_view_part& part) {
                                      return part.ref == ref;
                                    });
    if (found == image.parts.end()) throw std::runtime_error("site plan: port names a missing part");
    return *found;
  };
  for (const auto& join : image.joins) {
    const auto& parent = part_at(join.parent);
    const auto& child = part_at(join.child);
    const auto& path_shape = image.paths[join.parent.area];
    if (!path_shape.pieces.empty() && std::none_of(path_shape.ports.begin(), path_shape.ports.end(),
                                                   [&](const motif_path_port& port) {
                                                     return port.piece == join.parent.part && port.edge == join.edge;
                                                   })) {
      throw std::runtime_error("site plan: attachment uses an internal path section instead of a port");
    }
    const auto boundary = [&](const site_point point) {
      const auto a = parent.outline[join.edge];
      const auto b = parent.outline[(join.edge + 1) % parent.outline.size()];
      return cross(a, b, point) == 0 && point.x >= std::min(a.x, b.x) && point.x <= std::max(a.x, b.x) &&
             point.y >= std::min(a.y, b.y) && point.y <= std::max(a.y, b.y);
    };
    if (join.edge >= parent.outline.size() || join.a == join.b || !boundary(join.a) || !boundary(join.b)) {
      throw std::runtime_error("site plan: port is not a segment of the actual parent wall");
    }
    if (join.connector == UINT32_MAX) {
      if (!share_motif_boundary(parent.outline, child.outline)) {
        throw std::runtime_error("site plan: open join has no common edge");
      }
    } else {
      const auto& door = part_at({join.connector, 0});
      if (!share_motif_boundary(parent.outline, door.outline) || !share_motif_boundary(child.outline, door.outline)) {
        throw std::runtime_error(std::format("site plan: door #{} does not meet ({},{}) and ({},{})",
                                             join.connector, join.parent.area, join.parent.part, join.child.area, join.child.part));
      }
    }
  }
  if (require_endings) {
    std::set<std::pair<uint32_t, bool>> ends;
    for (const auto& ending : image.endings) {
      const auto& part = part_at(ending.part);
      const auto edge = ending.start ? 3u : 1u;
      if (!ends.emplace(ending.part.area, ending.start).second || part.outline.size() != 4 ||
          ending.a != part.outline[edge] || ending.b != part.outline[(edge + 1) % 4]) {
        throw std::runtime_error("site plan: path ending disagrees with its actual cap");
      }
      if (ending.outcome == "accent") {
        const auto& accent = part_at(ending.accent_part);
        if (ending.accent.empty() || ending.accent_part.area != ending.part.area ||
            !share_motif_boundary(part.outline, accent.outline)) {
          throw std::runtime_error("site plan: authored ending is not part of its functional area");
        }
      } else if (ending.outcome != "connected" && ending.outcome != "trim") {
        throw std::runtime_error("site plan: free path cap has no completion policy result");
      }
      if (ending.outcome == "connected") {
        bool connected = false;
        for (const auto& other : image.parts) {
          if (other.ref == part.ref) continue;
          for (const auto& segment : shared_motif_boundary(part.outline, other.outline)) {
            const auto on_cap = [&](const site_point p) {
              return cross(ending.a, ending.b, p) == 0;
            };
            connected |= on_cap(segment.a) && on_cap(segment.b);
          }
        }
        if (!connected || ending.removed != 0 || ending.accent_part.valid()) {
          throw std::runtime_error("site plan: occupied path cap was changed or lost its connection");
        }
      }
    }
    for (uint32_t id = 0; id < image.paths.size(); ++id) {
      if (!image.paths[id].pieces.empty() && (!ends.contains({id, true}) || !ends.contains({id, false}))) {
        throw std::runtime_error("site plan: a path needs separate start and end outcomes");
      }
    }
  }
}

std::vector<projection::route_crossing> trace_plan_route(const graph& graph, const projection& plan) {
  using devils_engine::originator::shared_motif_boundary;
  if (graph.route.empty()) throw std::runtime_error("site route: graph has no route");
  std::vector<uint32_t> route_index(graph.nodes.size(), UINT32_MAX);
  for (uint32_t i = 0; i < graph.route.size(); ++i) route_index[graph.route[i]] = i;
  const auto pair_of = [](site_part_ref a, site_part_ref b) {
    if (b < a) std::swap(a, b);
    return std::pair{a, b};
  };
  std::set<std::pair<site_part_ref, site_part_ref>> declared_crossings;
  for (const auto& join : plan.joins) {
    if (join.connector == UINT32_MAX) {
      declared_crossings.insert(pair_of(join.parent, join.child));
    } else {
      const site_part_ref door{join.connector, 0};
      declared_crossings.insert(pair_of(join.parent, door));
      declared_crossings.insert(pair_of(door, join.child));
    }
  }
  const auto find_part = [&](const site_part_ref ref) {
    const auto found = std::find_if(plan.parts.begin(), plan.parts.end(),
      [&](const site_view_part& part) { return part.ref == ref; });
    if (found == plan.parts.end()) throw std::runtime_error("site route: endpoint has no primary part");
    return size_t(found - plan.parts.begin());
  };
  const auto start = find_part({graph.route.front(), 0});
  const auto goal = find_part({graph.route.back(), 0});
  std::vector<size_t> previous(plan.parts.size(), SIZE_MAX);
  std::vector<devils_engine::originator::motif_boundary_segment> portals(plan.parts.size());
  std::queue<size_t> pending;
  previous[start] = start;
  pending.push(start);
  while (!pending.empty() && previous[goal] == SIZE_MAX) {
    const auto here = pending.front();
    pending.pop();
    const auto from_index = route_index[plan.parts[here].ref.area];
    for (size_t next = 0; next < plan.parts.size(); ++next) {
      if (previous[next] != SIZE_MAX || next == here) continue;
      const auto to_index = route_index[plan.parts[next].ref.area];
      if (from_index == UINT32_MAX || to_index == UINT32_MAX ||
          (from_index != to_index && from_index + 1 != to_index)) continue;
      if (from_index != to_index &&
          !declared_crossings.contains(pair_of(plan.parts[here].ref, plan.parts[next].ref))) continue;
      const auto shared = shared_motif_boundary(plan.parts[here].outline, plan.parts[next].outline);
      if (shared.empty()) continue;
      previous[next] = here;
      portals[next] = shared.front();
      pending.push(next);
    }
  }
  if (previous[goal] == SIZE_MAX)
    throw std::runtime_error("site route: graph route has no continuous chain of convex parts");
  std::vector<projection::route_crossing> result;
  for (auto at = goal; at != start; at = previous[at]) {
    const auto from = previous[at];
    result.push_back({plan.parts[from].ref, plan.parts[at].ref, portals[at].a, portals[at].b});
  }
  std::reverse(result.begin(), result.end());
  std::vector<uint32_t> traversed{graph.route.front()};
  for (const auto& crossing : result) {
    if (crossing.a == crossing.b)
      throw std::runtime_error("site route: a portal collapsed to a point");
    if (traversed.back() != crossing.to.area) traversed.push_back(crossing.to.area);
  }
  if (traversed != graph.route)
    throw std::runtime_error("site route: convex chain takes a different route through the location");
  return result;
}

bool inside_part_exact(const site_view_part& part, const site_point point) {
  for (size_t i = 0; i < part.outline.size(); ++i)
    if (cross(part.outline[i], part.outline[(i + 1) % part.outline.size()], point) < 0)
      return false;
  return true;
}

site_point part_centre(const site_view_part& part) {
  int64_t x = 0, y = 0;
  for (const auto vertex : part.outline) { x += vertex.x; y += vertex.y; }
  const site_point centre{int32_t(x / int64_t(part.outline.size())),
                          int32_t(y / int64_t(part.outline.size()))};
  if (!inside_part_exact(part, centre))
    throw std::runtime_error("site route: rounded convex centre left its part");
  return centre;
}

site_point point_along(const site_point a, const site_point b, const uint32_t numerator,
                       const uint32_t denominator) {
  const auto dx = b.x - a.x, dy = b.y - a.y;
  const auto steps = std::gcd(std::abs(dx), std::abs(dy));
  if (steps == 0) return a;
  const auto step = int64_t(steps) * numerator / denominator;
  return {int32_t(a.x + int64_t(dx / steps) * step),
          int32_t(a.y + int64_t(dy / steps) * step)};
}

std::vector<site_route_span> make_route_spans(const projection& plan, const graph& graph) {
  const auto part_at = [&](const site_part_ref ref) -> const site_view_part& {
    const auto found = std::find_if(plan.parts.begin(), plan.parts.end(),
      [&](const site_view_part& part) { return part.ref == ref; });
    if (found == plan.parts.end()) throw std::runtime_error("site route: a crossing lost its convex part");
    return *found;
  };
  const auto first = plan.plan_route.empty() ? site_part_ref{graph.route.front(), 0} : plan.plan_route.front().from;
  auto current = part_centre(part_at(first));
  std::vector<site_route_span> spans;
  for (const auto& crossing : plan.plan_route) {
    const auto portal = point_along(crossing.a, crossing.b, 1, 2);
    if (!inside_part_exact(part_at(crossing.from), portal) ||
        !inside_part_exact(part_at(crossing.to), portal))
      throw std::runtime_error("site route: midpoint left its shared portal");
    if (current != portal) spans.push_back({crossing.from, current, portal});
    const auto next = part_centre(part_at(crossing.to));
    if (portal != next) spans.push_back({crossing.to, portal, next});
    current = next;
  }
  if (spans.empty()) throw std::runtime_error("site route: no drawable path through its parts");
  for (size_t i = 1; i < spans.size(); ++i)
    if (spans[i - 1].b != spans[i].a)
      throw std::runtime_error("site route: drawable segments do not meet");
  return spans;
}

struct rational {
  int64_t numerator = 0;
  int64_t denominator = 1; // always positive
};

bool less_than(const rational a, const rational b) {
  return __int128(a.numerator) * b.denominator < __int128(b.numerator) * a.denominator;
}

struct ray_interval { rational first, last; };

std::optional<ray_interval> clip_ray(const site_view_part& part, const site_point eye,
                                     const site_point target) {
  ray_interval result{{0, 1}, {1, 1}};
  for (size_t i = 0; i < part.outline.size(); ++i) {
    const auto a = part.outline[i], b = part.outline[(i + 1) % part.outline.size()];
    const auto at_eye = cross(a, b, eye);
    const auto slope = int64_t(b.x - a.x) * (target.y - eye.y) -
                       int64_t(b.y - a.y) * (target.x - eye.x);
    if (slope == 0) {
      if (at_eye < 0) return std::nullopt;
      continue;
    }
    if (slope > 0) {
      const rational limit{-at_eye, slope};
      if (less_than(result.first, limit)) result.first = limit;
    } else {
      const rational limit{at_eye, -slope};
      if (less_than(limit, result.last)) result.last = limit;
    }
    if (less_than(result.last, result.first)) return std::nullopt;
  }
  return result;
}

bool clear_geometric_sight(const projection& plan, const site_point eye, const site_point target,
                           const uint32_t witness, const uint32_t door, const uint32_t via) {
  std::vector<ray_interval> before, after;
  std::optional<ray_interval> doorway;
  for (const auto& part : plan.parts) {
    if (part.ref.area != witness && part.ref.area != door && part.ref.area != via) continue;
    const auto clipped = clip_ray(part, eye, target);
    if (!clipped) continue;
    if (part.ref.area == witness) before.push_back(*clipped);
    else if (part.ref.area == via) after.push_back(*clipped);
    else doorway = *clipped;
  }
  if (!doorway || !less_than(doorway->first, doorway->last) ||
      !less_than({0, 1}, doorway->first)) return false;
  const auto covers = [](std::vector<ray_interval>& intervals, const rational from, const rational to) {
    std::sort(intervals.begin(), intervals.end(), [](const ray_interval& a, const ray_interval& b) {
      return less_than(a.first, b.first);
    });
    auto reached = from;
    for (const auto& interval : intervals) {
      if (less_than(reached, interval.first)) return false;
      if (less_than(reached, interval.last)) reached = interval.last;
      if (!less_than(reached, to)) return true;
    }
    return !less_than(reached, to);
  };
  return covers(before, {0, 1}, doorway->first) &&
         covers(after, doorway->last, {1, 1});
}

void trace_plan_perceptions(const site_decl& site, const graph& graph, projection& plan) {
  for (const auto& candidate : graph.cues) {
    const auto door = path(graph, candidate.witness, candidate.route_via)[1];
    const auto door_part = std::find_if(plan.parts.begin(), plan.parts.end(),
      [&](const site_view_part& part) { return part.ref == site_part_ref{door, 0}; });
    if (door_part == plan.parts.end()) throw std::runtime_error("site perception: technical door has no part");
    const auto door_centre = part_centre(*door_part);
    bool seen = false;
    for (const auto& witness_part : plan.parts) {
      if (witness_part.ref.area != candidate.witness) continue;
      for (const auto& portal : devils_engine::originator::shared_motif_boundary(
             witness_part.outline, door_part->outline)) {
        const auto aperture = point_along(portal.a, portal.b, 1, 2);
        const auto eye = point_along(aperture, part_centre(witness_part), 1, 2);
        for (const auto& span : plan.route_spans) {
          if (span.part.area != candidate.route_via) continue;
          for (const uint32_t step : {0u, 1u, 2u, 3u, 4u}) {
            const auto target = point_along(span.a, span.b, step, 4);
            const auto distance = std::abs(int64_t(target.x) - door_centre.x) +
                                  std::abs(int64_t(target.y) - door_centre.y);
            if (distance > int64_t(site.perception.max_distance) * site_precision ||
                !clear_geometric_sight(plan, eye, target, candidate.witness, door, candidate.route_via))
              continue;
            plan.perceptions.push_back({candidate, eye, target, door});
            seen = true;
            break;
          }
          if (seen) break;
        }
        if (seen) break;
      }
      if (seen) break;
    }
  }
}

void check_plan_facts(const site_decl& site, const graph& graph, const projection& plan) {
  if (plan.route_spans.empty()) throw std::runtime_error("site plan: route has no geometric segments");
  std::vector<uint32_t> traversed;
  for (size_t i = 0; i < plan.route_spans.size(); ++i) {
    const auto& span = plan.route_spans[i];
    const auto part = std::find_if(plan.parts.begin(), plan.parts.end(),
      [&](const site_view_part& shape) { return shape.ref == span.part; });
    if (part == plan.parts.end() || span.a == span.b ||
        !inside_part_exact(*part, span.a) || !inside_part_exact(*part, span.b) ||
        (i != 0 && plan.route_spans[i - 1].b != span.a))
      throw std::runtime_error("site plan: route segment escapes its convex part or breaks continuity");
    if (traversed.empty() || traversed.back() != span.part.area) traversed.push_back(span.part.area);
  }
  if (traversed != graph.route)
    throw std::runtime_error("site plan: route segments visit different areas than the declared route");
  for (const auto& perceived : plan.perceptions) {
    const auto& cue = perceived.candidate;
    if (std::find(graph.cues.begin(), graph.cues.end(), cue) == graph.cues.end() ||
        perceived.door != path(graph, cue.witness, cue.route_via)[1])
      throw std::runtime_error("site plan: perception has no declared candidate or door");
    const auto door = std::find_if(plan.parts.begin(), plan.parts.end(),
      [&](const site_view_part& part) { return part.ref == site_part_ref{perceived.door, 0}; });
    if (door == plan.parts.end()) throw std::runtime_error("site plan: perception door has no geometry");
    const auto door_centre = part_centre(*door);
    const auto distance = std::abs(int64_t(perceived.target.x) - door_centre.x) +
                          std::abs(int64_t(perceived.target.y) - door_centre.y);
    const auto on_route = std::any_of(plan.route_spans.begin(), plan.route_spans.end(),
      [&](const site_route_span& span) {
        return span.part.area == cue.route_via && cross(span.a, span.b, perceived.target) == 0 &&
          perceived.target.x >= std::min(span.a.x, span.b.x) &&
          perceived.target.x <= std::max(span.a.x, span.b.x) &&
          perceived.target.y >= std::min(span.a.y, span.b.y) &&
          perceived.target.y <= std::max(span.a.y, span.b.y);
      });
    const auto in_witness = std::any_of(plan.parts.begin(), plan.parts.end(),
      [&](const site_view_part& part) {
        return part.ref.area == cue.witness && inside_part_exact(part, perceived.eye);
      });
    if (!in_witness || !on_route || distance > int64_t(site.perception.max_distance) * site_precision ||
        !clear_geometric_sight(plan, perceived.eye, perceived.target,
                               cue.witness, perceived.door, cue.route_via))
      throw std::runtime_error("site plan: perception lacks a clear geometric ray to the route");
  }
}

site_projection_quality check_projection(const site_decl& site, const graph& graph, const projection& image,
                                         const std::vector<uint8_t>& surface) {
  check_site_plan(graph, image);
  check_plan_facts(site, graph, image);
  const auto width = size_t(image.layout.width);
  const auto extent = width * image.layout.height;
  if (image.owner.size() != extent || image.part_owner.size() != extent ||
      image.exposed.size() != extent || surface.size() != extent)
    throw std::runtime_error("site projection: image arrays disagree about their extent");
  const auto replay = sample_site_plan(image, {0, 0, image.layout.width, image.layout.height}, 1);
  if (image.owner != replay.owner || image.part_owner != replay.part_owner)
    throw std::runtime_error("site projection: sampled owners disagree with the exact parts");
  site_projection_quality quality;
  quality.lost_parts = replay.lost_parts;
  std::set<std::pair<uint32_t, uint32_t>> actual, expected;
  for (const auto& edge : graph.edges) expected.insert(pair_key(edge.a, edge.b));
  for (size_t cell = 0; cell < extent; ++cell) {
    const auto owner = image.owner[cell];
    if (image.exposed[cell] != uint8_t(owner != 0 && surface[cell] != 0))
      throw std::runtime_error("site projection: a surface contact was lost or invented");
    if (owner == 0) continue;
    const auto x = cell % width, y = cell / width;
    for (const auto next : {x + 1 < width ? cell + 1 : extent,
                            y + 1 < size_t(image.layout.height) ? cell + width : extent}) {
      if (next == extent || image.owner[next] == 0 || image.owner[next] == owner) continue;
      actual.insert(pair_key(owner - 1, image.owner[next] - 1));
    }
  }
  for (const auto& link : actual) quality.false_links += !expected.contains(link);
  for (const auto& link : expected) quality.missing_links += !actual.contains(link);
  std::vector<uint8_t> reached(extent, 0);
  std::queue<size_t> pending;
  const auto entry = size_t(image.layout.entry_y) * width;
  if (image.owner[entry] != 0) { reached[entry] = 1; pending.push(entry); }
  while (!pending.empty()) {
    const auto cell = pending.front(); pending.pop();
    const auto x = cell % width, y = cell / width;
    for (const auto next : {x > 0 ? cell - 1 : extent, x + 1 < width ? cell + 1 : extent,
                            y > 0 ? cell - width : extent,
                            y + 1 < size_t(image.layout.height) ? cell + width : extent}) {
      if (next == extent || image.owner[next] == 0 || reached[next]) continue;
      reached[next] = 1;
      pending.push(next);
    }
  }
  for (size_t cell = 0; cell < extent; ++cell)
    quality.unreachable_cells += image.owner[cell] != 0 && !reached[cell];
  return quality;
}

uint64_t site_layout_seed(const uint64_t seed, const uint32_t variant) {
  return variant == 0 ? seed :
    (uint64_t(roll(seed, 0x5a9eu, variant, 0)) << 32) | roll(seed, 0x5a9eu, variant, 1);
}

std::vector<uint8_t> projected_route_mask(const projection& image) {
  std::vector<uint8_t> mask(image.owner.size(), 0);
  const auto width = image.layout.width, height = image.layout.height;
  for (const auto& span : image.route_spans) {
    const auto left = std::max(0, std::min(span.a.x, span.b.x) / site_precision - 1);
    const auto top = std::max(0, std::min(span.a.y, span.b.y) / site_precision - 1);
    const auto right = std::min(width - 1, std::max(span.a.x, span.b.x) / site_precision + 1);
    const auto bottom = std::min(height - 1, std::max(span.a.y, span.b.y) / site_precision + 1);
    const int64_t dx = int64_t(span.b.x) - span.a.x, dy = int64_t(span.b.y) - span.a.y;
    const auto length_squared = dx * dx + dy * dy;
    for (int32_t y = top; y <= bottom; ++y)
      for (int32_t x = left; x <= right; ++x) {
        const auto cell = size_t(y) * width + x;
        if (image.owner[cell] != span.part.area + 1) continue;
        const int64_t px = int64_t(x) * site_precision + site_precision / 2 - span.a.x;
        const int64_t py = int64_t(y) * site_precision + site_precision / 2 - span.a.y;
        const auto dot = px * dx + py * dy;
        bool near = false;
        if (dot <= 0 || dot >= length_squared) {
          const auto rx = dot <= 0 ? px : px - dx;
          const auto ry = dot <= 0 ? py : py - dy;
          near = rx * rx + ry * ry <= int64_t(site_precision / 2) * (site_precision / 2);
        } else {
          const auto area = px * dy - py * dx;
          near = __int128(area) * area <=
            __int128(site_precision / 2) * (site_precision / 2) * length_squared;
        }
        if (near) mask[cell] = 1;
      }
  }
  return mask;
}

projection place(const site_decl& site, const graph& graph, const size_t scale,
                 const int32_t entry_y, const uint64_t seed,
                 const std::span<const site_motif_binding> motifs) {
  std::string last_refusal;
  std::string first_refusal;
  constexpr uint32_t max_layout_variants = 256;
  for (uint32_t variant = 0; variant < max_layout_variants; ++variant) {
    const auto layout_seed = site_layout_seed(seed, variant);
    try {
      return place_once(site, graph, scale, entry_y, layout_seed, motifs);
    } catch (const std::exception& error) {
      if (first_refusal.empty()) first_refusal = error.what();
      last_refusal = error.what();
    }
  }
  throw std::runtime_error(std::format(
    "site shape: {} deterministic layouts refused; first: {}; last: {}",
    max_layout_variants, first_refusal, last_refusal));
}

// Отображение не выбирает новую укладку даже при потерях сэмплирования.
projection make_site_projection(const site_decl& site, const graph& graph, const size_t scale,
                                const int32_t entry_y, const uint64_t seed,
                                const std::vector<uint8_t>& surface,
                                const std::span<const site_motif_binding> motifs) {
  auto image = place(site, graph, scale, entry_y, seed, motifs);
  project_site(image, surface);
  image.quality = check_projection(site, graph, image, surface);
  return image;
}

void display(const site_decl& site, const graph& graph, const projection& image,
             const bool ascii,
             const std::string_view dump, const uint64_t seed) {
  const auto width = size_t(image.layout.width);
  const auto route_cells = projected_route_mask(image);
  const auto glyph_at = [&](const size_t index) {
    if (image.exposed[index] != 0) return '^';
    if (image.owner[index] == 0) return '#';
    if (graph.nodes[image.owner[index] - 1].connector)
      return site.kinds[graph.nodes[image.owner[index] - 1].kind].glyph[0];
    if (route_cells[index] != 0) return '*';
    return site.kinds[graph.nodes[image.owner[index] - 1].kind].glyph[0];
  };
  if (ascii)
    for (size_t y = 0; y < size_t(image.layout.height); ++y) {
      for (size_t x = 0; x < width; ++x) std::cout << glyph_at(y * width + x);
      std::cout << '\n';
    }
  if (!dump.empty()) {
    std::ofstream out(std::string(dump), std::ios::binary);
    if (!out) throw std::runtime_error("site layout: cannot open PPM output");
    out << "P6\n" << image.layout.width << ' ' << image.layout.height << "\n255\n";
    for (size_t index = 0; index < image.owner.size(); ++index) {
      uint32_t rgb = 0x17191du;
      if (image.owner[index] != 0) {
        const auto id = image.owner[index] - 1;
        const auto& colour = site.kinds[graph.nodes[id].kind].colour;
        rgb = graph.nodes[id].connector ? appearance_of_connector(site, graph, id, seed).colour :
          colour[0] << 16 | colour[1] << 8 | colour[2];
      }
      if (route_cells[index] != 0 && image.owner[index] != 0 &&
          !graph.nodes[image.owner[index] - 1].connector) rgb = 0x78c4b8u;
      if (image.exposed[index] != 0) rgb = 0xb8c8d9u;
      const char bytes[3]{char(rgb >> 16), char(rgb >> 8), char(rgb)};
      out.write(bytes, 3);
    }
    if (!out) throw std::runtime_error("site layout: failed writing PPM output");
  }
}

void report(const site_decl& site, const graph& graph, const projection& image,
            const uint64_t seed, const std::string_view scale) {
  std::cout << "site=" << site.name << " theme=" << site.theme << " rules_version=" << site.version
            << " motifs_version=" << site.motifs.version
            << " scale=" << scale << " seed=" << seed << "\n";
  std::cout << "areas=" << graph.nodes.size() << " links=" << graph.edges.size()
            << " perception_candidates=" << graph.cues.size()
            << " perceptions=" << image.perceptions.size() << " footprint="
            << image.layout.width << 'x' << image.layout.height
            << " layout_attempts=" << image.layout.attempts
            << " projection=lost_parts:" << image.quality.lost_parts
            << ",false_links:" << image.quality.false_links
            << ",missing_links:" << image.quality.missing_links
            << ",unreachable_cells:" << image.quality.unreachable_cells
            << " exposures=" << std::count(image.exposed.begin(), image.exposed.end(), uint8_t(1)) << "\n";
  for (uint32_t id = 0; id < graph.nodes.size(); ++id) {
    std::cout << "  #" << id << " " << site.kinds[graph.nodes[id].kind].category << " "
              << site.kinds[graph.nodes[id].kind].name;
    for (const auto& part : image.parts)
      if (part.ref.area == id)
        std::cout << " part[" << part.ref.part << "]=(" << part.bounds.x << ','
                  << part.bounds.y << ',' << part.bounds.w << ',' << part.bounds.h << ')';
    std::cout << '\n';
  }
  for (const auto& edge : graph.edges)
    std::cout << "  #" << edge.a << " --[" << edge.port << "]--> #" << edge.b << "\n";
  std::cout << "route:";
  for (const auto id : graph.route) std::cout << " #" << id;
  std::cout << "\n";
  for (const auto& perceived : image.perceptions) {
    std::cout << "perception: dialogue_template=" << perceived.candidate.token
              << " witness=#" << perceived.candidate.witness
              << " route_via=#" << perceived.candidate.route_via
              << " seen_at=(" << double(perceived.target.x) / site_precision << ','
              << double(perceived.target.y) / site_precision << ")\n";
  }
  for (const auto& ending : image.endings) {
    std::cout << "ending: area=#" << ending.part.area << ' ' << (ending.start ? "start" : "end")
              << " policy=" << ending.policy << " outcome=" << ending.outcome
              << " removed=" << double(ending.removed) / site_precision;
    if (!ending.accent.empty()) std::cout << " accent=" << ending.accent;
    if (!ending.reason.empty()) std::cout << " fallback=" << ending.reason;
    std::cout << '\n';
  }
}

} // namespace

site_view_scene make_site_view_scene(const uint64_t seed, const std::string_view scale,
                                     const int32_t requested_entry_y, const bool surface_cut,
                                     const std::string_view source,
                                     const std::string_view motifs_source, const site_bounds bounds) {
  auto site = read_site(source, motifs_source);
  const auto scale_it = std::find(site.scales.begin(), site.scales.end(), scale);
  if (scale_it == site.scales.end())
    throw std::runtime_error(std::format("site: unknown scale '{}'", scale));
  const auto scale_index = size_t(scale_it - site.scales.begin());
  const auto motifs = motif_bindings(site);
  const auto graph = assemble(site, scale_index, seed);
  check(site, graph);
  site.bounds = resolve_bounds(site, scale_index, bounds);
  const auto entry_y = requested_entry_y == -1 ? int32_t(site.bounds.height / 2) : requested_entry_y;
  const auto surface = example_surface(site.bounds, surface_cut);
  auto image = make_site_projection(site, graph, scale_index, entry_y, seed, surface, motifs);

  site_view_scene scene;
  scene.scale = std::string(scale);
  scene.seed = seed;
  scene.catalogue_version = site.version;
  scene.motif_catalogue_version = site.motifs.version;
  scene.perceptions = image.perceptions.size();
  scene.owner = std::move(image.owner);
  scene.part_owner = std::move(image.part_owner);
  scene.exposed = std::move(image.exposed);
  scene.route_spans = image.route_spans;
  scene.projection_quality = image.quality;
  scene.zones.resize(graph.nodes.size());
  scene.joins = image.joins;
  scene.endings = image.endings;
  for (uint32_t id = 0; id < graph.nodes.size(); ++id) {
    auto& zone = scene.zones[id];
    const auto& kind = site.kinds[graph.nodes[id].kind];
    zone.connector = graph.nodes[id].connector;
    zone.glyph = kind.glyph[0];
    zone.colour = (kind.colour[0] << 16) | (kind.colour[1] << 8) | kind.colour[2];
    if (zone.connector) {
      const auto appearance = appearance_of_connector(site, graph, id, seed);
      zone.colour = appearance.colour;
      zone.details.push_back(std::format("connector appearance: {} from area #{} ({})",
        appearance.source, appearance.inherited_from,
        site.kinds[graph.nodes[appearance.inherited_from].kind].name));
    }
    for (const auto& part : image.parts)
      if (part.ref.area == id) {
        zone.parts.push_back(part);
        const auto& r = part.bounds;
        if (zone.parts.size() == 1) zone.bounds = r;
        else {
          const auto left = std::min(zone.bounds.x, r.x);
          const auto top = std::min(zone.bounds.y, r.y);
          const auto right = std::max(zone.bounds.x + zone.bounds.w, r.x + r.w);
          const auto bottom = std::max(zone.bounds.y + zone.bounds.h, r.y + r.h);
          zone.bounds = {left, top, right - left, bottom - top};
        }
      }
    if (zone.parts.empty())
      throw std::runtime_error(std::format("site viewer: area #{} has no convex parts", id));
    zone.details.push_back(std::format("area #{}  {} / {}  function {}", id, kind.category,
      kind.name, kind.function));
    zone.details.push_back(std::format("motif {}  {} convex part(s)", kind.motif, zone.parts.size()));
    if (!kind.path.mode.empty()) {
      zone.details.push_back(std::format("path recipe {}  width {}  L1 {}/1000  L2 {}/1000  M {}",
        kind.path.mode, image.paths[id].width, image.paths[id].parameters.l1_per_mille,
        image.paths[id].parameters.l2_per_mille, image.paths[id].parameters.bend));
      zone.details.push_back(std::format("ports: start (0,3), end ({},1); walls: edges 0/2 of each part",
        image.paths[id].pieces.size() - 1));
      for (const auto& ending : scene.endings) {
        if (ending.part.area != id) continue;
        zone.details.push_back(std::format("{}: {} -> {}  removed {:.2f}  accent {}",
          ending.start ? "start" : "end", ending.policy, ending.outcome,
          double(ending.removed) / site_precision, ending.accent.empty() ? "none" : ending.accent));
        if (!ending.reason.empty()) zone.details.push_back("ending fallback: " + ending.reason);
      }
    }
    for (const auto& part : zone.parts)
      zone.details.push_back(std::format("part_ref ({},{})  ({},{}) {}x{}  {} vertices{}",
        part.ref.area, part.ref.part, part.bounds.x, part.bounds.y,
        part.bounds.w, part.bounds.h, part.outline.size(), part.seam ? " seam" : ""));
    const auto inbound = std::find_if(graph.edges.begin(), graph.edges.end(),
      [&](const edge& link) { return link.b == id; });
    zone.details.push_back(inbound == graph.edges.end() ? "parent: external entry" :
      std::format("parent #{}  port {}", inbound->a, inbound->port));
    std::string adjacent = "adjacent:";
    for (const auto neighbour : neighbours(graph, id)) adjacent += std::format(" #{}", neighbour);
    zone.details.push_back(adjacent);
    for (const auto& join : scene.joins) {
      if (join.parent.area != id && join.child.area != id && join.connector != id) continue;
      zone.details.push_back(std::format("join ({},{}) -> ({},{}) {}  aperture {:.2f}  depth {:.2f}",
        join.parent.area, join.parent.part, join.child.area, join.child.part,
        join.connector == UINT32_MAX ? "open" : std::format("via #{}", join.connector),
        double(join.width) / site_precision, double(join.depth) / site_precision));
      zone.details.push_back(std::format("edge ({:.2f},{:.2f}) -> ({:.2f},{:.2f})",
        double(join.a.x) / site_precision, double(join.a.y) / site_precision,
        double(join.b.x) / site_precision, double(join.b.y) / site_precision));
    }
    const auto on_route = std::find(graph.route.begin(), graph.route.end(), id) != graph.route.end();
    size_t candidate_count = 0, witnessed = 0;
    for (const auto& cue : graph.cues) candidate_count += cue.witness == id;
    for (const auto& cue : image.perceptions) witnessed += cue.candidate.witness == id;
    zone.details.push_back(std::format("route {}  perception candidates {}  visible {}",
      on_route ? "yes" : "no", candidate_count, witnessed));
    size_t contacts = 0;
    for (size_t cell = 0; cell < scene.owner.size(); ++cell)
      contacts += scene.owner[cell] == id + 1 && scene.exposed[cell] != 0;
    zone.details.push_back(std::format("surface contacts {}  dialogue {}", contacts,
      witnessed != 0 ? site.perception.dialogue_template : "none"));
  }
  scene.layout = std::move(image.layout);
  return scene;
}

int run_site_graph(const uint64_t seed, const std::string_view scale, const int32_t requested_entry_y,
                   const bool surface_cut, const bool verify, const bool ascii,
                   const std::string_view dump, const std::string_view source,
                   const std::string_view motifs_source, const site_bounds bounds) {
  auto site = read_site(source, motifs_source);
  const auto motifs = motif_bindings(site);
  const auto scale_it = std::find(site.scales.begin(), site.scales.end(), scale);
  if (scale_it == site.scales.end())
    throw std::runtime_error(std::format("site: unknown scale '{}'", scale));
  const auto scale_index = size_t(scale_it - site.scales.begin());
  const auto graph = assemble(site, scale_index, seed);
  check(site, graph);
  site.bounds = resolve_bounds(site, scale_index, bounds);
  const auto entry_y = requested_entry_y == -1 ? int32_t(site.bounds.height / 2) : requested_entry_y;
  const auto surface = example_surface(site.bounds, surface_cut);
  auto image = make_site_projection(site, graph, scale_index, entry_y, seed, surface, motifs);
  if (verify) {
    projection sight_probe;
    sight_probe.parts = {
      rectangular_part({0, 0}, {0, 0, 2, 2}),
      precise_part({1, 0}, {{512, 192}, {768, 192}, {768, 320}, {512, 320}}),
      rectangular_part({2, 0}, {3, 0, 2, 2})
    };
    if (!clear_geometric_sight(sight_probe, {256, 256}, {1024, 256}, 0, 1, 2) ||
        clear_geometric_sight(sight_probe, {256, 256}, {1024, 512}, 0, 1, 2))
      throw std::runtime_error("site verify: geometric sight ignores a narrow door aperture");
    sight_probe.parts[1] = precise_part({1, 0}, {{513, 192}, {767, 192}, {767, 320}, {513, 320}});
    if (clear_geometric_sight(sight_probe, {256, 256}, {1024, 256}, 0, 1, 2))
      throw std::runtime_error("site verify: a 1/256-unit gap through the door was mistaken for sight");
    const auto plan = place(site, graph, scale_index, entry_y, seed, motifs);
    check_site_plan(graph, plan);
    if (!plan.owner.empty() || !plan.part_owner.empty() || !plan.exposed.empty())
      throw std::runtime_error("site verify: plan selection unexpectedly depends on projection");
    if (plan.parts != image.parts || plan.joins != image.joins ||
        plan.plan_route != image.plan_route || plan.route_spans != image.route_spans ||
        plan.perceptions != image.perceptions)
      throw std::runtime_error("site verify: projecting the plan changed its geometry or facts");
    const auto plan_replay = place(site, graph, scale_index, entry_y, seed, motifs);
    if (plan.parts != plan_replay.parts || plan.joins != plan_replay.joins ||
        plan.endings != plan_replay.endings || plan.plan_route != plan_replay.plan_route ||
        plan.route_spans != plan_replay.route_spans || plan.perceptions != plan_replay.perceptions ||
        plan.layout.instances != plan_replay.layout.instances)
      throw std::runtime_error("site verify: geometric plan did not replay from the seed");
    const auto full_sample = sample_site_plan(plan, {0, 0, plan.layout.width, plan.layout.height}, 1);
    std::set<std::pair<uint32_t, uint32_t>> sampled_links, declared_links;
    for (const auto& link : graph.edges) declared_links.insert(pair_key(link.a, link.b));
    for (int32_t y = 0; y < full_sample.height; ++y)
      for (int32_t x = 0; x < full_sample.width; ++x) {
        const auto owner = full_sample.owner[size_t(y) * full_sample.width + x];
        if (owner == 0) continue;
        for (const auto& [nx, ny] : {std::pair{x + 1, y}, std::pair{x, y + 1}}) {
          if (nx >= full_sample.width || ny >= full_sample.height) continue;
          const auto other = full_sample.owner[size_t(ny) * full_sample.width + nx];
          if (other != 0 && other != owner) sampled_links.insert(pair_key(owner - 1, other - 1));
        }
      }
    size_t merged_links = 0, missing_links = 0;
    for (const auto& link : sampled_links) merged_links += !declared_links.contains(link);
    for (const auto& link : declared_links) missing_links += !sampled_links.contains(link);
    const devils_engine::originator::motif_rect window{
      plan.layout.width / 4, plan.layout.height / 4,
      plan.layout.width / 2, plan.layout.height / 2};
    const auto window_sample = sample_site_plan(plan, window, 1);
    const auto detailed_sample = sample_site_plan(plan, window, 2);
    if (detailed_sample.width != window.w * 2 || detailed_sample.height != window.h * 2 ||
        plan.parts != plan_replay.parts || !plan.owner.empty())
      throw std::runtime_error("site verify: changing view resolution changed the plan");
    for (int32_t y = 0; y < window.h; ++y)
      for (int32_t x = 0; x < window.w; ++x) {
        const auto local = size_t(y) * window_sample.width + x;
        const auto global = size_t(y + window.y) * full_sample.width + x + window.x;
        if (window_sample.owner[local] != full_sample.owner[global] ||
            window_sample.part_owner[local] != full_sample.part_owner[global])
          throw std::runtime_error("site verify: a window differs from the whole plan projection");
      }
    auto other_bounds = site;
    other_bounds.bounds = {4096, 4096};
    if (assemble(other_bounds, scale_index, seed) != graph) {
      throw std::runtime_error("site verify: hard bounds changed the selected content profile");
    }
    auto outside = image;
    auto moved = outside.parts.back().outline;
    for (auto& point : moved) { point.x += int32_t(site.bounds.width) * site_precision; }
    outside.parts.back() = precise_part(outside.parts.back().ref, std::move(moved), outside.parts.back().seam);
    bool bounds_refused = false;
    try { check_site_plan(graph, outside); }
    catch (const std::exception&) { bounds_refused = true; }
    if (!bounds_refused) { throw std::runtime_error("site verify: geometry outside hard bounds was accepted"); }
    auto plan_only = image;
    plan_only.owner.clear();
    plan_only.part_owner.clear();
    check_site_plan(graph, plan_only);
    auto unfinished = plan_only;
    unfinished.endings.clear();
    bool endings_refused = false;
    try {
      check_site_plan(graph, unfinished);
    } catch (const std::exception&) {
      endings_refused = true;
    }
    if (!endings_refused) throw std::runtime_error("site verify: missing path completion outcomes escaped validation");
    const auto post_join = std::find_if(plan_only.joins.begin(), plan_only.joins.end(),
      [&](const site_view_join& join) { return site.kinds[graph.nodes[join.child.area].kind].name == "watch_post"; });
    if (post_join != plan_only.joins.end() && post_join->connector != UINT32_MAX) {
      auto& door = *std::find_if(plan_only.parts.begin(), plan_only.parts.end(),
        [&](const site_view_part& part) { return part.ref == site_part_ref{post_join->connector, 0}; });
      for (auto& point : door.outline) {
        if (post_join->a.x == post_join->b.x) ++point.x;
        else ++point.y;
      }
      bool refused = false;
      try {
        check_site_plan(graph, plan_only);
      } catch (const std::exception&) {
        refused = true;
      }
      if (!refused) throw std::runtime_error("site verify: a sub-unit gap in the bend door escaped plan validation");
    }
    size_t variants = 0;
    size_t high_bit_variants = 0;
    size_t labs_at_junction = 0;
    size_t labs_at_gallery = 0;
    size_t perceptions = 0;
    size_t bent_routes = 0;
    size_t oblique_posts = 0;
    uint32_t shortest_junction = UINT32_MAX;
    uint32_t longest_junction = 0;
    size_t doors_from_parent = 0;
    size_t doors_from_child = 0;
    std::set<std::pair<int32_t, int32_t>> torture_sizes;
    std::set<std::tuple<uint32_t, uint32_t, int32_t>> path_profiles;
    size_t positive_bends = 0, negative_bends = 0, open_halls = 0;
    size_t straight_joins = 0, corner_joins = 0, paired_checks = 0;
    size_t trimmed_caps = 0, accent_caps = 0, connected_caps = 0, accent_fallbacks = 0;
    uint64_t removed_length = 0;
    const auto baseline = assemble(site, scale_index, seed);
    if (graph != baseline) throw std::runtime_error("site verify: seed is not reproducible");
    for (uint64_t other = 0; other < 128; ++other) {
      const auto candidate = assemble(site, scale_index, other);
      check(site, candidate);
      high_bit_variants += candidate != assemble(site, scale_index, other ^ (uint64_t(1) << 40));
      projection candidate_image;
      try {
        candidate_image = make_site_projection(site, candidate, scale_index, entry_y, other, surface, motifs);
      } catch (const std::exception& error) {
        throw std::runtime_error(std::format("site verify seed {}: {}", other, error.what()));
      }
      if (candidate_image.quality != check_projection(site, candidate, candidate_image, surface))
        throw std::runtime_error("site verify: projection quality is not reproducible");
      for (const auto& ending : candidate_image.endings) {
        trimmed_caps += ending.outcome == "trim";
        accent_caps += ending.outcome == "accent";
        connected_caps += ending.outcome == "connected";
        accent_fallbacks += !ending.reason.empty();
        removed_length += ending.removed;
      }
      const auto part_shape = [&](const site_part_ref ref) -> const site_view_part& {
        return *std::find_if(candidate_image.parts.begin(), candidate_image.parts.end(),
          [&](const site_view_part& part) { return part.ref == ref; });
      };
      for (const auto& join : candidate_image.joins) {
        const auto& child_kind = site.kinds[candidate.nodes[join.child.area].kind].name;
        const auto& anchor = part_shape(join.parent);
        const auto& child = part_shape(join.child);
        if (child_kind == "exit_run") {
          if (join.connector != UINT32_MAX) throw std::runtime_error("site verify: a path chain has a fake door");
          const auto px = anchor.outline[1].x - anchor.outline[0].x;
          const auto py = anchor.outline[1].y - anchor.outline[0].y;
          const auto cx = child.outline[1].x - child.outline[0].x;
          const auto cy = child.outline[1].y - child.outline[0].y;
          if (int64_t(px) * cx + int64_t(py) * cy == 0) ++corner_joins;
          else ++straight_joins;
        }
        if (child_kind == "laboratory") {
          open_halls += join.connector == UINT32_MAX;
          if (candidate_image.paths[join.parent.area].pieces.empty()) continue;
          const auto pa = anchor.outline[1];
          const auto pb = anchor.outline[2];
          const auto ca = child.outline[5];
          const auto cb = child.outline[0];
          const auto dx = int64_t(pb.x) - pa.x;
          const auto dy = int64_t(pb.y) - pa.y;
          const auto tangential_offset = dx * (int64_t(ca.x) + cb.x - join.a.x - join.b.x) +
            dy * (int64_t(ca.y) + cb.y - join.a.y - join.b.y);
          if (std::abs(pa.x + pb.x - join.a.x - join.b.x) > 1 ||
              std::abs(pa.y + pb.y - join.a.y - join.b.y) > 1 ||
              std::abs(tangential_offset) > std::abs(dx) + std::abs(dy)) {
            throw std::runtime_error("site verify: a terminal hall is not centred at the path cap");
          }
        }
      }
      perceptions += candidate_image.perceptions.size();
      std::set<site_part_ref> route_gallery_parts;
      for (const auto& span : candidate_image.route_spans) {
        const auto ref = span.part;
        if (site.kinds[candidate.nodes[ref.area].kind].name == "gallery")
          route_gallery_parts.insert(ref);
      }
      const auto& recipe = site.kinds[kind_id(site, site.perception.via)].path;
      bent_routes += route_gallery_parts.size() >= (recipe.mode == "corner" ? 2u : 3u);
      variants += candidate != graph;
      for (uint32_t id = 0; id < candidate.nodes.size(); ++id) {
        if (candidate.nodes[id].connector) {
          const auto appearance = appearance_of_connector(site, candidate, id, other);
          const auto outbound = std::find_if(candidate.edges.begin(), candidate.edges.end(),
            [&](const edge& link) { return link.a == id; });
          if (outbound != candidate.edges.end() &&
              site.kinds[candidate.nodes[outbound->b].kind].name == "prison_cell") {
            doors_from_parent += appearance.source == "parent";
            doors_from_child += appearance.source == "child";
          }
          continue;
        }
        if (site.kinds[candidate.nodes[id].kind].height_policy == "frontage") {
          const auto instance = std::find(candidate_image.instance_nodes.begin(),
            candidate_image.instance_nodes.end(), id);
          if (instance == candidate_image.instance_nodes.end())
            throw std::runtime_error("site verify: frontage area has no geometry");
          const auto height = uint32_t(candidate_image.layout.instances[
            size_t(instance - candidate_image.instance_nodes.begin())].rect.h);
          shortest_junction = std::min(shortest_junction, height);
          longest_junction = std::max(longest_junction, height);
        }
        if (site.kinds[candidate.nodes[id].kind].name == "torture") {
          const auto part = std::find_if(candidate_image.parts.begin(), candidate_image.parts.end(),
            [&](const site_view_part& item) { return item.ref == site_part_ref{id, 0}; });
          if (part == candidate_image.parts.end())
            throw std::runtime_error("site verify: torture has no primary convex part");
          torture_sizes.emplace(part->bounds.w, part->bounds.h);
        }
        if (site.kinds[candidate.nodes[id].kind].name == "gallery") {
          const auto& parameters = candidate_image.paths[id].parameters;
          path_profiles.emplace(parameters.l1_per_mille, parameters.l2_per_mille, parameters.bend);
          positive_bends += parameters.bend > 0;
          negative_bends += parameters.bend < 0;
        }
        if (site.kinds[candidate.nodes[id].kind].name == "watch_post") {
          const auto instance = std::find(candidate_image.instance_parts.begin(),
            candidate_image.instance_parts.end(), site_part_ref{id, 0});
          if (instance == candidate_image.instance_parts.end())
            throw std::runtime_error("site verify: oblique post has no motif instance");
          const auto& placed = candidate_image.layout.instances[
            size_t(instance - candidate_image.instance_parts.begin())];
          if (placed.parent == devils_engine::originator::no_motif_parent ||
              candidate_image.instance_parts[placed.parent].part !=
                uint32_t(candidate_image.paths[
                  candidate_image.instance_parts[placed.parent].area].pieces.size() > 1) ||
              site.kinds[candidate.nodes[
                candidate_image.instance_parts[placed.parent].area].kind].name != "gallery")
            throw std::runtime_error("site verify: oblique post is not attached to gallery.bend");
          const auto parent = parent_of(candidate, id);
          const auto target_part = candidate_image.instance_parts[placed.parent];
          bool touches_bend = false;
          for (size_t cell = 0; cell < candidate_image.owner.size(); ++cell) {
            if (candidate_image.owner[cell] != parent.connector + 1) continue;
            const auto width = size_t(candidate_image.layout.width);
            for (const auto adjacent : {cell - 1, cell + 1, cell - width, cell + width}) {
              if (adjacent < candidate_image.part_owner.size() &&
                  candidate_image.part_owner[adjacent] == target_part) touches_bend = true;
            }
          }
          if (!touches_bend)
            throw std::runtime_error("site verify: post door touches another part instead of its bend anchor");
          const auto shape = std::find_if(candidate_image.parts.begin(), candidate_image.parts.end(),
            [&](const site_view_part& item) { return item.ref == site_part_ref{id, 0}; });
          bool slanted = false;
          if (shape != candidate_image.parts.end())
            for (size_t index = 0; index < shape->outline.size(); ++index) {
              const auto a = shape->outline[index];
              const auto b = shape->outline[(index + 1) % shape->outline.size()];
              slanted |= a.x != b.x && a.y != b.y;
            }
          const auto& anchor_path = candidate_image.paths[parent.parent];
          const auto expect_slant = anchor_path.parameters.mode == devils_engine::originator::motif_path_mode::offset &&
            anchor_path.pieces.size() == 3;
          if (shape == candidate_image.parts.end() || shape->outline.size() < 4 || (expect_slant && !slanted))
            throw std::runtime_error("site verify: post lost its oblique convex outline");
          ++oblique_posts;
        }
        if (candidate.nodes[id].kind != kind_id(site, site.perception.to)) continue;
        const auto parent = parent_of(candidate, id);
        const auto& parent_kind = site.kinds[candidate.nodes[parent.parent].kind].name;
        labs_at_junction += parent_kind == "junction";
        labs_at_gallery += parent_kind == "gallery" || parent_kind == "exit_run";
      }
    }
    if (variants == 0) throw std::runtime_error("site verify: seeds do not change the graph");
    if (high_bit_variants == 0)
      throw std::runtime_error("site verify: high seed bits do not affect the graph");
    const auto& gallery_recipe = site.kinds[kind_id(site, site.perception.via)].path;
    const auto expect_bend = !gallery_recipe.mode.empty() && gallery_recipe.bend[scale_index] != 0 &&
      (gallery_recipe.mode == "corner" ? gallery_recipe.l1_per_mille[scale_index] < 1000 :
        gallery_recipe.l1_per_mille[scale_index] + gallery_recipe.l2_per_mille[scale_index] < 1000);
    if (site.name == "black_castle_dungeon" &&
        (shortest_junction >= longest_junction || doors_from_parent == 0 ||
         doors_from_child == 0 || (expect_bend && bent_routes == 0)))
      throw std::runtime_error("site verify: frontage, door appearance or composite route never varies");
    if (site.name == "black_castle_dungeon" && torture_sizes.size() < 4)
      throw std::runtime_error("site verify: functional area dimensions do not vary enough");
    if (site.name == "black_castle_dungeon" && oblique_posts != 128)
      throw std::runtime_error("site verify: a gallery lacks its oblique functional post");
    if (site.name == "black_castle_dungeon" && (path_profiles.size() < 8 || positive_bends == 0 ||
        negative_bends == 0 || open_halls == 0 || straight_joins == 0 || corner_joins == 0)) {
      throw std::runtime_error("site verify: path profiles, turns or open hall entrances never vary");
    }
    // Этот контракт относится к авторскому каталогу GN06, не к общему сборщику: лаборатория
    // действительно может отходить и от развилки, и от галереи, а не просто двигаться по растру.
    if (site.name == "black_castle_dungeon" && (labs_at_junction == 0 || labs_at_gallery == 0))
      throw std::runtime_error("site verify: laboratory anchor never varies");
    if (site.name == "black_castle_dungeon" && scale == "large") {
      auto paired = site;
      auto& rule = *std::find_if(paired.rules.begin(), paired.rules.end(),
        [](const rule_decl& item) { return item.kind == "laboratory"; });
      rule.anchors = {"exit_run"};
      rule.per_anchor = 1;
      rule.min_count.assign(site.scales.size(), 2);
      rule.max_count = rule.min_count;
      rule.unique_port = 0;
      rule.open_percent = 0;
      rule.attachment = "paired_wall";
      rule.sides = {"north", "south"};
      auto& run = paired.kinds[kind_id(paired, "exit_run")];
      run.width.assign(site.scales.size(), 32);
      run.width_max.assign(site.scales.size(), 40);
      for (uint64_t pair_seed = 0; pair_seed < 16; ++pair_seed) {
        const auto paired_graph = assemble(paired, scale_index, pair_seed);
        projection paired_image;
        try {
          paired_image = make_site_projection(paired, paired_graph, scale_index, entry_y, pair_seed, surface, motifs);
        } catch (const std::exception& error) {
          throw std::runtime_error(std::format("site paired halls seed {}: {}", pair_seed, error.what()));
        }
        for (size_t i = 0; i < paired_image.joins.size(); ++i) {
          const auto& first = paired_image.joins[i];
          if (paired.kinds[paired_graph.nodes[first.child.area].kind].name != "laboratory") continue;
          const auto second = std::find_if(paired_image.joins.begin() + i + 1, paired_image.joins.end(),
            [&](const site_view_join& join) {
              return join.parent == first.parent && paired_graph.nodes[join.child.area].kind ==
                paired_graph.nodes[first.child.area].kind;
            });
          if (second == paired_image.joins.end()) continue;
          ++paired_checks;
          const auto shape = [&](const site_part_ref ref) -> const site_view_part& {
            return *std::find_if(paired_image.parts.begin(), paired_image.parts.end(),
              [&](const site_view_part& part) { return part.ref == ref; });
          };
          const auto& anchor = shape(first.parent);
          const auto vertical = anchor.outline[0].x == anchor.outline[1].x;
          const auto centre_sum = vertical ? anchor.outline[0].x + anchor.outline[3].x :
            anchor.outline[0].y + anchor.outline[3].y;
          for (auto point : shape(first.child).outline) {
            if (vertical) point.x = centre_sum - point.x;
            else point.y = centre_sum - point.y;
            const auto& other_outline = shape(second->child).outline;
            if (std::find(other_outline.begin(), other_outline.end(), point) == other_outline.end()) {
              throw std::runtime_error("site verify: paired halls are not exact reflections about the path axis");
            }
          }
        }
      }
      if (paired_checks != 32) throw std::runtime_error("site verify: expected 32 mirrored hall pairs");
    }
    auto replay = make_site_projection(site, graph, scale_index, entry_y, seed, surface, motifs);
    if (replay.layout.instances != image.layout.instances || replay.layout.passages != image.layout.passages ||
        replay.joins != image.joins || replay.endings != image.endings ||
        replay.plan_route != image.plan_route ||
        replay.owner != image.owner || replay.exposed != image.exposed ||
        replay.part_owner != image.part_owner || replay.parts != image.parts ||
        replay.route_spans != image.route_spans || replay.perceptions != image.perceptions ||
        replay.quality != image.quality)
      throw std::runtime_error("site layout verify: same inputs did not reproduce the picture");
    const auto view = make_site_view_scene(seed, scale, requested_entry_y, surface_cut,
                                           source, motifs_source, bounds);
    verify_site_viewer_details(view);
    if (view.owner != image.owner || view.part_owner != image.part_owner || view.joins != image.joins ||
        view.endings != image.endings ||
        view.zones.size() != graph.nodes.size() ||
        view.route_spans != image.route_spans || view.perceptions != image.perceptions.size() ||
        view.projection_quality != image.quality)
      throw std::runtime_error("site viewer verify: selectable zones disagree with the generated plan");
    for (uint32_t id = 0; id < graph.nodes.size(); ++id) {
      const auto& zone = view.zones[id];
      if (zone.connector != graph.nodes[id].connector)
        throw std::runtime_error("site viewer verify: connector marker disagrees with the graph");
      const auto accents = std::count_if(image.endings.begin(), image.endings.end(),
        [&](const site_path_ending& ending) { return ending.part.area == id && ending.accent_part.valid(); });
      if (site.kinds[graph.nodes[id].kind].name == "gallery" &&
          (zone.parts.size() != image.paths[id].pieces.size() + accents ||
           std::any_of(zone.parts.begin(), zone.parts.end(),
             [](const site_view_part& part) { return part.seam; })))
        throw std::runtime_error("site viewer verify: gallery parts disagree with its single path recipe");
      if (zone.connector) {
        const auto appearance = appearance_of_connector(site, graph, id, seed);
        if (zone.parts.size() != 1 || zone.colour != appearance.colour ||
            zone.details.empty() || zone.details.front().find("connector appearance:") == std::string::npos)
          throw std::runtime_error("site viewer verify: a door lost its own geometry or inherited appearance");
      }
    }
    std::cout << "site verify: 128 seeds connected, doors two-sided, routes and cues valid; "
              << variants << " variants, laboratory anchors gallery=" << labs_at_gallery
              << " junction=" << labs_at_junction << "; 2D " << image.layout.width << 'x'
              << image.layout.height << " exact plan adjacency and route checked; "
              << perceptions << " geometrically confirmed perceptions; junction height " << shortest_junction
              << ".." << longest_junction << "; cell-door appearance parent=" << doors_from_parent
              << " child=" << doors_from_child << "; routes through gallery parts=" << bent_routes
              << "; torture sizes=" << torture_sizes.size()
              << "; profiles=" << path_profiles.size() << " open halls=" << open_halls
              << " straight/corner joins=" << straight_joins << '/' << corner_joins
              << " mirrored pairs=" << paired_checks
              << "; endings connected/trim/accent=" << connected_caps << '/' << trimmed_caps << '/' << accent_caps
              << " removed=" << double(removed_length) / site_precision << " fallback=" << accent_fallbacks
              << "\n";
    if (image.quality.lost_parts != full_sample.lost_parts ||
        image.quality.false_links != merged_links || image.quality.missing_links != missing_links)
      throw std::runtime_error("site verify: projection diagnostics disagree with direct sampling");
    std::cout << "first geometric plan at 1:1: lost_parts=" << image.quality.lost_parts
              << " false_links=" << image.quality.false_links
              << " missing_links=" << image.quality.missing_links
              << " unreachable_cells=" << image.quality.unreachable_cells << '\n';
  } else {
    report(site, graph, image, seed, scale);
  }
  display(site, graph, image, ascii, dump, seed);
  return 0;
}

} // namespace gn06
