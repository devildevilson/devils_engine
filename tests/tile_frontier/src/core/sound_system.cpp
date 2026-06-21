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
  sound::system2 s;

  std::vector<command_sound> command_cache;
  message_dispatcher<command_sound> commands;

  std::string res_id;
  std::vector<char> music_data;
};

sound_simulation::sound_simulation(const size_t frame_time) noexcept : simul::advancer(frame_time) {}
sound_simulation::~sound_simulation() noexcept = default;

void sound_simulation::init() {
  container.reset(new sound_simulation_init);
  actor.add_receiver<command_sound>(&container->commands);

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
  container->s.setup_sound(t);
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

  dispatcher_consume(container->commands, container->command_cache, [] (const auto &cmd) {
    utils::info("Receive sound command {}", cmd.taskid);
  });

  //utils::time_log l("Sound update");
  //utils::info("Thread id {}", std::this_thread::get_id());
  container->s.update(time);
}

sound_actor* sound_simulation::get_actor() { return &actor; }

}
}
