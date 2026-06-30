#ifndef TILE_FRONTIER_CORE_ACTORS_H
#define TILE_FRONTIER_CORE_ACTORS_H

#include <atomic>
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

// Уникальный id задачи НА КАЖДЫЙ вызов (монотонный счётчик), а НЕ type-id.
// sequential_type_id<0, task_id_t>() возвращал бы одно и то же число каждый раз
// (он кеширует id на тип в `static const`), из-за чего у всех command_sound был
// одинаковый taskid → sound::system2::setup_sound отбрасывал дубли по id и звучал
// лишь один звук за раз. Счётчик atomic — id раздают разные потоки (sim/main).
inline size_t generate_task_id() noexcept {
  static std::atomic<size_t> counter{0};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

}
}

#endif
