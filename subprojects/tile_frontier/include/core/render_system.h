#ifndef TILE_FRONTIER_CORE_RENDER_SYSTEM_H
#define TILE_FRONTIER_CORE_RENDER_SYSTEM_H

#include <cstddef>
#include <memory>
#include <string>

#include <devils_engine/simul/systems.h>

#include "actors.h"

namespace devils_engine { namespace demiurg { class resource_system; } }

namespace tile_frontier {
namespace core {

using namespace devils_engine;

struct render_simulation_init;
struct broker;

// Параметры запуска рендера. Заполняются из app_config в simulation::init()
// и фиксируются на время жизни render_simulation.
struct render_simulation_config {
  // Движковый demiurg-реестр (config/shaders/render-graph), построенный в simulation::init
  // и доступный только на чтение. render-graph грузится из него по префиксу render_config_prefix
  // (ресурсы render_config_source), а не сканом папки. См. demiurg 1a, срез 2.
  const demiurg::resource_system* engine_registry = nullptr;
  std::string render_config_prefix; // напр. "render_config/"
  std::string shader_config_prefix; // префикс шейдеров в реестре, напр. "shaders/"
  // Pipeline cache через demiurg (Фаза 2): отдельный реестр над writable cache-папкой.
  // load — из ресурса cache_registry->get(pipeline_cache_id); dump — на диск в pipeline_cache_path.
  const demiurg::resource_system* cache_registry = nullptr;
  std::string pipeline_cache_id; // напр. "pipeline_cache/main"
  std::string pipeline_cache_path;
  std::string graph_name = "graphics1";
  // п.2/п.3: доп. resident-граф (его ресурсы входят в общий used-set). Пусто ⇒ только graph_name.
  std::string menu_graph_name;
  bool create_vulkan_on_init = true;
  bool headless = false;
};

class render_simulation : public simul::render_system<broker> {
public:
  render_simulation(const size_t frame_time, render_simulation_config config) noexcept;
  ~render_simulation() noexcept;
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

  graphics_actor* get_actor();

  // Единый broker всех каналов (main владеет). Задаётся до старта потока; заодно триггерит попытку
  // сборки графа (как раньше делал set_assets_actor).
  void set_broker(struct broker* b);
private:
  std::unique_ptr<render_simulation_init> container;
  graphics_actor actor;
};

}
}

#endif
