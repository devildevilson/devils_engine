#include "render_output.h"

namespace devils_engine {
namespace visage {
namespace tex_id {
uint32_t pack(const uint32_t type, const uint32_t index, const bool mirror_u,
              const bool mirror_v, const uint32_t sampler_id) {
  return utils::shared::tex_pack(type, index, mirror_u, mirror_v, sampler_id);
}

uint32_t index_of(const uint32_t id) {
  return utils::shared::tex_index_of(id);
}

uint32_t type_of(const uint32_t id) {
  return utils::shared::tex_type_of(id);
}

uint32_t sampler_of(const uint32_t id) {
  return utils::shared::tex_sampler_of(id);
}

bool mirror_u_of(const uint32_t id) {
  return utils::shared::tex_mirror_u_of(id);
}

bool mirror_v_of(const uint32_t id) {
  return utils::shared::tex_mirror_v_of(id);
}
} // namespace tex_id
} // namespace visage
} // namespace devils_engine
