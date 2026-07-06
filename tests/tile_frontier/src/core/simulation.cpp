#include "simulation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <thread>
#include <stop_token>
#include <unistd.h>

#include <lua.hpp> // lua_gc — метрика памяти lua в memory-probe

#include <devils_engine/input/core.h>
#include <devils_engine/input/events.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/time-utils.hpp>
#include <devils_engine/utils/prng.h>

#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/demiurg/module_system.h>

#include <gtl/phmap.hpp>
#include <devils_engine/sound/sound_resource.h>

#include <devils_engine/painter/render_config_source.h>
#include <devils_engine/painter/glsl_source_file.h>
#include <devils_engine/painter/shader_source_file.h>
#include <devils_engine/painter/pipeline_cache_resource.h>
#include <devils_engine/catalogue/introspection.h> // catalogue::statistics_store (perf UI)
#include <devils_engine/catalogue/logging.h>        // доменное логгирование (DE_LOG) + init_logging
#include <devils_engine/utils/fileio.h>

#include <filesystem>

#include <devils_engine/visage/system.h>
#include <devils_engine/visage/font.h>
#include <devils_engine/visage/image.h>
#include <devils_engine/visage/font_atlas_packer.h>

#include "config.h"

#include "messages.h"
#include "broker.h"
#include "sound_system.h"
#include "render_system.h"
#include "assets_system.h"
#include <devils_engine/painter/gpu_texture_resource.h>
#include "app_config_resource.h"
#include <devils_engine/visage/font_resource.h>
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
// Настройка логгирования из конфига: файловый сток + консоль + уровни доменов. Зовётся сразу
// после загрузки app.tavl, ДО создания подсистем — чтобы базовые always-on сообщения (устройства,
// подсистемы, окно) тоже попали в файл. Домены по умолчанию off; включаются здесь или в рантайме.
static void setup_logging(const logging_config& log_cfg) {
  std::string file;
  if (!log_cfg.file.empty()) {
    file = utils::project_folder() + log_cfg.file;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
  }
  catalogue::init_logging(file, log_cfg.console);
  catalogue::register_engine_domains(); 

  const auto apply = [] (const uint32_t id, const std::string& depth_str) {
    catalogue::log_depth d = catalogue::log_depth::off;
    if (catalogue::parse_log_depth(depth_str, d)) catalogue::logs().set_level(id, d);
    else utils::warn("logging: unknown depth '{}' for domain '{}'", depth_str, catalogue::logs().name(id));
  };
  namespace ld = catalogue::log_domain;
  apply(ld::main, log_cfg.main);
  apply(ld::assets, log_cfg.assets);
  apply(ld::sound, log_cfg.sound);
  apply(ld::render, log_cfg.render);
  apply(ld::ui, log_cfg.ui);
  apply(ld::gameplay, log_cfg.gameplay);
  apply(ld::resource, log_cfg.resource);
  apply(ld::demiurg, log_cfg.demiurg);
}

// Резидентная память процесса (RSS) из /proc/self/statm (2-е поле = резидентные страницы).
// Для аудита бюджета памяти — грубо, но без сторонних тулов.
static size_t read_rss_bytes() {
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr) return 0;
  long total = 0, resident = 0;
  const int got = std::fscanf(f, "%ld %ld", &total, &resident);
  std::fclose(f);
  if (got != 2) return 0;
  return size_t(resident) * size_t(::sysconf(_SC_PAGESIZE));
}

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
  // кнопки мыши как first-class действия input::events (GLFW press=1/release=0 == key_state)
  input::events::update_mouse_button(button, action);
}

void ui_scroll_cb(GLFWwindow*, double x, double y) noexcept {
  g_ui_input.scroll_x += float(x);
  g_ui_input.scroll_y += float(y);
}

void ui_char_cb(GLFWwindow*, unsigned int codepoint) noexcept {
  g_ui_input.text.push_back(uint32_t(codepoint));
}

void ui_key_cb(GLFWwindow*, int key, int scancode, int action, int mods) noexcept {
  const bool down = (action != glfw_const::release);
  g_ui_input.shift = (mods & glfw_const::mod_shift) != 0;
  g_ui_input.ctrl  = (mods & glfw_const::mod_control) != 0;
  // именованные действия (шаг 2d): runtime-состояние клавиш в input::events кейится сканкодом.
  // GLFW action (0/1/2) совпадает с input::key_state (release/press/repeated).
  input::events::update_key(scancode, action);
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

// --- оконные события (шаг 1b) ---
// Тот же паттерн, что g_ui_input: GLFW-коллбэки без захватов срабатывают на главном потоке в
// input::poll_events, поэтому копим их в file-local структуру без атомиков (одно окно/один main).
// update() разбирает накопленное: ресайз → пересоздание свопчейна + проекция; фокус/сворачивание
// → реакции по window_policy. Сворачивание трактуем как потерю фокуса (active = focused && !iconified).
struct window_events_t {
  bool resized = false;
  uint32_t fb_w = 0, fb_h = 0;
  bool focused = true;
  bool iconified = false;
  bool state_changed = false; // фокус/сворачивание изменились с прошлого разбора
};
window_events_t g_window_events;

void window_framebuffer_size_cb(GLFWwindow*, int w, int h) noexcept {
  g_window_events.fb_w = w < 0 ? 0u : uint32_t(w);
  g_window_events.fb_h = h < 0 ? 0u : uint32_t(h);
  g_window_events.resized = true;
}

void window_focus_cb(GLFWwindow*, int focused) noexcept {
  g_window_events.focused = (focused != 0);
  g_window_events.state_changed = true;
}

void window_iconify_cb(GLFWwindow*, int iconified) noexcept {
  g_window_events.iconified = (iconified != 0);
  g_window_events.state_changed = true;
}
} // namespace

// Политика реакций окна — крутится в C++ (см. план, шаг 1b). Не в фокусе → по умолчанию всё ещё
// рисуем (частичная видимость), но глушим звук; свёрнутое окно трактуем как потерю фокуса и по
// умолчанию НЕ рисуем (нет смысла гонять рендер-граф вхолостую, к тому же свопчейн 0×0).
struct window_policy {
  bool draw_when_unfocused = true;
  bool draw_when_minimized = false;
  bool mute_when_unfocused = true;
  float focused_master_gain = 1.0f;
  float unfocused_master_gain = 0.0f;
};

// FSM состояний движка (шаг 3). Лёгкий выделенный автомат (не mood — тот per-entity/act):
//   boot    — движок только поднялся; поверх экрана splash (UI), карта/акторы НЕ рисуются;
//   loading — ассеты грузят стартовый набор; на экране прогресс (usable()-ресурсов);
//   game    — стартовый набор usable(); публикуем тайлы/акторов, splash снят, картинка без миганий.
// Переход boot→loading — сразу (потоки/граф уже стартовали в init); loading→game — когда весь
// стартовый набор ресурсов usable() и mock-чанки применены. Синглтон-состояние приложения.
enum class app_state { boot, loading, game };

// тут что? все другие системы + потоки для них + тред пул
// кеш?
struct simulation_init {
  app_config config;

  // Движковый (незаменяемый) реестр ресурсов: config/shaders/render-graph. Отдельный
  // resource_system, изолированный от игрового (моды живут в assets). См. demiurg 1a (Q2).
  std::unique_ptr<demiurg::module_system> engine_modules;
  std::unique_ptr<demiurg::resource_system> engine_resources;

  // Pipeline cache (Фаза 2): ОТДЕЛЬНЫЙ demiurg-модуль над writable cache-папкой (не бандлится).
  // Ресурсы модуля дописываются в общий engine_resources (append_resources) — своего resource_system
  // у кэша нет. Модуль держим живым здесь: pipeline_cache_resource::load_cold читает через него.
  std::unique_ptr<demiurg::module_system> cache_modules;

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

  // Живой размер фреймбуфера (в пикселях). Инициализируется из config, обновляется коллбэком ресайза.
  // Проекция (view_proj/ui_proj/misc) и cam.aspect считаются ОТ НЕГО, а не от статичного config
  // (иначе картинка искажается на любом реальном размере окна — это был баг).
  uint32_t fb_width = 1;
  uint32_t fb_height = 1;
  window_policy policy;
  bool window_active = true; // focused && !iconified — последнее применённое состояние

  // сохранённый оконный прямоугольник для возврата из фуллскрина (glfwSetWindowMonitor)
  int32_t windowed_x = 0, windowed_y = 0;
  uint32_t windowed_w = 0, windowed_h = 0;
  bool is_fullscreen = false;

  std::unique_ptr<input::init> in;

  // интерфейс (visage) живёт в главном потоке. Метрики глифов (font_t) и байты атласа теперь
  // держит font_resource (многошаговый ресурс ttf->MSDF->GPU); visage::system заимствует font().
  std::unique_ptr<visage::system> ui;
  bool ui_logged = false;

  // prng-состояние UI (visage): 256-бит поток (xoshiro256starstar), продвигается каждый кадр;
  // value() уходит в ui->update как rng_state-сид. Отвязывает случайность UI от реальной математики.
  // ui_timestamp — монотонная метка времени (пока с нуля) для фиксации начала UI-анимаций.
  utils::xoshiro256starstar::state ui_rng = utils::xoshiro256starstar::init(utils::string_hash("visage_ui"));
  uint64_t ui_timestamp = 0;

  // шрифт как многошаговый ресурс: CPU-шаги (ttf/MSDF) — синхронно в setup_visage, GPU — асинхронно
  std::unique_ptr<visage::font_resource> ui_font_res;
  bool ui_font_logged = false;
  // второй именованный шрифт (шаг 2b) — тот же путь, регистрируется в visage как "italic"
  std::unique_ptr<visage::font_resource> ui_font_italic_res;
  bool ui_font_italic_logged = false;

  // Звуки — demiurg-ресурсы в потоке ассетов. main держит name_hash → stable handle (для резолва в
  // command_sound_play из UI/геймплея) и запрашивает их до warm. Сам звук-актор ресурсы не хранит.
  gtl::flat_hash_map<uint64_t, demiurg::resource_handle> sound_by_name;

  // Именованные картинки для UI: name_hash → stable handle текстуры (gpu_index + размер). Хост-мост к demiurg:
  // app.image(name) строит visage::image из usable()-текстуры. Позже заменится на demiurg require.
  gtl::flat_hash_map<uint64_t, demiurg::resource_handle> image_by_name;

  // Единый broker всех межпоточных каналов. Владелец — main; создаётся в init ДО подсистем и
  // раздаётся им указателем (set_broker) до старта потоков. Заменяет actor_ref/message_dispatcher.
  std::unique_ptr<broker> br;

  // модель тайловой карты (главная сторона)
  texture_set textures;     // текстуры карты, собранные по префиксу пути
  tile_grid grid;           // квадратная сетка тайлов
  uint32_t chunk_size = 16;
  uint32_t chunks_x = 4;
  uint32_t chunks_y = 4;
  std::vector<bool> chunks_requested;
  std::vector<bool> chunks_loaded;
  uint32_t chunks_loaded_count = 0;
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

  // FSM состояний движка (шаг 3). Стартовый набор ресурсов, от usable() которого зависит переход
  // loading→game (main держит stable handles, поэтому прогресс считается прямо тут, без публикации
  // из ассетов). Заполняется в init по мере запросов текстур/шрифтов.
  app_state state = app_state::boot;
  std::vector<resource_ref> startup_resources;

  std::vector<std::string> sound_devices;
  std::atomic_bool sound_devices_ready = false;
  bool sound_devices_requested = false;
  bool sound_devices_logged = false;
  bool sound_recreate_test_sent = false;
  size_t tick = 0;

  // Единая таблица состояния звука для UI (app.sound_state). Звуковой поток ПУШИТ полный слепок
  // (command_sound_state, latest-wins мейлбокс); consume в update СЛИВАЕТ его с оптимистичными
  // записями, добавленными в app.play_sound (см. ui_sound_state_entry.deadline).
  std::vector<ui_sound_state_entry> sound_state;
  std::vector<ui_sound_state_entry> sound_state_next;
  size_t sound_frame = 0; // счётчик main-кадров (для дедлайна оптимистичных записей)

  simulation_init() : pool(nullptr), window(nullptr), monitor(nullptr) {}
};

static void create_window_and_notify_render(simulation_init& c) {
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

  // оконные события: ресайз (в пикселях фреймбуфера), фокус, сворачивание (шаг 1b).
  input::set_framebuffer_size_callback(c.window, &window_framebuffer_size_cb);
  input::set_window_focus_callback(c.window, &window_focus_cb);
  input::set_window_iconify_callback(c.window, &window_iconify_cb);

  // именованные действия (шаг 2d): input::events — слой абстракции «клавиша → действие».
  // GLFW уже инициализирован (in создан), поэтому key_from_canonical даёт валидные сканкоды.
  input::events::init();
  input::events::set_engine_tick_time(main_frame_time);
  input::events::set_long_press_duration(utils::round(double(utils::global_time_resolution) * 0.3));
  input::events::set_double_press_duration(utils::round(double(utils::global_time_resolution) * 0.25));
  const auto bind_action = [](const char* action, const char* canonical) {
    const auto [glfw_key, scancode] = input::key_from_canonical(canonical);
    if (scancode >= 0) input::events::set_key(std::string_view(action), scancode, glfw_key, 0);
    else utils::warn("main: could not bind action '{}' to key '{}'", action, canonical);
  };
  bind_action("quit", "escape");       // Esc → действие quit (lua решает, что делать)
  bind_action("toggle_menu", "f1");    // F1 → переключить меню

  // синхронизируем стартовое состояние с реальным окном (коллбэки могли ещё не сработать)
  {
    const auto [fw, fh] = input::framebuffer_size(c.window);
    if (fw != 0 && fh != 0) { c.fb_width = fw; c.fb_height = fh; }
    g_window_events.fb_w = c.fb_width;
    g_window_events.fb_h = c.fb_height;
    g_window_events.focused = input::window_focused(c.window);
    g_window_events.iconified = input::window_iconified(c.window);
    c.window_active = g_window_events.focused && !g_window_events.iconified;
  }

  if (c.br) {
    c.br->window_recreation.write_slot() = command_window_recreation{
      c.window, c.monitor, c.config.window.width, c.config.window.height
    };
    c.br->window_recreation.publish();
  }
}

// Полноэкранный режим / возврат в окно (шаг 1f). glfwSetWindowMonitor. При смене режима GLFW
// сам пришлёт framebuffer_size → штатный путь ресайза (1c) пересоздаст свопчейн под новый размер.
static void apply_fullscreen(simulation_init& c, const bool enable) {
  if (c.window == nullptr) return;
  if (enable && !c.is_fullscreen) {
    const auto [x, y] = input::window_pos(c.window);
    const auto [w, h] = input::window_size(c.window);
    c.windowed_x = x; c.windowed_y = y; c.windowed_w = w; c.windowed_h = h;
    GLFWmonitor* m = c.monitor != nullptr ? c.monitor : input::primary_monitor();
    if (m == nullptr) { utils::warn("main: no monitor for fullscreen"); return; }
    const auto [mw, mh, refresh] = input::primary_video_mode(m);
    input::set_window_monitor(c.window, m, 0, 0, mw, mh, int32_t(refresh));
    c.is_fullscreen = true;
    DE_LOG(catalogue::log_domain::main, flow, "main: fullscreen on ({}x{}@{})", mw, mh, refresh);
  } else if (!enable && c.is_fullscreen) {
    const uint32_t w = c.windowed_w != 0 ? c.windowed_w : c.config.window.width;
    const uint32_t h = c.windowed_h != 0 ? c.windowed_h : c.config.window.height;
    input::set_window_monitor(c.window, nullptr, c.windowed_x, c.windowed_y, w, h, DEVILS_ENGINE_INPUT_DONT_CARE);
    c.is_fullscreen = false;
    DE_LOG(catalogue::log_domain::main, flow, "main: fullscreen off ({}x{})", w, h);
  }
}

// Прогресс загрузки стартового набора [0,1]: доля usable()-ресурсов + применённых mock-чанков.
// main держит resource_ref на стартовый набор, поэтому прогресс считается локально (без публикации
// из ассетов — см. водораздел; ассетный push-слепок оставлен на будущее).
static double loading_progress(const simulation_init& c) {
  const size_t total = c.startup_resources.size() + c.chunks_loaded.size();
  if (total == 0) return 1.0;
  size_t done = c.chunks_loaded_count;
  for (const auto& ref : c.startup_resources) {
    auto* r = ref.get();
    if (r != nullptr && r->usable()) ++done;
  }
  return double(done) / double(total);
}

// «Загрузка завершена» = ВЕСЬ стартовый набор ресурсов usable() И все mock-чанки применены.
static bool loading_complete(const simulation_init& c) {
  for (const auto& ref : c.startup_resources) {
    auto* r = ref.get();
    if (r == nullptr || !r->usable()) return false;
  }
  return c.chunks_loaded_count == c.chunks_loaded.size();
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

// Поднимаем интерфейс в главном потоке. Шрифт — многошаговый ресурс (font_resource):
// CPU-шаги ttf→MSDF (уровни 0→1→2) гоняем ЗДЕСЬ синхронно, т.к. visage::system нужны метрики
// глифов сразу для nk_convert; GPU-заливка (2→3) уйдёт штатным асинхронным путём ассетов из init().
// Это переносит генерацию MSDF из ad-hoc кода в FSM-шаг ресурса (demiurg 1a срез 3, первый 4-state).
static void setup_visage(simulation_init& c) {
  c.ui_font_res = std::make_unique<visage::font_resource>(utils::project_folder() + "resources/fonts/crimson.roman.ttf");
  c.ui_font_res->load(utils::safe_handle_t{}); // 0→1: читаем ttf (CPU, главный поток)
  c.ui_font_res->load(utils::safe_handle_t{}); // 1→2: MSDF-атлас + метрики глифов (CPU)

  auto* font = c.ui_font_res->font();
  if (font == nullptr) utils::error{}("visage: font_resource produced no font metrics");

  c.ui.reset(new visage::system(font)); // visage заимствует метрики; байты атласа ждут GPU-шага

  // Второй именованный шрифт (шаг 2b): тот же многошаговый ресурс, CPU-шаги синхронно, GPU — асинхронно.
  // Регистрируем в visage под именем "italic"; lua выбирает его в nk.push_font{ font="italic" }.
  c.ui_font_italic_res = std::make_unique<visage::font_resource>(utils::project_folder() + "resources/fonts/crimson.italic.ttf");
  c.ui_font_italic_res->load(utils::safe_handle_t{}); // 0→1: ttf
  c.ui_font_italic_res->load(utils::safe_handle_t{}); // 1→2: MSDF + метрики
  if (auto* italic = c.ui_font_italic_res->font()) {
    c.ui->add_font("italic", italic);
    DE_LOG(catalogue::log_domain::ui, flow, "visage: registered extra font 'italic'");
  } else {
    utils::warn("visage: italic font_resource produced no metrics; skipping");
    c.ui_font_italic_res.reset();
  }

  c.ui->load_entry_point(utils::project_folder() + "resources/ui/entry.lua");
  DE_LOG(catalogue::log_domain::ui, flow, "visage: system created, entry point loaded");
}

void simulation::init() {
  container.reset(new simulation_init);
  // Единый broker создаётся ДО подсистем; раздаётся каждой (set_broker) до старта их потоков.
  container->br = std::make_unique<broker>();

  // Движковый реестр: engine-module = папка resources/engine/ (не перебивается модами).
  // Новый demiurg-API не нужен — module_system::load_modules умеет директорию как модуль.
  container->engine_resources = std::make_unique<demiurg::resource_system>();
  container->engine_resources->register_type<app_config_resource>("config", "tavl");
  container->engine_resources->register_type<painter::render_config_source>("render_config", "tavl");
  // Шейдеры (Фаза 1): GLSL под папкой shaders/ (тип "shaders"), SPIR-V под shaders/spv/
  // (тип "spv" — сегмент пути). Компиляция/загрузка выбирается по расширению в create_pipeline.
  container->engine_resources->register_type<painter::glsl_source_file>("shaders", "glsl");
  container->engine_resources->register_type<painter::shader_source_file>("spv", "spv");
  // Pipeline cache (Фаза 2) живёт в ТОМ ЖЕ движковом реестре — отдельный resource_system не нужен.
  // Его МОДУЛЬ (отдельная cache-папка) дописывается в engine_resources позже (append_resources),
  // когда из конфига известен cache_folder. Тип регистрируем сразу.
  container->engine_resources->register_type<painter::pipeline_cache_resource>("pipeline_cache", "bin");
  container->engine_modules = std::make_unique<demiurg::module_system>(utils::project_folder() + "resources/");
  container->engine_modules->load_modules({ demiurg::module_system::list_entry{"engine", "", ""} });
  container->engine_resources->parse_resources(container->engine_modules.get());

  // Доводим все файлы описания render-graph до warm (текст в памяти) ЗДЕСЬ, на главном
  // потоке до старта рендера — дальше поток рендера читает их только на чтение (parse_data).
  {
    std::vector<painter::render_config_source*> rc;
    container->engine_resources->find<painter::render_config_source>("render_config", rc);
    for (auto* r : rc) r->load(utils::safe_handle_t{});
    DE_LOG(catalogue::log_domain::resource, flow, "engine registry: preloaded {} render-config sources", rc.size());
  }

  // Шейдер-исходники НЕ preload'им здесь: их lifecycle («загрузить→скомпилировать→выгрузить»)
  // живёт в рендер-потоке вокруг сборки пайплайнов (см. render_system set_shader_sources_loaded, п.1).

  const auto config_path = utils::project_folder() + "resources/engine/config/app.tavl"; // для лога
  if (auto* cfg_res = container->engine_resources->get<app_config_resource>("config/app")) {
    cfg_res->load(utils::safe_handle_t{}); // cold→warm: читает + парсит tavl (CPU, главный поток)
    container->config = cfg_res->config();
  } else {
    utils::warn("engine registry: 'config/app' not found, using app_config defaults");
  }

  // логгирование настраиваем СРАЗУ после конфига (до подсистем): файл+консоль, уровни доменов.
  // Базовый always-on слой (utils::info ниже) уже попадёт в файл.
  setup_logging(container->config.logging);

  set_frame_time(frame_time_from_fps(container->config.simulation.main_fps));

  // стартовый размер фреймбуфера = размер из конфига (до создания окна); коллбэк ресайза уточнит.
  container->fb_width = std::max(container->config.window.width, 1u);
  container->fb_height = std::max(container->config.window.height, 1u);

  const uint32_t hw_threads = std::max(std::thread::hardware_concurrency(), 1u);
  // reserved считаем ДИНАМИЧЕСКИ: выключенный движковый поток (render/sound) освобождает ядро под
  // worker'ов (топологическая настройка — применяется при старте движка). См. ROADMAP A-4.
  uint32_t reserved_threads = container->config.simulation.worker_threads_reserved;
  if (!container->config.render.enabled && reserved_threads > 0) reserved_threads -= 1;
  if (!container->config.simulation.sound_enabled && reserved_threads > 0) reserved_threads -= 1;
  const uint32_t min_worker_threads = std::max(container->config.simulation.min_worker_threads, 1u);
  const uint32_t thread_count = std::max(
    hw_threads > reserved_threads ? hw_threads - reserved_threads : min_worker_threads,
    min_worker_threads
  );

  const auto cpu_name = utils::get_cpu_name();
  utils::info("Using cpu '{}', cores: {}, worker threads: {}", cpu_name, hw_threads, thread_count);
  DE_LOG(catalogue::log_domain::main, flow,
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

  // Звуковой поток — опциональная подсистема (sound_enabled). Если выключен, sound_sim не создаётся,
  // sactor остаётся nullptr (все обращения к звуку уже защищены проверкой sactor != nullptr).
  if (container->config.simulation.sound_enabled) {
    container->sound_sim.reset(new sound_simulation(sound_ft));
    container->sound_sim->init();
    container->sound_sim->set_broker(container->br.get());
    sactor = container->sound_sim->get_actor();
  } else {
    DE_LOG(catalogue::log_domain::main, flow, "main: sound disabled (sound_enabled=false), skipping sound subsystem");
  }

  if (container->config.render.enabled) {
    // Pipeline cache — ОТДЕЛЬНЫЙ demiurg-модуль над выделенной cache-подпапкой (Фаза 2). Раздаёт
    // кэш на load; dump пишет на диск напрямую — round-trip через ре-скан модуля на каждом init.
    // Модуль = <cache_folder>/painter/ (ИЗОЛИРОВАН: system_info кладёт main_device.tavl прямо в
    // <cache_folder>/ — не хотим, чтобы он попадал под скан). Файл = painter/pipeline_cache/main.bin,
    // id (относительно модуля) = "pipeline_cache/main", тип матчится на сегмент "pipeline_cache".
    const std::string cache_module = container->config.render.cache_folder + "/painter";
    const std::string pipeline_cache_id = "pipeline_cache/main";
    const std::string pipeline_cache_path = make_project_path(cache_module + "/pipeline_cache/main.bin");
    file_io::create_directory(make_project_path(container->config.render.cache_folder)); // <cache_folder>/
    file_io::create_directory(make_project_path(cache_module));                          // .../painter/
    file_io::create_directory(make_project_path(cache_module + "/pipeline_cache"));       // .../painter/pipeline_cache/
    // Cache-МОДУЛЬ отдельный (свой корень-папка), но его ресурсы дописываем в ОБЩИЙ engine_resources
    // (append_resources — без clear, id не пересекаются с движковыми). Отдельный resource_system не нужен.
    container->cache_modules = std::make_unique<demiurg::module_system>(utils::project_folder());
    container->cache_modules->load_modules({ demiurg::module_system::list_entry{cache_module, "", ""} });
    container->engine_resources->append_resources(container->cache_modules.get());
    if (auto* pc = container->engine_resources->get<painter::pipeline_cache_resource>(pipeline_cache_id)) {
      pc->load(utils::safe_handle_t{}); // cold→warm: читает блоб через модуль (CPU, главный поток)
      DE_LOG(catalogue::log_domain::resource, flow, "engine registry: pipeline cache '{}' preloaded", pipeline_cache_id);
    }

    render_simulation_config render_cfg;
    render_cfg.engine_registry = container->engine_resources.get();
    // config.config_folder ("render_config") — теперь ПРЕФИКС в движковом реестре, не путь на диске
    render_cfg.render_config_prefix = container->config.render.config_folder + "/";
    render_cfg.shader_config_prefix = container->config.render.shader_folder + "/";
    render_cfg.cache_registry = container->engine_resources.get(); // тот же реестр, что engine_registry
    render_cfg.pipeline_cache_id = pipeline_cache_id;
    render_cfg.pipeline_cache_path = pipeline_cache_path;
    render_cfg.graph_name = container->config.render.graph;
    render_cfg.menu_graph_name = container->config.render.menu_graph;
    render_cfg.headless = container->config.render.headless;
    render_cfg.create_vulkan_on_init = container->config.window.create_on_start || container->config.render.headless;

    if (container->config.window.create_on_start && !container->config.render.headless && !container->in) {
      container->in = std::make_unique<input::init>(&error_callback);
    }

    container->render_sim.reset(new render_simulation(render_ft, std::move(render_cfg)));
    container->render_sim->init();
    container->render_sim->set_broker(container->br.get()); // заодно триггерит попытку сборки графа
    gactor = container->render_sim->get_actor();
  }

  container->assets_sim.reset(new assets_simulation(assets_ft));
  container->assets_sim->init();
  container->assets_sim->set_broker(container->br.get());
  aactor = container->assets_sim->get_actor();

  const auto gap_divisor = container->config.simulation.thread_start_gap_divisor;
  const auto sound_gap = thread_start_gap(sound_ft, gap_divisor);
  const auto render_gap = thread_start_gap(render_ft, gap_divisor);
  const auto assets_gap = thread_start_gap(assets_ft, gap_divisor);

  // жлямбды берут std::stop_token: разрушение jthread кооперативно останавливает run (см. advancer::run)
  if (container->sound_sim) {
    container->sound_thread.reset(new std::jthread([sys = container->sound_sim.get(), sound_gap](std::stop_token st){ sys->run(st, sound_gap); }));
  }
  if (container->render_sim) {
    container->render_thread.reset(new std::jthread([sys = container->render_sim.get(), render_gap](std::stop_token st){ sys->run(st, render_gap); }));
  }
  container->assets_thread.reset(new std::jthread([sys = container->assets_sim.get(), assets_gap](std::stop_token st){ sys->run(st, assets_gap); }));

  if (sactor != nullptr) {
    command_sound_devices devices;
    devices.request_id = generate_task_id();
    devices.out = &container->sound_devices;
    devices.ready = &container->sound_devices_ready;
    container->br->sound_devices.try_push(devices);
    container->sound_devices_requested = true;
    DE_LOG(catalogue::log_domain::sound, flow, "main: requested sound playback devices");
  }

  // Окно - поздний ресурс. Render thread должен жить и без него, а это событие
  // может прийти сейчас, после загрузки ассетов или после полного пересоздания окна.
  if (container->config.window.create_on_start && container->render_sim && !container->config.render.headless) {
    create_window_and_notify_render(*container);
  }

  // три grass-текстуры → texture_slots 0,1,2 (порядок запроса = порядок слотов, т.к. assets грузит по очереди)
  // (friendly-имя для UI-картинок, id ресурса в реестре)
  const std::pair<const char*, const char*> textures_named[] = {
    { "grass",  "textures/grass" },
    { "grass1", "textures/grass1_0" },
    { "grass3", "textures/grass3" },
    { "grad1",  "textures/grad1" }, // градиент-маски + 4-цветная маска для стенсил-эффектов (Стадия 2)
    { "grad2",  "textures/grad2" },
    { "quad",   "textures/quad" },
  };
  for (const auto& [friendly, res_id] : textures_named) {
    const auto tex_handle = container->assets_sim->resources()->handle(res_id);
    if (auto* tex = tex_handle.get<painter::gpu_texture_resource>()) {
      (void)tex;
      container->br->load_resource.try_push(command_load_resource{resource_ref::from_handle(tex_handle), static_cast<int32_t>(demiurg::state::hot)});
      container->startup_resources.push_back(resource_ref::from_handle(tex_handle)); // стартовый набор: от него зависит переход loading→game
      container->image_by_name.emplace(utils::string_hash(friendly), tex_handle); // для app.image (UI)
      DE_LOG(catalogue::log_domain::resource, flow, "main: requested texture '{}' -> hot", res_id);
    } else {
      utils::warn("main: texture resource '{}' not found in registry", res_id);
    }
  }

  // Звуки: резолвим короткое имя → demiurg-ресурс (поток ассетов), запрашиваем до warm и держим
  // handle в sound_by_name. UI/геймплей ссылаются по string_hash(имя), звук-актор получит handle.
  {
    const std::pair<const char*, const char*> named[] = {
      { "eating",  "sounds/eating/freesound_community-chomp-chew-bite-102031" },
      { "fleeing", "sounds/fleeing/freesound_community-escaping-downstairs-104907" },
      { "walking", "sounds/walking/freesound_community-walking-46245" },
      { "ambient", "sounds/ambient/soundreality-ambient-spring-forest-323801" },
    };
    for (const auto& [name, res_id] : named) {
      const auto snd_handle = container->assets_sim->resources()->handle(res_id);
      auto* snd = snd_handle.get<sound::sound_resource>();
      if (snd == nullptr) { utils::warn("main: sound resource '{}' not found in registry", res_id); continue; }
      container->br->load_resource.try_push(command_load_resource{resource_ref::from_handle(snd_handle), static_cast<int32_t>(demiurg::state::warm)});
      container->sound_by_name.emplace(utils::string_hash(name), snd_handle);
    }
    DE_LOG(catalogue::log_domain::resource, flow, "main: requested {} sounds -> warm", container->sound_by_name.size());
  }

  // --- модель тайловой карты ---
  // Набор ТАЙЛОВЫХ текстур = только grass (маски grad/quad НЕ должны попадать в террейн — подстрока
  // "textures/grass" их исключает: 'textures/grad*'/'textures/quad' её не содержат). Позже маски уедут
  // в отдельный тип (textures/mask/).
  const uint32_t tex_count = container->textures.gather(*container->assets_sim->resources(), "textures/grass");
  DE_LOG(catalogue::log_domain::resource, flow, "main: gathered {} tile textures by 'textures/grass'", tex_count);

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
        container->br->load_chunk.try_push(cmd);
        container->chunks_requested[idx] = true;
      }
    }
    DE_LOG(catalogue::log_domain::gameplay, flow, "main: requested {} mock world chunks via assets", container->chunks_requested.size());
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
  DE_LOG(catalogue::log_domain::gameplay, flow, "main: spawned {} lightweight actors in aesthetics world", initial_actor_count);

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
        // резолв имя → demiurg-handle. Неизвестное имя — тихо.
        const auto snd_it = container->sound_by_name.find(utils::string_hash(name));
        if (snd_it == container->sound_by_name.end()) return sound_handle{};
        play.res = resource_ref::from_handle(snd_it->second);
        container->br->sound_play.try_push(play);
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
      container->br->sound_stop.try_push(stop);
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

    // --- общий API управления игрой (шаг 2a/1f/2e) ---
    // quit_game — единственная точка выхода из UI: выставляет флаг, читаемый в stop_predicate.
    app.set_function("quit_game", [this]() { quit_requested.store(true, std::memory_order_release); });
    app.set_function("maximize", [this]() { if (container->window != nullptr) input::maximize_window(container->window); });
    app.set_function("restore",  [this]() { if (container->window != nullptr) input::restore_window(container->window); });
    app.set_function("set_fullscreen", [this](bool enable) { apply_fullscreen(*container, enable); });
    app.set_function("is_fullscreen", [this]() -> bool { return container->is_fullscreen; });

    // мастер-громкость [0,1] напрямую (UI-ползунок). Отдельно от политики фокуса — это явная настройка.
    app.set_function("set_master_volume", [this](double v) {
      if (sactor == nullptr || container->br == nullptr) return;
      const float gain = float(std::clamp(v, 0.0, 1.0));
      container->policy.focused_master_gain = gain; // чтобы возврат фокуса не сбросил громкость
      container->br->sound_master_gain.try_push(command_sound_set_master_gain{gain});
    });

    // poke настроек → реконфигурация систем (шаг 2e, паттерн diff→message).
    // Смена разрешения: меняем окно; GLFW пришлёт framebuffer_size → штатный путь ресайза (1c).
    app.set_function("set_resolution", [this](int w, int h) {
      if (container->window == nullptr || w <= 0 || h <= 0) return;
      container->config.window.width = uint32_t(w);
      container->config.window.height = uint32_t(h);
      input::set_window_size(container->window, uint32_t(w), uint32_t(h));
    });
    // Смена звукового устройства: пере-создаём system2 через уже существующий канал recreate.
    app.set_function("set_sound_device", [this](const std::string& name) {
      if (sactor == nullptr || container->br == nullptr) return;
      container->br->recreate_sound.try_push(command_recreate_sound_system{name});
    });

    // именованные действия (шаг 2d): lua опрашивает состояние действия по имени. pressed — сейчас
    // нажато (press/long/double); clicked — завершённое нажатие в этом кадре (click/long/double_click).
    app.set_function("action_pressed", [](const std::string& name) -> bool {
      return input::events::check_event(std::string_view(name), input::event_state::press_mask);
    });
    app.set_function("action_clicked", [](const std::string& name) -> bool {
      return input::events::check_event(std::string_view(name), input::event_state::click_mask);
    });

    // картинка для UI (хост-мост к demiurg): app.image(name [, {region={x,y,w,h}}]) -> visage::image | nil.
    // Резолвит имя → gpu_texture_resource; строит хендл из gpu_index+размера когда текстура usable() (на GPU),
    // иначе nil. Позже заменится на demiurg require/request — сигнатура/возврат подобраны так, чтобы lua не менять.
    app.set_function("image", [this](const std::string& name, sol::optional<sol::table> opts) -> sol::object {
      auto& lua = container->ui->script_state();
      const auto it = container->image_by_name.find(utils::string_hash(name));
      if (it == container->image_by_name.end()) return sol::nil;
      auto* tex = it->second.get<painter::gpu_texture_resource>();
      if (tex == nullptr || !tex->usable()) return sol::nil; // ещё не на GPU
      visage::image img{};
      img.texture_id = tex->gpu_index;
      img.w = uint16_t(tex->width);
      img.h = uint16_t(tex->height);
      if (opts) {
        const sol::optional<sol::table> region = (*opts)["region"];
        if (region) for (int i = 0; i < 4; ++i) img.region[i] = uint16_t(region->get_or(i + 1, 0));
      }
      return sol::make_object(lua, img);
    });

    // состояние движка для UI (шаг 3a): lua рисует splash/loading/game по app.state(),
    // прогресс-бар — по app.loading_progress() [0,1].
    app.set_function("state", [this]() -> std::string {
      switch (container->state) {
        case app_state::boot:    return "boot";
        case app_state::loading: return "loading";
        case app_state::game:    return "game";
      }
      return "game";
    });
    app.set_function("loading_progress", [this]() -> double { return loading_progress(*container); });

    // рантайм-переключение глубины логгирования домена (работает и в release): app.set_log_level("sound","trace").
    // Домены: main/assets/sound/render/ui/gameplay/resource/demiurg; глубина: off/info/flow/trace.
    app.set_function("set_log_level", [](const std::string& domain, const std::string& depth) -> bool {
      catalogue::log_depth d = catalogue::log_depth::off;
      if (!catalogue::parse_log_depth(depth, d)) { utils::warn("set_log_level: bad depth '{}'", depth); return false; }
      if (!catalogue::logs().set_level(domain, d)) { utils::warn("set_log_level: unknown domain '{}'", domain); return false; }
      utils::info("log domain '{}' -> {}", domain, depth);
      return true;
    });

    // perf-статистика фаз апдейта актора (catalogue). Актор-сим и UI — один поток, читаем напрямую.
    // Возвращает массив { name, avg, min, max, last, count, samples={...} }; samples — последние
    // замеры в хроно-порядке (для nk.plot). Round-trip значений в lua при 20fps main дешёв (~1%),
    // отдельного C++-пути рисования не нужно.
    app.set_function("perf_stats", [this]() -> sol::table {
      auto& lua = container->ui->script_state();
      sol::table out = lua.create_table();
      std::vector<uint64_t> samples;
      int32_t i = 0;
      core::actor_perf_statistics().for_each(
        [&] (const catalogue::statistics_store::function_record& r) {
          sol::table e = lua.create_table();
          e["name"]  = std::string(r.name);
          e["avg"]   = r.average_mcs();
          e["min"]   = double(r.min_mcs);
          e["max"]   = double(r.max_mcs);
          e["last"]  = double(r.last_mcs);
          e["count"] = double(r.call_count);
          r.ordered_samples(samples);
          sol::table s = lua.create_table(int32_t(samples.size()), 0);
          for (size_t k = 0; k < samples.size(); ++k) s[k + 1] = double(samples[k]);
          e["samples"] = s;
          out[++i] = e;
        });
      return out;
    });
  }

  // атлас шрифта → GPU тем же путём, что и текстуры: просим ассеты довести до hot.
  // Слот определится динамически (после grass-текстур); main прочитает gpu_index по hot.
  if (container->ui_font_res && aactor != nullptr) {
    // final_state() = 3 (font_resource много-шаговый); CPU-уровни (0..2) уже пройдены в setup_visage,
    // ассетам остаётся довести 2→3 (GPU). target=final_state(), не state::hot (иначе стоп на MSDF).
    const resource_ref font_ref = resource_ref::from_direct(container->ui_font_res.get());
    container->br->load_resource.try_push(command_load_resource{font_ref, container->ui_font_res->final_state()});
    container->startup_resources.push_back(font_ref);
    DE_LOG(catalogue::log_domain::ui, flow, "main: requested font atlas -> ready (level {})", container->ui_font_res->final_state());
  }
  if (container->ui_font_italic_res && aactor != nullptr) {
    const resource_ref font_ref = resource_ref::from_direct(container->ui_font_italic_res.get());
    container->br->load_resource.try_push(command_load_resource{font_ref, container->ui_font_italic_res->final_state()});
    container->startup_resources.push_back(font_ref);
    DE_LOG(catalogue::log_domain::ui, flow, "main: requested italic font atlas -> ready (level {})", container->ui_font_italic_res->final_state());
  }
}

bool simulation::stop_predicate() const {
  // Выход: запрос из UI (app.quit_game) ИЛИ пользователь закрыл окно (крестик/Alt-F4).
  if (quit_requested.load(std::memory_order_acquire)) return true;
  if (container && container->window != nullptr && input::should_close(container->window)) return true;
  return false;
}

void simulation::update(const size_t time) {
  if (container) {
    container->tick += 1;
  }

  // --- FSM состояний движка (шаг 3) ---
  // boot: движок поднимается; ждём готовности UI-шрифта (пока атлас не на GPU, текст не рисуется —
  //   splash это просто заливка/лого). Как только шрифт usable() → loading (можно рисовать текст).
  // loading: ассеты тянут стартовый набор; на экране прогресс. usable() всего набора → game.
  // game: рисуем карту/акторов. (Рендер без окна/headless шрифта не грузит — тогда сразу дальше.)
  if (container != nullptr) {
    switch (container->state) {
      case app_state::boot: {
        const bool font_ready = !container->ui_font_res || container->ui_font_res->usable();
        const bool no_render = (gactor == nullptr); // без рендера splash не нужен — не залипаем в boot
        if (font_ready || no_render) {
          container->state = app_state::loading;
          DE_LOG(catalogue::log_domain::main, flow, "app_state: boot -> loading (ui font {})", font_ready ? "ready" : "n/a");
        }
        break;
      }
      case app_state::loading:
        if (loading_complete(*container)) {
          container->state = app_state::game;
          DE_LOG(catalogue::log_domain::main, flow, "app_state: loading -> game (startup resources ready)");
        }
        break;
      case app_state::game:
        break;
    }
  }

  // Демо п.2/п.3: периодически переключаем активный render graph graph<->menu_graph, чтобы проверить
  // мгновенный своп без пересоздания ресурсов. Управляется render.demo_graph_toggle_ms (0 ⇒ выкл).
  if (container != nullptr && gactor != nullptr) {
    const auto& rc = container->config.render;
    if (rc.demo_graph_toggle_ms > 0 && !rc.menu_graph.empty() && rc.menu_graph != rc.graph) {
      const uint64_t period = std::max<uint64_t>(1,
        uint64_t(rc.demo_graph_toggle_ms) * uint64_t(container->config.simulation.main_fps) / 1000ull);
      if (container->tick % period == 0) {
        const bool to_menu = (container->tick / period) % 2 == 1;
        if (container->br) {
          container->br->set_active_graph.write_slot().name = to_menu ? rc.menu_graph : rc.graph;
          container->br->set_active_graph.publish();
        }
      }
    }
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
  // продвигаем стейт-машину именованных действий (шаг 2d) до сборки UI, чтобы app.action_* были свежими
  if (container && container->window != nullptr) input::events::update(time);

  // --- разбор оконных событий (шаг 1b/1c/1e): ресайз, фокус, сворачивание ---
  if (container && container->window != nullptr) {
    // ресайз фреймбуфера → пересоздать свопчейн + пересчитать проекцию. Нулевой размер (свёрнуто)
    // не шлём рендеру (свопчейн 0×0 недопустим); подхватим корректный размер при разворачивании.
    if (g_window_events.resized) {
      g_window_events.resized = false;
      if (g_window_events.fb_w != 0 && g_window_events.fb_h != 0) {
        container->fb_width = g_window_events.fb_w;
        container->fb_height = g_window_events.fb_h;
        container->cam.aspect = float(container->fb_width) / float(std::max(container->fb_height, 1u));
        if (container->br) {
          container->br->window_resize.write_slot() = command_window_resize{container->fb_width, container->fb_height};
          container->br->window_resize.publish();
        }
        DE_LOG(catalogue::log_domain::main, flow, "main: window resized to {}x{}", container->fb_width, container->fb_height);
      }
    }

    // фокус/сворачивание → реакции по window_policy. Сворачивание = потеря фокуса.
    if (g_window_events.state_changed) {
      g_window_events.state_changed = false;
      const bool focused = g_window_events.focused;
      const bool iconified = g_window_events.iconified;
      const bool active = focused && !iconified;
      container->window_active = active;

      const auto& pol = container->policy;
      const float gain = (active || !pol.mute_when_unfocused) ? pol.focused_master_gain : pol.unfocused_master_gain;
      const bool draw = active ? true : (iconified ? pol.draw_when_minimized : pol.draw_when_unfocused);

      if (sactor != nullptr && container->br) {
        container->br->sound_master_gain.try_push(command_sound_set_master_gain{gain});
      }
      if (gactor != nullptr && container->br) {
        container->br->render_set_active.write_slot() = command_render_set_active{draw};
        container->br->render_set_active.publish();
      }
      // фокус/сворачивание — частые события (alt-tab) → flow-домен, не спамим базовый лог.
      DE_LOG(catalogue::log_domain::main, flow, "window focus={} iconified={} -> draw={} master_gain={:.2f}", focused, iconified, draw, gain);
    }
  }

  if (
    container &&
    container->sound_devices_requested &&
    !container->sound_devices_logged &&
    container->sound_devices_ready.load(std::memory_order_acquire)
  ) {
    DE_LOG(catalogue::log_domain::sound, flow, "main: sound playback devices count {}", container->sound_devices.size());
    for (size_t i = 0; i < container->sound_devices.size(); ++i) {
      DE_LOG(catalogue::log_domain::sound, flow, "main: sound device[{}] '{}'", i, container->sound_devices[i]);
    }
    container->sound_devices_logged = true;
  }

  // (бывший тест test.mp3 удалён: test.mp3 больше нет, звук теперь грузится именованным набором
  //  и играется по событиям через мост sim→sound ниже + из UI в фазе D-UI.)

  // атлас шрифта доехал на GPU: фиксируем слот в шрифте (nuklear зашьёт его в texture.id
  // draw-команд текста; шейдер UI по нему сэмплит атлас). gpu_index записан рендером.
  if (container && container->ui_font_res && !container->ui_font_logged && container->ui_font_res->usable()) {
    const uint32_t slot = container->ui_font_res->gpu_index;
    if (auto* font = container->ui_font_res->font()) font->set_texture_id(slot);
    DE_LOG(catalogue::log_domain::ui, flow, "main: font atlas reached GPU (usable), texture slot={}", slot);
    container->ui_font_logged = true;
  }
  if (container && container->ui_font_italic_res && !container->ui_font_italic_logged && container->ui_font_italic_res->usable()) {
    const uint32_t slot = container->ui_font_italic_res->gpu_index;
    if (auto* font = container->ui_font_italic_res->font()) font->set_texture_id(slot);
    DE_LOG(catalogue::log_domain::ui, flow, "main: italic font atlas reached GPU (usable), texture slot={}", slot);
    container->ui_font_italic_logged = true;
  }

  if (container && container->br) {
    command_chunk_loaded cmd{};
    while (container->br->chunk_loaded.try_pop(cmd)) {
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
    }

    if (!container->chunks_logged && container->chunks_loaded_count == container->chunks_loaded.size()) {
      DE_LOG(catalogue::log_domain::gameplay, flow, "main: all {} mock world chunks loaded", container->chunks_loaded_count);
      container->chunks_logged = true;
    }
  }

  // --- пайплайн тайловой карты: фрустум камеры -> срез сетки -> инстансы -> сообщение ---
  if (container && container->batch.valid()) {
    // собираем сообщение в рендер: метаданные + упакованные байты инстансов ("v2ui1").
    const glm::mat4 vp = container->cam.view_proj();

    // Контракт записи в буфер: шлём общий UBO (view_proj + ui_proj + misc) в host-visible
    // camera_buffer. ВСЕГДА (не только в game): ui_proj нужен UI на splash/loading тоже.
    // ui_proj — ortho «пиксели окна -> clip» (Vulkan: y вниз, начало слева-сверху). misc=screen_size/px_range.
    if (gactor != nullptr) {
      // ЖИВОЙ размер фреймбуфера, а не статичный config (иначе проекция искажается на реальном
      // размере окна). cam.aspect уже обновлён коллбэком ресайза, так что и view_proj корректен.
      const float w = float(std::max(container->fb_width, 1u));
      const float h = float(std::max(container->fb_height, 1u));

      global_ubo_t ubo{};
      ubo.view_proj = vp;
      ubo.ui_proj = glm::mat4(1.0f);
      ubo.ui_proj[0][0] = 2.0f / w;
      ubo.ui_proj[1][1] = 2.0f / h;
      ubo.ui_proj[2][2] = -1.0f;
      ubo.ui_proj[3][0] = -1.0f;
      ubo.ui_proj[3][1] = -1.0f;
      ubo.misc = glm::vec4(w, h, 4.0f /* sdf px_range, = font_atlas_packer pixel_range */, 0.0f);

      static const uint64_t camera_buffer_hash = utils::string_hash("camera_buffer");
      if (container->br) {
        container->br->write_buffer.write(camera_buffer_hash,
          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&ubo), sizeof(global_ubo_t)));
      }
    }

    // Тайлы карты публикуем ТОЛЬКО в game (шаг 3d): на splash/loading карта не рисуется, поэтому
    // ничего не мигает и null-текстуры не вылезают (стартовый набор уже usable() к моменту game).
    // Срез сетки строим тут же (только когда реально публикуем — не тратим CPU на splash/loading).
    if (container->state == app_state::game && gactor != nullptr && container->br) {
      const tile_span span = visible_tiles(container->cam, container->grid, 1.0f);
      container->batch.build(container->grid, span);

      auto& slot = container->br->draw_tiles.write_slot();
      std::memcpy(slot.view_proj.data(), &vp[0][0], sizeof(float) * 16);
      slot.count = container->batch.count();
      slot.stride = tile_batch::stride();
      slot.bytes.resize(size_t(slot.count) * slot.stride);
      container->batch.blit(std::span<uint8_t>(slot.bytes));

      if (!container->tiles_logged) {
        DE_TRACE(catalogue::log_domain::gameplay,
          "tile slice [{},{})x[{},{}) = {} instances, {} B/inst, {} B payload",
          span.x0, span.x1, span.y0, span.y1, slot.count, slot.stride, slot.bytes.size());
        container->tiles_logged = true;
      }

      container->br->draw_tiles.publish();
    }
  }

  // --- actor simulation slice: simple AI -> move intents -> aesthetics components -> GPU batch ---
  // Только в game (шаг 3d): на splash/loading акторов не считаем и не публикуем.
  if (container && container->state == app_state::game && container->actors_batch.valid()) {
    const auto t0 = std::chrono::steady_clock::now();
    container->actors_last_metrics = container->actors.update(
      float(time) / float(utils::global_time_resolution),
      container->actors_batch,
      *container->pool
    );
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t update_us = uint64_t(std::max<int64_t>(utils::count_mcs(t0, t1), 0));

    // Снапшот акторов — latest-wins мейлбокс: заполняем слот-продюсер НА МЕСТЕ (bytes/ids
    // переиспользуют ёмкость между кадрами), затем publish. Строим только когда рендер включён.
    if (gactor != nullptr && container->br) {
      auto& slot = container->br->draw_actors.write_slot();
      slot.count = container->actors_batch.count();
      slot.stride = actor_batch::stride();
      slot.sim_frame_time = time;
      slot.bytes.resize(size_t(slot.count) * slot.stride);
      container->actors_batch.blit(std::span<uint8_t>(slot.bytes));
      slot.ids.assign(container->actors_batch.ids().begin(), container->actors_batch.ids().end());

      if (!container->actors_logged) {
        DE_TRACE(catalogue::log_domain::gameplay,
          "actor slice {} actors, {} intents, {} instances, {} B/inst, {} B payload",
          container->actors_last_metrics.actors,
          container->actors_last_metrics.intents,
          slot.count,
          slot.stride,
          slot.bytes.size()
        );
        container->actors_logged = true;
      }

      container->br->draw_actors.publish();
    }

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
        // резолв хеш-имя события → demiurg-ресурс; неизвестный звук пропускаем
        const auto snd_it = container->sound_by_name.find(e.name);
        if (snd_it == container->sound_by_name.end()) continue;
        command_sound_play play{};
        play.taskid = generate_task_id();
        play.after = SIZE_MAX; // без секвенсинга
        play.res = resource_ref::from_handle(snd_it->second);
        play.start = 0.0;
        container->br->sound_play.try_push(play);
        ++sent;
        DE_TRACE(catalogue::log_domain::sound, "sim-sound send task={} at ({:.1f},{:.1f})", play.taskid, e.pos.x, e.pos.y);
      }
      if (sent > 0) DE_LOG(catalogue::log_domain::sound, flow, "sim-sounds sent {} (of {} emits)", sent, emits.size());
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
        DE_LOG(catalogue::log_domain::main, flow,
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
    if (const command_sound_state* msg = container->br ? container->br->sound_state.consume() : nullptr) {
      auto& cur = container->sound_state;
      auto& next = container->sound_state_next;
      next.clear();
      for (const auto& s : msg->sounds) next.push_back({s.taskid, s.progress, 0});
      for (const auto& e : cur) {
        if (e.deadline == 0) continue;                     // была подтверждена, но публикации больше нет → конец
        if (e.deadline < container->sound_frame) continue; // окно старта вышло → считаем завершённой
        bool in_pub = false;
        for (const auto& s : msg->sounds) if (s.taskid == e.taskid) { in_pub = true; break; }
        if (!in_pub) next.push_back(e); // оптимистичная, ещё не доехала — оставляем
      }
      std::swap(cur, next);
    }

    // продвигаем UI-prng на кадр и накапливаем метку времени; отдаём в lua (time, timestamp, rng_state).
    container->ui_rng = utils::xoshiro256starstar::next(container->ui_rng);
    container->ui_timestamp += time;
    container->ui->update(time, container->ui_timestamp, utils::xoshiro256starstar::value(container->ui_rng));
    container->ui->convert();

    // memory-probe: раз в ~1с логируем RSS + lua-heap с дельтами. Отделяет рост lua от C++:
    // если растёт lua KiB — дело в скриптах (GC/утечка ссылок), иначе — в нативной части.
    {
      static uint64_t probe_tick = 0;
      if (++probe_tick % 20 == 0) { // main_fps=20 → ~раз в секунду
        lua_State* L = container->ui->script_state().lua_state();
        const int64_t lua_kib = int64_t(lua_gc(L, LUA_GCCOUNT, 0));
        const int64_t rss_kib = int64_t(read_rss_bytes() / 1024);
        static int64_t prev_rss = 0, prev_lua = 0;
        // память — движковый flow-домен (по умолчанию off; включить: logging.main="flow").
        DE_LOG(catalogue::log_domain::main, flow,
               "mem: RSS {} MiB (d{:+.2f}) | lua {} KiB (d{:+}) | sound_tasks {}",
               rss_kib / 1024, double(rss_kib - prev_rss) / 1024.0,
               lua_kib, lua_kib - prev_lua, container->sound_state.size());
        prev_rss = rss_kib; prev_lua = lua_kib;
      }
    }

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

      static const uint64_t ui_vertices_hash = utils::string_hash("ui_vertices");
      static const uint64_t ui_indices_hash  = utils::string_hash("ui_indices");
      static const uint64_t ui_commands_hash = utils::string_hash("ui_commands");
      if (container->br) {
        container->br->write_buffer.write(ui_vertices_hash, verts); // span<const uint8_t> напрямую
        container->br->write_buffer.write(ui_indices_hash, inds);

        // ui_commands самоописывающийся: [uint32 count]‖[тело] — scatter-запись без temp-буфера
        const uint32_t count = uint32_t(cmds.size());
        container->br->write_buffer.write(ui_commands_hash,
          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&count), sizeof(uint32_t)),
          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(cmds.data()), cmds.size() * sizeof(visage::gui_draw_command_t)));
      }
    }

    if (!container->ui_logged) {
      DE_LOG(catalogue::log_domain::ui, flow, "visage: ui buffers — {} vtx bytes, {} idx bytes, {} draw commands",
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
