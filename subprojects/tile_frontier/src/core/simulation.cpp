#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <span>

#include <devils_engine/input/core.h>
#include <devils_engine/simul/lua_resource_bindings.h>
#include <devils_engine/simul/window_runtime.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/time-utils.hpp>
#include <devils_engine/utils/prng.h>

#include <devils_engine/demiurg/resource_system.h>

#include <gtl/phmap.hpp>
#include <devils_engine/sound/sound_resource.h>

#include <devils_engine/catalogue/introspection.h> // catalogue::statistics_store (perf UI)
#include <devils_engine/catalogue/logging.h>        // доменное логгирование (DE_LOG) + init_logging

#include <devils_engine/visage/system.h>
#include <devils_engine/visage/font.h>
#include <devils_engine/visage/image.h>

#include "config.h"
#include <devils_engine/simul/lua_script_resource.h>

#include "messages.h"
#include "broker.h"
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

constexpr size_t main_frame_time = utils::round(double(utils::global_time_resolution) * (1.0/20.0));
constexpr uint32_t initial_actor_count = 4096;
//constexpr uint32_t initial_actor_count = 64000;

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
} // namespace

// тут что? все другие системы + потоки для них + тред пул
// кеш?
struct simulation_init {
  runtime_bootstrap* boot = nullptr;

  sound_simulation* sound_sim = nullptr;
  render_simulation* render_sim = nullptr;
  assets_simulation* assets_sim = nullptr;

  GLFWwindow* window;
  GLFWmonitor* monitor;

  // Живой размер фреймбуфера (в пикселях). Инициализируется из config, обновляется коллбэком ресайза.
  // Проекция (view_proj/ui_proj/misc) и cam.aspect считаются ОТ НЕГО, а не от статичного config
  // (иначе картинка искажается на любом реальном размере окна — это был баг).
  uint32_t fb_width = 1;
  uint32_t fb_height = 1;
  simul::window_policy policy;
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

  // Шрифты — demiurg-ресурсы ассетного реестра ("fonts/*", многошаговые ttf→MSDF→GPU).
  // CPU-уровни всех шрифтов проходим синхронно в setup_visage (метрики нужны nk_convert сразу),
  // GPU-шаг — асинхронно через load_resource. ui_font_h — дефолтный шрифт (метрики отдаются в
  // visage::system), ui_fonts — все шрифты реестра (+флаг «залогировали usable»).
  demiurg::resource_handle ui_font_h;
  std::vector<std::pair<demiurg::resource_handle, bool>> ui_fonts;

  // Звуки — demiurg-ресурсы в потоке ассетов. main держит name_hash → stable handle (для резолва в
  // command_sound_play из UI/геймплея) и запрашивает их до warm. Сам звук-актор ресурсы не хранит.
  gtl::flat_hash_map<uint64_t, demiurg::resource_handle> sound_by_name;

  // Единый broker всех межпоточных каналов. В обычном старом запуске main владеет owned_br; при
  // запуске через simul::app_runtime broker приходит извне через set_broker() и br только заимствует.
  std::unique_ptr<broker> owned_br;
  broker* br = nullptr;

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
  simul::app_state state = simul::app_state::boot;
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

  simulation_init() : window(nullptr), monitor(nullptr) {}
};

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

simulation::simulation(runtime_bootstrap* boot) noexcept : simul::main_system<::tile_frontier::core::broker>(main_frame_time), bootstrap_(boot) {}

simulation::~simulation() noexcept {
  if (!container) return;
  simul::destroy_window_runtime(*container);
}

void simulation::set_broker(struct broker* b) {
  simul::main_system<::tile_frontier::core::broker>::set_broker(b);
}

// Поднимаем интерфейс в главном потоке. Шрифты — demiurg-ресурсы ассетного реестра
// (resources/modules/core/fonts/*.ttf, многошаговый font_resource): CPU-шаги ttf→MSDF (уровни
// 0→1→2) гоняем ЗДЕСЬ синхронно для ВСЕХ шрифтов, т.к. visage::system нужны метрики глифов сразу
// для nk_convert (и lua может выбрать любой шрифт с первого кадра); GPU-заливка (2→3) уйдёт
// штатным асинхронным путём ассетов из init(). lua выбирает шрифт хендлом:
// nk.push_font{ font = request("fonts/crimson.italic") } — строковых имён в visage больше нет.
static void setup_visage(simulation_init& c) {
  constexpr std::string_view default_font_id = "fonts/crimson.roman";

  auto* reg = c.assets_sim != nullptr ? c.assets_sim->resources() : nullptr;
  if (reg == nullptr) utils::error{}("visage: assets registry is not initialized (fonts live there)");

  const auto v = reg->find("fonts/");
  for (auto* res : v) {
    if (res == nullptr) continue;
    const auto h = reg->handle(res->id);
    auto* fr = h.get<visage::font_resource>();
    if (fr == nullptr) { utils::warn("visage: resource '{}' under fonts/ is not a font_resource; skipping", res->id); continue; }
    while (fr->state() < 2) fr->load(utils::safe_handle_t{}); // 0→1: ttf, 1→2: MSDF-атлас + метрики (CPU, главный поток)
    if (fr->font() == nullptr) utils::error{}("visage: font resource '{}' produced no font metrics", res->id);
    c.ui_fonts.emplace_back(h, false);
    DE_LOG(catalogue::log_domain::ui, flow, "visage: font '{}' CPU-ready ({} glyphs)", res->id, fr->font()->glyphs.size());
  }

  c.ui_font_h = reg->handle(default_font_id);
  auto* def = c.ui_font_h.get<visage::font_resource>();
  if (def == nullptr || def->font() == nullptr) utils::error{}("visage: default ui font '{}' not found in assets registry", default_font_id);

  c.ui.reset(new visage::system(def->font())); // visage заимствует метрики; байты атласа ждут GPU-шага
  DE_LOG(catalogue::log_domain::ui, flow, "visage: system created (default font '{}', {} fonts total)", default_font_id, c.ui_fonts.size());
}

void runtime_traits::init_bootstrap(bootstrap_type& boot) {
  simul::init_standard_bootstrap<app_config_resource>(boot);
}

void simulation::init() {
  if (bootstrap_ == nullptr) utils::error{}("simulation: runtime_bootstrap is not set");
  container.reset(new simulation_init);
  container->boot = bootstrap_;
  // Единый broker создаётся ДО подсистем; раздаётся каждой (set_broker) до старта их потоков.
  // Если runtime уже поставил broker через стандартный контракт, используем его.
  if (broker_ != nullptr) {
    container->br = broker_;
  } else {
    container->owned_br = std::make_unique<::tile_frontier::core::broker>();
    container->br = container->owned_br.get();
  }

  set_frame_time(simul::frame_time_from_fps(bootstrap_->engine.main_fps));

  // стартовый размер фреймбуфера = размер из конфига (до создания окна); коллбэк ресайза уточнит.
  container->fb_width = std::max(bootstrap_->settings.window.width, 1u);
  container->fb_height = std::max(bootstrap_->settings.window.height, 1u);
}

std::unique_ptr<runtime_traits::bootstrap_type> runtime_traits::make_bootstrap() {
  return std::make_unique<bootstrap_type>();
}

std::unique_ptr<runtime_traits::broker_type> runtime_traits::make_broker(bootstrap_type&) {
  return std::make_unique<broker_type>();
}

std::unique_ptr<runtime_traits::main_type> runtime_traits::make_main(bootstrap_type& boot) {
  return std::make_unique<main_type>(&boot);
}

std::unique_ptr<runtime_traits::sound_type> runtime_traits::make_sound(bootstrap_type& boot) {
  const auto sound_ft = simul::frame_time_from_fps(boot.engine.sound_fps);
  if (boot.engine.sound_enabled) {
    return std::make_unique<sound_type>(sound_ft);
  }
  DE_LOG(catalogue::log_domain::main, flow, "main: sound disabled (sound_enabled=false), skipping sound subsystem");
  return nullptr;
}

std::unique_ptr<runtime_traits::render_type> runtime_traits::make_render(bootstrap_type& boot) {
  return simul::make_standard_render<render_type>(boot, "tile_frontier");
}

std::unique_ptr<runtime_traits::assets_type> runtime_traits::make_assets(bootstrap_type& boot) {
  const auto assets_ft = simul::frame_time_from_fps(boot.engine.assets_fps);
  return std::make_unique<assets_type>(assets_ft);
}

void runtime_traits::bind_systems(main_type& main, bootstrap_type&, sound_type* sound, render_type* render, assets_type* assets) {
  main.bind_systems(sound, render, assets);
}

runtime_traits::boot_config_type& runtime_traits::boot_config(bootstrap_type& boot) noexcept {
  return boot.engine;
}

runtime_traits::settings_type& runtime_traits::settings(bootstrap_type& boot) noexcept {
  return boot.settings;
}

bool runtime_traits::save_settings(bootstrap_type& boot) {
  return simul::save_settings(boot);
}

bool runtime_traits::reload_settings(bootstrap_type& boot) {
  return simul::reload_settings(boot);
}

void runtime_traits::settings_reloaded(main_type& main, bootstrap_type& boot) {
  simul::setup_logging(boot.settings.logging);
  main.set_frame_time(simul::frame_time_from_fps(boot.engine.main_fps));
}

void runtime_traits::after_workers_started(main_type& main) {
  main.after_workers_started();
}

size_t runtime_traits::main_wait_mcs(const main_type&) {
  return 0;
}

size_t runtime_traits::sound_wait_mcs(const bootstrap_type& boot, const sound_type& sound) {
  return simul::thread_start_gap(sound.frame_time(), boot.engine.thread_start_gap_divisor);
}

size_t runtime_traits::render_wait_mcs(const bootstrap_type& boot, const render_type& render) {
  return simul::thread_start_gap(render.frame_time(), boot.engine.thread_start_gap_divisor);
}

size_t runtime_traits::assets_wait_mcs(const bootstrap_type& boot, const assets_type& assets) {
  return simul::thread_start_gap(assets.frame_time(), boot.engine.thread_start_gap_divisor);
}

int runtime_traits::exit_code(const main_type& main) {
  return main.exit_code();
}

void simulation::bind_systems(sound_simulation* sound, render_simulation* render, assets_simulation* assets) {
  if (container == nullptr) return;
  container->sound_sim = sound;
  container->render_sim = render;
  container->assets_sim = assets;
  systems.sound = sound != nullptr;
  systems.render = render != nullptr;
  systems.assets = assets != nullptr;
}

int simulation::exit_code() const noexcept {
  return 0;
}

void simulation::after_workers_started() {
  if (container == nullptr) return;
  if (container->assets_sim == nullptr) utils::error{}("main: assets subsystem is required before startup resource binding");

  if (systems.sound) {
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
  if (bootstrap_->settings.window.create_on_start && systems.render && !bootstrap_->settings.render.headless) {
    simul::create_window_and_notify_render(*container, bootstrap_->settings, main_frame_time);
  }

  // три grass-текстуры → texture_slots 0,1,2 (порядок запроса = порядок слотов, т.к. assets грузит по очереди)
  const char* texture_resources[] = {
    "textures/grass",
    "textures/grass1_0",
    "textures/grass3",
    "textures/grad1", // градиент-маски + 4-цветная маска для стенсил-эффектов (Стадия 2)
    "textures/grad2",
    "textures/quad",
  };
  for (const char* res_id : texture_resources) {
    const auto tex_handle = container->assets_sim->resources()->handle(res_id);
    if (auto* tex = tex_handle.get<painter::gpu_texture_resource>()) {
      (void)tex;
      container->br->load_resource.try_push(command_load_resource{resource_ref::from_handle(tex_handle), static_cast<int32_t>(demiurg::state::hot)});
      container->startup_resources.push_back(resource_ref::from_handle(tex_handle)); // стартовый набор: от него зависит переход loading→game
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

  if (systems.assets) {
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
  container->cam.aspect = float(bootstrap_->settings.window.width) / float(std::max(bootstrap_->settings.window.height, 1u));

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
  //   app.play_sound(resource_handle [, {start=0..1, after=sound_handle}]) -> sound_handle
  //   app.play_sound{ resource=handle, start=0..1, after=sound_handle } -> sound_handle
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

    simul::install_resource_lua_bindings(
      L,
      env,
      bootstrap_->engine_resources.get(),
      container->assets_sim != nullptr ? container->assets_sim->resources() : nullptr
    );

    // первый аргумент — sol::object (resource_handle ИЛИ таблица-опции с полем resource/res), второй —
    // необязательная таблица-опции (когда ресурс задан первым аргументом). sol::object у НЕпоследнего
    // параметра безопаснее sol::optional (тот съедает не тот слот стека при nil).
    app.set_function("play_sound",
      [this](sol::object a, sol::optional<sol::table> b) -> sound_handle {
        if (!systems.sound) return sound_handle{};

        demiurg::resource_handle sound_res;
        sol::optional<sol::table> opts;
        if (a.is<sol::table>()) {
          opts = a.as<sol::table>();
          const sol::optional<demiurg::resource_handle> resource = (*opts)["resource"];
          const sol::optional<demiurg::resource_handle> res = (*opts)["res"];
          if (resource) sound_res = *resource;
          else if (res) sound_res = *res;
        } else if (a.is<demiurg::resource_handle>()) {
          sound_res = a.as<demiurg::resource_handle>();
          opts = b;
        }
        if (sound_res.get() == nullptr) return sound_handle{};

        command_sound_play play{};
        play.taskid = generate_task_id();
        play.after = SIZE_MAX;
        play.start = 0.0;
        if (opts) {
          play.start = std::clamp(opts->get_or("start", 0.0), 0.0, 1.0);
          const sol::optional<sound_handle> after = (*opts)["after"];
          if (after && after->valid()) play.after = after->value; // хэндл → секвенсинг
        }
        play.res = resource_ref::from_handle(sound_res);
        container->br->sound_play.try_push(play);
        // оптимистичная запись в ту же таблицу: пока play не доедет в публикацию (latency
        // 1-2 кадра), app.sound_state по ней вернёт 0, а не nil (deadline = окно старта).
        constexpr size_t startup_grace_frames = 30;
        container->sound_state.push_back({play.taskid, 0.0, container->sound_frame + startup_grace_frames});
        return sound_handle{play.taskid};
      });

    app.set_function("stop_sound", [this](const sound_handle& h) {
      if (!systems.sound || !h.valid()) return;
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

    // Общий API управления окном/вводом: quit_game/maximize/restore/fullscreen/resolution/actions.
    simul::install_window_lua_bindings(
      app,
      *container,
      bootstrap_->settings,
      systems.sound,
      [this]() { quit_requested.store(true, std::memory_order_release); }
    );

    // Смена звукового устройства: пере-создаём system2 через уже существующий канал recreate.
    app.set_function("set_sound_device", [this](const std::string& name) {
      if (!systems.sound || container->br == nullptr) return;
      container->br->recreate_sound.try_push(command_recreate_sound_system{name});
    });

    // картинка для UI (хост-мост к demiurg): app.image(resource_handle [, {region={x,y,w,h}}]) -> visage::image | nil.
    // Строит хендл из gpu_index+размера когда текстура usable() (на GPU), иначе nil.
    app.set_function("image", [this](sol::object resource, sol::optional<sol::table> opts) -> sol::object {
      auto& lua = container->ui->script_state();
      if (!resource.is<demiurg::resource_handle>()) return sol::nil;
      const auto handle = resource.as<demiurg::resource_handle>();
      auto* tex = handle.get<painter::gpu_texture_resource>();
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
      return std::string(simul::to_string(container->state));
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

  {
    sol::environment env = container->ui->script_env();
    sol::protected_function require = env["require"];
    const auto ret = require("ui/entry");
    if (!ret.valid()) {
      const sol::error err = ret;
      utils::error{}("visage: could not require entry module 'ui/entry': {}", err.what());
    }

    sol::object entry = ret.return_count() > 0 ? ret.get<sol::object>() : sol::nil;
    container->ui->set_entry_point(entry);
    DE_LOG(catalogue::log_domain::ui, flow, "visage: entry point loaded from demiurg resource 'ui/entry'");
  }

  // атласы шрифтов → GPU тем же путём, что и текстуры: просим ассеты довести до final_state.
  // Слоты определятся динамически (после grass-текстур); main прочитает gpu_index по hot.
  if (systems.assets) {
    for (const auto& [h, logged] : container->ui_fonts) {
      auto* fr = h.get<visage::font_resource>();
      if (fr == nullptr) continue;
    // final_state() = 3 (font_resource много-шаговый); CPU-уровни (0..2) уже пройдены в setup_visage,
    // ассетам остаётся довести 2→3 (GPU). target=final_state(), не state::hot (иначе стоп на MSDF).
      const resource_ref font_ref = resource_ref::from_handle(h);
      container->br->load_resource.try_push(command_load_resource{font_ref, fr->final_state()});
    container->startup_resources.push_back(font_ref);
      DE_LOG(catalogue::log_domain::ui, flow, "main: requested font atlas '{}' -> ready (level {})", fr->id, fr->final_state());
  }
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
    simul::begin_main_frame(
      *container,
      time,
      systems.sound,
      systems.render,
      [this]() {
        auto* boot_font = container->ui_font_h.get<visage::font_resource>();
        return boot_font == nullptr || boot_font->usable();
      },
      [this]() { return loading_complete(*container); },
      [this](const uint32_t w, const uint32_t h) {
        container->cam.aspect = float(w) / float(std::max(h, 1u));
      }
    );
  }

  // Демо п.2/п.3: периодически переключаем активный render graph graph<->menu_graph, чтобы проверить
  // мгновенный своп без пересоздания ресурсов. Управляется render.demo_graph_toggle_ms (0 ⇒ выкл).
  if (container != nullptr && systems.render) {
    const auto& rc = bootstrap_->settings.render;
    if (rc.demo_graph_toggle_ms > 0 && !rc.menu_graph.empty() && rc.menu_graph != rc.graph) {
      const uint64_t period = std::max<uint64_t>(1,
        uint64_t(rc.demo_graph_toggle_ms) * uint64_t(bootstrap_->settings.simulation.main_fps) / 1000ull);
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
  if (container) {
    for (auto& [h, done] : container->ui_fonts) {
      if (done) continue;
      auto* fr = h.get<visage::font_resource>();
      if (fr == nullptr || !fr->usable()) continue;
      const uint32_t slot = fr->gpu_index;
      if (auto* font = fr->font()) font->set_texture_id(slot);
      DE_LOG(catalogue::log_domain::ui, flow, "main: font atlas '{}' reached GPU (usable), texture slot={}", fr->id, slot);
      done = true;
    }
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
    if (systems.render) {
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
    if (container->state == simul::app_state::game && systems.render && container->br) {
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
  if (container && container->state == simul::app_state::game && container->actors_batch.valid()) {
    const auto t0 = std::chrono::steady_clock::now();
    container->actors_last_metrics = container->actors.update(
      float(time) / float(utils::global_time_resolution),
      container->actors_batch,
      *bootstrap_->pool
    );
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t update_us = uint64_t(std::max<int64_t>(utils::count_mcs(t0, t1), 0));

    // Снапшот акторов — latest-wins мейлбокс: заполняем слот-продюсер НА МЕСТЕ (bytes/ids
    // переиспользуют ёмкость между кадрами), затем publish. Строим только когда рендер включён.
    if (systems.render && container->br) {
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
    if (systems.sound) {
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

    if (bootstrap_->settings.metrics.enabled) {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - container->metrics_last_log).count();
      if (elapsed_ms >= bootstrap_->settings.metrics.log_interval_ms && container->metrics_frames != 0) {
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

  if (container) {
    simul::run_visage_frame(*container, time, systems.render, [this]() {
      container->ui->set_env_number("tf_main_fps", container->ui_main_fps);
      container->ui->set_env_number("tf_actor_count", double(container->actors_last_metrics.actors));
      container->ui->set_env_number("tf_actor_intents", double(container->actors_last_metrics.intents));
      container->ui->set_env_number("tf_actor_instances", double(container->actors_last_metrics.instances));
      container->ui->set_env_number("tf_actor_ticks", double(container->actors_last_metrics.ticks));
      container->ui->set_env_number("tf_intents_per_sec", container->ui_intents_per_sec);
      container->ui->set_env_number("tf_instances_per_sec", container->ui_instances_per_sec);
      container->ui->set_env_number("tf_actor_update_avg_us", container->ui_actor_update_avg_us);

      // sound-state merge пока остаётся здесь: это API звукового плеера tile_frontier,
      // а не базовая обработка visage кадра.
      container->sound_frame += 1;
      if (const command_sound_state* msg = container->br ? container->br->sound_state.consume() : nullptr) {
        auto& cur = container->sound_state;
        auto& next = container->sound_state_next;
        next.clear();
        for (const auto& s : msg->sounds) next.push_back({s.taskid, s.progress, 0});
        for (const auto& e : cur) {
          if (e.deadline == 0) continue;
          if (e.deadline < container->sound_frame) continue;
          bool in_pub = false;
          for (const auto& s : msg->sounds) if (s.taskid == e.taskid) { in_pub = true; break; }
          if (!in_pub) next.push_back(e);
        }
        std::swap(cur, next);
      }
    });
  }

  // app.send_event требует функцию которая
  // получит тип системы и вернет id
  // этот id передается в системы и используется потом чтобы понять что происходит
}

}
}
