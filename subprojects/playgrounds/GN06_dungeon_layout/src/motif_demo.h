#ifndef DEVILS_ENGINE_GN06_MOTIF_DEMO_H
#define DEVILS_ENGINE_GN06_MOTIF_DEMO_H

#include <cstddef>
#include <cstdint>
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
  uint8_t chance_percent = 100;
};

struct motif_view_scene {
  devils_engine::originator::motif_layout layout;
  std::vector<motif_view_zone> zones;
  std::string scale;
  uint64_t seed = 0;
  uint32_t catalogue_version = 0;
};

motif_view_scene make_motif_view_scene(uint64_t seed, std::string_view scale,
                                       size_t requested_side, int32_t entry_y,
                                       bool surface_cut, std::string_view catalogue_text);

int run_motif_demo(uint64_t seed, std::string_view scale, size_t requested_side,
                   int32_t entry_y, bool surface_cut,
                   bool verify, bool ascii, std::string_view dump,
                   std::string_view catalogue_text);

} // namespace gn06

#endif
