#ifndef DEVILS_ENGINE_MOOD_REGISTRY_H
#define DEVILS_ENGINE_MOOD_REGISTRY_H

// Stable-name registry owning immutable finite-state-machine systems.

#include <memory>
#include <string>
#include <string_view>

#include <gtl/phmap.hpp>

#include "devils_engine/utils/string_id.h"
#include "system.h"

namespace devils_engine {
namespace mood {

using system_id = utils::id;

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

} // namespace mood
} // namespace devils_engine

#endif
