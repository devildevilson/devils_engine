#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "devils_engine/originator/motif_layout.h"
#include "devils_engine/originator/motif_path.h"

namespace gn06 {

struct site_part_ref {
  uint32_t area = UINT32_MAX;
  uint32_t part = UINT32_MAX;
  bool valid() const noexcept { return area != UINT32_MAX; }
  bool operator==(const site_part_ref&) const noexcept = default;
  bool operator<(const site_part_ref& other) const noexcept {
    return area != other.area ? area < other.area : part < other.part;
  }
};

using site_point = devils_engine::originator::motif_path_point;
inline constexpr int32_t site_precision = 256;

// Внешняя жёсткая граница, независимая от профиля состава. {0,0} выбирает
// демонстрационный участок из темы; ненулевой размер никогда не расширяется молча.
struct site_bounds {
  uint32_t width = 0;
  uint32_t height = 0;
};

struct site_view_part {
  site_part_ref ref{};
  devils_engine::originator::motif_rect bounds{};
  std::vector<site_point> outline; // 1/site_precision единицы плана, не координаты клеток
  bool seam = false;
  bool operator==(const site_view_part&) const noexcept = default;
};

struct site_view_zone {
  devils_engine::originator::motif_rect bounds{};
  uint32_t colour = 0;
  bool connector = false;
  char glyph = ' ';
  std::vector<site_view_part> parts;
  std::vector<std::string> details;
};

struct site_view_join {
  site_part_ref parent;
  site_part_ref child;
  site_point a;
  site_point b;
  uint32_t connector = UINT32_MAX;
  uint32_t width = 0; // в долях единицы плана
  uint32_t depth = 0;
  uint32_t edge = 0;
  bool operator==(const site_view_join&) const noexcept = default;
};

// Каждый отрезок лежит в одной выпуклой части; соседние отрезки встречаются
// на точном общем портале. Это геометрия маршрута, а не след из клеток картинки.
struct site_route_span {
  site_part_ref part;
  site_point a;
  site_point b;
  bool operator==(const site_route_span&) const noexcept = default;
};

struct site_projection_quality {
  size_t lost_parts = 0;
  size_t false_links = 0;
  size_t missing_links = 0;
  size_t unreachable_cells = 0;
  bool exact() const noexcept {
    return lost_parts == 0 && false_links == 0 && missing_links == 0 && unreachable_cells == 0;
  }
  bool operator==(const site_projection_quality&) const noexcept = default;
};

struct site_path_ending {
  site_part_ref part;
  site_part_ref accent_part;
  site_point a;
  site_point b;
  std::string policy;
  std::string outcome;
  std::string accent;
  std::string reason;
  uint32_t removed = 0; // в долях единицы плана
  uint32_t colour = 0;
  bool start = false;
  bool operator==(const site_path_ending&) const noexcept = default;
};

struct site_view_scene {
  devils_engine::originator::motif_layout layout;
  std::vector<site_view_zone> zones; // one per graph node, including every door
  std::vector<site_view_join> joins;
  std::vector<site_path_ending> endings;
  std::vector<site_route_span> route_spans;
  site_projection_quality projection_quality;
  std::vector<uint32_t> owner;        // one-based graph node id; zero is wall
  std::vector<site_part_ref> part_owner; // (area, convex part) for every floor cell
  std::vector<uint8_t> exposed;
  std::string scale;
  uint64_t seed = 0;
  uint32_t catalogue_version = 0;
  uint32_t motif_catalogue_version = 0;
  size_t perceptions = 0;
};

site_view_scene make_site_view_scene(uint64_t seed, std::string_view scale,
                                     int32_t entry_y, bool surface_cut,
                                     std::string_view source, std::string_view motifs_source,
                                     site_bounds bounds = {});

int run_site_graph(uint64_t seed, std::string_view scale, int32_t entry_y,
                   bool surface_cut, bool verify, bool ascii, std::string_view dump,
                   std::string_view source, std::string_view motifs_source,
                   site_bounds bounds = {});

} // namespace gn06
