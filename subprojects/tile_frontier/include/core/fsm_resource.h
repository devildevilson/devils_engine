#ifndef TILE_FRONTIER_CORE_FSM_RESOURCE_H
#define TILE_FRONTIER_CORE_FSM_RESOURCE_H

#include <vector>

#include <devils_engine/demiurg/resource_base.h>
#include <devils_engine/mood/system.h>

// Disk config for a mood FSM. TAVL parses transition rows directly:
//   state + event [guard0, guard1] / (action0, action1) = next_state
// Parentheses around actions make the action list an unambiguous TAVL tuple. The resource produces
// structured mood::transition_config values; mood does not reparse synthetic quoted strings.

namespace tile_frontier {
namespace core {

struct fsm_config {
  std::vector<devils_engine::mood::transition_config> transitions;
};

class fsm_resource : public devils_engine::demiurg::resource_interface {
public:
  fsm_resource();
  const std::vector<devils_engine::mood::transition_config>& transitions() const noexcept {
    return config_.transitions;
  }

  void load_cold(const devils_engine::utils::safe_handle_t& handle) override;
  void load_warm(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_hot(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_warm(const devils_engine::utils::safe_handle_t& handle) override;

private:
  fsm_config config_;
};

} // namespace core
} // namespace tile_frontier

#endif
