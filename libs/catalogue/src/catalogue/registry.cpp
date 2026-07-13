#include "devils_engine/catalogue/registry.h"

namespace devils_engine {
namespace catalogue {
bool operator==(const registry::info& a, const registry::info& b) noexcept {
  return a.name == b.name;
}

bool operator!=(const registry::info& a, const registry::info& b) noexcept {
  return !(a == b);
}

void registry::reg(const size_t id, const std::string_view& name, const info::invoke_fn fn) {
  funcs.erase(id);
  funcs.emplace(id, info{id, name, fn});
}
} // namespace catalogue
} // namespace devils_engine
