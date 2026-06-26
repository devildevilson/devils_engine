#include "sound_system.h"

#include <cassert>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <devils_engine/sound/system.h>
#include <devils_engine/sound/resource.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/fileio.h>

#include "messages.h"
#include "message_dispatcher.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// тут что? система звуков + кеш?
// вообще лучше бы иметь возможность найти устройство подходящее как у графики + сделать так чтобы его можно было выбрать
struct sound_simulation_init {
  std::unique_ptr<sound::system2> s;

  std::vector<command_sound> command_cache;
  std::vector<command_sound_update> update_cache;
  std::vector<command_sound_snapshot> snapshot_cache;
  std::vector<command_sound_devices> devices_cache;
  std::vector<command_recreate_sound_system> recreate_cache;
  std::vector<sound::task_status> snapshot_status_cache;
  message_dispatcher<command_sound> commands;
  message_dispatcher<command_sound_update> updates;
  message_dispatcher<command_sound_snapshot> snapshots;
  message_dispatcher<command_sound_devices> devices;
  message_dispatcher<command_recreate_sound_system> recreate;

  std::string res_id;
  std::vector<char> music_data;
};

sound_simulation::sound_simulation(const size_t frame_time) noexcept : simul::advancer(frame_time) {}
sound_simulation::~sound_simulation() noexcept = default;

void sound_simulation::init() {
  container.reset(new sound_simulation_init);
  container->s.reset(new sound::system2);
  actor.add_receiver<command_sound>(&container->commands);
  actor.add_receiver<command_sound_update>(&container->updates);
  actor.add_receiver<command_sound_snapshot>(&container->snapshots);
  actor.add_receiver<command_sound_devices>(&container->devices);
  actor.add_receiver<command_recreate_sound_system>(&container->recreate);

  // нужно задать какой нибудь звук, как теперь это делается...
  // по идее грузим музыку в массив, да и передаем в структуре sound::resource2

  std::string test_sound_path = utils::project_folder() + "resources/test.mp3";
  container->music_data = file_io::read<char>(test_sound_path);
  assert(!container->music_data.empty());

  container->res_id = utils::project_folder() + "resources/test";

  sound::task t;
  t.id = generate_task_id();
  t.res.id = std::string_view(container->res_id);
  t.res.type = sound::data_type::mp3;
  t.res.data = std::span(container->music_data);
  t.type = sound::type::music;
  container->s->setup_sound(t);
}

bool sound_simulation::stop_predicate() const { return false; }

// вообще желательно звук чтобы зависел от среды
// среду задать бы какой нибудь функцией от времени
// в команде просто указать ресурс, таск ид, тип звука, среда
// где среда это микшер?
// + к этому указать среду для слушателя
void sound_simulation::update(const size_t time) {
  // на самом деле тут тоже будет пост инит, где мы бы хотели передать звуковое устройство
  // звуки тоже поддаются настройке: дропаем систему и заново ее собираем? очень похоже на то

  dispatcher_consume(container->devices, container->devices_cache, [] (const auto &cmd) {
    if (cmd.out != nullptr) sound::system2::playback_devices(*cmd.out);
    if (cmd.ready != nullptr) cmd.ready->store(true, std::memory_order_release);
  });

  dispatcher_consume(container->recreate, container->recreate_cache, [this] (const auto &cmd) {
    container->s.reset(new sound::system2(cmd.device_name));
  });

  dispatcher_consume(container->commands, container->command_cache, [this] (const auto &cmd) {
    if (!container->s) return;

    utils::info("Receive sound command {}", cmd.taskid);
    if (cmd.cmd != 0) return;

    sound::task t;
    t.id = cmd.taskid == SIZE_MAX ? generate_task_id() : cmd.taskid;
    t.after = cmd.after;
    t.res.id = std::string_view(container->res_id);
    t.res.type = sound::data_type::mp3;
    t.res.data = std::span(container->music_data);
    t.type = sound::type::music;
    container->s->setup_sound(t);
  });

  dispatcher_consume(container->updates, container->update_cache, [this] (const auto &cmd) {
    if (!container->s) return;
    sound::task_update update;
    update.id = cmd.taskid;
    update.pos = sound::vec3(cmd.pos[0], cmd.pos[1], cmd.pos[2]);
    update.dir = sound::vec3(cmd.dir[0], cmd.dir[1], cmd.dir[2]);
    update.vel = sound::vec3(cmd.vel[0], cmd.vel[1], cmd.vel[2]);
    container->s->update_sound(update);
  });

  dispatcher_consume(container->snapshots, container->snapshot_cache, [this] (const auto &cmd) {
    if (cmd.out == nullptr || !container->s) return;

    container->s->snapshot(container->snapshot_status_cache);
    cmd.out->clear();
    cmd.out->reserve(container->snapshot_status_cache.size());
    for (const auto &status : container->snapshot_status_cache) {
      sound_status out;
      out.taskid = status.id;
      out.after = status.after;
      out.type = static_cast<uint32_t>(status.type);
      out.state = static_cast<uint32_t>(status.state);
      out.progress = status.progress;
      out.frames_decoded = status.frames_decoded;
      out.frames_total = status.frames_total;
      out.underruns = status.underruns;
      out.pos[0] = status.pos.x;
      out.pos[1] = status.pos.y;
      out.pos[2] = status.pos.z;
      out.dir[0] = status.dir.x;
      out.dir[1] = status.dir.y;
      out.dir[2] = status.dir.z;
      out.vel[0] = status.vel.x;
      out.vel[1] = status.vel.y;
      out.vel[2] = status.vel.z;
      cmd.out->push_back(out);
    }
  });

  //utils::time_log l("Sound update");
  //utils::info("Thread id {}", std::this_thread::get_id());
  if (container->s) container->s->update(time);
}

sound_actor* sound_simulation::get_actor() { return &actor; }

}
}
