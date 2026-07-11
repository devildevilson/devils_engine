#include "script_environment.h"

#include <string>

#include <devils_engine/aesthetics/world.h>
#include <devils_engine/aesthetics/common.h> // entityid_t
#include <devils_engine/utils/core.h>        // utils::warn

#include "actor_simulation.h" // компоненты actor_drives / actor_perception

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// ── нативные аксессоры над root-скоупом entity_scope ──
// Возвращают дефолт при отсутствии компонента — паритет с нативными предикатами
// (predicate_is_hungry: нет actor_drives ⇒ голода нет). Скрипт вида `hunger >= 0.5` навигирует
// `hunger` на root (см. entity_scope).

static double scope_hunger(entity_scope s) {
  if (s.w == nullptr) return 0.0;
  const auto* dr = s.w->get<actor_drives>(aesthetics::entityid_t(s.id));
  return dr != nullptr ? double(dr->hunger) : 0.0;
}

static double scope_boredom(entity_scope s) {
  if (s.w == nullptr) return 0.0;
  const auto* dr = s.w->get<actor_drives>(aesthetics::entityid_t(s.id));
  return dr != nullptr ? double(dr->boredom) : 0.0;
}

static bool scope_threat_present(entity_scope s) {
  if (s.w == nullptr) return false;
  const auto* per = s.w->get<actor_perception>(aesthetics::entityid_t(s.id));
  return per != nullptr && per->has_threat;
}

static bool scope_prey_present(entity_scope s) {
  if (s.w == nullptr) return false;
  const auto* per = s.w->get<actor_perception>(aesthetics::entityid_t(s.id));
  return per != nullptr && per->has_prey;
}

// Добыча в радиусе хвата. Логика зеркалит нативный predicate_prey_in_range; eat_radius ДОЛЖЕН
// совпадать с actor_simulation.cpp (0.9). Дублирование константы — временно (до общего заголовка).
static bool scope_prey_in_range(entity_scope s) {
  if (s.w == nullptr) return false;
  const auto id = aesthetics::entityid_t(s.id);
  const auto* per = s.w->get<actor_perception>(id);
  const auto* pos = s.w->get<actor_position>(id);
  if (per == nullptr || pos == nullptr || !per->has_prey) return false;
  const glm::vec2 d = per->prey_pos - pos->value;
  constexpr float eat_radius = 0.9f;
  return (d.x * d.x + d.y * d.y) <= eat_radius * eat_radius;
}

static devils_script::system::options make_options() {
  devils_script::system::options opts;
  opts.error   = [](const std::string& m) { utils::warn("devils_script error: {}", m); };
  opts.warning = [](const std::string& m) { utils::warn("devils_script warning: {}", m); };
  return opts;
}

script_environment::script_environment() : sys(make_options()) {
  sys.init_basic_functions();
  sys.init_math();
  // scope авто-выводится из первого аргумента = entity_scope (value-скоуп, как handle<person>).
  sys.register_function<&scope_hunger>("hunger");
  sys.register_function<&scope_boredom>("boredom");
  sys.register_function<&scope_threat_present>("threat_present");
  sys.register_function<&scope_prey_present>("prey_present");
  sys.register_function<&scope_prey_in_range>("prey_in_range");
}

}
}
