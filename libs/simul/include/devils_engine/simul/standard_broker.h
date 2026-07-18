#ifndef DEVILS_ENGINE_SIMUL_STANDARD_BROKER_H
#define DEVILS_ENGINE_SIMUL_STANDARD_BROKER_H

#include <cstddef>

#include <devils_engine/thread/mailbox.h>
#include <devils_engine/thread/spsc_queue.h>

#include "messages.h"
#include "write_buffer_channel.h"

namespace devils_engine {
namespace simul {

// Движковые каналы лежат ПЛОСКО прямо в broker — так плоские имена (`br.window_recreation`,
// `br.sound_play`, ...) принадлежат ДВИЖКУ: и стандартные системы simul, и проектный subclass
// адресуют их одинаково без ручного reference-алиасинга. Внешний проект наследует standard_broker
// и добавляет ТОЛЬКО свои каналы value-членами (см. tile_frontier::core::broker). Каждый канал
// строго SPSC (один продюсер, один консьюмер); бюджеты фиксированы конструктором (преаллокация,
// рантайм-роста пока нет). Группировка ниже — по топологии; порядок членов = порядок инициализации.
struct standard_broker {
  // main → render (latest-wins snapshots + reliable/one-shot)
  thread::mailbox<command_window_recreation> window_recreation;
  thread::mailbox<command_window_resize> window_resize;         // ресайз/фуллскрин (только свопчейн)
  thread::mailbox<command_render_set_active> render_set_active; // гейт отрисовки
  thread::mailbox<command_render_set_graph> set_active_graph;
  thread::spsc_queue<command_render_update_constant> update_constant; // обновление render-graph константы
  write_buffer_channel write_buffer;                                  // camera + UI буферы

  // assets → render
  thread::mailbox<command_shaders_prepared> shaders_prepared; // latest-wins (one-shot)
  thread::spsc_queue<command_gpu_transition> gpu_transition;  // reliable

  // render → assets
  thread::spsc_queue<command_gpu_done> gpu_done;               // reliable (ack)
  thread::spsc_queue<command_prepare_shaders> prepare_shaders; // reliable (one-shot)

  // main → assets
  thread::spsc_queue<command_load_resource> load_resource; // reliable

  // sound → main
  thread::mailbox<command_sound_state> sound_state; // latest-wins snapshot

  // main → sound
  thread::spsc_queue<command_sound_play> sound_play;         // lossy
  thread::spsc_queue<command_sound_stop> sound_stop;         // lossy
  thread::spsc_queue<command_sound_update> sound_update;     // lossy (latest-per-id)
  thread::mailbox<command_sound_listener> sound_listener;    // latest-wins снапшот слушателя (камера)
  thread::spsc_queue<command_sound_devices> sound_devices; // handshake
  thread::spsc_queue<command_recreate_sound_system> recreate_sound;
  thread::spsc_queue<command_sound_set_master_gain> sound_master_gain; // lossy (latest-wins по смыслу)

  standard_broker()
    : update_constant(64), write_buffer(64, size_t(1) << 20), gpu_transition(256), gpu_done(256), prepare_shaders(8), load_resource(256), sound_play(64), sound_stop(64), sound_update(64), sound_devices(8), recreate_sound(8), sound_master_gain(8) {}

  standard_broker(const standard_broker&) = delete;
  standard_broker& operator=(const standard_broker&) = delete;
};

} // namespace simul
} // namespace devils_engine

#endif
