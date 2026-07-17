#ifndef TILE_FRONTIER_CORE_SCRIPT_ENVIRONMENT_H
#define TILE_FRONTIER_CORE_SCRIPT_ENVIRONMENT_H

#include <devils_engine/act/script_compiler.h>
#include <devils_script/system.h>

#include "entity_scope.h"

// Проектный holder devils_script::system: ЕДИНСТВЕННАЯ точка регистрации нативных геймплейных
// функций (скрипты обязаны их видеть) + concrete act::script_compiler для entity_scope. Нативки
// трогают aesthetics-компоненты, поэтому root-scope dispatch живёт в проекте; generic script/GOAP
// resources остаются в act/acumen. Владелец — assets-система и переживает parse_resources.

namespace tile_frontier {
namespace core {

struct script_environment final : devils_engine::act::script_compiler {
  devils_script::system sys;
  script_environment();

  void configure_parser(tavl::parser& parser) const override;
  devils_engine::act::compiled_script compile(std::string_view name,
                                               std::string_view return_type,
                                               std::string_view scope,
                                               std::string_view expression) const override;
  devils_script::container compile_predicate(std::string_view name,
                                              tavl::parser& parser) const override;
  devils_script::container compile_effect(std::string_view name,
                                           tavl::parser& parser) const override;
};

} // namespace core
} // namespace tile_frontier

#endif
