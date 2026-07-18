#include "devils_engine/utils/core.h"
#include "registry.h"

namespace devils_engine {
namespace acumen {

system_id registry::add(const std::string_view name, system&& value) {
  const system_id id = utils::string_hash(name);
  const auto [it, inserted] = systems_.emplace(id, std::make_unique<system>(std::move(value)));
  if (!inserted) {
    const auto prev = names_.find(id);
    const std::string_view prev_name = prev != names_.end() ? std::string_view(prev->second) : "<unknown>";
    utils::error{}("acumen::registry: duplicate/hash collision '{}' vs '{}' ({:#x})", prev_name, name, id);
  }
  // соль кеша планов = system_id: несколько систем реестра могут разделять один solution_cache
  // (per-entity мозги на общих per-thread кешах), план же хранит индексы действий своей системы.
  it->second->set_cache_salt(id);
  names_.emplace(id, std::string(name));
  return id;
}

const system* registry::get(const system_id id) const noexcept {
  const auto it = systems_.find(id);
  return it != systems_.end() ? it->second.get() : nullptr;
}

const system* registry::get(const std::string_view name) const noexcept {
  return get(utils::string_hash(name));
}

size_t registry::size() const noexcept {
  return systems_.size();
}

} // namespace acumen
} // namespace devils_engine
