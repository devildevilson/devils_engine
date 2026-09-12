#ifndef FRONTIER_ONLINE_CORE_CAUSAL_CONTENT_H
#define FRONTIER_ONLINE_CORE_CAUSAL_CONTENT_H

#include <memory>
#include <string>

#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>

#include "core/actor_simulation.h"
#include "core/content_root.h"
#include "core/script_environment.h"
#include "core/terrain.h"

// ПРИЧИННОЕ СОДЕРЖИМОЕ — всё, что определяет поведение мира, и НИЧЕГО больше: предикаты, наборы
// FSM и GOAP, префабы, генератор земли.
//
// Один класс на оба процесса намеренно. Авторитет, читающий своё особое дерево ресурсов, заложил
// бы расхождение сборок в само устройство стенда: две стороны считали бы разные миры, и обе были
// бы «правы». Здесь же считается отпечаток этого дерева — то единственное, что едет на
// рукопожатии вместо самих правил.

namespace frontier_online::core {

class causal_content {
public:
  // module_name — имя модуля внутри resource_root (у стенда единственный, "core").
  explicit causal_content(std::string resource_root, std::string module_name = "core");

  const brain_config& config() const noexcept {
    return config_;
  }
  const devils_engine::demiurg::resource_system& resources() const noexcept {
    return resources_;
  }
  const causal_content_manifest& manifest() const noexcept {
    return manifest_;
  }
  const std::string& resource_root() const noexcept {
    return resource_root_;
  }

  // Источник земли по объявленному миру. Отдельный экземпляр на каждый вызов: пайплайн
  // переиспользует буферы между чанками, поэтому делить его между потоками нельзя.
  std::unique_ptr<terrain_source> make_terrain(const std::string& generator, uint32_t chunk_size,
                                               uint64_t world_seed) const;

private:
  std::string resource_root_;
  std::string module_name_;
  script_environment scripts_;
  devils_engine::demiurg::module_system modules_;
  devils_engine::demiurg::resource_system resources_;
  brain_config config_;
  causal_content_manifest manifest_;
};

} // namespace frontier_online::core

#endif
