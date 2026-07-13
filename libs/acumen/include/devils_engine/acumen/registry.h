#ifndef DEVILS_ENGINE_ACUMEN_REGISTRY_H
#define DEVILS_ENGINE_ACUMEN_REGISTRY_H

#include <memory>
#include <string>
#include <string_view>

#include <gtl/phmap.hpp>

#include "devils_engine/utils/string_id.h"
#include "system.h"

namespace devils_engine {
namespace acumen {

using system_id = utils::id;

// Immutable-system registry. Populate in the single-threaded load phase; returned pointers stay
// stable because systems are individually allocated even if the hash table grows.
class registry {
public:
  system_id add(std::string_view name, system&& value);
  const system* get(system_id id) const noexcept;
  const system* get(const std::string_view name) const noexcept;
  size_t size() const noexcept;

private:
  gtl::flat_hash_map<system_id, std::unique_ptr<system>> systems_;
  gtl::flat_hash_map<system_id, std::string> names_;
};

} // namespace acumen
} // namespace devils_engine

#endif
