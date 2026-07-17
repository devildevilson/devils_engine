#ifndef DEVILS_ENGINE_SIMUL_STARTUP_RESOURCES_H
#define DEVILS_ENGINE_SIMUL_STARTUP_RESOURCES_H

// Resource types describing the startup entry and switchable runtime states.

#include <string>
#include <vector>

#include <devils_engine/demiurg/resource_base.h>

namespace devils_engine {
namespace simul {

struct startup_entry_config {
  std::string state;
};

struct runtime_state_config {
  std::string script;
  std::string default_font;
  std::vector<std::string> resources;
  std::string scene;
};

// Одна декларативная resource transition внутри scene manifest. target принимает имена
// cold/warm/hot/final либо неотрицательный номер ступени. group/alias движок не интерпретирует:
// это стабильные метаданные для project scene (например tile_textures и sounds/eating).
struct scene_resource_request {
  std::string id;
  std::string target = "final";
  std::string group;
  std::string alias;
  bool startup = false;
};

struct scene_manifest_config {
  // Optional logical id CPU-only project descriptor. Движок гарантирует, что он usable до
  // begin_project_loading(), но не знает его concrete type и содержимое.
  std::string project;
  std::vector<scene_resource_request> resources;
};

class startup_entry_resource : public demiurg::resource_interface {
public:
  startup_entry_resource();
  const startup_entry_config& config() const noexcept {
    return config_;
  }

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;

private:
  startup_entry_config config_;
};

class runtime_state_resource : public demiurg::resource_interface {
public:
  runtime_state_resource();
  const runtime_state_config& config() const noexcept {
    return config_;
  }

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;

private:
  runtime_state_config config_;
};

class scene_manifest_resource : public demiurg::resource_interface {
public:
  scene_manifest_resource();
  const scene_manifest_config& config() const noexcept {
    return config_;
  }

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;

private:
  scene_manifest_config config_;
};

} // namespace simul
} // namespace devils_engine

#endif
