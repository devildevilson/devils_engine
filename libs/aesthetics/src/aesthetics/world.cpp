#define DEVILS_ENGINE_AESTHETICS_IMPLEMENTATION
#include "devils_engine/aesthetics/world.h"

namespace devils_engine {
namespace aesthetics {
void basic_system::receive(const update_event& event) {
  update(event.time);
}

bool all_of_not_null() {
  return true;
}

bool all_of_is_null() {
  return true;
}

world::component_storage::component_storage(
  const std::string_view& type_name,
  const size_t type_index) noexcept
  : type_name(type_name), type_index(type_index) {}

world::raw_itr::raw_itr(underlying_itr it, underlying_itr end, const size_t index) noexcept
  : it(it), end(end), index(index) {
  skip_invalid();
}

entityid_t world::raw_itr::operator*() const noexcept {
  return make_entityid(index, get_entityid_version(*it));
}

world::raw_itr& world::raw_itr::operator++() noexcept {
  if (it != end) {
    ++it;
    ++index;
  }

  skip_invalid();
  return *this;
}

world::raw_itr world::raw_itr::operator++(int) noexcept {
  auto cur = *this;
  ++(*this);
  return cur;
}

void world::raw_itr::skip_invalid() noexcept {
  while (it != end && is_invalid_entityid(*it)) {
    ++it;
    ++index;
  }
}
} // namespace aesthetics
} // namespace devils_engine
