#include "simulation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <span>
#include <thread>

#include <devils_engine/input/core.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/time-utils.hpp>

#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/demiurg/module_system.h>

#include <devils_engine/visage/system.h>
#include <devils_engine/visage/font.h>
#include <devils_engine/visage/font_atlas_packer.h>

#include "config.h"
#include "messages.h"
#include "message_dispatcher.h"
#include "sound_system.h"
#include "render_system.h"
#include "assets_system.h"
#include "texture_resource.h"
#include "app_config_resource.h"
#include "font_atlas_resource.h"
#include "global_ubo.h"
#include "texture_set.h"
#include "tile_map.h"
#include "tile_batch.h"
#include "actor_simulation.h"

/*
вопрос в том как правильно передать окно в рендер?
от окна нужен только сюрфейс, но может быть ситуация
когда нам придется полностью сбросить окно и пересоздать все с нуля
тут очевидно что инициализация окна будет происходить отложенно
и нужно дополнительно отправить ивент с указателями на окно
который пересоздаст все ресурсы при смене окна
дополнительно отправить событие на изменение размера окна
*/

namespace tile_frontier {
namespace core {

using namespace devils_engine;

static size_t frame_time_from_fps(const uint32_t fps) noexcept {
  const auto valid_fps = std::max(fps, 1u);
  return utils::round(double(utils::global_time_resolution) / double(valid_fps));
}

static size_t thread_start_gap(const size_t frame_time, const uint32_t divisor) noexcept {
  const auto valid_divisor = std::max(divisor, 1u);
  return utils::round(double(frame_time) / double(valid_divisor));
}

constexpr size_t main_frame_time = utils::round(double(utils::global_time_resolution) * (1.0/20.0));
constexpr uint32_t initial_actor_count = 4096;
//constexpr uint32_t initial_actor_count = 64000;

static void error_callback(int, const char* msg) noexcept {
  utils::warn("GLFW error: {}", msg);
}

// --- ввод для интерфейса (visage), шаг 1.1 ---
// GLFW-коллбэки не имеют захватов (это C-указатели на функции) и срабатывают в input::poll_events
// на главном потоке, в том же кадре до сборки снапшота. Поэтому аккумулируем в файловую структуру
// без атомиков (одно окно/один UI). Позицию мыши опрашиваем напрямую (input::cursor_pos), а
// дискретные события (кнопки/колесо/текст/клавиши) копим тут.
namespace {
// Lua-ОБЁРТКА id звуковой задачи. Живёт ТОЛЬКО здесь, на границе биндинга — в контракте
// сообщений (command_sound) ходит голый size_t, чтобы не тащить лишний тип по всем хедерам.
// Смысл обёртки чисто lua-шный: usertype без арифметических метаметодов, поэтому скрипт не
// сделает над id вычислений и не передаст случайное число в поле after (секвенсинг).
struct sound_handle {
  size_t value = SIZE_MAX;
  bool valid() const noexcept { return value != SIZE_MAX; }
};

// Main-локальная запись таблицы состояния звука. = wire-запись (taskid/progress) + deadline:
// 0 — ПОДТВЕРЖДЕНА последней публикацией звука; >0 — ОПТИМИСТИЧНАЯ (только что запрошен play,
// ещё не доехал в публикацию), живёт до этого main-кадра. Срок отличает «ещё не стартовал»
// (вернём 0) от «уже закончился» (nil), убирая отдельный pending-контейнер.
struct ui_sound_state_entry {
  size_t taskid;
  double progress;
  size_t deadline;
};

// Зеркало стабильных констант GLFW (публичный ABI, не меняется). input-обёртка их наружу не
// отдаёт, а коллбэки приходят с сырыми int-кодами, поэтому держим нужный минимум здесь.
namespace glfw_const {
  enum { release = 0, press = 1, repeat = 2 };
  enum { mouse_left = 0, mouse_right = 1, mouse_middle = 2 };
  enum { mod_shift = 0x0001, mod_control = 0x0002 };
  enum {
    key_enter = 257, key_tab = 258, key_backspace = 259, key_delete = 261,
    key_right = 262, key_left = 263, key_down = 264, key_up = 265
  };
}

struct ui_input_t {
  bool mouse_left = false, mouse_middle = false, mouse_right = false;
  float scroll_x = 0.0f, scroll_y = 0.0f;   // накапливается между кадрами, обнуляется при consume
  std::vector<uint32_t> text;                // utf32-кодпоинты, введённые между кадрами
  bool shift = false, ctrl = false;
  bool backspace = false, del = false, enter = false, tab = false;
  bool left = false, right = false, up = false, down = false;
};
ui_input_t g_ui_input;

void ui_mouse_button_cb(GLFWwindow*, int button, int action, int) noexcept {
  const bool down = (action != glfw_const::release);
  switch (button) {
    case glfw_const::mouse_left:   g_ui_input.mouse_left = down; break;
    case glfw_const::mouse_right:  g_ui_input.mouse_right = down; break;
    case glfw_const::mouse_middle: g_ui_input.mouse_middle = down; break;
    default: break;
  }
}

void ui_scroll_cb(GLFWwindow*, double x, double y) noexcept {
  g_ui_input.scroll_x += float(x);
  g_ui_input.scroll_y += float(y);
}

void ui_char_cb(GLFWwindow*, unsigned int codepoint) noexcept {
  g_ui_input.text.push_back(uint32_t(codepoint));
}

void ui_key_cb(GLFWwindow*, int key, int, int action, int mods) noexcept {
  const bool down = (action != glfw_const::release);
  g_ui_input.shift = (mods & glfw_const::mod_shift) != 0;
  g_ui_input.ctrl  = (mods & glfw_const::mod_control) != 0;
  switch (key) {
    case glfw_const::key_backspace: g_ui_input.backspace = down; break;
    case glfw_const::key_delete:    g_ui_input.del = down; break;
    case glfw_const::key_enter:     g_ui_input.enter = down; break;
    case glfw_const::key_tab:       g_ui_input.tab = down; break;
    case glfw_const::key_left:      g_ui_input.left = down; break;
    case glfw_const::key_right:     g_ui_input.right = down; break;
    case glfw_const::key_up:        g_ui_input.up = down; break;
    case glfw_const::key_down:      g_ui_input.down = down; break;
    default: break;
  }
}
} // namespace

// тут что? все другие системы + потоки для них + тред пул
// кеш?
struct simulation_init {
  app_config config;

  // Движковый (незаменяемый) реестр ресурсов: config/shaders/render-graph. Отдельный
  // resource_system, изолированный от игрового (моды живут в assets). См. demiurg 1a (Q2).
  std::unique_ptr<demiurg::module_system> engine_modules;
  std::unique_ptr<demiurg::resource_system> engine_resources;

  std::unique_ptr<thread::atomic_pool> pool_container;
  thread::atomic_pool* pool;

  std::unique_ptr<sound_simulation> sound_sim;
  std::unique_ptr<render_simulation> render_sim;
  std::unique_ptr<assets_simulation> assets_sim;

  std::unique_ptr<std::jthread> sound_thread;
  std::unique_ptr<std::jthread> render_thread;
  std::unique_ptr<std::jthread> assets_thread;

  GLFWwindow* window;
  GLFWmonitor* monitor;

  std::unique_ptr<input::init> in;

  // интерфейс (visage) живёт в главном потоке. ui_font_image — CPU-байты атласа,
  // их позже (шаг 2) зальём в GPU-ресурс шрифта на потоке рендера.
  std::unique_ptr<visage::font_t> ui_font;
  visage::font_atlas_packer::font_image_t ui_font_image;
  std::unique_ptr<visage::system> ui;
  bool ui_logged = false;

  // атлас шрифта как GPU-текстура в таблице ассетов (см. font_atlas_resource)
  std::unique_ptr<font_atlas_resource> ui_atlas;
  bool ui_atlas_logged = false;

  // модель тайловой карты (главная сторона)
  texture_set textures;     // текстуры карты, собранные по префиксу пути
  tile_grid grid;           // квадратная сетка тайлов
  uint32_t chunk_size = 16;
  uint32_t chunks_x = 4;
  uint32_t chunks_y = 4;
  std::vector<bool> chunks_requested;
  std::vector<bool> chunks_loaded;
  uint32_t chunks_loaded_count = 0;
  cached_message_dispatcher<command_chunk_loaded> chunk_loaded_commands;
  camera2d cam;             // орто top-down камера
  tile_batch batch;         // продюсер инстансов видимого среза
  bool tiles_logged = false;
  bool chunks_logged = false;

  // первый actor simulation slice: aesthetics компоненты -> intents -> apply -> GPU batch
  actor_world_slice actors;
  actor_batch actors_batch;
  bool actors_logged = false;
  actor_metrics actors_last_metrics;
  uint64_t metrics_frames = 0;
  uint64_t metrics_actor_ticks = 0;
  uint64_t metrics_intents = 0;
  uint64_t metrics_instances = 0;
  uint64_t metrics_actor_update_us = 0;
  double ui_main_fps = 0.0;
  double ui_intents_per_sec = 0.0;
  double ui_instances_per_sec = 0.0;
  double ui_actor_update_avg_us = 0.0;
  std::chrono::steady_clock::time_point metrics_last_log = std::chrono::steady_clock::now();

  std::vector<std::string> sound_devices;
  std::atomic_bool sound_devices_ready = false;
  bool sound_devices_requested = false;
  bool sound_devices_logged = false;
  bool sound_recreate_test_sent = false;
  size_t tick = 0;

  // Единая таблица состояния звука для UI (app.sound_state). Звуковой поток ПУШИТ полный слепок
  // (command_sound_state); consume в update СЛИВАЕТ его с оптимистичными записями, добавленными
  // в app.play_sound (см. ui_sound_state_entry.deadline). _next — скретч под слияние без аллокаций.
  cached_message_dispatcher<command_sound_state> sound_state_commands;
  std::vector<ui_sound_state_entry> sound_state;
  std::vector<ui_sound_state_entry> sound_state_next;
  size_t sound_frame = 0; // счётчик main-кадров (для дедлайна оптимистичных записей)

  simulation_init() : pool(nullptr), window(nullptr), monitor(nullptr) {}
};

static void create_window_and_notify_render(simulation_init& c, graphics_actor* gactor) {
  if (c.window != nullptr) return;

  if (!c.in) c.in = std::make_unique<input::init>(&error_callback);

  c.monitor = input::primary_monitor();
  c.window = input::create_window(c.config.window.width, c.config.window.height, c.config.window.title);
  if (c.window == nullptr) {
    utils::error{}(
      "Could not create window '{}' {}x{}",
      c.config.window.title,
      c.config.window.width,
      c.config.window.height
    );
  }

  if (c.monitor != nullptr) {
    const auto monitor_name = input::monitor_name(c.monitor);
    utils::info("Using monitor '{}'", monitor_name);
  }

  // ввод для интерфейса: позицию курсора опрашиваем в update через input::cursor_pos,
  // а дискретные события копим коллбэками (срабатывают в input::poll_events).
  input::set_window_callback(c.window, &ui_mouse_button_cb);
  input::set_window_callback(c.window, &ui_scroll_cb);
  input::set_window_callback(c.window, &ui_char_cb);
  input::set_window_callback(c.window, &ui_key_cb);

  command_window_recreation wr{
    c.window,
    c.monitor,
    c.config.window.width,
    c.config.window.height
  };
  gactor->send(wr);
}

simulation::simulation() noexcept : simul::advancer(main_frame_time), sactor(nullptr), gactor(nullptr), aactor(nullptr) {}

simulation::~simulation() noexcept {
  if (!container) return;

  if (container->sound_sim)  container->sound_sim->stop();
  if (container->render_sim) container->render_sim->stop();
  if (container->assets_sim) container->assets_sim->stop();

  container->sound_thread.reset();
  container->render_thread.reset();
  container->assets_thread.reset();

  container->render_sim.reset();

  if (container->window != nullptr) {
    input::destroy(container->window);
    container->window = nullptr;
  }
  container->monitor = nullptr;
  container->in.reset();

  container->sound_sim.reset();
  container->assets_sim.reset();

  aactor = nullptr;
  gactor = nullptr;
  sactor = nullptr;
}

// Поднимаем интерфейс в главном потоке: строим MSDF-атлас шрифта на CPU (GPU пока не нужен —
// nk_convert требует только метрик глифов), создаём visage::system на дефолтном шрифте и
// загружаем lua entry. Байты атласа держим в контейнере под будущую заливку на GPU (шаг 2).
static void setup_visage(simulation_init& c) {
  visage::font_atlas_packer packer;
  packer.setup_font(utils::project_folder() + "resources/fonts/crimson.roman.ttf");

  visage::font_atlas_packer::config fcfg{};
  fcfg.max_corner_angle = 3.0;
  fcfg.minimum_scale = 32.0;
  fcfg.pixel_range = 4.0; // ширина полосы SDF (текселей) — должна совпадать с px_range в ui.frag
  fcfg.mitter_limit = 1.0;
  fcfg.color_channels = 4; // mtsdf — пригодится под границы/эффекты на шаге SDF
  fcfg.thread_count = 4;
  fcfg.save_png = false;
  // charsets оставляем пустым: load_fonts всегда грузит ASCII. Кириллицу/локали — на шаге 2/3.

  auto [fonts, img] = packer.load_fonts(fcfg);
  if (fonts.empty()) utils::error{}("visage: font atlas packer produced no fonts");

  c.ui_font = std::move(fonts.front());
  c.ui_font_image = std::move(img);
  utils::info("visage: font atlas {}x{}x{}ch, {} glyphs",
    c.ui_font_image.width, c.ui_font_image.height, c.ui_font_image.channels, c.ui_font->glyphs.size());

  c.ui.reset(new visage::system(c.ui_font.get()));
  c.ui->load_entry_point(utils::project_folder() + "resources/ui/entry.lua");
  utils::info("visage: system created, entry point loaded");

  // атлас на GPU как обычная текстура: переносим байты в ресурс, который пройдёт штатный путь
  // ассетов (cold→warm→hot) и сядет в дескриптор 'textures'. Запрос на загрузку шлёт init().
  c.ui_atlas = std::make_unique<font_atlas_resource>(
    std::move(c.ui_font_image.bytes), c.ui_font_image.width, c.ui_font_image.height, c.ui_font_image.channels);
}

void simulation::init() {
  container.reset(new simulation_init);
  actor.add_receiver<command_chunk_loaded>(&container->chunk_loaded_commands.dis);
  actor.add_receiver<command_sound_state>(&container->sound_state_commands.dis);

  // Движковый реестр: engine-module = папка resources/engine/ (не перебивается модами).
  // Новый demiurg-API не нужен — module_system::load_modules умеет директорию как модуль.
  container->engine_resources = std::make_unique<demiurg::resource_system>();
  container->engine_resources->register_type<app_config_resource>("config", "tavl");
  container->engine_modules = std::make_unique<demiurg::module_system>(utils::project_folder() + "resources/");
  container->engine_modules->load_modules({ demiurg::module_system::list_entry{"engine", "", ""} });
  container->engine_resources->parse_resources(container->engine_modules.get());

  const auto config_path = utils::project_folder() + "resources/engine/config/app.tavl"; // для лога
  if (auto* cfg_res = container->engine_resources->get<app_config_resource>("config/app")) {
    cfg_res->load(utils::safe_handle_t{}); // cold→warm: читает + парсит tavl (CPU, главный поток)
    container->config = cfg_res->config();
  } else {
    utils::warn("engine registry: 'config/app' not found, using app_config defaults");
  }
  set_frame_time(frame_time_from_fps(container->config.simulation.main_fps));

  const uint32_t hw_threads = std::max(std::thread::hardware_concurrency(), 1u);
  const uint32_t reserved_threads = container->config.simulation.worker_threads_reserved;
  const uint32_t min_worker_threads = std::max(container->config.simulation.min_worker_threads, 1u);
  const uint32_t thread_count = std::max(
    hw_threads > reserved_threads ? hw_threads - reserved_threads : min_worker_threads,
    min_worker_threads
  );

  const auto cpu_name = utils::get_cpu_name();
  utils::info("Using cpu '{}', cores: {}, worker threads: {}", cpu_name, hw_threads, thread_count);
  utils::info(
    "Loaded app config '{}': window {}x{}, render config '{}', GPU preference '{}' / index {}",
    config_path,
    container->config.window.width,
    container->config.window.height,
    container->config.render.config_folder,
    container->config.render.preferred_gpu,
    container->config.render.preferred_gpu_index
  );

  container->pool_container.reset(new thread::atomic_pool(thread_count));
  container->pool = container->pool_container.get();

  const auto sound_ft = frame_time_from_fps(container->config.simulation.sound_fps);
  const auto render_ft = frame_time_from_fps(container->config.simulation.render_fps);
  const auto assets_ft = frame_time_from_fps(container->config.simulation.assets_fps);

  container->sound_sim.reset(new sound_simulation(sound_ft));
  container->sound_sim->init();
  container->sound_sim->set_main_actor(&actor); // звук пушит сюда command_sound_state
  sactor = container->sound_sim->get_actor();

  if (container->config.render.enabled) {
    render_simulation_config render_cfg;
    render_cfg.render_config_folder = make_project_folder_path(container->config.render.config_folder);
    render_cfg.pipeline_cache_path = make_project_path(container->config.render.pipeline_cache);
    render_cfg.graph_name = container->config.render.graph;
    render_cfg.headless = container->config.render.headless;
    render_cfg.create_vulkan_on_init = container->config.window.create_on_start || container->config.render.headless;

    if (container->config.window.create_on_start && !container->config.render.headless && !container->in) {
      container->in = std::make_unique<input::init>(&error_callback);
    }

    container->render_sim.reset(new render_simulation(render_ft, std::move(render_cfg)));
    container->render_sim->init();
    gactor = container->render_sim->get_actor();
  }

  container->assets_sim.reset(new assets_simulation(assets_ft));
  container->assets_sim->init();
  aactor = container->assets_sim->get_actor();
  container->assets_sim->set_render_actor(gactor); // gactor может быть null (render выключен)
  if (container->render_sim) container->render_sim->set_assets_actor(aactor);

  const auto gap_divisor = container->config.simulation.thread_start_gap_divisor;
  const auto sound_gap = thread_start_gap(sound_ft, gap_divisor);
  const auto render_gap = thread_start_gap(render_ft, gap_divisor);
  const auto assets_gap = thread_start_gap(assets_ft, gap_divisor);

  container->sound_thread.reset (new std::jthread([sys = container->sound_sim.get(), sound_gap] (){ sys->run(sound_gap); }));
  if (container->render_sim) {
    container->render_thread.reset(new std::jthread([sys = container->render_sim.get(), render_gap](){ sys->run(render_gap); }));
  }
  container->assets_thread.reset(new std::jthread([sys = container->assets_sim.get(), assets_gap](){ sys->run(assets_gap); }));

  if (sactor != nullptr) {
    command_sound_devices devices;
    devices.request_id = generate_task_id();
    devices.out = &container->sound_devices;
    devices.ready = &container->sound_devices_ready;
    sactor->send(devices);
    container->sound_devices_requested = true;
    utils::info("main: requested sound playback devices");
  }

  // Окно - поздний ресурс. Render thread должен жить и без него, а это событие
  // может прийти сейчас, после загрузки ассетов или после полного пересоздания окна.
  if (container->config.window.create_on_start && container->render_sim && !container->config.render.headless) {
    create_window_and_notify_render(*container, gactor);
  }

  // три grass-текстуры → texture_slots 0,1,2 (порядок запроса = порядок слотов, т.к. assets грузит по очереди)
  for (const auto* name : { "textures/grass", "textures/grass1_0", "textures/grass3" }) {
    if (auto* tex = container->assets_sim->resources()->get<texture_resource>(name)) {
      command_load_resource cmd{tex, static_cast<int32_t>(demiurg::state::hot)};
      aactor->send(cmd);
      utils::info("main: requested texture '{}' -> hot", name);
    } else {
      utils::warn("main: texture resource '{}' not found in registry", name);
    }
  }

  // --- модель тайловой карты ---
  // Набор текстур = все ресурсы с id-префиксом "textures/" (детерминированный порядок реестра).
  const uint32_t tex_count = container->textures.gather(*container->assets_sim->resources(), "textures/");
  utils::info("main: gathered {} textures by prefix 'textures/'", tex_count);

  // Квадратная сетка чанков 4x4 по 16 тайлов. Стартово всё заполнено текстурой 0, затем assets
  // thread вернёт mock CPU payload для каждого чанка и main применит его к grid.
  container->grid.tile_size = 1.0f;
  container->grid.resize(container->chunks_x * container->chunk_size, container->chunks_y * container->chunk_size);
  container->chunks_requested.assign(size_t(container->chunks_x) * container->chunks_y, false);
  container->chunks_loaded.assign(size_t(container->chunks_x) * container->chunks_y, false);

  if (aactor != nullptr) {
    for (uint32_t cy = 0; cy < container->chunks_y; ++cy) {
      for (uint32_t cx = 0; cx < container->chunks_x; ++cx) {
        const size_t idx = size_t(cy) * container->chunks_x + cx;
        command_load_chunk cmd;
        cmd.x = int32_t(cx);
        cmd.y = int32_t(cy);
        cmd.size = container->chunk_size;
        cmd.texture_count = std::max(tex_count, 1u);
        cmd.reply_to = &actor;
        aactor->send(cmd);
        container->chunks_requested[idx] = true;
      }
    }
    utils::info("main: requested {} mock world chunks via assets", container->chunks_requested.size());
  }

  // Камера смотрит в центр карты; половина ширины обзора = 8 тайлов.
  const glm::vec2 extent = container->grid.world_extent();
  container->cam.center = extent * 0.5f;
  container->cam.half_width = 8.0f;
  container->cam.aspect = float(container->config.window.width) / float(std::max(container->config.window.height, 1u));

  // Валидируем раскладку tile_instance против layout "v2ui1" один раз.
  if (const auto r = container->batch.bind("v2ui1"); !r) {
    utils::error{}("tile_instance layout mismatch vs 'v2ui1': {} (attr {}, expected {}, actual {})",
      instance_layout::match_error::to_string(r.error), r.where, r.expected, r.actual);
  }

  if (const auto r = container->actors_batch.bind("v2ui1c4v1"); !r) {
    utils::error{}("actor_instance layout mismatch vs 'v2ui1c4v1': {} (attr {}, expected {}, actual {})",
      instance_layout::match_error::to_string(r.error), r.where, r.expected, r.actual);
  }

  container->actors.init(
    initial_actor_count,
    glm::vec2{0.5f, 0.5f},
    glm::max(extent - glm::vec2{0.5f, 0.5f}, glm::vec2{0.5f, 0.5f}),
    std::max(tex_count, 1u)
  );
  container->metrics_last_log = std::chrono::steady_clock::now();
  utils::info("main: spawned {} lightweight actors in aesthetics world", initial_actor_count);

  // интерфейс (visage) в главном потоке
  setup_visage(*container);

  // UI-биндинги звука. visage — чисто UI и про звук не знает, поэтому их регистрирует ХОСТ
  // в lua-песочнице UI. Каждый — "обычный message на звуковой тред" (presentation→sound
  // напрямую, в лог реплея НЕ попадает — см. водораздел звука). Живут в namespace `app`
  // (общая точка для хост-биндингов; старый префикс tf_ выводим из обихода). Набор для плеера:
  //   app.play_sound(name [, {start=0..1, after=handle}]) -> sound_handle
  //   app.play_sound{ name="...", start=0..1, after=handle } -> sound_handle  (один аргумент-таблица)
  //   app.stop_sound(handle)                                  -- остановить и освободить голос
  //   app.sound_state(handle) -> progress(0..1) | nil         -- nil = задачи нет (играет ⇔ вернули число)
  // Очередь/кроссфейд плеер собирает сам на lua, опрашивая state и запуская следующий трек.
  // sound_handle — непрозрачный usertype без арифметики: скрипт не считает id и не суёт число.
  {
    auto& L = container->ui->script_state();
    L.new_usertype<sound_handle>("sound_handle",
      sol::no_constructor,
      "valid", &sound_handle::valid);

    sol::environment env = container->ui->script_env();
    sol::table app = env["app"].get_or_create<sol::table>(); // общий namespace хост-биндингов

    // первый аргумент — sol::object (строка-имя ИЛИ таблица-опции с полем name), второй —
    // необязательная таблица-опции (когда имя задано строкой). sol::object у НЕпоследнего
    // параметра безопаснее sol::optional (тот съедает не тот слот стека при nil).
    app.set_function("play_sound",
      [this](sol::object a, sol::optional<sol::table> b) -> sound_handle {
        if (sactor == nullptr) return sound_handle{};

        std::string name;
        sol::optional<sol::table> opts;
        if (a.is<sol::table>()) { opts = a.as<sol::table>(); name = opts->get_or("name", std::string{}); }
        else if (a.is<std::string>()) { name = a.as<std::string>(); opts = b; }
        if (name.empty()) return sound_handle{};

        command_sound_play play{};
        play.taskid = generate_task_id();
        play.after = SIZE_MAX;
        play.start = 0.0;
        if (opts) {
          play.start = std::clamp(opts->get_or("start", 0.0), 0.0, 1.0);
          const sol::optional<sound_handle> after = (*opts)["after"];
          if (after && after->valid()) play.after = after->value; // хэндл → секвенсинг
        }
        play.name = utils::string_hash(name); // тот же хеш, что у предзагруженных имён в акторе (позже — demiurg handle)
        sactor->send(play);
        // оптимистичная запись в ту же таблицу: пока play не доедет в публикацию (latency
        // 1-2 кадра), app.sound_state по ней вернёт 0, а не nil (deadline = окно старта).
        constexpr size_t startup_grace_frames = 30;
        container->sound_state.push_back({play.taskid, 0.0, container->sound_frame + startup_grace_frames});
        return sound_handle{play.taskid};
      });

    app.set_function("stop_sound", [this](const sound_handle& h) {
      if (sactor == nullptr || !h.valid()) return;
      command_sound_stop stop{};
      stop.taskid = h.value;
      sactor->send(stop);
    });

    // ищет id в единой таблице sound_state. Возвращает прогресс [0,1] или nil. Раз вернули
    // число — звук в обработке (играет/в очереди/только что запрошен); nil — задачи уже нет.
    // Оптимистичная запись с истёкшим окном старта (так и не доехала) трактуется как nil.
    app.set_function("sound_state", [this](const sound_handle& h) -> sol::object {
      auto& lua = container->ui->script_state();
      if (!h.valid()) return sol::nil;
      for (const auto& s : container->sound_state) {
        if (s.taskid != h.value) continue;
        if (s.deadline != 0 && container->sound_frame > s.deadline) return sol::nil; // окно вышло
        return sol::make_object(lua, s.progress);
      }
      return sol::nil;
    });
  }

  // атлас шрифта → GPU тем же путём, что и текстуры: просим ассеты довести до hot.
  // Слот определится динамически (после grass-текстур); main прочитает gpu_index по hot.
  if (container->ui_atlas && aactor != nullptr) {
    command_load_resource cmd{container->ui_atlas.get(), static_cast<int32_t>(demiurg::state::hot)};
    aactor->send(cmd);
    utils::info("main: requested font atlas -> hot");
  }
}

static size_t test_counter = 0;
bool simulation::stop_predicate() const {
  test_counter += 1;
  return test_counter > 200;
  //return false; // выход из приложения?
}

void simulation::update(const size_t time) {
  if (container) {
    container->tick += 1;
  }

  // думаем, собираем инпут, считаем физику, разбираемся с геймплеем, пробегаем UI, отправляем данные другим акторам
  // тут нужны состояния у системы + пауза
  // пауза это что? не думаем, не считаем физику, не разбираемся с геймплеем, но пробегаем UI

  // после "думаем" и инпут мы получаем intent

  // сама симуляция у нас находится в нескольких состояниях
  // init -> main_menu -> game (игровых состояний тоже несколько)
  // между каждым состояние есть состояние загрузки
  // в инит желательно не попадать больше одного раза
  // в главном меню должен быть способ просмотра модификаций которые есть на диске
  // для этого нужно добавить способ подгрузить картиночку в маленький пул
  // пул должен существовать только для main_menu

  // да думаю что у нас 3 фиксированных состояния + в игре несколько динамических состояний
  // динамические состояния все равно потребуют указать какие ресурсы грузить
  // в этом случае мне всего лишь нужно передать иерархическое описание "уровня"
  // которое в итоге развернется в конкретную загрузку штук
  // описание уровня поди можно составить на месте... в интерфейсе?
  // я бы наверное сказал что луа все же контролирует не интерфейс а скорее сам игровой процесс
  // получаем состояния объектов на карте и принимаем решение что именно сейчас произойдет
  // где здесь интерфейс и нужно ли его отдельно выделять? так как мы в луа получаем
  // состояние игры и у нас есть куча разных инструментов для контроля того что происходит
  // то и интерфейс нарисовать поверх этого не составит труда
  // нужно ли его как то совсем отдельно абстрагировать? интерфейс еще служит собственно управлением
  // процессом игры (собственно с помощью интерфейса игрок принимает решение выйти из игры, например)
  // поэтому эти две вещи связаны между собой

  // так да действительно полностью спихиваем control flow в луа (или в другой скриптовой язык)
  // для луа чисто задаем ряд функций как АПИ + вводим понятие зоны ответственности
  // вызов функции в луа проверяем на зону отвественности (запрещаем менять состояния неподконтрольных сущностей)
  // для интерфейса существуют функции собственно интерфейса (наклир)
  // для разных игр разные АПИ функции

  if (container && container->in) input::poll_events();

  if (
    container &&
    container->sound_devices_requested &&
    !container->sound_devices_logged &&
    container->sound_devices_ready.load(std::memory_order_acquire)
  ) {
    utils::info("main: sound playback devices count {}", container->sound_devices.size());
    for (size_t i = 0; i < container->sound_devices.size(); ++i) {
      utils::info("main: sound device[{}] '{}'", i, container->sound_devices[i]);
    }
    container->sound_devices_logged = true;
  }

  // (бывший тест test.mp3 удалён: test.mp3 больше нет, звук теперь грузится именованным набором
  //  и играется по событиям через мост sim→sound ниже + из UI в фазе D-UI.)

  // атлас шрифта доехал на GPU: фиксируем слот в шрифте (nuklear зашьёт его в texture.id
  // draw-команд текста; шейдер UI по нему сэмплит атлас). gpu_index записан рендером.
  if (container && container->ui_atlas && !container->ui_atlas_logged && container->ui_atlas->usable()) {
    const uint32_t slot = container->ui_atlas->gpu_index;
    if (container->ui_font) container->ui_font->set_texture_id(slot);
    utils::info("main: font atlas reached HOT, texture slot={}", slot);
    container->ui_atlas_logged = true;
  }

  if (container) {
    dispatcher_consume(container->chunk_loaded_commands, [this] (const auto& cmd) {
      tile_chunk chunk;
      chunk.coord = chunk_coord{cmd.x, cmd.y};
      chunk.size = cmd.size;
      chunk.tiles.resize(cmd.textures.size());
      for (size_t i = 0; i < cmd.textures.size(); ++i) chunk.tiles[i].texture = cmd.textures[i];

      apply_chunk(container->grid, chunk);

      if (cmd.x >= 0 && cmd.y >= 0 && uint32_t(cmd.x) < container->chunks_x && uint32_t(cmd.y) < container->chunks_y) {
        const size_t idx = size_t(cmd.y) * container->chunks_x + size_t(cmd.x);
        if (!container->chunks_loaded[idx]) {
          container->chunks_loaded[idx] = true;
          container->chunks_loaded_count += 1;
        }
      }
    });

    if (!container->chunks_logged && container->chunks_loaded_count == container->chunks_loaded.size()) {
      utils::info("main: all {} mock world chunks loaded", container->chunks_loaded_count);
      container->chunks_logged = true;
    }
  }

  // --- пайплайн тайловой карты: фрустум камеры -> срез сетки -> инстансы -> сообщение ---
  if (container && container->batch.valid()) {
    const tile_span span = visible_tiles(container->cam, container->grid, 1.0f);
    container->batch.build(container->grid, span);

    // собираем сообщение в рендер: метаданные + упакованные байты инстансов ("v2ui1").
    command_draw_tiles msg;
    const glm::mat4 vp = container->cam.view_proj();
    std::memcpy(msg.view_proj.data(), &vp[0][0], sizeof(float) * 16);

    // Контракт записи в буфер: шлём общий UBO (view_proj + ui_proj + misc) в host-visible
    // camera_buffer. ui_proj — ortho «пиксели окна -> clip» (Vulkan: y вниз, начало слева-сверху),
    // как старый матрикс nuklear-конвертера. screen_size/px_range кладём в misc.
    if (gactor != nullptr) {
      const float w = float(std::max(container->config.window.width, 1u));
      const float h = float(std::max(container->config.window.height, 1u));

      global_ubo_t ubo{};
      ubo.view_proj = vp;
      ubo.ui_proj = glm::mat4(1.0f);
      ubo.ui_proj[0][0] = 2.0f / w;
      ubo.ui_proj[1][1] = 2.0f / h;
      ubo.ui_proj[2][2] = -1.0f;
      ubo.ui_proj[3][0] = -1.0f;
      ubo.ui_proj[3][1] = -1.0f;
      ubo.misc = glm::vec4(w, h, 4.0f /* sdf px_range, = font_atlas_packer pixel_range */, 0.0f);

      command_write_buffer cam;
      cam.buffer = "camera_buffer";
      cam.bytes.resize(sizeof(global_ubo_t));
      std::memcpy(cam.bytes.data(), &ubo, sizeof(global_ubo_t));
      gactor->send(cam);
    }

    msg.count = container->batch.count();
    msg.stride = tile_batch::stride();
    msg.bytes.resize(size_t(msg.count) * msg.stride);
    container->batch.blit(std::span<uint8_t>(msg.bytes));

    if (!container->tiles_logged) {
      utils::info(
        "main: tile slice [{},{})x[{},{}) = {} instances, {} B/inst, {} B payload",
        span.x0, span.x1, span.y0, span.y1, msg.count, msg.stride, msg.bytes.size());
      container->tiles_logged = true;
    }

    if (gactor != nullptr) gactor->send(std::move(msg));
  }

  // --- actor simulation slice: simple AI -> move intents -> aesthetics components -> GPU batch ---
  if (container && container->actors_batch.valid()) {
    const auto t0 = std::chrono::steady_clock::now();
    container->actors_last_metrics = container->actors.update(
      float(time) / float(utils::global_time_resolution),
      container->actors_batch,
      *container->pool
    );
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t update_us = uint64_t(std::max<int64_t>(utils::count_mcs(t0, t1), 0));

    command_draw_actors msg;
    msg.count = container->actors_batch.count();
    msg.stride = actor_batch::stride();
    msg.sim_frame_time = time;
    msg.bytes.resize(size_t(msg.count) * msg.stride);
    container->actors_batch.blit(std::span<uint8_t>(msg.bytes));
    msg.ids.assign(container->actors_batch.ids().begin(), container->actors_batch.ids().end());

    if (!container->actors_logged) {
      utils::info(
        "main: actor slice {} actors, {} intents, {} instances, {} B/inst, {} B payload",
        container->actors_last_metrics.actors,
        container->actors_last_metrics.intents,
        msg.count,
        msg.stride,
        msg.bytes.size()
      );
      container->actors_logged = true;
    }

    if (gactor != nullptr) gactor->send(std::move(msg));

    // презентационный мост sim→sound: эмиты звука (вход в состояние FSM) → звуковой актор.
    // Куллинг по близости к слушателю (камере) + кап на тик (ограничение голосов). Звук
    // эфемерен и НЕ реплицируется — здесь решается лишь что РЕАЛЬНО проиграть.
    if (sactor != nullptr) {
      const auto emits = container->actors.sound_events();
      const glm::vec2 listener = container->cam.center;
      const float audible = container->cam.half_width * 1.5f;
      const float audible2 = audible * audible;
      constexpr uint32_t max_sounds_per_tick = 8;
      uint32_t sent = 0;
      for (const auto& e : emits) {
        if (sent >= max_sounds_per_tick) break;
        const glm::vec2 d = e.pos - listener;
        if (d.x * d.x + d.y * d.y > audible2) continue;
        command_sound_play play{};
        play.taskid = generate_task_id();
        play.after = SIZE_MAX; // без секвенсинга
        play.name = e.name;
        play.start = 0.0;
        sactor->send(play);
        ++sent;
      }
    }

    container->metrics_frames += 1;
    container->metrics_actor_ticks += 1;
    container->metrics_intents += container->actors_last_metrics.intents;
    container->metrics_instances += container->actors_last_metrics.instances;
    container->metrics_actor_update_us += update_us;

    if (container->config.metrics.enabled) {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - container->metrics_last_log).count();
      if (elapsed_ms >= container->config.metrics.log_interval_ms && container->metrics_frames != 0) {
        const double seconds = double(elapsed_ms) / 1000.0;
        const double fps = double(container->metrics_frames) / seconds;
        const double intent_rate = double(container->metrics_intents) / seconds;
        const double instance_rate = double(container->metrics_instances) / seconds;
        const double avg_actor_us = double(container->metrics_actor_update_us) / double(container->metrics_actor_ticks);
        container->ui_main_fps = fps;
        container->ui_intents_per_sec = intent_rate;
        container->ui_instances_per_sec = instance_rate;
        container->ui_actor_update_avg_us = avg_actor_us;
        utils::info(
          "metrics: main_fps={:.1f}, actors={}, intents/s={:.0f}, actor_instances/s={:.0f}, actor_update_avg_us={:.1f}",
          fps,
          container->actors_last_metrics.actors,
          intent_rate,
          instance_rate,
          avg_actor_us
        );

        container->metrics_last_log = now;
        container->metrics_frames = 0;
        container->metrics_actor_ticks = 0;
        container->metrics_intents = 0;
        container->metrics_instances = 0;
        container->metrics_actor_update_us = 0;
      }
    }
  }

  // --- интерфейс (visage): главный поток раздаёт ввод, строит UI и гонит nk_convert в буферы ---
  // Порядок строгий: input() -> update() -> convert(). Отправку буферов в рендер (через
  // command_update_ui + шаг draw_ui) подключим на шаге 4; пока только производим и логируем.
  if (container && container->ui) {
    // собираем снапшот ввода: позиция мыши — опросом, остальное — из аккумулятора коллбэков
    visage::input_snapshot_t snap;
    if (container->window != nullptr) {
      const auto [mx, my] = input::cursor_pos(container->window);
      snap.mouse_x = float(mx);
      snap.mouse_y = float(my);
    }
    snap.mouse_left = g_ui_input.mouse_left;
    snap.mouse_middle = g_ui_input.mouse_middle;
    snap.mouse_right = g_ui_input.mouse_right;
    snap.scroll_x = g_ui_input.scroll_x;
    snap.scroll_y = g_ui_input.scroll_y;
    snap.text = g_ui_input.text.data();
    snap.text_count = g_ui_input.text.size();
    snap.key_shift = g_ui_input.shift;
    snap.key_ctrl = g_ui_input.ctrl;
    snap.key_backspace = g_ui_input.backspace;
    snap.key_delete = g_ui_input.del;
    snap.key_enter = g_ui_input.enter;
    snap.key_tab = g_ui_input.tab;
    snap.key_left = g_ui_input.left;
    snap.key_right = g_ui_input.right;
    snap.key_up = g_ui_input.up;
    snap.key_down = g_ui_input.down;

    container->ui->input(snap); // читает snap.text — должен отработать до clear ниже
    container->ui->set_env_number("tf_main_fps", container->ui_main_fps);
    container->ui->set_env_number("tf_actor_count", double(container->actors_last_metrics.actors));
    container->ui->set_env_number("tf_actor_intents", double(container->actors_last_metrics.intents));
    container->ui->set_env_number("tf_actor_instances", double(container->actors_last_metrics.instances));
    container->ui->set_env_number("tf_actor_ticks", double(container->actors_last_metrics.ticks));
    container->ui->set_env_number("tf_intents_per_sec", container->ui_intents_per_sec);
    container->ui->set_env_number("tf_instances_per_sec", container->ui_instances_per_sec);
    container->ui->set_env_number("tf_actor_update_avg_us", container->ui_actor_update_avg_us);

    // забираем последний слепок состояния звука (звук пушит его сам) и СЛИВАЕМ его с
    // оптимистичными записями из app.play_sound: публикация — подтверждённые (deadline 0),
    // плюс ещё-живые оптимистичные, которых публикация пока не знает (latency старта).
    // Подтверждённая запись, исчезнувшая из публикации = звук закончился → выпадает сама.
    container->sound_frame += 1;
    dispatcher_consume_last(container->sound_state_commands, [this](command_sound_state& msg) {
      auto& cur = container->sound_state;
      auto& next = container->sound_state_next;
      next.clear();
      for (const auto& s : msg.sounds) next.push_back({s.taskid, s.progress, 0});
      for (const auto& e : cur) {
        if (e.deadline == 0) continue;                     // была подтверждена, но публикации больше нет → конец
        if (e.deadline < container->sound_frame) continue; // окно старта вышло → считаем завершённой
        bool in_pub = false;
        for (const auto& s : msg.sounds) if (s.taskid == e.taskid) { in_pub = true; break; }
        if (!in_pub) next.push_back(e); // оптимистичная, ещё не доехала — оставляем
      }
      std::swap(cur, next);
    });

    container->ui->update(time);
    container->ui->convert();

    // потребляем накопленные за кадр события (уровневые состояния кнопок/клавиш оставляем)
    g_ui_input.scroll_x = 0.0f;
    g_ui_input.scroll_y = 0.0f;
    g_ui_input.text.clear();

    // отправляем буферы UI в рендер (контракт записи в host-visible ресурсы). Шаг draw_ui
    // забиндит ui_vertices/ui_indices и проитерирует ui_commands. Буфер команд самоописывающийся:
    // [uint32 count][gui_draw_command_t...].
    if (gactor != nullptr) {
      const auto verts = container->ui->vertices();
      const auto inds = container->ui->indices();
      const auto cmds = container->ui->commands();

      command_write_buffer wv;
      wv.buffer = "ui_vertices";
      wv.bytes.assign(verts.begin(), verts.end());
      gactor->send(wv);

      command_write_buffer wi;
      wi.buffer = "ui_indices";
      wi.bytes.assign(inds.begin(), inds.end());
      gactor->send(wi);

      command_write_buffer wc;
      wc.buffer = "ui_commands";
      const uint32_t count = uint32_t(cmds.size());
      const size_t body = cmds.size() * sizeof(visage::gui_draw_command_t);
      wc.bytes.resize(sizeof(uint32_t) + body);
      std::memcpy(wc.bytes.data(), &count, sizeof(uint32_t));
      if (body != 0) std::memcpy(wc.bytes.data() + sizeof(uint32_t), cmds.data(), body);
      gactor->send(wc);
    }

    if (!container->ui_logged) {
      utils::info("visage: ui buffers — {} vtx bytes, {} idx bytes, {} draw commands",
        container->ui->vertices().size(), container->ui->indices().size(), container->ui->commands().size());
      container->ui_logged = true;
    }
  }

  // app.send_event требует функцию которая
  // получит тип системы и вернет id
  // этот id передается в системы и используется потом чтобы понять что происходит
}

}
}
