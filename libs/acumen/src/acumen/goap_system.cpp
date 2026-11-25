#include "devils_engine/acumen/goap_system.h"

#include <algorithm>

#include <devils_engine/utils/error.h>

namespace devils_engine {
namespace acumen {

namespace {

class bit_numbering {
public:
  bit_numbering(const std::string_view brain, const goap_config& config) : brain_(brain) {
    if (config.metrics.size() > state{}.size()) {
      utils::error{}("GOAP brain '{}' declares {} metrics, acumen state holds {} bits (DEVILS_ENGINE_ACUMEN_STATE_SIZE)",
                     brain, config.metrics.size(), state{}.size());
    }
    names_.reserve(config.metrics.size() + 4);
    for (const auto& metric : config.metrics) {
      names_.push_back(metric.key);
    }
  }

  size_t index_of(const std::string& key) {
    const auto it = std::find(names_.begin(), names_.end(), key);
    if (it != names_.end()) {
      return size_t(it - names_.begin());
    }
    if (names_.size() >= state{}.size()) {
      utils::error{}("GOAP brain '{}' needs bit '{}' beyond the {} state bits (DEVILS_ENGINE_ACUMEN_STATE_SIZE)",
                     brain_, key, state{}.size());
    }
    names_.push_back(key);
    return names_.size() - 1;
  }

  scoped_state build(const std::vector<goap_bit>& bits) {
    scoped_state out;
    for (const auto& bit : bits) {
      out.set(index_of(bit.key), bit.value);
    }
    return out;
  }

private:
  std::string_view brain_;
  std::vector<std::string> names_;
};

} // namespace

system build_system(const act::registry* functions, const std::string_view brain, const goap_config& config) {
  bit_numbering bits(brain, config);

  std::vector<state_metric> metrics;
  metrics.reserve(config.metrics.size());
  for (const auto& metric : config.metrics) {
    metrics.emplace_back(metric.key);
  }

  std::vector<action> actions;
  actions.reserve(config.actions.size());
  for (const auto& a : config.actions) {
    actions.emplace_back(a.name, bits.build(a.requirements), bits.build(a.next_state), bits.build(a.weight_state));
  }

  std::vector<goal> goals;
  goals.reserve(config.goals.size());
  for (const auto& g : config.goals) {
    goals.push_back(goal{g.name, bits.build(g.requirements), bits.build(g.goal)});
  }

  return system(functions, std::move(metrics), std::move(goals), std::move(actions));
}

bool goap_function_names::claim(const std::string_view key, const std::string_view origin) {
  for (const auto& [known, known_origin] : origins_) {
    if (known == key) {
      if (known_origin != origin) {
        utils::error{}("GOAP name '{}' is defined by both '{}' and '{}': brains share one act namespace, "
                       "rename one of them",
                       key, known_origin, origin);
      }
      return false;
    }
  }
  origins_.emplace_back(std::string(key), std::string(origin));
  return true;
}

void goap_function_names::clear() noexcept {
  origins_.clear();
}

} // namespace acumen
} // namespace devils_engine
