#ifndef FRONTIER_ONLINE_CORE_WRITE_BUFFER_CHANNEL_H
#define FRONTIER_ONLINE_CORE_WRITE_BUFFER_CHANNEL_H

// Project alias for the broker-owned variable-size write-buffer channel.

#include <devils_engine/simul/write_buffer_channel.h>

namespace frontier_online {
namespace core {

using wb_msg = devils_engine::simul::wb_msg;
using write_buffer_channel = devils_engine::simul::write_buffer_channel;

} // namespace core
} // namespace frontier_online

#endif
