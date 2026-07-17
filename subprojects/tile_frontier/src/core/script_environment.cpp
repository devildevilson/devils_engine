#include <cstdint>
#include <string>

#include <devils_engine/act/stat_accessors.h>
#include <devils_engine/aesthetics/common.h> // entityid_t
#include <devils_engine/aesthetics/world.h>
#include <devils_engine/utils/core.h> // utils::warn

#include "actor_simulation.h" // компоненты stats / actor_perception
#include "script_environment.h"
#include "spawn_scope.h" // spawn_scope / scope_spawn_at — примитивный ds-спавн

namespace tile_frontier {
namespace core {

using namespace devils_engine;

namespace {
// Домен catalogue для эффектов stats (add_<field>). Значение — стабильный id домена интроспекции/реплея.
enum class stat_domain : uint32_t { actor_stats = 1 };
} // namespace

// getter entity_scope -> stats*: scope-функция `stats` возвращает типизированный толстый указатель.
// const_cast: мир
// реально мутабелен (эффект add_<field> пишет), const на entity_scope.w — лишь контракт чтения (как
// mutable_world_of). null ⇒ read вернёт дефолт, add — no-op (паритет с прежним scope_hunger).
static stats* get_actor_stats(entity_scope s) noexcept {
  if (s.w == nullptr) {
    return nullptr;
  }
  return const_cast<stats*>(s.w->get<stats>(aesthetics::entityid_t(s.id)));
}

// ── нативные bool-аксессоры над entity_scope (перцепция — не плоские числа, остаются ручными) ──
static bool scope_threat_present(entity_scope s) {
  if (s.w == nullptr) {
    return false;
  }
  const auto* per = s.w->get<actor_perception>(aesthetics::entityid_t(s.id));
  return per != nullptr && per->has_threat;
}

static bool scope_prey_present(entity_scope s) {
  if (s.w == nullptr) {
    return false;
  }
  const auto* per = s.w->get<actor_perception>(aesthetics::entityid_t(s.id));
  return per != nullptr && per->has_prey;
}

// Добыча в радиусе хвата. Логика зеркалит нативный predicate_prey_in_range; eat_radius ДОЛЖЕН
// совпадать с actor_simulation.cpp (0.9). Дублирование константы — временно (до общего заголовка).
static bool scope_prey_in_range(entity_scope s) {
  if (s.w == nullptr) {
    return false;
  }
  const auto id = aesthetics::entityid_t(s.id);
  const auto* per = s.w->get<actor_perception>(id);
  const auto* pos = s.w->get<actor_position>(id);
  if (per == nullptr || pos == nullptr || !per->has_prey) {
    return false;
  }
  const glm::vec2 d = per->prey_pos - pos->value;
  constexpr float eat_radius = 0.9f;
  return (d.x * d.x + d.y * d.y) <= eat_radius * eat_radius;
}

static devils_script::system::options make_options() {
  devils_script::system::options opts;
  opts.error = [](const std::string& m) {
    utils::warn("devils_script error: {}", m);
  };
  opts.warning = [](const std::string& m) {
    utils::warn("devils_script warning: {}", m);
  };
  return opts;
}

script_environment::script_environment() : sys(make_options()) {
  sys.init_basic_functions();
  sys.init_math();

  // Effects used by config-loaded GOAP action scripts. These are generated catalogue deferred
  // pointers, so a script pass only records typed calls; actor_world_slice::apply owns commit.
  register_actor_effect_building_blocks(sys);

  // `stats` меняет ds-scope; внутри него рефлексией доступны hunger/boredom/strength и add_<field>.
  act::register_stats<stats, entity_scope, &get_actor_stats, stat_domain::actor_stats>(sys, "stats");

  // Перцепция (bool, не плоские числа) — остаётся ручными аксессорами над entity_scope.
  sys.register_function<&scope_threat_present>("threat_present");
  sys.register_function<&scope_prey_present>("prey_present");
  sys.register_function<&scope_prey_in_range>("prey_in_range");

  // Примитивный спавн: spawn_at(prefab, x, y) над spawn_scope (несёт мутабельный sink). Скрипт-спавнеры
  // (события/триггеры), запросы filter/pick и динамические точки — тех-долг (ROADMAP/AGENTS).
  sys.register_function<&scope_spawn_at>("spawn_at");
}

void script_environment::configure_parser(tavl::parser& parser) const {
  sys.configure_parser(parser);
}

act::compiled_script script_environment::compile(
  const std::string_view name,
  const std::string_view return_type,
  const std::string_view scope,
  const std::string_view expression) const {
  if (scope != "actor") {
    utils::error{}("script '{}': scope '{}' is not supported by tile_frontier", name, scope);
  }
  if (return_type == "bool") {
    return act::compiled_script{sys.parse<bool, entity_scope>(name, expression), act::category::predicate};
  }
  utils::error{}("script '{}': return type '{}' is not supported by tile_frontier", name, return_type);
}

devils_script::container script_environment::compile_predicate(
  const std::string_view name,
  tavl::parser& parser) const {
  devils_script::container program;
  devils_script::system::parse_context ctx;
  sys.parse<bool, entity_scope>(name, parser, ctx, program);
  return program;
}

devils_script::container script_environment::compile_effect(
  const std::string_view name,
  tavl::parser& parser) const {
  devils_script::container program;
  devils_script::system::parse_context ctx;
  sys.parse<void, entity_scope>(name, parser, ctx, program);
  return program;
}

} // namespace core
} // namespace tile_frontier
