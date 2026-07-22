#ifndef DEVILS_ENGINE_CARDGAME_COMBAT_SCRIPT_H
#define DEVILS_ENGINE_CARDGAME_COMBAT_SCRIPT_H

#include <cstddef>
#include <cstdint>
#include <span>

#include <devils_script/container.h>
#include <devils_script/context.h>
#include <devils_script/system.h>

#include <devils_engine/act/script_compiler.h>
#include <devils_engine/utils/string_id.h>

#include "cardgame/combat.h"

namespace devils_engine::demiurg {
class resource_system;
}

namespace cardgame {
namespace core {

// Runtime lookup seam for pointer-free script ids stored in resolution_work. A combat snapshot can
// be loaded into another combat instance backed by an equivalent resource catalogue.
class combat_effect_script_provider {
public:
  virtual ~combat_effect_script_provider() noexcept = default;
  virtual const devils_script::container* find(
    devils_engine::utils::id script) const noexcept = 0;
};

// Read-only adapter over a loaded demiurg catalogue. It does not own or load resources; missing,
// unloaded, or non-effect entries fail at the combat invocation boundary.
class combat_effect_script_resources final : public combat_effect_script_provider {
public:
  explicit combat_effect_script_resources(
    const devils_engine::demiurg::resource_system& resources) noexcept;

  const devils_script::container* find(
    devils_engine::utils::id script) const noexcept override;

private:
  const devils_engine::demiurg::resource_system* resources_ = nullptr;
};

// Project compiler adapter used by generic act::script_resource documents with
// `ret = void`, `scope = combat_effect`.
class combat_effect_script_compiler final
  : public devils_engine::act::script_compiler {
public:
  combat_effect_script_compiler();

  void configure_parser(tavl::parser& parser) const override;
  devils_engine::act::compiled_script compile(
    std::string_view name,
    std::string_view return_type,
    std::string_view scope,
    std::string_view expression) const override;
  devils_script::container compile_predicate(
    std::string_view name, tavl::parser& parser) const override;
  devils_script::container compile_effect(
    std::string_view name, tavl::parser& parser) const override;

private:
  devils_script::system sys_;
};

// Transient writer supplied to one authored-effect emit script. Pointers/spans never enter
// resolution_work or a snapshot: the script only appends pointer-free typed instances there.
struct combat_effect_emit_context {
  resolution_work* work = nullptr;
  entity_id source = invalid_entity;
  std::span<const entity_id> target_snapshot{};
  size_t max_emitted_instances = 1024;
  size_t emitted_instances = 0;

  bool valid() const noexcept {
    return work != nullptr && source != invalid_entity;
  }
};

// The implicit DS root is an invocation scope, not an entity and not resolve::work_header.root.
struct combat_effect_scope {
  combat_effect_emit_context* invocation = nullptr;

  bool valid() const noexcept {
    return invocation != nullptr && invocation->valid();
  }
};

// Scope installed by each_target for one member of the authored effect's frozen target snapshot.
// Typed emitters are deliberately available only here: target selection remains explicit in DS.
struct combat_target_scope {
  combat_effect_emit_context* invocation = nullptr;
  entity_id target = invalid_entity;

  bool valid() const noexcept {
    return invocation != nullptr && invocation->valid() && target != invalid_entity;
  }
};

// Registers the leaf authored-effect vocabulary. each_target traverses the frozen snapshot in its
// project order; emit_* append one typed instance for the current target. Nothing resolves or
// commits synchronously, and scripts cannot select actors outside the snapshot.
void register_combat_effect_script(devils_script::system& sys);

// Runs one already compiled emit script against a transient invocation writer.
void run_combat_effect_script(const devils_script::container& program,
                              devils_script::context& vm,
                              combat_effect_emit_context& invocation);

} // namespace core
} // namespace cardgame

#endif
