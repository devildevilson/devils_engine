#ifndef DEVILS_ENGINE_SIMUL_LUA_SCRIPT_RESOURCE_H
#define DEVILS_ENGINE_SIMUL_LUA_SCRIPT_RESOURCE_H

// Demiurg resource owning Lua source text and its resolved module-relative identity.

#include <string>

#include <devils_engine/demiurg/resource_base.h>

namespace devils_engine {
namespace simul {

class lua_script_resource : public demiurg::resource_interface {
public:
  std::string text;

  lua_script_resource();

  void ensure_text_loaded();
  void drop_text();

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;
};

} // namespace simul
} // namespace devils_engine

#endif
