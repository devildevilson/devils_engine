#ifndef DEVILS_ENGINE_PAINTER_RENDER_CONFIG_SOURCE_H
#define DEVILS_ENGINE_PAINTER_RENDER_CONFIG_SOURCE_H

#include <string>
#include "devils_engine/demiurg/resource_base.h"

namespace devils_engine {
namespace painter {

// Один tavl-файл описания render-graph как demiurg-ресурс (движковый, незаменяемый
// реестр). CPU-only (warm_and_hot_same): load_cold читает текст через модуль. parse_data
// кормится этим текстом вместо скана папки с диска (см. demiurg 1a, срез 2).
class render_config_source : public demiurg::resource_interface {
public:
  std::string text;

  render_config_source();

  void ensure_text_loaded();
  void drop_text();

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;
};

}
}

#endif
