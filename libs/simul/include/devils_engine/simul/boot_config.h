#ifndef DEVILS_ENGINE_SIMUL_BOOT_CONFIG_H
#define DEVILS_ENGINE_SIMUL_BOOT_CONFIG_H

// Engine bootstrap configuration independent of any project's settings schema.

#include <cstdint>
#include <string>

namespace devils_engine {
namespace simul {

struct engine_boot_config {
  std::string resource_root = "resources/";
  std::string engine_module = "engine";
  std::string app_config_id = "config/app";
  std::string settings_file = "settings.tavl";
  std::string cache_root = "cache";

  bool render_enabled = true;
  bool sound_enabled = true;
  bool assets_enabled = true;
  bool headless = false;

  uint32_t main_fps = 20;
  uint32_t render_fps = 60;
  uint32_t sound_fps = 60;
  uint32_t assets_fps = 60;

  uint32_t worker_threads_reserved = 4;
  uint32_t min_worker_threads = 1;
  uint32_t thread_start_gap_divisor = 4;
};

} // namespace simul
} // namespace devils_engine

#endif
