#include "simulation.h"

#include <algorithm>
#include <thread>

#include <devils_engine/input/core.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/time-utils.hpp>

#include <devils_engine/demiurg/resource_system.h>

#include "config.h"
#include "messages.h"
#include "sound_system.h"
#include "render_system.h"
#include "assets_system.h"
#include "mesh_resource.h"

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

static void error_callback(int, const char* msg) noexcept {
  utils::warn("GLFW error: {}", msg);
}

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

  GLFWwindow* window;
  GLFWmonitor* monitor;

  std::unique_ptr<input::init> in;

  // тестовое наблюдение за прогоном контракта загрузки
  mesh_resource* watch_res = nullptr;
  bool watch_logged = false;

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

  // Окно - поздний ресурс. Render thread должен жить и без него, а это событие
  // может прийти сейчас, после загрузки ассетов или после полного пересоздания окна.
  if (container->config.window.create_on_start && container->render_sim && !container->config.render.headless) {
    create_window_and_notify_render(*container, gactor);
  }

  // Тестовый прогон контракта загрузки: просим у ассетов довести меш 'test' до hot.
  // Указатель ресурса берём из реестра (он стабилен после init), а сам запрос шлём
  // сообщением в актор ассетов — менеджмент крутится на его потоке.
  if (auto* res = container->assets_sim->resources()->get<mesh_resource>("mesh/test")) {
    command_load_resource cmd{res, static_cast<int32_t>(demiurg::state::hot)};
    aactor->send(cmd);
    container->watch_res = res;
    utils::info("main: requested mesh 'mesh/test' -> hot");
  } else {
    utils::warn("main: test mesh resource not found in registry");
  }
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

  // наблюдаем за тестовым ресурсом: main видит hot и читает gpu_index (записан рендером)
  if (container && container->watch_res && !container->watch_logged && container->watch_res->usable()) {
    utils::info("main: resource '{}' reached HOT, gpu_index={}", container->watch_res->id, container->watch_res->gpu_index);
    container->watch_logged = true;
  }

  // задаем нажатие кнопок для интерфейса

  // обходим visage

  // собираем буферы команд в кучу и отправляем их на отрисовку

  // тут придется уже сразу сделать преобразование шрифта в СДФ
  // а это значит еще вопрос локализации

  // app.send_event требует функцию которая
  // получит тип системы и вернет id
  // этот id передается в системы и используется потом чтобы понять что происходит
}

}
}
