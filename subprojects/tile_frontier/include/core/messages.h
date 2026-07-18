#ifndef TILE_FRONTIER_CORE_MESSAGES_H
#define TILE_FRONTIER_CORE_MESSAGES_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <devils_engine/simul/messages.h>

// Контракты между системами: структуры, которые ходят через диспетчер сообщений.
// Это намеренно "тупые" POD-подобные сообщения без логики и без тяжёлых зависимостей
// (окно/пул объявлены вперёд), чтобы хедер был дешёвым для включения отовсюду.

namespace devils_engine {
namespace thread {
class atomic_pool;
}
} // namespace devils_engine
namespace devils_engine {
namespace demiurg {
class resource_system;
}
} // namespace devils_engine

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// Монотонный task-id перенесён в движок (simul::generate_task_id). Alias сохраняет вызовы
// generate_task_id() в проектном коде и host-биндингах.
using devils_engine::simul::generate_task_id;

// все равно придется делить по типам ресурсов
// помоему только за звуком нужно вот так следить
struct resource_status {
  // resource
  // value
  // ???
};

using resource_ref = simul::resource_ref;
using command_sound_play = simul::command_sound_play;
using command_sound_stop = simul::command_sound_stop;
using command_sound_update = simul::command_sound_update;
using sound_state_entry = simul::sound_state_entry;
using command_sound_state = simul::command_sound_state;

struct command_update_ui {
  std::vector<uint32_t> vertices;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> commands;
  // ... ?
};

using command_recreate_sound_system = simul::command_recreate_sound_system;
using command_sound_devices = simul::command_sound_devices;
using command_window_recreation = simul::command_window_recreation;
using command_window_resize = simul::command_window_resize;
using command_render_set_active = simul::command_render_set_active;
using command_sound_set_master_gain = simul::command_sound_set_master_gain;
using command_update_constant = simul::command_render_update_constant;

// обратно должны вернуть id для ресурса
struct command_register_asset {
  void* resource;
};

// тут может быть достаточно длинная операция
// может быть нужно просто скопировать, а может быть нужно
// ужать текстурку и еще может быть нужно будет переделать ее в BC7
// в целом наверное пусть этим займется графическая подсистема
// ей нужно будет закидывать задания, и она обратно будет отправлять статус
// как раз система ресурсов будет потихоньку обновлять статус собственно ресурсов
struct command_gpu_load {
  void* resource;
};

// как должна выглядеть в таком случае система стриминга мира?
// геймплей постит текущий стейт относительно происходящего в мире
// ассеты подхватывают это дело и вгружают/выгружают нужные вещи

// вызываем когда переходим в состояние загрузки
struct command_thread_pool_change_owner {
  thread::atomic_pool* pool;
};

// закидываем в основной поток, чтобы в интерфейсе нарисовать что нибудь интересное
struct command_current_loading_state {
  std::string msg;
  // размеры?
};

using command_load_resource = simul::command_load_resource;

// main → assets: запросить CPU-чанк мира. textures — конкретный palette stable handles;
// assets выбирает для каждой клетки ресурс из него, не предполагая ничего о GPU slots.
// Позже coord/size останутся ключом запроса, а генератор заменится на demiurg-backed ресурс.
struct command_load_chunk {
  uint64_t generation = 0;
  int32_t x = 0;
  int32_t y = 0;
  uint32_t size = 0;
  std::vector<demiurg::resource_handle> textures;
};

// assets → main: готовые texture handles клеток. textures.size() == size*size, row-major.
struct command_chunk_loaded {
  uint64_t generation = 0;
  int32_t x = 0;
  int32_t y = 0;
  uint32_t size = 0;
  std::vector<demiurg::resource_handle> textures;
};

using command_gpu_transition = simul::command_gpu_transition;
using command_gpu_done = simul::command_gpu_done;
using command_prepare_shaders = simul::command_prepare_shaders;
using command_shaders_prepared = simul::command_shaders_prepared;
using command_set_active_graph = simul::command_render_set_graph;

// main → render: срез ВИДИМЫХ тайлов на отрисовку. ВОТ ОНО — «сообщение в рендер тред».
// Метаданные (как трансформировать + сколько инстансов + страйд) + СЫРЫЕ БАЙТЫ инстансов в
// формате instance_layout draw_group ("v2ui1"). Байты пакует tile_batch через draw_intent.
//
// ХРАНИЛИЩЕ БАЙТ — точка для будущей ревизии hot-path: сейчас std::vector<uint8_t> (аллокация
// на сообщение). Производитель пишет через draw_intent::blit в ЛЮБОЙ буфер, поэтому замена на
// пул/ring буферов позже НЕ затронет логику сборки среза. view_proj держим как std::array<float,16>
// (= column-major glm::mat4), чтобы не тянуть glm в этот лёгкий, всюду включаемый хедер.
struct command_draw_tiles {
  std::array<float, 16> view_proj{}; // world(xy) -> clip (ortho top-down)
  uint32_t count = 0;                // число инстансов
  uint32_t stride = 0;               // байт на инстанс (= sizeof(tile_instance))
  std::vector<uint8_t> bytes;        // упакованные инстансы
};

// main → render: lightweight actor instances. Actor layout is v2ui1c4v1:
// world center + type/texture index + color + visual size. Kept as a separate command so render can
// use a different draw_group/pair budget without changing producer code later.
struct command_draw_actors {
  uint32_t count = 0;
  uint32_t stride = 0;
  size_t sim_frame_time = 0; // time between simulation snapshots, in utils::global_time_resolution units
  std::vector<uint8_t> bytes;
  std::vector<uint32_t> ids; // stable ids aligned with bytes/current instances
};

// main → render: снапшот 2D-камеры. Рендер интерполирует prev→cur по реальным часам (тот же
// рецепт, что у акторов: nominal_clock + lerp) и САМ собирает global_ubo каждый рендер-кадр —
// камера скользит плавно на любом fps рендера, а не шагает с частотой main-тика. Хедер лёгкий,
// glm не тянем — плоские float. fb_* — размер фреймбуфера для ui_proj/misc (дискретен, снап
// к новейшему снапшоту, как texture у акторов).
struct command_draw_camera {
  float center_x = 0.0f;
  float center_y = 0.0f;
  float half_width = 1.0f;
  float aspect = 1.0f;
  float fb_width = 1.0f;
  float fb_height = 1.0f;
  size_t frame_time = 0; // интервал до следующего снапшота, единицы utils::global_time_resolution
};

// (command_write_buffer удалён — запись буферов переведена на SPSC write_buffer_channel:
//  POD-сообщение {name_hash,pos,size} + byte_ring под payload, см. write_buffer_channel.h.)

} // namespace core
} // namespace tile_frontier

#endif
