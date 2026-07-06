#ifndef DEVILS_ENGINE_DEMIURG_RESOURCE_MANIFEST_H
#define DEVILS_ENGINE_DEMIURG_RESOURCE_MANIFEST_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace devils_engine {
namespace demiurg {
class module_interface;

struct resource_candidate {
  std::string path;
  std::string id;
  std::string ext;
  std::string_view module_name;
  const module_interface* module;
  size_t raw_size;
  uint32_t module_priority;
};
}
}

#endif
