#ifndef DEVILS_ENGINE_DEMIURG_RESOURCE_MANIFEST_H
#define DEVILS_ENGINE_DEMIURG_RESOURCE_MANIFEST_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace devils_engine {
namespace demiurg {
class module_interface;

struct resource_candidate {
  std::string path;
  std::string id;
  std::string ext;
  std::vector<std::string> aliases;
  std::string list_name;
  std::string list_section;
  std::string_view module_name;
  const module_interface* module;
  size_t raw_size;
  uint32_t module_priority;
  uint32_t list_index = UINT32_MAX;
  uint32_t list_start_line = 0;
  size_t list_offset = SIZE_MAX;
  size_t list_size = 0;
};

bool append_tavl_list_candidates(
  std::vector<resource_candidate>& out,
  const resource_candidate& base,
  std::string_view content
);
}
}

#endif
