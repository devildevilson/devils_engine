#ifndef FRONTIER_ONLINE_CORE_MESSAGES_H
#define FRONTIER_ONLINE_CORE_MESSAGES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <devils_engine/simul/messages.h>

#include "core/terrain.h" // terrain_code — причинная величина клетки

// Контракты между системами: структуры, которые ходят через диспетчер сообщений.
// Это намеренно "тупые" POD-подобные сообщения без логики и без тяжёлых зависимостей,
// чтобы хедер был дешёвым для включения отовсюду.

namespace frontier_online {
namespace core {

using namespace devils_engine;

// Монотонный task-id перенесён в движок (simul::generate_task_id). Alias сохраняет вызовы
// generate_task_id() в проектном коде и host-биндингах.
using devils_engine::simul::generate_task_id;

// Сообщения движка, которыми проект пользуется под своими именами.
using resource_ref = simul::resource_ref;
using command_sound_play = simul::command_sound_play;
using command_window_recreation = simul::command_window_recreation;

// main → assets: посчитать чанк. Запрос САМООПИСЫВАЮЩИЙ — он называет мир, к которому относится.
//
// Это не избыточность. Отдельное сообщение «открой мир» жило бы в своей очереди, и порядок между
// двумя очередями ничем не задан: запрос чанка мог бы прийти раньше мира, и его пришлось бы либо
// уронить (чанк никогда не загрузится), либо копить. Мир, названный в самом запросе, снимает
// вопрос целиком, а заодно оказывается ровно тем, что понадобится сетевой стороне: чанк
// принадлежит (генератор, зерно, размер), а не «текущему состоянию загрузчика».
struct command_load_chunk {
  uint64_t generation = 0;
  uint64_t world_build = 0; // какая постройка сетки заказала чанк (мир сессии меняется без смены сцены)
  int32_t x = 0;
  int32_t y = 0;
  uint32_t chunk_size = 0;
  uint64_t world_seed = 0;
  std::string generator; // demiurg-id точки входа генератора
};

// assets → main: КОДЫ РЕЛЬЕФА клеток, terrain.size() == size*size, row-major.
//
// Раньше здесь ездили handle'ы текстур, то есть сырые указатели в реестр ресурсов. Теперь едет
// причинная величина: тот же чанк можно сохранить, сравнить с чанком другого процесса и вообще
// посчитать у себя, не имея ни одной текстуры.
struct command_chunk_loaded {
  uint64_t generation = 0;
  uint64_t world_build = 0;
  int32_t x = 0;
  int32_t y = 0;
  uint32_t size = 0;
  std::vector<terrain_code> terrain;
};

// main → render: срез ВИДИМЫХ тайлов на отрисовку. ВОТ ОНО — «сообщение в рендер тред».
// Метаданные (как трансформировать + сколько инстансов + страйд) + СЫРЫЕ БАЙТЫ инстансов в
// формате instance_layout draw_group ("v2ui1"). Байты пакует tile_batch через draw_intent.
//
// ХРАНИЛИЩЕ БАЙТ — точка для будущей ревизии hot-path: сейчас std::vector<uint8_t> (аллокация
// на сообщение). Производитель пишет через draw_intent::blit в ЛЮБОЙ буфер, поэтому замена на
// пул/ring буферов позже НЕ затронет логику сборки среза. Проекцию тайлы не несут: её собирает
// рендер из снапшота камеры (command_draw_camera).
struct command_draw_tiles {
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
// рецепт, что у акторов: simul::value_interpolator + render_frame_clock) и САМ собирает global_ubo
// каждый рендер-кадр —
// камера скользит плавно на любом fps рендера, а не шагает с частотой main-тика. Хедер лёгкий,
// glm не тянем — плоские float. Размер кадра для интерфейса рендер берёт у своего swapchain.
struct command_draw_camera {
  float center_x = 0.0f;
  float center_y = 0.0f;
  float half_width = 1.0f;
  float aspect = 1.0f;
  size_t frame_time = 0; // интервал до следующего снапшота, единицы utils::global_time_resolution
};

// main -> render: состояние единственной active-gameplay timeline. Render продолжает обслуживать
// окно/UI на паузе, но не продвигает world interpolation по wall clock. Latest-wins достаточно:
// важен текущий уровень, а не история его переключений.
struct command_active_gameplay_state {
  bool active = true;
};

} // namespace core
} // namespace frontier_online

#endif
