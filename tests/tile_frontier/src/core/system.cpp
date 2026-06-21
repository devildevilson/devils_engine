#include "system.h"

#include <thread>
#include <devils_engine/sound/system.h>
#include <devils_engine/utils/event_dispatcher.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/time-utils.hpp>
#include <devils_engine/input/core.h>
#include <devils_engine/thread/atomic_pool.h>

#include <devils_engine/sound/resource.h>
#include <devils_engine/utils/fileio.h>

#include <devils_engine/painter/graphics_base.h>

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

constexpr size_t main_frame_time = utils::round(double(utils::global_time_resolution) * (1.0/20.0));

constexpr size_t sound_frame_time = utils::round(double(utils::global_time_resolution) * (1.0/60.0));
constexpr size_t sound_thread_gap = utils::round(double(sound_frame_time) / 4.0);

constexpr size_t render_frame_time = utils::round(double(utils::global_time_resolution) * (1.0/60.0));
constexpr size_t render_thread_gap = utils::round(double(sound_frame_time) / 4.0);

constexpr size_t assets_frame_time = utils::round(double(utils::global_time_resolution) * (1.0/60.0));
constexpr size_t assets_thread_gap = utils::round(double(sound_frame_time) / 4.0);

// все равно придется делить по типам ресурсов
// помоему только за звуком нужно вот так следить
struct resource_status {
  // resource
  // value
  // ???
};

template <typename T>
class event_dispatcher2_adapter : public utils::message_reciever<T> {
public:
  std::mutex mutex;
  std::vector<T> messages;

  event_dispatcher2_adapter() noexcept = default;
  event_dispatcher2_adapter(const size_t reserved) noexcept {
    messages.reserve(reserved);
  }

  utils::send_status send(T msg) override {
    const std::lock_guard l(mutex);
    messages.push_back(std::move(msg));
    return utils::send_status::ok;
  }

  utils::send_status send(std::vector<T>& msg) override {
    const std::lock_guard l(mutex);
    std::swap(messages, msg);
    return utils::send_status::ok;
  }

  void consume(std::vector<T>& msg) {
    const std::lock_guard l(mutex);
    std::swap(messages, msg);
  }

  std::vector<T> consume() {
    std::vector<T> msg;
    const std::lock_guard l(mutex);
    std::swap(messages, msg);
    return msg;
  }
};

template <typename T>
struct cached_event_dispatcher {
  event_dispatcher2_adapter<T> dis;
  std::vector<T> cache;

  cached_event_dispatcher() noexcept = default;
  cached_event_dispatcher(const size_t reserved) noexcept : dis(reserved) {
    cache.reserve(reserved);
  }
};

template <typename T, typename F>
void dispatcher_consume(event_dispatcher2_adapter<T>& dis, std::vector<T>& arr, F f) {
  dis.consume(arr);
  for (const auto& cmd : arr) { std::invoke(f, cmd); }
  arr.clear();
}

template <typename T, typename F>
void dispatcher_consume_last(event_dispatcher2_adapter<T>& dis, std::vector<T>& arr, F f) {
  dis.consume(arr);
  if (!arr.empty()) { std::invoke(f, arr.back()); }
  arr.clear();
}

template <typename T, typename F>
void dispatcher_consume(cached_event_dispatcher<T>& ced, F f) {
  dispatcher_consume<T>(ced.dis, ced.cache, std::move(f));
}

template <typename T, typename F>
void dispatcher_consume_last(cached_event_dispatcher<T>& ced, F f) {
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
  std::unique_ptr<thread::atomic_pool> pool_container;
  thread::atomic_pool* pool;

  std::unique_ptr<sound_simulation> sound_sim;
  std::unique_ptr<render_simulation> render_sim;
  std::unique_ptr<assets_simulation> assets_sim;

  std::unique_ptr<std::jthread> sound_thread;
  std::unique_ptr<std::jthread> render_thread;
  std::unique_ptr<std::jthread> assets_thread;

  //event_dispatcher2_adapter<> ;

  GLFWwindow* window;
  GLFWmonitor* monitor;

  input::init in;

  simulation_init() : pool(nullptr), window(nullptr), monitor(nullptr), in(&error_callback) {}
};

struct assets_simulation_init {
  thread::atomic_pool* pool;
};

simulation::simulation() noexcept : simul::advancer(main_frame_time), sactor(nullptr), gactor(nullptr), aactor(nullptr) {}

simulation::~simulation() noexcept {
  container->sound_sim->stop();
  container->render_sim->stop();
  container->assets_sim->stop();
}

void simulation::init() {
  const uint32_t thread_count = std::min(int(std::thread::hardware_concurrency()) - 4, 1);

  const auto cpu_name = utils::get_cpu_name();
  utils::info("Using cpu '{}', cores: {}", cpu_name, std::thread::hardware_concurrency());

  container.reset(new simulation_init);
  container->pool_container.reset(new thread::atomic_pool(thread_count));
  container->pool = container->pool_container.get();

  container->sound_sim.reset(new sound_simulation(sound_frame_time));
  container->sound_sim->init();
  sactor = container->sound_sim->get_actor();
  
  container->render_sim.reset(new render_simulation(render_frame_time));
  container->render_sim->init();
  gactor = container->render_sim->get_actor();

  container->assets_sim.reset(new assets_simulation(assets_frame_time));
  container->assets_sim->init();
  aactor = container->assets_sim->get_actor();

  // нужно еще создать окно и передать его в графику
  // окно создаем здесь, а в графику передадим с помощью ивента
  // в последствии может возникнуть ситуация где мы окно полностью пересоздадим
  // тогда мы снова отправим запрос на пересоздание окна в графику

  container->sound_thread.reset (new std::jthread([sys = container->sound_sim.get()] (){ sys->run(sound_thread_gap); }));
  container->render_thread.reset(new std::jthread([sys = container->render_sim.get()](){ sys->run(render_thread_gap); }));
  container->assets_thread.reset(new std::jthread([sys = container->assets_sim.get()](){ sys->run(assets_thread_gap); }));

  container->monitor = input::primary_monitor();
  container->window = input::create_window(800, 600, "test_actors");

  const auto monitor_name = input::monitor_name(container->monitor);
  utils::info("Using monitor '{}'", monitor_name);

  command_window_recreation wr{ container->window, container->monitor };
  gactor->send(wr);
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

  input::poll_events();

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
  event_dispatcher2_adapter<command_sound> commands;

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
  cached_event_dispatcher<command_window_recreation> window_recreation_commands;
  cached_event_dispatcher<command_window_resize> window_resizing_commands;

  std::unique_ptr<painter::graphics_base> base;
};

render_simulation::render_simulation(const size_t frame_time) noexcept : simul::advancer(frame_time) {}
void render_simulation::init() {
  container = std::make_unique<render_simulation_init>();
  actor.add_receiver<command_window_recreation>(&container->window_recreation_commands.dis);
  actor.add_receiver<command_window_resize>(&container->window_resizing_commands.dis);

  // нужно создать рендер стейт
  // для него нужен инстанс + устройство + физическое устройство
  // по идее мы должны получить это дело из json конфига с диска
  //container->base = std::make_unique<painter::graphics_base>();

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
  dispatcher_consume_last(container->window_recreation_commands, [] (const auto& cmd) {
      
  });

  // ловим событие изменение размеров окна
  dispatcher_consume_last(container->window_resizing_commands, [] (const auto& cmd) {
    
  });
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
