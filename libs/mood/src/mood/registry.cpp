#include "registry.h"

#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace mood {

system_id registry::add(const std::string_view name, system&& value) {
  const system_id id = utils::string_hash(name);
  const auto [it, inserted] = systems_.emplace(id, std::make_unique<system>(std::move(value)));
  if (!inserted) {
    const auto prev = names_.find(id);
    const std::string_view prev_name = prev != names_.end() ? std::string_view(prev->second) : "<unknown>";
    utils::error{}("mood::registry: duplicate/hash collision '{}' vs '{}' ({:#x})", prev_name, name, id);
  }
  names_.emplace(id, std::string(name));
  return id;
}

const system* registry::get(const system_id id) const noexcept {
  const auto it = systems_.find(id);
  return it != systems_.end() ? it->second.get() : nullptr;
}

}
}
