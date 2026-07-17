#ifndef DEVILS_ENGINE_ACT_SCRIPT_COMPILER_H
#define DEVILS_ENGINE_ACT_SCRIPT_COMPILER_H

#include <string_view>

#include <devils_script/container.h>

#include "function.h"

namespace tavl {
class parser;
}

namespace devils_engine {
namespace act {

// Type-erased bridge between generic config resources and a project's devils_script root scope.
// Owner libraries keep their schemas/parsers; the project decides which (return type, scope) pairs
// are valid and supplies the concrete parse<Ret, RootScope> instantiations.
struct compiled_script {
  devils_script::container program;
  category type = category::predicate;
};

class script_compiler {
public:
  virtual ~script_compiler() noexcept = default;

  virtual void configure_parser(tavl::parser& parser) const = 0;
  virtual compiled_script compile(std::string_view name,
                                  std::string_view return_type,
                                  std::string_view scope,
                                  std::string_view expression) const = 0;
  virtual devils_script::container compile_predicate(std::string_view name,
                                                      tavl::parser& parser) const = 0;
  virtual devils_script::container compile_effect(std::string_view name,
                                                   tavl::parser& parser) const = 0;
};

} // namespace act
} // namespace devils_engine

#endif
