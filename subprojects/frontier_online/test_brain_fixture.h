#ifndef FRONTIER_ONLINE_TEST_BRAIN_FIXTURE_H
#define FRONTIER_ONLINE_TEST_BRAIN_FIXTURE_H

#include <string>

#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>

#include "core/actor_simulation.h"
#include "core/script_environment.h"

// Test-only owner of the real shipped actor resources. Tests use this fixture instead of
// reconstructing production FSM/GOAP/prefab/script defaults in C++.
class test_brain_fixture {
public:
  explicit test_brain_fixture(std::string resource_root);

  const frontier_online::core::brain_config& config() const noexcept {
    return config_;
  }

  // Прямой доступ к проектному ds::system — тесты проверяют компиляцию building blocks
  // (сигнатуры/регистрацию) без полного record/commit пайплайна.
  frontier_online::core::script_environment& scripts() noexcept {
    return scripts_;
  }

private:
  frontier_online::core::script_environment scripts_;
  devils_engine::demiurg::module_system modules_;
  devils_engine::demiurg::resource_system resources_;
  frontier_online::core::brain_config config_;
};

#endif
