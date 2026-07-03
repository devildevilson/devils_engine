#include "pipeline_cache_resource.h"

#include "devils_engine/demiurg/module_interface.h"

namespace devils_engine {
namespace painter {

pipeline_cache_resource::pipeline_cache_resource() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, true);
}

void pipeline_cache_resource::load_cold(const utils::safe_handle_t&) {
  module->load_binary(path, memory);
}

void pipeline_cache_resource::load_warm(const utils::safe_handle_t&) {}
void pipeline_cache_resource::unload_hot(const utils::safe_handle_t&) {}
void pipeline_cache_resource::unload_warm(const utils::safe_handle_t&) { memory.clear(); memory.shrink_to_fit(); }

}
}
