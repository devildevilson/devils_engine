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
  std::vector<uint32_t> owner;        // one-based graph node id; zero is wall
  std::vector<site_part_ref> part_owner; // (area, convex part) for every floor cell
  std::vector<uint32_t> cell_colours; // WFC detail for floor, semantic colour for passages
  std::vector<uint8_t> route;
  std::vector<uint8_t> exposed;
  std::string scale;
  uint64_t seed = 0;
  uint32_t catalogue_version = 0;
  size_t perceptions = 0;
  size_t detail_cells = 0;
};

site_view_scene make_site_view_scene(uint64_t seed, std::string_view scale,
                                     int32_t entry_y, bool surface_cut,
                                     std::string_view source, std::string_view motifs_source);

int run_site_graph(uint64_t seed, std::string_view scale, int32_t entry_y,
                   bool surface_cut, bool verify, bool ascii, std::string_view dump,
                   std::string_view source, std::string_view motifs_source);

} // namespace gn06
