#ifndef DEVILS_ENGINE_ACT_SCRIPT_PARSE_H
#define DEVILS_ENGINE_ACT_SCRIPT_PARSE_H

#include <source_location>
#include <string_view>

#include <devils_script/system.h>
#include <tavl/parser.h>

#include <devils_engine/utils/error.h>

namespace devils_engine {
namespace act {

// Разбор инлайн-скрипта ds из потока tavl-парсера (метрики GOAP, guard'ы FSM, эффекты конфигов).
// ds на этом входе сознательно не бросает: сообщения он отдаёт в `options.error` системы и возвращает
// отказ значением, чтобы вызывающий мог собрать все ошибки. Для скрипта из конфига отказ критичен —
// пустая программа сошла бы за годную, — поэтому здесь `utils::error` с именем скрипта и позицией в
// тексте. Место вызова сохраняется через `location`.
template <typename RETURN_T, typename ROOT_T>
devils_script::container parse_script(const devils_script::system& sys, const std::string_view name,
                                      tavl::parser& parser,
                                      const std::source_location location = std::source_location::current()) {
  devils_script::container program;
  devils_script::system::parse_context ctx;
  const auto [event, err] = sys.template parse<RETURN_T, ROOT_T>(name, parser, ctx, program);
  static_cast<void>(event);
  if (err.is_critical()) {
    utils::error{location}("script '{}': devils_script rejected it at line {}, column {} ({}); the reason goes to "
                           "options.error of the devils_script system",
                           name, err.span.line, err.span.column, tavl::to_string(err.type));
  }
  return program;
}

} // namespace act
} // namespace devils_engine

#endif
