#include "devils_engine/flow/animation_resource.h"

#include "devils_engine/demiurg/module_interface.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace flow {

animation_resource::animation_resource(library* lib, const demiurg::resource_system* resources) :
  lib_(lib),
  resources_(resources)
{
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void animation_resource::load_cold(const utils::safe_handle_t&) {
  if (lib_ == nullptr) {
    utils::warn("flow animation_resource '{}': no library, skipping parse", id);
    return;
  }

  const std::string content = module->load_text(path);
  std::vector<std::string> next_names;
  const auto parsed = parse_state_text(content, id, resources_, &next_names);
  const uint32_t first = lib_->append_resource_states(id, parsed, next_names);

  indices_.clear();
  indices_.reserve(parsed.size());
  for (uint32_t i = 0; i < parsed.size(); ++i) {
    indices_.push_back(first + i);
  }

  utils::info("flow animation_resource '{}': parsed {} states", id, parsed.size());
}

void animation_resource::load_warm(const utils::safe_handle_t&) {}
void animation_resource::unload_hot(const utils::safe_handle_t&) {}
void animation_resource::unload_warm(const utils::safe_handle_t&) {
  // States are currently append-only in flow::library. Runtime users may keep
  // state indices, so removing/repacking them would invalidate playback.
  indices_.clear();
}

}
}
