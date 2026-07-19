#ifndef DEVILS_ENGINE_SIMUL_STANDARD_SOUND_SYSTEM_H
#define DEVILS_ENGINE_SIMUL_STANDARD_SOUND_SYSTEM_H

// Broker-driven sound-system implementation shared by standard applications.

#include <atomic>
#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include <devils_engine/sound/resource.h>
#include <devils_engine/sound/sound_resource.h>
#include <devils_engine/sound/system.h>
#include <devils_engine/utils/core.h>

#include "messages.h"
#include "systems.h"

namespace devils_engine {
namespace simul {

template <typename Broker>
class standard_sound_system : public sound_system<Broker> {
public:
  using base_type = sound_system<Broker>;

  explicit standard_sound_system(const size_t frame_time) noexcept : base_type(frame_time) {}
  ~standard_sound_system() noexcept override = default;

  void init() override {
    container.reset(new state);
    container->s.reset(new sound::system2);
  }

  bool stop_predicate() const override {
    return false;
  }

  void update(const size_t time) override {
    if (container == nullptr || this->broker_ == nullptr) {
      return;
    }
    auto& br = *this->broker_;

    {
      command_sound_devices cmd{};
      while (br.sound_devices.try_pop(cmd)) {
        if (cmd.out != nullptr) {
          sound::system2::playback_devices(*cmd.out);
        }
        if (cmd.ready != nullptr) {
          cmd.ready->store(true, std::memory_order_release);
        }
      }
    }

    {
      command_recreate_sound_system cmd{};
      while (br.recreate_sound.try_pop(cmd)) {
        container->s.reset(new sound::system2(cmd.device_name));
        if (container->s) {
          container->s->set_master_volume(container->master_gain);
          for (uint32_t i = 0; i < container->source_gain.size(); ++i) {
            container->s->set_source_volume(i, container->source_gain[i]);
          }
        }
      }
    }

    {
      command_sound_set_source_gain cmd{};
      while (br.sound_source_gain.try_pop(cmd)) {
        if (cmd.type >= container->source_gain.size()) {
          utils::warn("sound: source gain type {} is out of range", cmd.type);
          continue;
        }
        container->source_gain[cmd.type] = cmd.gain;
        if (container->s) {
          container->s->set_source_volume(cmd.type, cmd.gain);
        }
      }
    }

    {
      command_sound_set_master_gain cmd{};
      bool changed = false;
      while (br.sound_master_gain.try_pop(cmd)) {
        container->master_gain = cmd.gain;
        changed = true;
      }
      if (changed && container->s) {
        container->s->set_master_volume(container->master_gain);
      }
    }

    if (container->s) {
      // слушатель = latest-wins снапшот (камера); set_listener_ori ждёт ТОЧКУ взгляда — pos + dir
      if (const command_sound_listener* lsn = br.sound_listener.consume()) {
        container->s->set_listener_pos(sound::vec3(lsn->pos[0], lsn->pos[1], lsn->pos[2]));
        container->s->set_listener_ori(
          sound::vec3(lsn->pos[0] + lsn->dir[0], lsn->pos[1] + lsn->dir[1], lsn->pos[2] + lsn->dir[2]),
          sound::vec3(lsn->up[0], lsn->up[1], lsn->up[2]));
        container->s->set_listener_vel(sound::vec3(lsn->vel[0], lsn->vel[1], lsn->vel[2]));
      }

      command_sound_play cmd{};
      while (br.sound_play.try_pop(cmd)) {
        auto* sres = cmd.res.template get<sound::sound_resource>();
        if (sres == nullptr) {
          utils::warn("sound: play task {} with null resource", cmd.taskid);
          continue;
        }

        const auto view = sres->view();
        if (view.data.empty() || view.type == sound::data_type::undefined) {
          utils::warn("sound: resource '{}' not ready (task {})", sres->id, cmd.taskid);
          continue;
        }

        sound::task t;
        t.id = cmd.taskid == SIZE_MAX ? next_task_id() : cmd.taskid;
        t.after = cmd.after;
        t.res = view;
        t.type = cmd.type < static_cast<uint32_t>(sound::type::count)
                   ? static_cast<sound::type>(cmd.type)
                   : sound::type::sfx;
        t.command = sound::task::command::play;
        t.pitch = cmd.pitch;
        t.volume = cmd.volume;
        t.start = cmd.start;
        t.pos = sound::vec3(cmd.pos[0], cmd.pos[1], cmd.pos[2]);
        t.min_distance = cmd.min_distance;
        t.max_distance = cmd.max_distance; // 0 = непозиционный (см. command_sound_play)
        container->s->setup_sound(t);
      }

      command_sound_stop stop{};
      while (br.sound_stop.try_pop(stop)) {
        container->s->remove_sound(stop.taskid);
      }

      command_sound_update upd{};
      while (br.sound_update.try_pop(upd)) {
        sound::task_update update;
        update.id = upd.taskid;
        update.pos = sound::vec3(upd.pos[0], upd.pos[1], upd.pos[2]);
        update.dir = sound::vec3(upd.dir[0], upd.dir[1], upd.dir[2]);
        update.vel = sound::vec3(upd.vel[0], upd.vel[1], upd.vel[2]);
        container->s->update_sound(update);
      }
    }

    if (container->s) {
      container->s->update(time);
    }

    if (container->s) {
      container->s->snapshot(container->snapshot_status_cache);
      command_sound_state& snapshot = br.sound_state.write_slot();
      snapshot.sounds.clear();
      snapshot.sounds.reserve(container->snapshot_status_cache.size());
      for (const auto& status : container->snapshot_status_cache) {
        snapshot.sounds.push_back(sound_state_entry{status.id, status.progress});
      }
      br.sound_state.publish();
    }
  }

private:
  struct state {
    std::unique_ptr<sound::system2> s;
    float master_gain = 1.0f;
    std::array<float, static_cast<size_t>(sound::type::count)> source_gain = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<sound::task_status> snapshot_status_cache;
  };

  static size_t next_task_id() noexcept {
    static std::atomic_size_t counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
  }

  std::unique_ptr<state> container;
};

} // namespace simul
} // namespace devils_engine

#endif
