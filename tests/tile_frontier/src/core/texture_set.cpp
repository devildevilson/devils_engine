#include "texture_set.h"

#include <devils_engine/demiurg/resource_system.h>

namespace tile_frontier {
namespace core {

uint32_t texture_set::gather(demiurg::resource_system& reg, const std::string_view& prefix) {
  textures.clear();
  // filter<T>: подстрочный поиск по id ("по части пути") + фильтр по типу T. Итерирует массив
  // ресурсов напрямую. НЕ используем resource_system::find<T> — он идёт через demiurg::view<>,
  // у которого operator[] возвращает висячую ссылку на временный T* (баг: "returning reference
  // to temporary", resource_system.h:50) → UB/segfault. См. заметку в обзоре.
  reg.filter<texture_resource>(prefix, textures);
  return uint32_t(textures.size());
}

} // namespace core
} // namespace tile_frontier
