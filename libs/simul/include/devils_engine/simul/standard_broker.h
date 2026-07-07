#ifndef DEVILS_ENGINE_SIMUL_STANDARD_BROKER_H
#define DEVILS_ENGINE_SIMUL_STANDARD_BROKER_H

#include <cstddef>

#include <devils_engine/thread/mailbox.h>
#include <devils_engine/thread/spsc_queue.h>

#include "messages.h"
#include "write_buffer_channel.h"

namespace devils_engine {
namespace simul {

struct render_channels {
  thread::mailbox<command_window_recreation> window_recreation;
  thread::mailbox<command_window_resize> window_resize;
  thread::mailbox<command_render_set_active> render_set_active;
  thread::mailbox<command_render_set_graph> set_active_graph;
  thread::spsc_queue<command_render_update_constant> update_constant;
  write_buffer_channel write_buffer;
  thread::mailbox<command_shaders_prepared> shaders_prepared;
  thread::spsc_queue<command_gpu_transition> gpu_transition;

  render_channels()
    : update_constant(64)
    , write_buffer(64, size_t(1) << 20)
    , gpu_transition(256)
  {}
};

struct assets_channels {
  thread::spsc_queue<command_gpu_done> gpu_done;
  thread::spsc_queue<command_prepare_shaders> prepare_shaders;
  thread::spsc_queue<command_load_resource> load_resource;

  assets_channels()
    : gpu_done(256)
    , prepare_shaders(8)
    , load_resource(256)
  {}
};

struct sound_channels {
  thread::mailbox<command_sound_state> sound_state;
  thread::spsc_queue<command_sound_play> sound_play;
  thread::spsc_queue<command_sound_stop> sound_stop;
  thread::spsc_queue<command_sound_update> sound_update;
  thread::spsc_queue<command_sound_devices> sound_devices;
  thread::spsc_queue<command_recreate_sound_system> recreate_sound;
  thread::spsc_queue<command_sound_set_master_gain> sound_master_gain;

  sound_channels()
    : sound_play(64)
    , sound_stop(64)
    , sound_update(64)
    , sound_devices(8)
    , recreate_sound(8)
    , sound_master_gain(8)
  {}
};

struct standard_broker {
  render_channels render;
  assets_channels assets;
  sound_channels sound;

  standard_broker() = default;
  standard_broker(const standard_broker&) = delete;
  standard_broker& operator=(const standard_broker&) = delete;
};

}
}

#endif
