#include "cache.h"
#include "devils_engine/utils/hash.h" // utils::fmix64 / utils::hash_combine

namespace devils_engine {
namespace acumen {

size_t plan_key_hash::operator()(const plan_key& k) const noexcept {
  uint64_t h = utils::hash_combine(0, k.goal_id);
  for (const uint64_t w : k.bits) {
    h = utils::hash_combine(h, w);
  }
  return size_t(utils::fmix64(h));
}

solution_cache::solution_cache(const size_t max_bytes) noexcept
  : max_entries(max_bytes / entry_bytes < 1 ? 1 : max_bytes / entry_bytes) {}

const cached_plan* solution_cache::find(const plan_key& k) const noexcept {
  const auto it = entries.find(k);
  if (it == entries.end()) {
    ++miss_count;
    return nullptr;
  }
  ++hit_count;
  return &it->second;
}

bool solution_cache::insert(const plan_key& k, const cached_plan& p) {
  const auto it = entries.find(k);
  if (it != entries.end()) {
    it->second = p;
    return true;
  }
  if (entries.size() >= max_entries) {
    return false;
  }
  entries.emplace(k, p);
  return true;
}

void solution_cache::merge(const solution_cache& other) {
  for (const auto& [k, v] : other.entries) {
    if (entries.size() >= max_entries) {
      break;
    }
    entries.try_emplace(k, v);
  }
}

void solution_cache::clear() noexcept {
  entries.clear();
  hit_count = 0;
  miss_count = 0;
}

size_t solution_cache::size() const noexcept {
  return entries.size();
}
size_t solution_cache::capacity_entries() const noexcept {
  return max_entries;
}
bool solution_cache::full() const noexcept {
  return entries.size() >= max_entries;
}
size_t solution_cache::hits() const noexcept {
  return hit_count;
}
size_t solution_cache::misses() const noexcept {
  return miss_count;
}

} // namespace acumen
} // namespace devils_engine
