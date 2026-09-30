#include <algorithm>
#include <cstdint>
#include <fstream>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tavl/tavl.h>

#include "devils_engine/originator/motif_layout.h"

#include "motif_demo.h"

// Старый прямоугольный опыт и чтение авторского словаря. Здесь нет декоратора пола:
// геометрия и её показ не требуют WFC. У нового site собственный единый ресурс мотивов;
// здесь остались только данные и сборщик старого прямоугольного эксперимента.

namespace gn06 {
namespace {

using namespace devils_engine::originator;
namespace originator = devils_engine::originator;

struct scale_spec {
  std::string name;
  uint32_t side = 0;
};

struct motif_style {
  std::string name;
  std::string glyph;
  std::vector<uint32_t> colour;
};

struct authored_rule {
  std::string name;
  std::string motif;
  std::string location;
  std::string parent;
  std::vector<std::string> sides;
  std::vector<uint32_t> min_width;
  std::vector<uint32_t> max_width;
  uint32_t min_height = 0;
  uint32_t max_height = 0;
  std::vector<uint32_t> count;
  std::vector<uint32_t> required;
  uint32_t optional_chance = 100;
};

struct catalogue_document {
  uint32_t version = 0;
  std::vector<std::string> locations;
  std::vector<scale_spec> scales;
  std::vector<motif_style> motifs;
  std::vector<authored_rule> rules;
};

struct catalogue {
  uint32_t version = 0;
  scale_spec scale;
  std::vector<std::string> locations;
  std::vector<motif_style> motifs;
  std::vector<motif_rule> rules;
  std::vector<std::string> rule_names;
};

struct rule_anchor {
  std::string name;
  uint32_t first = 0;
  uint32_t count = 0;
};

uint32_t named_index(const std::vector<std::string>& names, const std::string_view name,
                     const std::string_view field) {
  const auto found = std::find(names.begin(), names.end(), name);
  if (found == names.end())
    throw std::runtime_error(std::format("motifs: unknown {} '{}'", field, name));
  return uint32_t(found - names.begin());
}

catalogue read_catalogue(const std::string_view text, const std::string_view scale_name,
                         const size_t requested_side) {
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(std::string(text));
  parser.finish();
  tavl::ct_context ctx;
  catalogue_document doc;
  if (!tavl::deserialize_next(parser, ctx, doc))
    throw std::runtime_error("motifs: the catalogue is empty");
  for (const auto& diagnostic : ctx.diagnostics) {
    if (diagnostic.error.is_critical())
      throw std::runtime_error(std::format("motifs: catalogue parse error '{}' at {}:{} field '{}'",
        tavl::to_string(diagnostic.error.type), diagnostic.error.span.line,
        diagnostic.error.span.column, diagnostic.field));
  }
  if (doc.version == 0 || doc.locations.empty() || doc.scales.empty() ||
      doc.motifs.empty() || doc.rules.empty())
    throw std::runtime_error("motifs: the catalogue needs a version, locations, scales, motifs and rules");
  for (size_t i = 0; i < doc.locations.size(); ++i)
    if (doc.locations[i].empty() ||
        std::find(doc.locations.begin(), doc.locations.begin() + i, doc.locations[i]) != doc.locations.begin() + i)
      throw std::runtime_error("motifs: location names must be nonempty and unique");
  for (size_t i = 0; i < doc.scales.size(); ++i)
    if (doc.scales[i].name.empty() ||
        std::find_if(doc.scales.begin(), doc.scales.begin() + i,
          [&](const auto& item) { return item.name == doc.scales[i].name; }) != doc.scales.begin() + i)
      throw std::runtime_error("motifs: scale names must be nonempty and unique");
  for (size_t i = 0; i < doc.scales.size(); ++i)
    if (doc.scales[i].side < 8 || doc.scales[i].side > 4096 ||
        (i > 0 && doc.scales[i].side <= doc.scales[i - 1].side))
      throw std::runtime_error("motifs: scales must have increasing supported sides");
  if (requested_side != 0 && requested_side < doc.scales.front().side)
    throw std::runtime_error(std::format("motifs: {} is smaller than the smallest authored footprint ({})",
                                         requested_side, doc.scales.front().side));
  const auto found = requested_side == 0 ?
    std::find_if(doc.scales.begin(), doc.scales.end(),
      [&](const auto& item) { return item.name == scale_name; }) :
    std::find_if(doc.scales.begin(), doc.scales.end(),
      [&](const auto& item) { return item.side >= requested_side; });
  const auto selected = found == doc.scales.end() && requested_side != 0 ?
                        doc.scales.end() - 1 : found;
  if (selected == doc.scales.end())
    throw std::runtime_error(std::format("motifs: unknown scale '{}'", scale_name));
  const auto scale_index = size_t(selected - doc.scales.begin());

  catalogue result;
  result.version = doc.version;
  result.scale = *selected;
  result.locations = std::move(doc.locations);
  result.motifs = std::move(doc.motifs);
  std::vector<std::string> motif_names;
  for (const auto& motif : result.motifs) {
    if (motif.name.empty() || motif.glyph.size() != 1 || motif.colour.size() != 3 ||
        std::any_of(motif.colour.begin(), motif.colour.end(), [](const auto channel) { return channel > 255; }))
      throw std::runtime_error(std::format("motifs: invalid presentation for '{}' (glyph '{}', colour {} components)",
                                           motif.name, motif.glyph, motif.colour.size()));
    if (std::find(motif_names.begin(), motif_names.end(), motif.name) != motif_names.end())
      throw std::runtime_error(std::format("motifs: duplicate motif '{}'", motif.name));
    motif_names.push_back(motif.name);
  }

  std::vector<rule_anchor> anchors;
  for (const auto& item : doc.rules) {
    if (item.name.empty() || item.min_width.size() != doc.scales.size() ||
        item.max_width.size() != doc.scales.size() ||
        (!item.count.empty() && item.count.size() != doc.scales.size()) ||
        (!item.required.empty() && item.required.size() != doc.scales.size()))
      throw std::runtime_error(std::format("motifs: rule '{}' must declare one width per scale", item.name));
    if (std::find_if(anchors.begin(), anchors.end(), [&](const auto& anchor) { return anchor.name == item.name; }) != anchors.end())
      throw std::runtime_error(std::format("motifs: duplicate rule '{}'", item.name));
    const auto motif = named_index(motif_names, item.motif, "motif") + 1;
    const auto location = named_index(result.locations, item.location, "location") + 1;
    uint32_t parent = no_motif_parent;
    if (item.parent != "none") {
      const auto preceding = std::find_if(anchors.begin(), anchors.end(),
        [&](const auto& anchor) { return anchor.name == item.parent; });
      if (preceding == anchors.end())
        throw std::runtime_error(std::format("motifs: rule '{}' has no earlier parent '{}'", item.name, item.parent));
      if (preceding->count != 1)
        throw std::runtime_error(std::format("motifs: rule '{}' names repeated parent '{}'", item.name, item.parent));
      parent = preceding->first;
    }
    uint8_t sides = 0;
    for (const auto& side : item.sides) {
      if (side == "east") sides |= motif_east;
      else if (side == "west") sides |= motif_west;
      else if (side == "south") sides |= motif_south;
      else if (side == "north") sides |= motif_north;
      else throw std::runtime_error(std::format("motifs: unknown side '{}'", side));
    }
    const auto count = item.count.empty() ? 1u : item.count[scale_index];
    const auto required = item.required.empty() ? count : item.required[scale_index];
    if (count == 0 || required > count || item.optional_chance > 100)
      throw std::runtime_error(std::format("motifs: rule '{}' has an invalid count or chance", item.name));
    anchors.push_back({item.name, uint32_t(result.rules.size()), count});
    for (uint32_t i = 0; i < count; ++i) {
      result.rules.push_back({motif, location, parent,
        item.min_width[scale_index], item.max_width[scale_index], item.min_height, item.max_height,
        sides, uint8_t(i < required ? 100 : item.optional_chance)});
      result.rule_names.push_back(count == 1 ? item.name : std::format("{} #{}", item.name, i + 1));
    }
  }
  return result;
}

std::vector<uint8_t> example_surface(const scale_spec& scale, const bool cut) {
  std::vector<uint8_t> mask(size_t(scale.side) * scale.side, 0);
  if (!cut) return mask;
  // Узкая трещина в СИНТЕТИЧЕСКОМ чанковом окружении. Она задана в мировых координатах до
  // сборки; каждый попавший под неё участок потолка возвращается как exposure, не исчезая молча.
  const auto side = int32_t(scale.side);
  for (int32_t y = 1; y < side - 1; ++y) {
    for (int32_t x = side / 3; x < side / 3 + 2; ++x)
      mask[size_t(y) * side + x] = 1;
  }
  return mask;
}

char glyph(const catalogue& book, const uint32_t motif) {
  return motif > 0 && motif <= book.motifs.size() ? book.motifs[motif - 1].glyph[0] : '?';
}

uint32_t colour(const catalogue& book, const uint32_t motif) {
  if (motif == 0) return 0x17191du;
  if (motif > book.motifs.size()) return 0xffffffu;
  const auto& rgb = book.motifs[motif - 1].colour;
  return (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
}

struct display_cell {
  uint32_t rgb = 0x17191du;
  char mark = '#';
};

std::vector<display_cell> project(const motif_layout& layout, const catalogue& book) {
  std::vector<display_cell> cells(size_t(layout.width) * layout.height);
  for (const auto& instance : layout.instances) {
    const auto& r = instance.rect;
    for (int32_t y = r.y; y < r.y + r.h; ++y) {
      for (int32_t x = r.x; x < r.x + r.w; ++x) {
        const auto index = size_t(y) * layout.width + x;
        auto& cell = cells[index];
        cell.rgb = colour(book, instance.motif);
        cell.mark = glyph(book, instance.motif);
      }
    }
  }
  for (const auto& passage : layout.passages)
    cells[size_t(passage.y) * layout.width + passage.x] = {0xe1d3a5u, 'o'};
  cells[size_t(layout.entry_y) * layout.width] = {0xe1d3a5u, 'o'};
  for (const auto& opening : layout.exposures)
    cells[size_t(opening.y) * layout.width + opening.x] = {0xb8c8d9u, '^'};
  return cells;
}

bool write_ppm(const std::string_view path, const motif_layout& layout,
               const std::vector<display_cell>& cells) {
  std::ofstream out(std::string(path), std::ios::binary);
  if (!out) return false;
  out << "P6\n" << layout.width << ' ' << layout.height << "\n255\n";
  for (const auto& cell : cells) {
    const auto rgb = cell.rgb;
    const char bytes[3]{char(rgb >> 16), char(rgb >> 8), char(rgb)};
    out.write(bytes, 3);
  }
  return bool(out);
}

} // namespace

motif_view_scene make_motif_view_scene(const uint64_t seed, const std::string_view scale_name,
                                       const size_t requested_side, const int32_t requested_entry_y,
                                       const bool surface_cut, const std::string_view catalogue_text) {
  const auto book = read_catalogue(catalogue_text, scale_name, requested_side);
  const auto entry_y = requested_entry_y == -1 ? int32_t(book.scale.side / 2) : requested_entry_y;
  const auto surface = example_surface(book.scale, surface_cut);
  auto layout = assemble_motifs(book.rules, book.scale.side, book.scale.side, entry_y, seed, surface);
  if (!layout.valid()) throw std::runtime_error("motifs: " + layout.refusal);

  motif_view_scene scene;
  scene.scale = book.scale.name;
  scene.seed = seed;
  scene.catalogue_version = book.version;
  scene.zones.reserve(layout.instances.size());
  for (const auto& instance : layout.instances) {
    scene.zones.push_back({book.locations[instance.location - 1],
                           book.motifs[instance.motif - 1].name,
                           book.rule_names[instance.rule], colour(book, instance.motif),
                           book.rules[instance.rule].chance_percent});
  }
  scene.layout = std::move(layout);
  return scene;
}

int run_motif_demo(const uint64_t seed, const std::string_view scale_name, const size_t requested_side,
                   const int32_t requested_entry_y, const bool surface_cut,
                   const bool verify, const bool ascii, const std::string_view dump,
                   const std::string_view catalogue_text) {
  const auto book = read_catalogue(catalogue_text, scale_name, requested_side);
  const auto& scale = book.scale;
  const auto& rules = book.rules;
  const auto surface = example_surface(scale, surface_cut);
  const auto entry_y = requested_entry_y == -1 ? int32_t(scale.side / 2) : requested_entry_y;
  const auto layout = assemble_motifs(rules, scale.side, scale.side, entry_y, seed, surface);
  if (!layout.valid()) throw std::runtime_error("motifs: " + layout.refusal);

  if (requested_side > scale.side)
    std::cout << "GN06 motifs: requested " << requested_side << ", clamped to " << scale.name
              << " (" << scale.side << ")\n";
  else if (requested_side != 0 && requested_side < scale.side)
    std::cout << "GN06 motifs: requested " << requested_side << ", rounded to " << scale.name
              << " (" << scale.side << ")\n";

  std::vector<uint32_t> location_counts(book.locations.size(), 0);
  for (const auto& item : layout.instances) ++location_counts[item.location - 1];
  std::cout << "GN06 motifs: " << scale.name << ' ' << scale.side << 'x' << scale.side
            << ", catalogue v" << book.version << ", seed " << seed
            << ", entry (0," << entry_y << "), blocks " << layout.instances.size();
  for (size_t i = 0; i < book.locations.size(); ++i)
    std::cout << (i == 0 ? " (" : ", ") << book.locations[i] << ' ' << location_counts[i];
  std::cout << ")\n  passages " << layout.passages.size()
            << ", surface exposures " << layout.exposures.size()
            << ", attempts " << layout.attempts << "\n";

  if (verify) {
    size_t checks = 0;
    size_t failures = 0;
    const auto check = [&](const bool success, const std::string_view label) {
      ++checks;
      failures += size_t(!success);
      std::cout << "  " << (success ? "ok" : "FAIL") << " " << label << "\n";
    };
    check(validate_motif_layout(layout).empty(), "каждую меру пола можно обойти от входа и вернуться");
    check(book.locations.size() == 2 &&
          std::all_of(location_counts.begin(), location_counts.end(), [](const auto count) { return count >= 5; }),
          "обе тематические локации содержат несколько блоков");
    check(layout.passages.size() == layout.instances.size(), "каждый блок имеет путь к внешнему входу");
    const auto again = assemble_motifs(rules, scale.side, scale.side, entry_y, seed, surface);
    check(again.valid() && again.instances == layout.instances && again.passages == layout.passages &&
          again.exposures == layout.exposures, "одинаковый вход даёт тот же план и пересечения");
    const auto other = assemble_motifs(rules, scale.side, scale.side, entry_y, seed + 1, surface);
    check(other.valid() && other.instances != layout.instances, "другое зерно меняет сборку");
    if (surface_cut) {
      size_t expected = 0;
      for (const auto& item : layout.instances) {
        const auto& r = item.rect;
        for (int32_t y = r.y; y < r.y + r.h; ++y)
          for (int32_t x = r.x; x < r.x + r.w; ++x)
            expected += size_t(surface[size_t(y) * scale.side + x] != 0);
      }
      for (const auto& passage : layout.passages)
        if (passage.a != no_motif_parent)
          expected += size_t(surface[size_t(passage.y) * scale.side + passage.x] != 0);
      check(layout.exposures.size() == expected, "каждое пересечение поверхности с полом объявлено");
    }
    std::cout << "GN06 verify motifs: " << checks - failures << '/' << checks << "\n";
    if (failures != 0) return 1;
  }

  if (ascii || !dump.empty()) {
    const auto cells = project(layout, book);
    if (ascii) {
      for (int32_t y = 0; y < layout.height; ++y) {
        for (int32_t x = 0; x < layout.width; ++x) {
          std::cout << cells[size_t(y) * layout.width + x].mark;
        }
        std::cout << '\n';
      }
    }
    if (!dump.empty() && !write_ppm(dump, layout, cells))
      throw std::runtime_error("motifs: cannot write the requested PPM");
  }
  return 0;
}

} // namespace gn06
