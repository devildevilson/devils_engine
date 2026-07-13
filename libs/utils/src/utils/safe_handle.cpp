#include "devils_engine/utils/safe_handle.h"

namespace devils_engine {
namespace utils {
safe_handle_t::safe_handle_t() noexcept : type(utils::type_id<void>()), ptr(nullptr) {}

bool safe_handle_t::valid() const noexcept {
  return !is<void>();
}
} // namespace utils
} // namespace devils_engine
