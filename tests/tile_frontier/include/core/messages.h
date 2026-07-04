#ifndef TILE_FRONTIER_CORE_MESSAGES_H
#define TILE_FRONTIER_CORE_MESSAGES_H

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "actors.h"

// Контракты между системами: структуры, которые ходят через диспетчер сообщений.
// Это намеренно "тупые" POD-подобные сообщения без логики и без тяжёлых зависимостей
// (окно/пул объявлены вперёд), чтобы хедер был дешёвым для включения отовсюду.

struct GLFWwindow;
struct GLFWmonitor;

namespace devils_engine { namespace thread { class atomic_pool; } }
namespace devils_engine { namespace demiurg { class resource_interface; class resource_system; } }

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

// main → sound: проиграть предзагруженный звук. taskid генерит main, чтобы СРАЗУ вернуть
// хэндл в lua (актор берёт его как id задачи). name — хеш имени (в будущем — handle ресурса
// demiurg). start — [0,1] откуда играть. after — секвенсинг (SIZE_MAX = сразу).
struct command_sound_play {
  size_t taskid;
  size_t after = SIZE_MAX;
  // demiurg-хендл звук-ресурса (sound::sound_resource*), управляемого потоком ассетов. Звуковая
  // система читает из него (view()) и НЕ хранит ресурсы. main резолвит имя→ресурс и шлёт указатель.
  demiurg::resource_interface* res = nullptr;
  double start = 0.0;
};

// main → sound: завершить задачу (remove_sound). Отдельное сообщение — без поля-команды-энума.
struct command_sound_stop {
  size_t taskid;
};

// main → sound: обновить позиционные параметры живой задачи (3D-листенер — позже).
struct command_sound_update {
  size_t taskid;
  float pos[3];
  float dir[3];
  float vel[3];
};

// sound → main: упрощённое состояние ОДНОГО звука в обработке (для UI «здесь и сейчас»).
struct sound_state_entry {
  size_t taskid;
  double progress; // [0,1]
};

// sound → main: ПОЛНЫЙ слепок состояния звуковой системы, публикуется периодически самим
// звуковым потоком. main держит последний (consume_last) и читает из него app.sound_state.
// Это «двойная буферизация через сообщение»: у звука своя живая копия задач, у main — её
// снимок для UI; никаких сырых указателей/atomic наружу. Тот же паттерн пойдёт ассетам
// (публиковать список ресурсов в обработке → прогресс-бар загрузки на экране).
struct command_sound_state {
  std::vector<sound_state_entry> sounds;
};

struct command_update_ui {
  std::vector<uint32_t> vertices;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> commands;
  // ... ?
};

// тут по идее нужно указать выбранное звуковое устройство
struct command_recreate_sound_system {
  std::string device_name;
};

struct command_sound_devices {
  size_t request_id;
  std::vector<std::string>* out;
  std::atomic_bool* ready;
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

// main → assets: запросить CPU-чанк мира. Это mock-ассетный путь для первого world slice:
// assets thread детерминированно генерирует payload и отвечает command_chunk_loaded в reply_to.
// Позже coord/size останутся ключом запроса, а генератор заменится на demiurg-backed ресурс.
struct command_load_chunk {
  int32_t x = 0;
  int32_t y = 0;
  uint32_t size = 0;
  uint32_t texture_count = 0;
  simulation_actor* reply_to = nullptr;
};

// assets → main: готовый CPU payload чанка. textures.size() == size*size, row-major.
struct command_chunk_loaded {
  int32_t x = 0;
  int32_t y = 0;
  uint32_t size = 0;
  std::vector<uint32_t> textures;
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

// render → assets: подготовить CPU-heavy shader payload для render graph. Assets thread грузит
// GLSL из engine registry, компилирует в SPIR-V cache внутри glsl_source_file и отвечает render.
struct command_prepare_shaders {
  const demiurg::resource_system* registry = nullptr;
  std::string prefix;
  graphics_actor* reply_to = nullptr;
};

// assets → render: shader prepare завершён. failed > 0 означает, что render не должен собирать graph.
struct command_shaders_prepared {
  uint32_t compiled = 0;
  uint32_t failed = 0;
};

// main → render: сменить активный render graph (п.2/п.3). Целевой граф должен входить в resident-набор
// (иначе его GPU-ресурсы не созданы). Своп мгновенный: ресурсы/дескрипторы НЕ пересоздаются, строится
// только graph-local инстанс (пайплайны из подготовленного SPIR-V), синхронизация — по fence кадров.
struct command_set_active_graph {
  std::string name;
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

// main → render: lightweight actor instances. Actor layout is v2ui1c4v1:
// world center + type/texture index + color + visual size. Kept as a separate command so render can
// use a different draw_group/pair budget without changing producer code later.
struct command_draw_actors {
  uint32_t count = 0;
  uint32_t stride = 0;
  size_t sim_frame_time = 0;          // time between simulation snapshots, in utils::global_time_resolution units
  std::vector<uint8_t> bytes;
  std::vector<uint32_t> ids;          // stable ids aligned with bytes/current instances
};

// main → render: записать СЫРЫЕ БАЙТЫ в host-visible буфер-ресурс рендер-графа (по имени).
// Контракт записи в произвольный буфер — аналог draw_group-контракта, но для общих буферов
// (камера/константы кадра и т.п.). Render пишет байты во ВСЕ per_update-копии буфера; смену
// активной копии делает отдельное событие update (флип per_update в конце апдейтов main).
// bytes обрезаются до размера одной копии буфера. Имя стабильно (резолвится find_resource).
struct command_write_buffer {
  std::string buffer;          // имя ресурса (напр. "camera_buffer")
  std::vector<uint8_t> bytes;  // сырые данные
};

}
}

#endif
