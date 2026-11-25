#ifndef FRONTIER_ONLINE_CORE_BRAIN_CONFIG_LOADER_H
#define FRONTIER_ONLINE_CORE_BRAIN_CONFIG_LOADER_H

#include <string_view>

#include "core/actor_simulation.h"

namespace devils_engine {
namespace demiurg {
class resource_system;
}
}

namespace frontier_online {
namespace core {

// Каталоги типов, из которых собираются мозги и префабы. Тип ресурса задаётся каталогом: под этими
// именами реестры регистрируют типы, мод добавляет файлы в каталог — и они подхватываются. Других
// строк, называющих те же каталоги, нет ни в коде, ни в конфигах.
inline constexpr std::string_view fsm_resource_type = "fsm";
inline constexpr std::string_view goap_resource_type = "goap";
inline constexpr std::string_view prefab_resource_type = "prefab";

// Собирает multi-brain конфиг: КАЖДЫЙ ресурс fsm/ и goap/ — отдельный именованный мозг (имя = id
// без каталога типа), на который префабы ссылаются строками goap=/fsm= (per-entity refs); все
// префабы prefab/ — в общий набор, обязательны `actor` и `food`.
brain_config load_brain_config(devils_engine::demiurg::resource_system& resources);

} // namespace core
} // namespace frontier_online

#endif
