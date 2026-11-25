#include "devils_engine/utils/log.h"
#include "devils_engine/act/script_math.h"
#include <cstdint>
#include <string>
#include <tuple>

#include <devils_engine/act/script_parse.h>
#include <devils_engine/act/building_blocks.h>
#include <devils_engine/act/stat_accessors.h>
#include <devils_engine/aesthetics/common.h> // entityid_t
#include <devils_engine/aesthetics/world.h>
#include <devils_engine/utils/core.h> // utils::warn / utils::error

#include "actor_simulation.h" // компонент stats + actor_building_blocks()
#include "script_environment.h"

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

static devils_script::system::options make_options() {
  devils_script::system::options opts;
  // Начиная с devils_script 1.3.1 подбор перегрузки НЕ диагностичен: отбракованный кандидат
  // не доходит ни до одного из этих двух обработчиков (внутри библиотеки `raise_error` помечен
  // [[noreturn]] и бросает свой собственный тип). Значит каждое сообщение здесь — настоящий отказ
  // разбора, и уровень ему полагается error, а не warn.
  //
  // Бросок отсюда библиотека ГЛУШИТ: перегрузка `parse`, принимающая tavl::parser, вызывает этот
  // обработчик внутри своего catch и держит свой контракт «отказ = возвращаемое значение». Поэтому
  // остановка загрузки живёт не тут, а в act::parse_script у compile_predicate/compile_effect.
  opts.error = [](const std::string& m) {
    utils::error{}("devils_script: {}", m);
  };
  opts.warning = [](const std::string& m) {
    utils::warn("devils_script warning: {}", m);
  };
  return opts;
}

script_environment::script_environment() : sys(make_options()) {
  sys.init_basic_functions();
  devils_engine::act::init_deterministic_math(sys); // не init_math: libm у платформ расходится

  // ds-словарь gameplay building blocks (deferred-эффекты + аксессоры восприятия + spawn_at) —
  // один декларативный список в actor_simulation.cpp; здесь только его ds-часть.
  actor_building_blocks().register_ds(sys);

  // `stats` меняет ds-scope; внутри него рефлексией доступны hunger/boredom/strength и add_<field>.
  act::register_stats<stats, entity_scope, &get_actor_stats, stat_domain::actor_stats>(sys, "stats");
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
  return act::parse_script<bool, entity_scope>(sys, name, parser);
}

devils_script::container script_environment::compile_effect(
  const std::string_view name,
  tavl::parser& parser) const {
  return act::parse_script<void, entity_scope>(sys, name, parser);
}

} // namespace core
} // namespace tile_frontier
