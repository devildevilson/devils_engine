#ifndef DEVILS_ENGINE_SIMUL_STARTUP_RESOURCES_H
#define DEVILS_ENGINE_SIMUL_STARTUP_RESOURCES_H

#include <string>
#include <vector>

#include <devils_engine/demiurg/resource_base.h>

namespace devils_engine {
namespace simul {

struct startup_entry_config {
  std::string ui_state;
  std::string scene;
};

struct ui_state_config {
  std::string script;
  std::vector<std::string> resources;
};

class startup_entry_resource : public demiurg::resource_interface {
public:
  startup_entry_resource();
  const startup_entry_config& config() const noexcept { return config_; }

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;

private:
  startup_entry_config config_;
};

class ui_state_resource : public demiurg::resource_interface {
public:
  ui_state_resource();
  const ui_state_config& config() const noexcept { return config_; }

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;

private:
  ui_state_config config_;
};

}
}

#endif
