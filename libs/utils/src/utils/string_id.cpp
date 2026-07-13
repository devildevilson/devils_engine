#include "devils_engine/utils/string_id.h"
#include "rapidhash.h"

namespace devils_engine {
namespace utils {
id string_hash(const std::string_view& str) noexcept {
  return rapidhash(str.data(), str.size());
}

id string_hash(const std::string_view& str, const uint64_t seed) noexcept {
  return rapidhash_withSeed(str.data(), str.size(), seed);
}
} // namespace utils
} // namespace devils_engine
