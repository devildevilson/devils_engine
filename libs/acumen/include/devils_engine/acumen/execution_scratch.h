#ifndef DEVILS_ENGINE_ACUMEN_EXECUTION_SCRATCH_H
#define DEVILS_ENGINE_ACUMEN_EXECUTION_SCRATCH_H

#include "astar.h"
#include "cache.h"
#include "common.h"
#include "devils_engine/act/execution_scratch.h"

namespace devils_engine {
namespace acumen {

// Complete reusable worker lane for GOAP evaluation. It embeds the generic act VM/call lane and
// adds planner-specific storage; applications may embed this again alongside output buffers.
struct execution_scratch {
  astar<astar_data>::container planner;
  solution_cache cache;
  act::execution_scratch act;
};

}
}

#endif
