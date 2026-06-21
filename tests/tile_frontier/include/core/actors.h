#ifndef TILE_FRONTIER_CORE_ACTORS_H
#define TILE_FRONTIER_CORE_ACTORS_H

#include <cstddef>
#include <devils_engine/utils/actor_ref.h>
#include <devils_engine/utils/type_traits.h>

// Общая "идентичность" систем: типы акторов и их id.
// Все системы (sound/render/assets/simulation) ссылаются сюда, чтобы
// знать друг про друга только через actor_ref, а не через классы.

namespace tile_frontier {
namespace core {

using namespace devils_engine;

constexpr size_t seq_simul_type_id = 0xaa1;
constexpr size_t seq_sound_type_id = 0xaa2;
constexpr size_t seq_graphics_type_id = 0xaa3;
constexpr size_t seq_assets_type_id = 0xaa4;

using simulation_actor = utils::actor_ref<seq_simul_type_id>;
using sound_actor = utils::actor_ref<seq_sound_type_id>;
using graphics_actor = utils::actor_ref<seq_graphics_type_id>;
using assets_actor = utils::actor_ref<seq_assets_type_id>;

struct task_id_t {};
inline size_t generate_task_id() noexcept { return utils::sequential_type_id<0, task_id_t>(); }

}
}

#endif
