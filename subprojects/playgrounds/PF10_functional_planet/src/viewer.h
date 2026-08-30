#pragma once

#include <cstdint>
#include <string>

namespace devils_engine::pf10 {

struct viewer_options {
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t frames = 0;
  uint32_t mesh_side = 256;
  bool validation = false;
  bool fixed_rotation = false;
  std::string dump_path;
};

int run_viewer(const viewer_options& options);

} // namespace devils_engine::pf10
