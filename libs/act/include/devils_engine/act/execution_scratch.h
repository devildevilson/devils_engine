#ifndef DEVILS_ENGINE_ACT_EXECUTION_SCRATCH_H
#define DEVILS_ENGINE_ACT_EXECUTION_SCRATCH_H

#include <devils_script/context.h>

#include "call_context.h"

namespace devils_engine {
namespace act {

// Reusable mutable lane for one executor/worker. Subsystems embed it into their wider scratch.
// Ownership and worker-slot selection stay with the executor; there is no global/TLS distributor.
struct execution_scratch {
  devils_script::context vm;
  call_context call;
};

} // namespace act
} // namespace devils_engine

#endif
