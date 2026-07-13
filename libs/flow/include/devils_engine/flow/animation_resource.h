#ifndef DEVILS_ENGINE_FLOW_ANIMATION_RESOURCE_H
#define DEVILS_ENGINE_FLOW_ANIMATION_RESOURCE_H

// Demiurg-backed animation resource consumed by the flow presentation system.

#include "devils_engine/demiurg/resource_base.h"
#include "devils_engine/flow/system.h"

namespace devils_engine {
namespace flow {

class animation_resource : public demiurg::resource_interface {
public:
  animation_resource(library* lib = nullptr, const demiurg::resource_system* resources = nullptr);

  const std::vector<uint32_t>& state_indices() const noexcept;

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;

private:
  library* lib_ = nullptr;
  const demiurg::resource_system* resources_ = nullptr;
  std::vector<uint32_t> indices_;
};

} // namespace flow
} // namespace devils_engine

#endif
