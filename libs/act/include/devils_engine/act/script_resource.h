#ifndef DEVILS_ENGINE_ACT_SCRIPT_RESOURCE_H
#define DEVILS_ENGINE_ACT_SCRIPT_RESOURCE_H

#include <string>

#include <devils_engine/act/script_compiler.h>
#include <devils_engine/demiurg/resource_base.h>

namespace devils_engine {
namespace act {

// Generic TAVL script document. The injected script_compiler owns the project-specific dispatch
// from (ret, scope) to a concrete devils_script::system::parse<Ret, RootScope> instantiation.
struct script_config {
  std::string ret;
  std::string scope;
  std::string expr;
};

// Demiurg resource that parses the document and delegates compilation to the injected adapter.
// Runtime consumers wrap program() in the matching act::script_function category.
class script_resource : public devils_engine::demiurg::resource_interface {
public:
  explicit script_resource(const script_compiler* compiler);

  const devils_script::container* program() const noexcept {
    return &program_;
  }
  act::category category() const noexcept {
    return category_;
  }

  void load_cold(const devils_engine::utils::safe_handle_t& handle) override;
  void load_warm(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_hot(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_warm(const devils_engine::utils::safe_handle_t& handle) override;

private:
  const script_compiler* compiler_ = nullptr;
  devils_script::container program_;
  act::category category_ = act::category::predicate;
};

} // namespace act
} // namespace devils_engine

#endif
