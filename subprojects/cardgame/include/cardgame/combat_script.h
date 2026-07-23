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

// Project compiler adapter used by generic act::script_resource documents with `ret = void` and
// `scope = combat_effect`, `scope = retaliation_rule`, `scope = follow_up_rule` or the read-only
// `scope = execution_report`.
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

// Reconstituted read-only view over one frozen report and the resolution work that owns every
// referenced typed store. Neither pointer is serialized; a resumed host supplies the owning work
// again and every iterator validates the report ranges before dereferencing them.
struct execution_report_view_context {
  const resolution_work* work = nullptr;
  const execution_report* report = nullptr;

  bool valid() const noexcept {
    return work != nullptr && report != nullptr &&
           report->execution != resolve::invalid_instance;
  }
};

struct execution_report_scope {
  execution_report_view_context* invocation = nullptr;

  bool valid() const noexcept {
    return invocation != nullptr && invocation->valid();
  }
};

// One follow-up rule reads the complete prefix sealed before its party group and prepares one new
// execution. The input span and writer are transient; only the pointer-free authored program and
// its later resolved work enter the action report.
struct follow_up_rule_emit_context {
  std::span<const resolution_work> input{};
  resolution_work* output = nullptr;
  // Owner/source of the follow-up rule.
  entity_id actor = invalid_entity;
  // Actor whose card/enemy ability opened this action report.
  entity_id action_actor = invalid_entity;
  // Optional target selected by the opening execution.
  entity_id original_target = invalid_entity;
  // Live opponents in frozen project priority order.
  std::span<const entity_id> priority_opponents{};
  size_t max_authored_effects = 16;
  size_t emitted_effects = 0;
  size_t visited_executions = 0;

  bool valid() const noexcept {
    return output != nullptr && actor != invalid_entity &&
           action_actor != invalid_entity;
  }
};

struct follow_up_rule_scope {
  follow_up_rule_emit_context* invocation = nullptr;

  bool valid() const noexcept {
    return invocation != nullptr && invocation->valid();
  }
};

// A script must enter one of the explicit follow-up selectors before it may emit targeted work.
// This prevents emitters from silently guessing what an absent original target means.
struct follow_up_target_scope {
  follow_up_rule_emit_context* invocation = nullptr;
  entity_id target = invalid_entity;

  bool valid() const noexcept {
    return invocation != nullptr && invocation->valid() &&
           target != invalid_entity;
  }
};

struct report_attack_scope {
  execution_report_view_context* invocation = nullptr;
  const attack_instance* value = nullptr;
  bool valid() const noexcept {
    return invocation != nullptr && invocation->valid() && value != nullptr;
  }
};

struct report_damage_scope {
  execution_report_view_context* invocation = nullptr;
  const damage_outcome* value = nullptr;
  bool valid() const noexcept {
    return invocation != nullptr && invocation->valid() && value != nullptr;
  }
};

struct report_healing_scope {
  execution_report_view_context* invocation = nullptr;
  const healing_outcome* value = nullptr;
  bool valid() const noexcept {
    return invocation != nullptr && invocation->valid() && value != nullptr;
  }
};

struct report_shield_scope {
  execution_report_view_context* invocation = nullptr;
  const shield_outcome* value = nullptr;
  bool valid() const noexcept {
    return invocation != nullptr && invocation->valid() && value != nullptr;
  }
};

struct report_attribute_damage_scope {
  execution_report_view_context* invocation = nullptr;
  const attribute_damage_outcome* value = nullptr;
  bool valid() const noexcept {
    return invocation != nullptr && invocation->valid() && value != nullptr;
  }
};

struct report_status_scope {
  execution_report_view_context* invocation = nullptr;
  const effect_outcome* value = nullptr;
  bool valid() const noexcept {
    return invocation != nullptr && invocation->valid() && value != nullptr;
  }
};

// One invocation per subscribed rule and committed damage leaf. The damage outcome is already
// authoritative (including shield/health destination, zero or negative delta and death), while the
// script is still only allowed to append a bounded immediate response.
struct retaliation_rule_emit_context {
  resolution_work* work = nullptr;
  const effect_state* rule = nullptr;
  const damage_outcome* trigger = nullptr;
  bool target_dead = false;
  uint16_t rule_ordinal = 0;
  size_t max_emitted_responses = 1;
  size_t emitted_responses = 0;

  bool valid() const noexcept {
    return work != nullptr && rule != nullptr && trigger != nullptr &&
           trigger->damage.header.id != resolve::invalid_instance;
  }
};

struct retaliation_rule_scope {
  retaliation_rule_emit_context* invocation = nullptr;

  bool valid() const noexcept {
    return invocation != nullptr && invocation->valid();
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

// Runs a read-only report consumer. The program may inspect metadata/categories and iterate typed
// instances/outcomes, but this scope deliberately has no emitters; follow-up output belongs to the
// next prepare-scope layer.
void run_execution_report_script(const devils_script::container& program,
                                 devils_script::context& vm,
                                 execution_report_view_context& invocation);

// Runs one party follow-up rule over a frozen action-report prefix. The script may inspect each
// prior execution and append bounded authored effects to `output`; it never resolves them inline.
void run_follow_up_rule_script(const devils_script::container& program,
                               devils_script::context& vm,
                               follow_up_rule_emit_context& invocation);

// Runs one immediate-reaction rule for one already committed damage leaf. C++ only supplies the
// candidate and enforces lineage/capacity; emitting no response is a normal script decision.
void run_retaliation_rule_script(const devils_script::container& program,
                                 devils_script::context& vm,
                                 retaliation_rule_emit_context& invocation);

} // namespace core
} // namespace cardgame

#endif
