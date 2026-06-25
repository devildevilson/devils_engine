#ifndef TILE_FRONTIER_CORE_MESSAGES_H
#define TILE_FRONTIER_CORE_MESSAGES_H

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Контракты между системами: структуры, которые ходят через диспетчер сообщений.
// Это намеренно "тупые" POD-подобные сообщения без логики и без тяжёлых зависимостей
// (окно/пул объявлены вперёд), чтобы хедер был дешёвым для включения отовсюду.

struct GLFWwindow;
struct GLFWmonitor;

namespace devils_engine { namespace thread { class atomic_pool; } }
namespace devils_engine { namespace demiurg { class resource_interface; } }

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// все равно придется делить по типам ресурсов
// помоему только за звуком нужно вот так следить
struct resource_status {
  // resource
  // value
  // ???
};

struct command_sound {
  // ресурс
  // команда (стоп, плей)
  // данные

  size_t taskid;
  size_t after;
  void* res;
  uint32_t sourceid; // по нему мы должны получить текущее положение
  uint32_t cmd; // старт, стоп
  uint32_t type;
  // fadein fadeout ? по идее описывается в микшере
  uint32_t mix;
};

struct command_update_ui {
  std::vector<uint32_t> vertices;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> commands;
  // ... ?
};

// тут по идее нужно указать выбранное звуковое устройство
struct command_recreate_sound_system {

};

struct command_window_recreation {
  GLFWwindow* w;
  GLFWmonitor* m;
  uint32_t width, height;
};

struct command_window_resize {
  uint32_t width, height;
};

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

// main → assets: «доведи ресурс до состояния target».
// Валюта контракта — указатель на resource_interface из реестра ассетов
// (реестр строится один раз в init и потом только читается, поэтому указатель стабилен).
// target — это demiurg::state::values (cold=0/warm=1/hot=2); храним int чтобы не тянуть demiurg в хедер.
struct command_load_resource {
  demiurg::resource_interface* res;
  int32_t target;
};

// assets → render: «ресурс в памяти, подготовь его на GPU» (warm→hot) либо выгрузи (hot→warm).
struct command_gpu_transition {
  demiurg::resource_interface* res;
  bool load; // true: warm→hot (load_warm); false: hot→warm (unload_hot)
};

// render → assets: «GPU-переход завершён» (рендер уже флипнул _state и записал gpu_index в ресурс).
struct command_gpu_done {
  demiurg::resource_interface* res;
};

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

}
}

#endif
