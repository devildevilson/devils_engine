#ifndef TILE_FRONTIER_CORE_SPAWN_SCOPE_H
#define TILE_FRONTIER_CORE_SPAWN_SCOPE_H

#include <cstdint>
#include <string_view>

#include <devils_engine/aesthetics/common.h> // entityid_t
#include <glm/glm.hpp>

// Примитивный спавн из devils_script. spawn_scope — root-скоуп спавн-скриптов: несёт МУТАБЕЛЬНУЮ
// способность спавна (spawn_sink), в отличие от entity_scope (const world для аксессоров). Натив
// spawn_at(prefab, x, y) вызывает sink. Пока это примитив «префаб в точку»; спавнеры-энтити, запросы
// (filter/pick) и динамический выбор точки — тех-долг (см. ROADMAP / AGENTS). Засев spawn_scope в
// живые скрипты (события/триггеры) придёт вместе с ними — сейчас натив зарегистрирован и вызываем.

namespace tile_frontier {
namespace core {

// Мутабельная способность спавна: реализует actor_world_slice (спавн через prefab_registry). Тест —
// через мок. Форма минимальна намеренно (примитив): имя префаба + точка.
struct spawn_sink {
  virtual devils_engine::aesthetics::entityid_t spawn_prefab(std::string_view name, glm::vec2 pos) = 0;

protected:
  ~spawn_sink() = default;
};

// Root-скоуп спавн-скрипта (≤16 байт, trivially destructible, .valid() — как entity_scope/handle<>).
struct spawn_scope {
  spawn_sink* sink = nullptr;
  bool valid() const noexcept {
    return sink != nullptr;
  }
};

// ds-натив: спавн префаба по имени в точке. Мутирует через скоуп (как add_charisma/add_<stat>), без
// on_effect. Инлайн, чтобы точку регистрации переиспользовал юнит-тест. prefab — bareword-строка
// скрипта → string_view; x/y — числа.
inline void scope_spawn_at(spawn_scope s, std::string_view prefab, double x, double y) {
  if (s.sink != nullptr) {
    s.sink->spawn_prefab(prefab, glm::vec2{float(x), float(y)});
  }
}

} // namespace core
} // namespace tile_frontier

#endif
