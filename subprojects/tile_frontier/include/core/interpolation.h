#ifndef TILE_FRONTIER_CORE_INTERPOLATION_H
#define TILE_FRONTIER_CORE_INTERPOLATION_H

#include <devils_engine/simul/interpolation.h>

// Обобщённая интерполяция снапшотов (render thread) переехала в libs/simul как движковый
// контракт. Здесь — тонкий шим, чтобы core:: имена продолжали работать. Специализация
// blend_traits<T> под конкретный инстанс живёт в namespace devils_engine::simul у потребителя
// (см. render_system.cpp) — специализировать через using-алиас нельзя.

namespace tile_frontier {
namespace core {

using devils_engine::simul::blend_traits;
using devils_engine::simul::interpolation_alpha;
using devils_engine::simul::interpolation_track;
using devils_engine::simul::nominal_clock;
using devils_engine::simul::snapshot_interpolator;

}
}

#endif
