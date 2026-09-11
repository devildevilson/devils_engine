#ifndef FRONTIER_ONLINE_CORE_INSTANCE_LAYOUT_H
#define FRONTIER_ONLINE_CORE_INSTANCE_LAYOUT_H

#include <devils_engine/simul/instance_layout.h>

// Матчер C++-агрегата с layout-строкой draw_group переехал в libs/simul как движковый контракт
// инстансного рендера. Здесь — тонкий шим, чтобы core:: имена продолжали работать.

namespace frontier_online {
namespace core {

namespace instance_layout = devils_engine::simul::instance_layout;

}
} // namespace frontier_online

#endif
