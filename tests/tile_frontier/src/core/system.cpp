#include "system.h"

#include <thread>
#include <algorithm>
#include <cmath>
#include <limits>
#include <devils_engine/sound/system.h>
#include <devils_engine/utils/event_dispatcher.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/time-utils.hpp>
#include <devils_engine/input/core.h>
#include <devils_engine/thread/atomic_pool.h>

#include <devils_engine/sound/resource.h>
#include <devils_engine/utils/fileio.h>

#include <devils_engine/painter/graphics_base.h>
#include <devils_engine/painter/assets_base.h>
#include <devils_engine/painter/auxiliary.h>
#include <devils_engine/painter/makers.h>
#include <devils_engine/painter/system_info.h>

#include <tavl/deserialize.h>

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

struct window_config {
  std::string title = "tile_frontier";
  uint32_t width = 1280;
  uint32_t height = 720;
  bool create_on_start = true;
};

struct simulation_config {
  uint32_t main_fps = 20;
  uint32_t sound_fps = 60;
  uint32_t render_fps = 60;
  uint32_t assets_fps = 60;
  uint32_t worker_threads_reserved = 4;
  uint32_t min_worker_threads = 1;
  uint32_t thread_start_gap_divisor = 4;
};

struct render_config {
  std::string config_folder = "render_config";
  std::string cache_folder = "cache/render";
  std::string pipeline_cache = "cache/render/pipeline_cache.bin";
  std::string preferred_gpu;
  uint32_t preferred_gpu_index = 0;
  std::string graph = "graphics1";
};

struct metrics_config {
  bool enabled = true;
  uint32_t log_interval_ms = 1000;
};

struct app_config {
  window_config window;
  simulation_config simulation;
  render_config render;
  metrics_config metrics;
};

static size_t frame_time_from_fps(const uint32_t fps) noexcept {
  const auto valid_fps = std::max(fps, 1u);
  return utils::round(double(utils::global_time_resolution) / double(valid_fps));
}

static size_t thread_start_gap(const size_t frame_time, const uint32_t divisor) noexcept {
  const auto valid_divisor = std::max(divisor, 1u);
  return utils::round(double(frame_time) / double(valid_divisor));
}

static std::string make_project_path(std::string path) {
  if (path.empty()) return utils::project_folder();
  if (path.front() == '/') return path;
  return utils::project_folder() + path;
}

static std::string make_project_folder_path(std::string path) {
  path = make_project_path(std::move(path));
  if (!path.empty() && path.back() != '/') path.push_back('/');
  return path;
}

static uint32_t make_color(const float r, const float g, const float b, const float a) {
  const auto pack = [] (const float v) {
    return uint32_t(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  return (pack(r) << 0) | (pack(g) << 8) | (pack(b) << 16) | (pack(a) << 24);
}

static app_config load_app_config(const std::string& path) {
  app_config cfg;
  if (!file_io::exists(path)) {
    utils::warn("Could not find app config '{}', using defaults", path);
    return cfg;
  }

  const auto content = file_io::read(path);
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(content);
  parser.finish();

  tavl::ct_context ctx;
  tavl::deserialize(parser, ctx, cfg);

  if (!ctx.diagnostics.empty()) {
    utils::warn("App config '{}' produced {} tavl diagnostics", path, ctx.diagnostics.size());
    for (const auto& d : ctx.diagnostics) {
      utils::warn("  tavl diagnostic {}, field '{}'", static_cast<size_t>(d.error.type), d.field);
    }
  }

  return cfg;
}

constexpr size_t main_frame_time = utils::round(double(utils::global_time_resolution) * (1.0/20.0));

// все равно придется делить по типам ресурсов
// помоему только за звуком нужно вот так следить
struct resource_status {
  // resource
  // value
  // ???
};

struct message_dispatcher_stats {
  size_t sent = 0;
  size_t consumed = 0;
  size_t dropped = 0;
  size_t high_watermark = 0;
  size_t capacity = std::numeric_limits<size_t>::max();
};

template <typename T>
class message_dispatcher : public utils::message_reciever<T> {
public:
  message_dispatcher() noexcept = default;
  message_dispatcher(const size_t reserved, const size_t capacity = std::numeric_limits<size_t>::max()) noexcept :
    capacity(capacity)
  {
    messages.reserve(reserved);
  }

  utils::send_status send(T msg) override {
    const std::lock_guard l(mutex);
    if (messages.size() >= capacity) {
      stats_data.dropped += 1;
      return utils::send_status::mailbox_full;
    }

    messages.push_back(std::move(msg));
    on_sent(1);
    return utils::send_status::ok;
  }

  utils::send_status send(std::vector<T>& msg) override {
    if (msg.empty()) return utils::send_status::ok;

    const std::lock_guard l(mutex);
    const size_t available = capacity - messages.size();
    if (msg.size() > available) {
      stats_data.dropped += msg.size();
      return utils::send_status::backpressure;
    }

    const size_t count = msg.size();
    messages.insert(
      messages.end(),
      std::make_move_iterator(msg.begin()),
      std::make_move_iterator(msg.end())
    );
    msg.clear();
    on_sent(count);
    return utils::send_status::ok;
  }

  void reserve(const size_t count) {
    const std::lock_guard l(mutex);
    messages.reserve(count);
  }

  void set_capacity(const size_t value) {
    const std::lock_guard l(mutex);
    capacity = value;
    stats_data.capacity = value;
  }

  void consume_all(std::vector<T>& msg) {
    const std::lock_guard l(mutex);
    std::swap(messages, msg);
    stats_data.consumed += msg.size();
  }

  std::vector<T> consume_all() {
    std::vector<T> msg;
    consume_all(msg);
    return msg;
  }

  bool consume_last(std::vector<T>& cache) {
    consume_all(cache);
    if (cache.empty()) return false;
    return true;
  }

  size_t pending() const {
    const std::lock_guard l(mutex);
    return messages.size();
  }

  message_dispatcher_stats stats() const {
    const std::lock_guard l(mutex);
    return stats_data;
  }
private:
  mutable std::mutex mutex;
  std::vector<T> messages;
  size_t capacity = std::numeric_limits<size_t>::max();
  message_dispatcher_stats stats_data;

  void on_sent(const size_t count) noexcept {
    stats_data.sent += count;
    stats_data.high_watermark = std::max(stats_data.high_watermark, messages.size());
  }
};

template <typename T>
struct cached_message_dispatcher {
  message_dispatcher<T> dis;
  std::vector<T> cache;

  cached_message_dispatcher() noexcept = default;
  cached_message_dispatcher(const size_t reserved) noexcept : dis(reserved) {
    cache.reserve(reserved);
  }
};

template <typename T, typename F>
void dispatcher_consume(message_dispatcher<T>& dis, std::vector<T>& arr, F f) {
  dis.consume_all(arr);
  for (const auto& cmd : arr) { std::invoke(f, cmd); }
  arr.clear();
}

template <typename T, typename F>
void dispatcher_consume_last(message_dispatcher<T>& dis, std::vector<T>& arr, F f) {
  if (dis.consume_last(arr)) { std::invoke(f, arr.back()); }
  arr.clear();
}

template <typename T, typename F>
void dispatcher_consume(cached_message_dispatcher<T>& ced, F f) {
  dispatcher_consume<T>(ced.dis, ced.cache, std::move(f));
}

template <typename T, typename F>
void dispatcher_consume_last(cached_message_dispatcher<T>& ced, F f) {
  dispatcher_consume_last<T>(ced.dis, ced.cache, std::move(f));
}

static void error_callback(int, const char* msg) noexcept {
  utils::warn("GLFW error: {}", msg);
}

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

// тут что? все другие системы + потоки для них + тред пул
// кеш?
struct simulation_init {
  app_config config;

  std::unique_ptr<thread::atomic_pool> pool_container;
  thread::atomic_pool* pool;

  std::unique_ptr<sound_simulation> sound_sim;
  std::unique_ptr<render_simulation> render_sim;
  std::unique_ptr<assets_simulation> assets_sim;

  std::unique_ptr<std::jthread> sound_thread;
  std::unique_ptr<std::jthread> render_thread;
  std::unique_ptr<std::jthread> assets_thread;

  //message_dispatcher<> ;

  GLFWwindow* window;
  GLFWmonitor* monitor;

  std::unique_ptr<input::init> in;

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

  command_window_recreation wr{
    c.window,
    c.monitor,
    c.config.window.width,
    c.config.window.height
  };
  gactor->send(wr);
}

struct assets_simulation_init {
  thread::atomic_pool* pool;
};

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

void simulation::init() {
  container.reset(new simulation_init);
  const auto config_path = utils::project_folder() + "resources/config/app.tavl";
  container->config = load_app_config(config_path);
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
  sactor = container->sound_sim->get_actor();
  
  render_simulation_config render_cfg;
  render_cfg.render_config_folder = make_project_folder_path(container->config.render.config_folder);
  render_cfg.pipeline_cache_path = make_project_path(container->config.render.pipeline_cache);
  render_cfg.graph_name = container->config.render.graph;
  render_cfg.create_vulkan_on_init = container->config.window.create_on_start;

  if (container->config.window.create_on_start && !container->in) {
    container->in = std::make_unique<input::init>(&error_callback);
  }

  container->render_sim.reset(new render_simulation(render_ft, std::move(render_cfg)));
  container->render_sim->init();
  gactor = container->render_sim->get_actor();

  container->assets_sim.reset(new assets_simulation(assets_ft));
  container->assets_sim->init();
  aactor = container->assets_sim->get_actor();

  const auto gap_divisor = container->config.simulation.thread_start_gap_divisor;
  const auto sound_gap = thread_start_gap(sound_ft, gap_divisor);
  const auto render_gap = thread_start_gap(render_ft, gap_divisor);
  const auto assets_gap = thread_start_gap(assets_ft, gap_divisor);

  container->sound_thread.reset (new std::jthread([sys = container->sound_sim.get(), sound_gap] (){ sys->run(sound_gap); }));
  container->render_thread.reset(new std::jthread([sys = container->render_sim.get(), render_gap](){ sys->run(render_gap); }));
  container->assets_thread.reset(new std::jthread([sys = container->assets_sim.get(), assets_gap](){ sys->run(assets_gap); }));

  // Окно - поздний ресурс. Render thread должен жить и без него, а это событие
  // может прийти сейчас, после загрузки ассетов или после полного пересоздания окна.
  if (container->config.window.create_on_start) create_window_and_notify_render(*container, gactor);
}

static size_t test_counter = 0;
bool simulation::stop_predicate() const {
  test_counter += 1;
  return test_counter > 100;
  //return false; // выход из приложения?
}

void simulation::update(const size_t time) {
  // думаем, собираем инпут, считаем физику, разбираемся с геймплеем, пробегаем UI, отправляем данные другим акторам
  // тут нужны состояния у системы + пауза
  // пауза это что? не думаем, не считаем физику, не разбираемся с геймплеем, но пробегаем UI

  // после "думаем" и инпут мы получаем intent

  // в системах звуков отправляем новый звук 
  /*static size_t command_counter = 0;
  const size_t curid = command_counter;
  command_counter += 1;
  command_sound cs{};
  cs.taskid = curid;
  utils::info("Send sound command {}", curid);
  const auto res = sactor->send(cs);*/

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
  // 

  //utils::info("Main loop");

  if (container && container->in) input::poll_events();

  // задаем нажатие кнопок для интерфейса 

  // обходим visage 

  // собираем буферы команд в кучу и отправляем их на отрисовку
  //command_update_ui uu{};
  //const auto res = sactor->send(uu);

  // тут придется уже сразу сделать преобразование шрифта в СДФ
  // а это значит еще вопрос локализации

  // app.send_event требует функцию которая 
  // получит тип системы и вернет id
  // этот id передается в системы и используется потом чтобы понять что происходит

  // думаю что для начала нужно вернуть 3 треугольника на экран
}

// команда для обновления позиций и скоростей и направлений
// в принципе можно актору отправить vector с данными по звукам которые сейчас проигрываются
// выглядит норм

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

struct render_simulation_init {
  cached_message_dispatcher<command_window_recreation> window_recreation_commands;
  cached_message_dispatcher<command_window_resize> window_resizing_commands;

  render_simulation_config config;

  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  painter::physical_device_data physical_device_data;

  VkQueue graphics_queue = VK_NULL_HANDLE;
  VkQueue transfer_queue = VK_NULL_HANDLE;

  std::unique_ptr<painter::graphics_base> base;
  std::unique_ptr<painter::assets_base> assets;
  painter::graphics_ctx ctx;

  uint32_t triangle_pair_index = painter::INVALID_RESOURCE_SLOT;
  bool instance_ready = false;
  bool device_ready = false;
  bool base_ready = false;
  bool surface_ready = false;
  bool graph_ready = false;
  bool triangles_ready = false;

  ~render_simulation_init() noexcept {
    if (device != VK_NULL_HANDLE) {
      painter::load_dispatcher3(device);
      vk::Device(device).waitIdle();
    }

    ctx = painter::graphics_ctx{};
    assets.reset();
    base.reset();

    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE) {
      vk::Instance(instance).destroy(surface);
      surface = VK_NULL_HANDLE;
    }

    if (device != VK_NULL_HANDLE) {
      vk::Device(device).destroy();
      device = VK_NULL_HANDLE;
    }

    if (instance != VK_NULL_HANDLE) {
      if (debug_messenger != VK_NULL_HANDLE) {
        painter::destroy_debug_messenger(instance, debug_messenger);
        debug_messenger = VK_NULL_HANDLE;
      }

      vk::Instance(instance).destroy();
      instance = VK_NULL_HANDLE;
    }
  }
};

static void render_create_instance(render_simulation_init& c) {
  if (c.instance_ready) return;

  painter::load_dispatcher1();

  vk::ApplicationInfo ai{};
  ai.pApplicationName = "tile_frontier";
  ai.applicationVersion = VK_MAKE_VERSION(0, 1, 1);
  ai.pEngineName = "devils_engine";
  ai.engineVersion = VK_MAKE_VERSION(0, 1, 1);
  ai.apiVersion = VK_API_VERSION_1_0;

  std::vector<const char*> exts = painter::get_required_extensions();
  vk::InstanceCreateInfo ici{};
  ici.pApplicationInfo = &ai;
  assert(enable_validation_layers);
  if (::enable_validation_layers) {
    if (!painter::check_validation_layer_support(painter::default_validation_layers)) {
      utils::error{}("Requested Vulkan validation layers are not available");
    }

    ici.enabledLayerCount = painter::default_validation_layers.size();
    ici.ppEnabledLayerNames = painter::default_validation_layers.data();
  }
  ici.enabledExtensionCount = exts.size();
  ici.ppEnabledExtensionNames = exts.data();

  utils::info("tile_frontier: creating Vulkan instance");
  c.instance = vk::createInstance(ici);
  utils::info("tile_frontier: Vulkan instance created");
  painter::load_dispatcher2(c.instance);
  c.debug_messenger = painter::create_debug_messenger(c.instance);
  c.instance_ready = true;
}

static void render_create_device(render_simulation_init& c, const command_window_recreation* window = nullptr) {
  if (c.device_ready) return;

  bool cached = painter::system_info::try_load_cached_data(c.instance, &c.physical_device_data, nullptr);
  if (!cached) {
    if (window == nullptr || window->w == nullptr) return;

    painter::system_info si(c.instance);
    si.check_devices_surface_capability(c.surface);
    c.physical_device_data = si.choose_physical_device();
    si.dump_cache_to_disk(c.physical_device_data.handle, nullptr);
  }

  painter::system_info::print_choosed_device(c.physical_device_data.handle);

  painter::device_maker dm(c.instance);
  dm.beginDevice(c.physical_device_data.handle);
  dm.createQueues(1);
  dm.features(vk::PhysicalDevice(c.physical_device_data.handle).getFeatures());
  dm.setExtensions(painter::default_device_extensions);
  c.device = dm.create({}, "tile_frontier.device");
  painter::load_dispatcher3(c.device);

  vk::Device dev(c.device);
  c.graphics_queue = dev.getQueue(c.physical_device_data.graphics_queue, 0);
  c.transfer_queue = dev.getQueue(c.physical_device_data.transfer_queue, 0);
  painter::set_name(dev, vk::Queue(c.graphics_queue), "tile_frontier.graphics_queue");

  c.device_ready = true;
}

static void render_create_base_resources(render_simulation_init& c) {
  if (c.base_ready || !c.device_ready) return;

  c.base = std::make_unique<painter::graphics_base>(
    c.instance,
    c.device,
    c.physical_device_data.handle,
    painter::presentation_engine_type::main
  );

  c.base->create_allocator();
  c.base->create_command_pool(c.physical_device_data.graphics_queue, c.graphics_queue);
  c.base->create_descriptor_pool();
  c.base->get_or_create_pipeline_cache(c.config.pipeline_cache_path);

  const auto res = c.base->recreate_basic_resources(c.config.render_config_folder);
  if (res != 0) utils::error{}("Could not parse render config folder '{}'", c.config.render_config_folder);

  c.assets = std::make_unique<painter::assets_base>(c.device, c.physical_device_data.handle);
  c.assets->create_fence();
  c.assets->create_allocator(c.instance);
  c.assets->create_command_buffer(c.transfer_queue, c.physical_device_data.transfer_queue);
  c.assets->set_graphics_base(c.base.get());

  c.ctx.base = c.base.get();
  c.ctx.assets = c.assets.get();
  c.base_ready = true;
}

static void render_create_test_triangles(render_simulation_init& c) {
  if (c.triangles_ready || !c.graph_ready) return;

  const auto tri_h = c.assets->register_buffer_storage("triangle");
  painter::buffer_create_info bci{ "g1", 3, 0 };
  c.assets->create_buffer_storage(tri_h, bci);

  struct buffer_data { float x, y, z; uint32_t c; };
  const buffer_data buffer_mem[] = {
    { -1.0f, -1.0f, 0.0f, make_color(1.0f, 0.0f, 0.0f, 1.0f) },
    {  1.0f, -1.0f, 0.0f, make_color(0.0f, 1.0f, 0.0f, 1.0f) },
    {  0.0f,  1.0f, 0.0f, make_color(0.0f, 0.0f, 1.0f, 1.0f) }
  };

  const auto vertex_bytes = std::span(reinterpret_cast<const uint8_t*>(buffer_mem), sizeof(buffer_mem));
  c.assets->populate_buffer_storage(tri_h, vertex_bytes, std::span<const uint8_t>());
  c.assets->mark_ready_buffer_slot(tri_h);

  const uint32_t dg_index = c.base->find_draw_group("dg1");
  if (dg_index == painter::INVALID_RESOURCE_SLOT) utils::error{}("Could not find draw group 'dg1'");

  c.triangle_pair_index = c.base->register_pair(dg_index, tri_h, 500);

  const auto inst = c.base->get_current_instance_resource_frame(c.triangle_pair_index, 1);
  const auto indi = c.base->get_current_indirect_resource_frame(c.triangle_pair_index, 1);

  struct vec4 { float x, y, z, w; };
  auto ptr = reinterpret_cast<vec4*>(&reinterpret_cast<uint8_t*>(inst.mapped)[inst.sub.offset]);
  auto cmd = reinterpret_cast<VkDrawIndirectCommand*>(&reinterpret_cast<uint8_t*>(indi.mapped)[indi.sub.offset]);

  ptr[0] = vec4{  0.0f,  0.5f, 0.0f, 0.0f };
  ptr[1] = vec4{  0.5f,  0.0f, 0.0f, 0.0f };
  ptr[2] = vec4{  0.0f, -0.5f, 0.0f, 0.0f };
  ptr[3] = vec4{ -0.5f,  0.0f, 0.0f, 0.0f };

  cmd[0].vertexCount = 3;
  cmd[0].instanceCount = 4;
  cmd[0].firstVertex = 0;
  cmd[0].firstInstance = 0;

  c.base->update_event();
  c.triangles_ready = true;
}

static void render_attach_window(render_simulation_init& c, const command_window_recreation& cmd) {
  if (!c.instance_ready) render_create_instance(c);

  if (c.surface != VK_NULL_HANDLE) {
    if (c.base) c.base->wait_all_fences();
    vk::Instance(c.instance).destroy(c.surface);
    c.surface = VK_NULL_HANDLE;
    c.surface_ready = false;
    c.graph_ready = false;
    c.triangles_ready = false;
  }

  const auto res = input::create_window_surface(c.instance, cmd.w, nullptr, &c.surface);
  if (res != static_cast<uint32_t>(vk::Result::eSuccess)) {
    utils::error{}("Could not create window surface, got {}", vk::to_string(static_cast<vk::Result>(res)));
  }
  c.surface_ready = true;

  render_create_device(c, &cmd);
  render_create_base_resources(c);

  c.base->set_surface(c.surface, cmd.width, cmd.height);
  c.base->resize_viewport(cmd.width, cmd.height);

  const uint32_t graph_index = c.base->find_render_graph(c.config.graph_name);
  if (graph_index == painter::INVALID_RESOURCE_SLOT) utils::error{}("Could not find render graph '{}'", c.config.graph_name);

  c.base->populate_constant_default_values();
  c.base->change_render_graph(graph_index);
  c.base->dump_cache_on_disk(c.config.pipeline_cache_path);

  c.graph_ready = true;
  render_create_test_triangles(c);
}

render_simulation::render_simulation(const size_t frame_time, render_simulation_config config) noexcept :
  simul::advancer(frame_time),
  container(std::make_unique<render_simulation_init>())
{
  container->config = std::move(config);
}

void render_simulation::init() {
  actor.add_receiver<command_window_recreation>(&container->window_recreation_commands.dis);
  actor.add_receiver<command_window_resize>(&container->window_resizing_commands.dis);

  if (container->config.create_vulkan_on_init) {
    render_create_instance(*container);
    render_create_device(*container);
    render_create_base_resources(*container);
  }

  // еще дополнительно нужно создать менеджера GPU ресурсов
  // я вот о чем подумал: должен быть менеджер ассетов, который
  // раздаст память и займется вопросами копирования
  // + к этому сделать реестр текущих ресурсов, то есть
  // внешняя система получает id -> он ведет к менеджеру ->
  // тот подсказывает в каком состоянии находится GPU ресурс ->
  // если еще не готов, то получаем индекс ресурса по умолчанию (0) ->
  // если готов то он к этому времени окажется в binding'е, отправляем индекс в сете
  // ресурсы GPU могут находиться в нескольких состояниях
  // empty, resource_exists, resource_has_data, ready

  // тогда у менеджера ассетов задача такая: быть реестром всех возможных GPU ресурсов на текущий момент
  // он будет иногда перекидывать информацию в поток ассетов и принимать задачи по загрузке из хоста
  // чем тогда поток ассетов будет заниматься? вообще по идее его задача распарсить дерево 
  // ресурсов и поработать с диском, в том числе что то наоборот спихнуть на диск
}

bool render_simulation::stop_predicate() const { return false; }
void render_simulation::update(const size_t time) {
  // тут че вообще делаем? пробегаем события 
  // заходим в рендер граф, обрабатываем
  // вообще ничего особо сложного 

  //container->commands.consume(container->command_cache);
  //// тут можно взять самую последнюю команду
  //for (const auto& cmd : container->command_cache) {
  //  // создадим сюрфейс
  //  VkSurfaceKHR s = nullptr;
  //  const uint32_t res = input::create_window_surface(nullptr, cmd.w, nullptr, &s);
  //  // graph->setup_surface();
  //  // graph->resize_window();
  //}

  //container->command_cache.clear();

  // событие изменения размеров окна
  // событиe обновления данных
  // событиe обновления настроек
  // ...


  // в конце заходим в рендер граф и рисуем все подряд

  //utils::info("Render loop");

  // ловим событие пересоздания окна
  dispatcher_consume_last(container->window_recreation_commands, [this] (const auto& cmd) {
    render_attach_window(*container, cmd);
  });

  // ловим событие изменение размеров окна
  dispatcher_consume_last(container->window_resizing_commands, [this] (const auto& cmd) {
    if (container->base && container->surface_ready) container->base->resize_viewport(cmd.width, cmd.height);
  });

  if (container->triangles_ready && container->base->can_draw()) {
    container->base->prepare_frame();
    container->ctx.prepare();
    container->ctx.draw();
    container->base->submit_frame();
  }
}

graphics_actor* render_simulation::get_actor() { return &actor; }

assets_simulation::assets_simulation(const size_t frame_time) noexcept : simul::advancer(frame_time) {}
void assets_simulation::init() {
  container.reset(new assets_simulation_init);
  container->pool = nullptr;
}

bool assets_simulation::stop_predicate() const { return false; }

void assets_simulation::update(const size_t time) {
  // тут что? принимаем заявки на загрузку ресурсов 
  // желательно чтобы заявка была сразу как можно больше
  // после чего работаем в двух режимах:
  // один основной поток или владеет тредпулом

  if (container->pool != nullptr) {
    // закидываем задачи в тредпул + берем задачу для себя
    // нужно как то составить список того что сейчас загружается
    // возвращаем пул после того как все выполнили
  } else {
    // берем по одной задаче
  }

  // тут в плане логики все довольно легко, 
  // нужно убедиться что ресурсы доступны в остальных системах только в состоянии ready
  // загрузка может зависить от загрузки зависимого ресурса
  // как минимум карта обращается к текстуре у которой уже должен быть известен слот

  //utils::info("Assets loop");
}

assets_actor* assets_simulation::get_actor() { return &actor; }

}
}
