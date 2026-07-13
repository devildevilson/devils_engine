#ifndef TILE_FRONTIER_CORE_FSM_RESOURCE_H
#define TILE_FRONTIER_CORE_FSM_RESOURCE_H

#include <string>
#include <vector>

#include <devils_engine/demiurg/resource_base.h>

// Дисковый конфиг FSM-исполнителя (mood): список строк-переходов "current + event [guards] = next".
// Гварды/действия ссылаются по имени на функции act::registry (нативки/скрипты) — mood резолвит их
// при построении. Здесь только данные; поведение остаётся в зарегистрированных функциях.

namespace tile_frontier {
namespace core {

struct fsm_config {
  std::vector<std::string> transitions;
};

class fsm_resource : public devils_engine::demiurg::resource_interface {
public:
  fsm_resource();
  const std::vector<std::string>& transitions() const noexcept {
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
