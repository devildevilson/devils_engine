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

  const std::string content = is_list_entry() && !list_section.empty() ? list_section : module->load_text(path);
  std::vector<std::string> next_names;
  parse_options options{};
  if (is_list_entry() && list_start_line > 0) options.line_offset = list_start_line - 1;
  const auto parsed = parse_state_text(content, id, resources_, &next_names, options);

  indices_.clear();

  if (is_list_entry() && parsed.size() == 1) {
    const uint32_t index = lib_->add_state(std::string(id), parsed[0]);
    indices_.push_back(index);
    if (!next_names.empty() && !next_names[0].empty()) lib_->set_next(index, next_names[0]);
    lib_->resolve_pending_links(false);
  } else {
    const uint32_t first = lib_->append_resource_states(id, parsed, next_names);
    indices_.reserve(parsed.size());
    for (uint32_t i = 0; i < parsed.size(); ++i) {
      indices_.push_back(first + i);
    }
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
