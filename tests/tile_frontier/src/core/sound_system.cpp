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
#include "broker.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// тут что? система звуков + кеш?
// вообще лучше бы иметь возможность найти устройство подходящее как у графики + сделать так чтобы его можно было выбрать
struct sound_simulation_init {
  std::unique_ptr<sound::system2> s;
  broker* br = nullptr; // все каналы — в общем broker (main владеет)
  std::vector<sound::task_status> snapshot_status_cache; // буфер под публикацию состояния

  // Звуковой актор НЕ хранит звук-ресурсы: играет из demiurg-хендла (command_sound_play.res),
  // которым владеет поток ассетов. main резолвит имя→ресурс и шлёт указатель.
};

sound_simulation::sound_simulation(const size_t frame_time) noexcept : simul::advancer(frame_time) {}
sound_simulation::~sound_simulation() noexcept = default;

void sound_simulation::init() {
  container.reset(new sound_simulation_init);
  container->s.reset(new sound::system2);
  // Звуки грузит поток АССЕТОВ (demiurg): main запрашивает их до warm и шлёт указатель в
  // command_sound_play. Актор ничего не предзагружает и не хранит. Каналы — в broker (set_broker).
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
  if (container->br == nullptr) return; // broker ещё не задан
  broker& br = *container->br;

  {
    command_sound_devices cmd{};
    while (br.sound_devices.try_pop(cmd)) {
      if (cmd.out != nullptr) sound::system2::playback_devices(*cmd.out);
      if (cmd.ready != nullptr) cmd.ready->store(true, std::memory_order_release);
    }
  }

  {
    command_recreate_sound_system cmd{};
    while (br.recreate_sound.try_pop(cmd)) container->s.reset(new sound::system2(cmd.device_name));
  }

  if (container->s) {
    command_sound_play cmd{};
    while (br.sound_play.try_pop(cmd)) {
      // Звук берём из demiurg-хендла (владеет поток ассетов); НЕ храним. Читаем данные через view().
      auto* sres = static_cast<sound::sound_resource*>(cmd.res);
      if (sres == nullptr) { utils::warn("sound: play task {} with null resource", cmd.taskid); continue; }
      const auto view = sres->view();
      if (view.data.empty() || view.type == sound::data_type::undefined) {
        utils::warn("sound: resource '{}' not ready (task {})", sres->id, cmd.taskid);
        continue;
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
    }

    command_sound_stop stop{};
    while (br.sound_stop.try_pop(stop)) container->s->remove_sound(stop.taskid);

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

  //utils::time_log l("Sound update");
  //utils::info("Thread id {}", std::this_thread::get_id());
  if (container->s) container->s->update(time);

  // ПУБЛИКАЦИЯ полного состояния → main (для app.sound_state / UI). Звук ПИШЕТ упрощённый слепок в
  // latest-wins мейлбокс; main читает последний. Никаких сырых указателей/atomic наружу.
  if (container->s) {
    container->s->snapshot(container->snapshot_status_cache);
    command_sound_state& state = br.sound_state.write_slot();
    state.sounds.clear(); // слот переиспользует ёмкость
    state.sounds.reserve(container->snapshot_status_cache.size());
    for (const auto &status : container->snapshot_status_cache) {
      state.sounds.push_back(sound_state_entry{ status.id, status.progress });
    }
    br.sound_state.publish();
  }
}

sound_actor* sound_simulation::get_actor() { return &actor; }
void sound_simulation::set_broker(broker* b) { if (container) container->br = b; }

}
}
