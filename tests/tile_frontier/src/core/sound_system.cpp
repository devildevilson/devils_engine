#include "sound_system.h"

#include <cassert>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <devils_engine/sound/system.h>
#include <devils_engine/sound/resource.h>
#include <devils_engine/sound/sound_resource.h>
#include <devils_engine/utils/core.h>

#include "messages.h"
#include "message_dispatcher.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// тут что? система звуков + кеш?
// вообще лучше бы иметь возможность найти устройство подходящее как у графики + сделать так чтобы его можно было выбрать
struct sound_simulation_init {
  std::unique_ptr<sound::system2> s;
  simulation_actor* main_actor = nullptr; // куда пушим command_sound_state (может быть nullptr)

  std::vector<command_sound_play> play_cache;
  std::vector<command_sound_stop> stop_cache;
  std::vector<command_sound_update> update_cache;
  std::vector<command_sound_devices> devices_cache;
  std::vector<command_recreate_sound_system> recreate_cache;
  std::vector<sound::task_status> snapshot_status_cache; // буфер под публикацию состояния
  message_dispatcher<command_sound_play> plays;
  message_dispatcher<command_sound_stop> stops;
  message_dispatcher<command_sound_update> updates;
  message_dispatcher<command_sound_devices> devices;
  message_dispatcher<command_recreate_sound_system> recreate;

  // Звуковой актор НЕ хранит звук-ресурсы: играет из demiurg-хендла (command_sound_play.res),
  // которым владеет поток ассетов. main резолвит имя→ресурс и шлёт указатель.
};

sound_simulation::sound_simulation(const size_t frame_time) noexcept : simul::advancer(frame_time) {}
sound_simulation::~sound_simulation() noexcept = default;

void sound_simulation::init() {
  container.reset(new sound_simulation_init);
  container->s.reset(new sound::system2);
  actor.add_receiver<command_sound_play>(&container->plays);
  actor.add_receiver<command_sound_stop>(&container->stops);
  actor.add_receiver<command_sound_update>(&container->updates);
  actor.add_receiver<command_sound_devices>(&container->devices);
  actor.add_receiver<command_recreate_sound_system>(&container->recreate);
  // Звуки грузит поток АССЕТОВ (demiurg): main запрашивает их до warm и шлёт указатель в
  // command_sound_play. Актор ничего не предзагружает и не хранит.
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

  dispatcher_consume(container->plays, container->play_cache, [this] (const auto &cmd) {
    if (!container->s) return;

    // Звук берём из demiurg-хендла (владеет поток ассетов); НЕ храним. Читаем данные через view().
    auto* sres = static_cast<sound::sound_resource*>(cmd.res);
    if (sres == nullptr) { utils::warn("sound: play task {} with null resource", cmd.taskid); return; }
    const auto view = sres->view();
    if (view.data.empty() || view.type == sound::data_type::undefined) {
      utils::warn("sound: resource '{}' not ready (task {})", sres->id, cmd.taskid);
      return;
    }

    sound::task t;
    t.id = cmd.taskid == SIZE_MAX ? generate_task_id() : cmd.taskid;
    t.after = cmd.after;
    t.res = view; // {id, type, span} — span живёт в ресурсе (поток ассетов)
    t.type = sound::type::sfx; // TODO: категорию (sfx/music) нести в play-команде (стадийно: sfx)
    t.command = sound::task::command::play; // POD без инициализации — явно задаём важные поля
    t.pitch = 1.0f;
    t.volume = 1.0f;
    t.start = cmd.start; // [0,1] откуда играть (плеер может попросить продолжить с места)
    t.pos = sound::vec3(0.0f, 0.0f, 0.0f); // MVP: 2D, без 3D-листенера (позиционность — позже)
    container->s->setup_sound(t);
  });

  dispatcher_consume(container->stops, container->stop_cache, [this] (const auto &cmd) {
    if (!container->s) return;
    container->s->remove_sound(cmd.taskid); // остановить и освободить голос
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

  //utils::time_log l("Sound update");
  //utils::info("Thread id {}", std::this_thread::get_id());
  if (container->s) container->s->update(time);

  // ПУБЛИКАЦИЯ полного состояния → main (для app.sound_state / UI). Звук сам шлёт упрощённый
  // слепок; main держит последний (consume_last). Никаких сырых указателей/atomic наружу.
  // (Шлём каждый звуковой кадр; при желании легко троттлить аккумулятором времени.)
  if (container->main_actor != nullptr && container->s) {
    container->s->snapshot(container->snapshot_status_cache);
    command_sound_state state;
    state.sounds.reserve(container->snapshot_status_cache.size());
    for (const auto &status : container->snapshot_status_cache) {
      state.sounds.push_back(sound_state_entry{ status.id, status.progress });
    }
    container->main_actor->send(std::move(state));
  }
}

sound_actor* sound_simulation::get_actor() { return &actor; }
void sound_simulation::set_main_actor(simulation_actor* a) { container->main_actor = a; }

}
}
