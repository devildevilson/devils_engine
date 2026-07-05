#ifndef TILE_FRONTIER_CORE_BROKER_H
#define TILE_FRONTIER_CORE_BROKER_H

#include <cstddef>

#include <devils_engine/thread/mailbox.h>
#include <devils_engine/thread/spsc_queue.h>

#include "messages.h"
#include "write_buffer_channel.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// Единый holder всех межпоточных каналов сообщений (broker). Владелец — main: создаётся в
// simulation::init ДО подсистем и раздаётся указателем каждой (set_broker) до старта их потоков.
// Заменяет baseline actor_ref/message_dispatcher/add_receiver. Каждый канал строго SPSC (один
// продюсер, один консьюмер — см. топологию ниже):
//   mailbox<T>           — latest-wins (свежак вытесняет старое), drop-oldest штатно;
//   spsc_queue<T>        — reliable/lossy FIFO (drop-newest при переполнении);
//   write_buffer_channel — payload_channel<wb_msg> (POD-сообщение + byte_ring под сырые байты).
// Бюджеты ФИКСИРОВАНЫ конструктором (преаллокация; рантайм-роста пока нет).
struct broker {
  // main → render (latest-wins)
  thread::mailbox<command_window_recreation> window_recreation;
  thread::mailbox<command_window_resize>     window_resize;    // ресайз/фуллскрин (только свопчейн)
  thread::mailbox<command_render_set_active> render_set_active; // гейт отрисовки
  thread::mailbox<command_set_active_graph>  set_active_graph;
  thread::mailbox<command_draw_tiles>        draw_tiles;
  thread::mailbox<command_draw_actors>       draw_actors;
  write_buffer_channel                       write_buffer; // camera + UI буферы

  // assets → render
  thread::mailbox<command_shaders_prepared>   shaders_prepared; // latest-wins (one-shot)
  thread::spsc_queue<command_gpu_transition>  gpu_transition;   // reliable

  // render → assets
  thread::spsc_queue<command_gpu_done>        gpu_done;         // reliable (ack)
  thread::spsc_queue<command_prepare_shaders> prepare_shaders;  // reliable (one-shot)

  // main → assets
  thread::spsc_queue<command_load_resource>   load_resource;    // reliable
  thread::spsc_queue<command_load_chunk>      load_chunk;       // reliable

  // assets → main
  thread::spsc_queue<command_chunk_loaded>    chunk_loaded;     // reliable (vector payload)

  // sound → main
  thread::mailbox<command_sound_state>        sound_state;      // latest-wins snapshot

  // main → sound
  thread::spsc_queue<command_sound_play>      sound_play;       // lossy
  thread::spsc_queue<command_sound_stop>      sound_stop;       // lossy
  thread::spsc_queue<command_sound_update>    sound_update;     // lossy (latest-per-id)
  thread::spsc_queue<command_sound_devices>   sound_devices;    // handshake
  thread::spsc_queue<command_recreate_sound_system> recreate_sound;
  thread::spsc_queue<command_sound_set_master_gain> sound_master_gain; // lossy (latest-wins по смыслу)

  broker()
    : write_buffer(64, size_t(1) << 20)
    , gpu_transition(256)
    , gpu_done(256)
    , prepare_shaders(8)
    , load_resource(256)
    , load_chunk(256)
    , chunk_loaded(256)
    , sound_play(64)
    , sound_stop(64)
    , sound_update(64)
    , sound_devices(8)
    , recreate_sound(8)
    , sound_master_gain(8)
  {}

  broker(const broker&) = delete;
  broker& operator=(const broker&) = delete;
};

}
}

#endif
