#ifndef TILE_FRONTIER_CORE_MESSAGES_H
#define TILE_FRONTIER_CORE_MESSAGES_H

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include <devils_engine/demiurg/resource_system.h>

#include "actors.h"

// Контракты между системами: структуры, которые ходят через диспетчер сообщений.
// Это намеренно "тупые" POD-подобные сообщения без логики и без тяжёлых зависимостей
// (окно/пул объявлены вперёд), чтобы хедер был дешёвым для включения отовсюду.

struct GLFWwindow;
struct GLFWmonitor;

namespace devils_engine { namespace thread { class atomic_pool; } }
namespace devils_engine { namespace demiurg { class resource_system; } }

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

struct resource_ref {
  demiurg::resource_handle handle;
  demiurg::resource_interface* direct = nullptr;

  static resource_ref from_handle(const demiurg::resource_handle h) noexcept {
    resource_ref ref;
    ref.handle = h;
    return ref;
  }

  static resource_ref from_direct(demiurg::resource_interface* const ptr) noexcept {
    resource_ref ref;
    ref.direct = ptr;
    return ref;
  }

  static resource_ref from_system(const demiurg::resource_system* const system, demiurg::resource_interface* const ptr) noexcept {
    if (system != nullptr && ptr != nullptr) {
      const demiurg::resource_handle h = system->handle(ptr->id);
      if (h.get() == ptr) return from_handle(h);
    }
    return from_direct(ptr);
  }

  demiurg::resource_interface* get() const noexcept {
    if (auto* ptr = handle.get()) return ptr;
    return direct;
  }

  template <typename T>
  T* get() const noexcept {
    if (auto* ptr = handle.template get<T>()) return ptr;
    if (direct == nullptr || direct->loading_type_id != utils::type_id<T>()) return nullptr;
    return static_cast<T*>(direct);
  }
};

// main → sound: проиграть предзагруженный звук. taskid генерит main, чтобы СРАЗУ вернуть
// хэндл в lua (актор берёт его как id задачи). res — стабильный demiurg handle ресурса.
// start — [0,1] откуда играть. after — секвенсинг (SIZE_MAX = сразу).
struct command_sound_play {
  size_t taskid;
  size_t after = SIZE_MAX;
  // demiurg-хендл звук-ресурса, управляемого потоком ассетов. Звуковая система читает из него
  // (view()) и НЕ хранит ресурсный указатель.
  resource_ref res;
  double start = 0.0;
  // категория звука (sound::type: music/talk/talk_pos/ui_effect/sfx). Держим как uint32, чтобы не
  // тянуть sound-хедер сюда; UINT32_MAX = не задано → актор возьмёт sfx. Раньше актор хардкодил sfx.
  uint32_t type = UINT32_MAX;
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

// main → render: изменился размер фреймбуфера (ресайз/фуллскрин). Продюсер — main при событии
// framebuffer size от GLFW. Рендер пересоздаёт ТОЛЬКО свопчейн (surface/device/graph не трогает),
// в отличие от тяжёлого command_window_recreation. Нулевые размеры (сворачивание) сюда не шлём.
struct command_window_resize {
  uint32_t width = 0, height = 0;
};

// main → render: включить/выключить блок отрисовки. Продюсер — main по window_policy (потеря фокуса
// со свёрнутым окном ⇒ не крутим кадры). Свёрнутое окно и так не рисуется (свопчейн 0×0), это
// оптимизация «не гонять рендер-граф вхолостую».
struct command_render_set_active {
  bool draw = true;
};

// main → sound: мастер-громкость [0,1] финального микса (приглушение при потере фокуса и т.п.).
// Звуковой актор зовёт system2::set_master_volume. НЕ реплицируется (презентация).
struct command_sound_set_master_gain {
  float gain = 1.0f;
};

// main → render: обновить именованную render-graph КОНСТАНТУ новыми байтами (напр. clear-цвет для
// шага clear берётся из константы). Рендер резолвит name→find_constant, пишет write_constant_data и
// публикует через update_constant_memory. bytes должны совпадать по размеру/раскладке с константой.
struct command_update_constant {
  std::string name;
  std::vector<uint8_t> bytes;
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
// Валюта контракта — stable handle ресурса из реестра ассетов. direct fallback остаётся только для
// synthetic ресурсов, которые пока создаются вне registry (например, UI font_resource).
// target — это demiurg::state::values (cold=0/warm=1/hot=2); храним int чтобы не тянуть demiurg в хедер.
struct command_load_resource {
  resource_ref res;
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
  resource_ref res;
  bool load; // true: warm→hot (load_warm); false: hot→warm (unload_hot)
};

// render → assets: «GPU-переход завершён» (рендер уже флипнул _state и записал gpu_index в ресурс).
struct command_gpu_done {
  resource_ref res;
};

// render → assets: подготовить CPU-heavy shader payload для render graph. Assets thread грузит
// GLSL из engine registry, компилирует в SPIR-V cache внутри glsl_source_file и отвечает render.
struct command_prepare_shaders {
  const demiurg::resource_system* registry = nullptr;
  std::string prefix;
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

// (command_write_buffer удалён — запись буферов переведена на SPSC write_buffer_channel:
//  POD-сообщение {name_hash,pos,size} + byte_ring под payload, см. write_buffer_channel.h.)

}
}

#endif
