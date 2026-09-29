#ifndef DEVILS_ENGINE_GN06_MOTIF_DEMO_H
#define DEVILS_ENGINE_GN06_MOTIF_DEMO_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "devils_engine/originator/motif_layout.h"

namespace gn06 {

struct motif_view_zone {
  std::string location;
  std::string motif;
  std::string rule;
  uint32_t colour = 0;
  uint32_t detail_cells = 0;
  uint8_t chance_percent = 100;
};

struct motif_view_scene {
  devils_engine::originator::motif_layout layout;
  std::vector<motif_view_zone> zones;
  std::vector<uint32_t> cell_colours; // RGB для подробного вида; план зон остаётся в layout
  std::string scale;
  uint64_t seed = 0;
  uint32_t catalogue_version = 0;
};

struct authored_motif_binding {
  uint32_t id = 0; // one-based id from motifs.tavl; zero for `none`
  uint32_t colour = 0;
  char glyph = '?';
};

struct authored_motif_detail {
  std::vector<uint32_t> colours; // RGB, zero outside motif rectangles
  size_t marked = 0;
};

std::vector<authored_motif_binding> bind_authored_motifs(
  std::string_view catalogue_text, std::span<const std::string> names);
authored_motif_detail make_authored_motif_detail(
  const devils_engine::originator::motif_layout& layout, uint64_t seed,
  std::string_view catalogue_text);

motif_view_scene make_motif_view_scene(uint64_t seed, std::string_view scale,
                                       size_t requested_side, int32_t entry_y,
                                       bool surface_cut, std::string_view catalogue_text);

int run_motif_demo(uint64_t seed, std::string_view scale, size_t requested_side,
                   int32_t entry_y, bool surface_cut,
                   bool verify, bool ascii, std::string_view dump,
                   std::string_view catalogue_text);

} // namespace gn06

#endif
