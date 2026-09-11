#ifndef FRONTIER_ONLINE_CORE_DRAW_INTENT_H
#define FRONTIER_ONLINE_CORE_DRAW_INTENT_H

#include <devils_engine/simul/draw_intent.h>

#include "instance_layout.h" // сохраняем прежний транзитивный core::instance_layout для потребителей

// Типизированный упаковщик инстансов в сырые байты переехал в libs/simul как движковый контракт
// producer->render. Здесь — тонкий шим, чтобы core::draw_intent продолжал работать.

namespace frontier_online {
namespace core {

using devils_engine::simul::draw_intent;

}
} // namespace frontier_online

#endif
