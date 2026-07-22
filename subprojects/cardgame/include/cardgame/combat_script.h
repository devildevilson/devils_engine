#ifndef DEVILS_ENGINE_CARDGAME_COMBAT_SCRIPT_H
#define DEVILS_ENGINE_CARDGAME_COMBAT_SCRIPT_H

#include <cstddef>
#include <cstdint>
#include <span>

#include <devils_script/container.h>
#include <devils_script/context.h>
#include <devils_script/system.h>

#include "cardgame/combat.h"

namespace cardgame {
namespace core {

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
