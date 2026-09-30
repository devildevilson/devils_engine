#include "site_motif_resource.h"

#include <algorithm>
#include <format>
#include <stdexcept>

#include <tavl/tavl.h>

#include "devils_engine/originator/motif_path.h"

// Каталог строительных ресурсов читается без тематического графа и без legacy-программы.
// Параметры процедуры и локальные рёбра проверяем здесь; ссылки на акценты темы — у сборщика.

namespace gn06 {

const devils_engine::originator::motif_geometry_registry& site_geometry_operations() {
  static const auto registry = [] {
    devils_engine::originator::motif_geometry_registry result;
    devils_engine::originator::add_standard_motif_geometry(result);
    // Проектные процедуры регистрируются здесь; сборщик не требует нового if по имени формы.
    return result;
  }();
  return registry;
}

site_motif_catalogue read_site_motifs(const std::string_view source) {
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(std::string(source));
  parser.finish();
  tavl::ct_context context;
  site_motif_catalogue catalogue;
  if (!tavl::deserialize_next(parser, context, catalogue)) {
    throw std::runtime_error("site motifs: empty catalogue");
  }
  for (const auto& diagnostic : context.diagnostics) {
    if (diagnostic.error.is_critical()) {
      throw std::runtime_error(std::format("site motifs: parse error '{}' at {}:{} field '{}'",
        tavl::to_string(diagnostic.error.type), diagnostic.error.span.line,
        diagnostic.error.span.column, diagnostic.field));
    }
  }
  if (catalogue.version != 1 || catalogue.profiles.empty() || catalogue.motifs.empty()) {
    throw std::runtime_error("site motifs: version 1 needs profiles and motifs");
  }
  for (size_t i = 0; i < catalogue.profiles.size(); ++i) {
    const auto& profile = catalogue.profiles[i];
    if (profile.empty() || std::find(catalogue.profiles.begin(), catalogue.profiles.begin() + i,
                                    profile) != catalogue.profiles.begin() + i) {
      throw std::runtime_error("site motifs: profile names must be unique and nonempty");
    }
  }
  for (size_t i = 0; i < catalogue.motifs.size(); ++i) {
    const auto& motif = catalogue.motifs[i];
    const auto& geometry = motif.geometry;
    const auto& placement = motif.placement;
    const auto& presentation = motif.presentation;
    if (motif.name.empty() || std::any_of(catalogue.motifs.begin(), catalogue.motifs.begin() + i,
      [&](const site_motif_resource& prior) { return prior.name == motif.name; })) {
      throw std::runtime_error(std::format("site motifs: invalid or duplicate motif '{}'", motif.name));
    }
    if (geometry.operation != "path" && !site_geometry_operations().contains(geometry.operation)) {
      throw std::runtime_error(std::format("site motifs: '{}' uses unknown geometry operation '{}'",
        motif.name, geometry.operation));
    }
    if ((geometry.operation == "path") != !geometry.path.mode.empty()) {
      throw std::runtime_error(std::format("site motifs: '{}' path operation/recipe mismatch", motif.name));
    }
    if ((placement.strategy != "quarter_turn" && placement.strategy != "edge_parallel" &&
         placement.strategy != "parent_wrap") || placement.wall_edges.size() != 4) {
      throw std::runtime_error(std::format("site motifs: '{}' has an invalid placement contract", motif.name));
    }
    if ((placement.strategy != "quarter_turn" &&
          (geometry.operation != "rectangle" || !geometry.parts.empty())) ||
        (placement.strategy == "parent_wrap" && placement.entrance_edge != 3)) {
      throw std::runtime_error(std::format("site motifs: '{}' ports do not match geometry/placement", motif.name));
    }
    if (presentation.glyph.size() != 1 || presentation.colour.size() != 3 ||
        std::any_of(presentation.colour.begin(), presentation.colour.end(),
          [](const uint32_t channel) { return channel > 255; })) {
      throw std::runtime_error(std::format("site motifs: '{}' has invalid presentation", motif.name));
    }
    const auto validate_range = [&](const auto& low, const auto& high, const std::string_view axis) {
      if (low.size() != catalogue.profiles.size() ||
          (!high.empty() && high.size() != low.size())) {
        throw std::runtime_error(std::format("site motifs: '{}' needs one {} range per profile", motif.name, axis));
      }
      for (size_t profile = 0; profile < low.size(); ++profile) {
        if (low[profile] == 0 || low[profile] > 256 ||
            (!high.empty() && (high[profile] < low[profile] || high[profile] > 256))) {
          throw std::runtime_error(std::format("site motifs: '{}' has invalid {} range", motif.name, axis));
        }
      }
    };
    validate_range(geometry.width, geometry.width_max, "width");
    validate_range(geometry.height, geometry.height_max, "height");
    if ((!geometry.height_policy.empty() && geometry.height_policy != "frontage") ||
        (geometry.height_policy.empty() && (geometry.frontage_margin || geometry.frontage_gap)) ||
        geometry.frontage_margin > 32 || geometry.frontage_gap > 32) {
      throw std::runtime_error(std::format("site motifs: '{}' has invalid frontage policy", motif.name));
    }
    for (size_t profile = 0; profile < catalogue.profiles.size(); ++profile) {
      if (geometry.operation == "path") {
        // Первый сегмент пути — четырёхугольник; внутренние miter-рёбра позже
        // отсекаются make_motif_path. Здесь не подменяем путь фиктивной геометрией.
        if (placement.entrance_edge >= 4 || std::any_of(placement.wall_edges.begin(),
            placement.wall_edges.end(), [](const auto edge) { return edge >= 4; })) {
          throw std::runtime_error(std::format("site motifs: '{}' has invalid path ports", motif.name));
        }
        continue;
      }
      const auto pieces = site_geometry_operations().build(geometry.operation,
        {geometry.width[profile], geometry.height[profile], 0, geometry.parameters});
      if (!pieces.valid()) {
        throw std::runtime_error(std::format("site motifs: '{}': {}", motif.name, pieces.refusal));
      }
      if (placement.entrance_edge >= pieces.pieces.front().size() ||
          std::any_of(placement.wall_edges.begin(), placement.wall_edges.end(),
            [&](const uint32_t edge) { return edge >= pieces.pieces.front().size(); }) ||
          (pieces.pieces.size() != 1 && !geometry.parts.empty())) {
        throw std::runtime_error(std::format("site motifs: '{}' ports/extra parts do not match geometry", motif.name));
      }
    }
    if (geometry.operation == "path" && !geometry.parameters.empty()) {
      throw std::runtime_error(std::format("site motifs: '{}' path uses its typed recipe, not parameters", motif.name));
    }
    if (!geometry.path.mode.empty()) {
      const auto& path = geometry.path;
      if ((path.mode != "offset" && path.mode != "corner") || !geometry.parts.empty() ||
          path.l1_per_mille.size() != catalogue.profiles.size() ||
          path.l2_per_mille.size() != catalogue.profiles.size() ||
          path.bend.size() != catalogue.profiles.size() ||
          (!path.l1_max.empty() && path.l1_max.size() != catalogue.profiles.size()) ||
          (!path.l2_max.empty() && path.l2_max.size() != catalogue.profiles.size()) ||
          (!path.bend_max.empty() && path.bend_max.size() != catalogue.profiles.size()) ||
          path.mirror > 1 || path.min_bend_length == 0) {
        throw std::runtime_error(std::format("site motifs: '{}' has invalid path recipe/ranges", motif.name));
      }
      for (const auto* ending : {&path.start, &path.end}) {
        if ((ending->policy != "trim" && ending->policy != "accent" && ending->policy != "vary") ||
            ending->clearance > 32 || ending->minimum_length == 0 || ending->minimum_length > 64 ||
            ending->trim_percent > 100 ||
            (ending->policy == "trim" ? !ending->accent.empty() : ending->accent.empty())) {
          throw std::runtime_error(std::format("site motifs: '{}' has invalid start/end policy", motif.name));
        }
      }
      for (size_t profile = 0; profile < catalogue.profiles.size(); ++profile) {
        const auto shape = devils_engine::originator::make_motif_path({geometry.width[profile],
          geometry.height[profile], path.l1_per_mille[profile], path.l2_per_mille[profile],
          path.bend[profile], path.mode == "corner" ?
            devils_engine::originator::motif_path_mode::corner :
            devils_engine::originator::motif_path_mode::offset, path.min_bend_length});
        if (!shape.valid()) {
          throw std::runtime_error(std::format("site motifs: '{}': {}", motif.name, shape.refusal));
        }
        if ((!path.l1_max.empty() && (path.l1_max[profile] < path.l1_per_mille[profile] || path.l1_max[profile] > 1000)) ||
            (!path.l2_max.empty() && (path.l2_max[profile] < path.l2_per_mille[profile] || path.l2_max[profile] > 1000)) ||
            (!path.bend_max.empty() && (path.bend_max[profile] < path.bend[profile] || path.bend_max[profile] > 4096))) {
          throw std::runtime_error(std::format("site motifs: '{}' has reversed/unsupported path range", motif.name));
        }
      }
    }
    for (size_t part_index = 0; part_index < geometry.parts.size(); ++part_index) {
      const auto& part = geometry.parts[part_index];
      if (part.name.empty() || part.name == "main" || part.name == "last" ||
          part.width.size() != catalogue.profiles.size() || part.height.size() != catalogue.profiles.size() ||
          (!part.width_max.empty() && part.width_max.size() != catalogue.profiles.size()) ||
          (!part.height_max.empty() && part.height_max.size() != catalogue.profiles.size()) ||
          part.sides.empty() || (part.align != "start" && part.align != "end" &&
                                 part.align != "centre" && part.align != "random") ||
          (!part.passage_align.empty() && part.passage_align != "start" &&
           part.passage_align != "centre" && part.passage_align != "end") ||
          (!part.attach_to.empty() && part.attach_to != "main" && part.attach_to != "previous") ||
          (!part.join.empty() && part.join != "direct") ||
          (!part.shape.empty() && !site_geometry_operations().contains(part.shape)) ||
          std::any_of(part.width.begin(), part.width.end(), [](const auto v) { return v < 3 || v > 256; }) ||
          std::any_of(part.height.begin(), part.height.end(), [](const auto v) { return v < 3 || v > 256; }) ||
          std::any_of(part.width_max.begin(), part.width_max.end(), [](const auto v) { return v < 3 || v > 256; }) ||
          std::any_of(part.height_max.begin(), part.height_max.end(), [](const auto v) { return v < 3 || v > 256; }) ||
          std::find_if(geometry.parts.begin(), geometry.parts.begin() + part_index,
            [&](const part_decl& earlier) { return earlier.name == part.name; }) != geometry.parts.begin() + part_index) {
        throw std::runtime_error(std::format("site motifs: invalid part of motif '{}'", motif.name));
      }
      for (size_t scale = 0; scale < catalogue.profiles.size(); ++scale) {
        if ((!part.width_max.empty() && part.width_max[scale] < part.width[scale]) ||
            (!part.height_max.empty() && part.height_max[scale] < part.height[scale])) {
          throw std::runtime_error(std::format("site motifs: size range is reversed for part '{}'", part.name));
        }
        const auto shape = site_geometry_operations().build(part.shape.empty() ? "rectangle" : part.shape,
          {part.width[scale], part.height[scale], 0, {}});
        if (!shape.valid() || shape.pieces.size() != 1) {
          throw std::runtime_error(std::format("site motifs: part '{}' needs one valid convex piece: {}",
            part.name, shape.refusal));
        }
      }
      for (const auto& side : part.sides) {
        if (side != "east" && side != "west" && side != "north" && side != "south") {
          throw std::runtime_error(std::format("site motifs: unknown part side '{}'", side));
        }
      }
    }
  }
  return catalogue;
}

const site_motif_resource& find_site_motif(const site_motif_catalogue& catalogue,
                                          const std::string_view name) {
  const auto found = std::find_if(catalogue.motifs.begin(), catalogue.motifs.end(),
    [&](const site_motif_resource& motif) { return motif.name == name; });
  if (found == catalogue.motifs.end()) {
    throw std::runtime_error(std::format("site motifs: unknown motif '{}'", name));
  }
  return *found;
}

} // namespace gn06
