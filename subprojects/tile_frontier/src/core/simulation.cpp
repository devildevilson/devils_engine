#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <span>

#include <devils_engine/catalogue/introspection.h> // catalogue::statistics_store (perf UI)
#include <devils_engine/catalogue/logging.h>       // доменное логгирование (DE_LOG) + init_logging
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/input/core.h>
#include <devils_engine/painter/gpu_texture_resource.h>
#include <devils_engine/simul/loading_runtime.h>
#include <devils_engine/simul/lua_app_bindings.h> // движковые app-биндинги: звук/image/lifecycle
#include <devils_engine/simul/lua_resource_bindings.h>
#include <devils_engine/simul/lua_script_resource.h>
#include <devils_engine/simul/pause.h>
#include <devils_engine/simul/startup_resources.h>
#include <devils_engine/simul/window_runtime.h>
#include <devils_engine/sound/sound_resource.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/prng.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/time-utils.hpp>
#include <devils_engine/visage/font.h>
#include <devils_engine/visage/font_resource.h>
#include <devils_engine/visage/image.h>
#include <devils_engine/visage/system.h>
#include <gtl/phmap.hpp>

#include "actor_simulation.h"
#include "app_config_resource.h"
#include "assets_system.h"
#include "broker.h"
#include "config.h"
#include "fsm_resource.h" // fsm_resource::transitions() для mood FSM из конфига
#include "global_ubo.h"
#include "goap_resource.h" // goap_resource::config() для GOAP из конфига
#include "messages.h"
#include "prefab_resource.h" // prefab_resource::text() для префабов из конфига
#include "render_system.h"
#include "runtime.h"
#include "script_resource.h" // script_resource::program() для скрипт-предиката actor.is_hungry
#include "texture_set.h"
#include "tile_batch.h"
#include "tile_map.h"

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

constexpr size_t main_frame_time = utils::round(double(utils::global_time_resolution) * (1.0 / 20.0));
constexpr uint32_t initial_actor_count = 4096;
//constexpr uint32_t initial_actor_count = 64000;

// sound_handle и ui_sound_state_entry — движковые типы звукового плеера (simul::lua_app_bindings.h).

// тут что? все другие системы + потоки для них + тред пул
// кеш?
// Наследует движковый simul::standard_loading_state: lifecycle + машина runtime-состояния (allowlist,
// pending/committed вид, generation) живут в базе, а проектные поля (мир/сцена/чанки/актёры) — здесь.
struct simulation_init : public simul::standard_loading_state {
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
  // value() уходит в ui->update как rng_state-сид. Engine clock идёт всегда и является UI timestamp;
  // game clock идёт только в app_state::game (те же ворота, что actor/gameplay systems).
  utils::xoshiro256starstar::state ui_rng = utils::xoshiro256starstar::init(utils::string_hash("visage_ui"));
  utils::timelines clocks;
  utils::calendar_clock calendar;
  simul::pause_state pause;

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
  texture_set textures; // текстуры карты, собранные по префиксу пути
  tile_grid grid;       // квадратная сетка тайлов
  uint32_t chunk_size = 16;
  uint32_t chunks_x = 4;
  uint32_t chunks_y = 4;
  std::vector<bool> chunks_requested;
  std::vector<bool> chunks_loaded;
  uint32_t chunks_loaded_count = 0;
  camera2d cam;     // орто top-down камера
  tile_batch batch; // продюсер инстансов видимого среза
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

  // lifecycle + машина runtime-состояния (allowlist/pending/committed/generation) — в базе
  // simul::standard_loading_state. main держит stable handles startup-набора, поэтому прогресс
  // считается прямо тут, без публикации из ассетов.

  std::vector<std::string> sound_devices;
  std::atomic_bool sound_devices_ready = false;
  bool sound_devices_requested = false;
  bool sound_devices_logged = false;
  bool sound_recreate_test_sent = false;
  size_t tick = 0;

  // Единая таблица состояния звука для UI (app.sound_state). Звуковой поток ПУШИТ полный слепок
  // (command_sound_state, latest-wins мейлбокс); consume в update СЛИВАЕТ его с оптимистичными
  // записями, добавленными в app.play_sound (см. ui_sound_state_entry.deadline).
  std::vector<simul::ui_sound_state_entry> sound_state;
  std::vector<simul::ui_sound_state_entry> sound_state_next;
  size_t sound_frame = 0; // счётчик main-кадров (для дедлайна оптимистичных записей)

  simulation_init() : window(nullptr), monitor(nullptr) {}
};

// Прогресс загрузки стартового набора [0,1]: доля usable()-ресурсов + применённых mock-чанков.
// main держит resource_ref на стартовый набор, поэтому прогресс считается локально (без публикации
// из ассетов — см. водораздел; ассетный push-слепок оставлен на будущее).
static double loading_progress(const simulation_init& c) {
  const size_t total = c.startup_resources.size() + c.chunks_loaded.size();
  if (total == 0) {
    return 1.0;
  }
  size_t done = c.chunks_loaded_count;
  for (const auto& ref : c.startup_resources) {
    auto* r = ref.get();
    if (r != nullptr && r->usable()) {
      ++done;
    }
  }
  return double(done) / double(total);
}

simulation::simulation(runtime_bootstrap* boot) noexcept
  : simul::game_host<simulation, runtime_bootstrap, ::tile_frontier::core::broker>(boot, main_frame_time) {}

simulation::~simulation() noexcept {
  if (!container) {
    return;
  }
  simul::destroy_window_runtime(*container);
}

simulation_init& simulation::state() {
  if (!container) {
    utils::error{}("simulation: state accessed before init()");
  }
  return *container;
}

const simulation_init& simulation::state() const {
  if (!container) {
    utils::error{}("simulation: state accessed before init()");
  }
  return *container;
}

// main_system/advancer виртуали — тонкие форвардеры в generic game_host (определены в этом TU, где
// simulation_init полный, поэтому inline-логика host_* инстанцируется тут; см. simulation.h).
void simulation::init() {
  host_init();
}
bool simulation::stop_predicate() const {
  return host_stop_predicate();
}
void simulation::update(const size_t time) {
  host_update(time);
}
void simulation::workers_started() {
  host_workers_started();
}
void simulation::runtime_settings_reloaded() {
  host_runtime_settings_reloaded();
}

// Visage получает только шрифты из активного runtime state resources. К этому моменту assets loader
// уже подготовил их метрики; main не запускает ресурсные переходы напрямую.
static void setup_visage(simulation_init& c) {
  constexpr std::string_view default_font_id = "fonts/crimson.roman";

  auto* reg = c.assets_sim != nullptr ? c.assets_sim->resources() : nullptr;
  if (reg == nullptr) {
    utils::error{}("visage: assets registry is not initialized (fonts live there)");
  }

  for (const auto& ref : c.ui_resources) {
    const auto h = ref.handle;
    auto* fr = h.get<visage::font_resource>();
    if (fr == nullptr) {
      continue;
    }
    if (fr->font() == nullptr) {
      utils::error{}("visage: font resource '{}' produced no font metrics", fr->id);
    }
    c.ui_fonts.emplace_back(h, false);
    DE_LOG(catalogue::log_domain::ui, flow, "visage: font '{}' CPU-ready ({} glyphs)", fr->id, fr->font()->glyphs.size());
  }

  c.ui_font_h = reg->handle(default_font_id);
  auto* def = c.ui_font_h.get<visage::font_resource>();
  if (def == nullptr || def->font() == nullptr) {
    utils::error{}("visage: default ui font '{}' not found in assets registry", default_font_id);
  }

  c.ui.reset(new visage::system(def->font())); // visage заимствует метрики; байты атласа ждут GPU-шага
  DE_LOG(catalogue::log_domain::ui, flow, "visage: system created (default font '{}', {} fonts total)", default_font_id, c.ui_fonts.size());
}

// project_init зовётся движковым game_host в начале init(): аллоцирует проектное состояние (его тип
// известен только тут), резолвит конкретные worker-подсистемы и ставит проектный календарь. Broker,
// frame_time, game_scale, presence и стартовый fb-размер настраивает сам game_host после этого хука.
void simulation::project_init() {
  container.reset(new simulation_init);
  auto& c = *container;
  c.sound_sim = runtime_system<sound_simulation>();
  c.render_sim = runtime_system<render_simulation>();
  c.assets_sim = runtime_system<assets_simulation>();
  c.calendar = make_calendar_clock(bootstrap()->settings.time);
}

devils_engine::demiurg::resource_system* simulation::asset_registry() {
  auto& c = state();
  return c.assets_sim != nullptr ? c.assets_sim->resources() : nullptr;
}

simul::worker_systems<runtime_traits::broker_type> runtime_traits::make_workers(bootstrap_type& boot) {
  return simul::make_standard_workers<render_simulation, assets_simulation, sound_simulation>(boot, "tile_frontier");
}

void simulation::project_settings_reloaded() {
  // setup_logging / frame_time / game_scale уже выполнил движковый game_host; проектных настроек,
  // требующих реакции на runtime reload, пока нет (calendar source/policy намеренно не трогаем).
}

void simulation::start_project_ui() {
  auto& c = state();

  simul::standard_commit_runtime_state(c); // pending(target) -> committed(active) вид
  c.ui_fonts.clear();
  c.ui_font_h = {};

  // интерфейс (visage) в главном потоке
  setup_visage(c);

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
    auto* cptr = &c;
    auto& L = c.ui->script_state();
    sol::environment env = c.ui->script_env();
    sol::table app = env["app"].get_or_create<sol::table>(); // общий namespace хост-биндингов

    // Движковые биндинги регистрирует ДВИЖОК через simul-хелперы: ресурсы, звук, окно/ввод, картинка,
    // lifecycle/pause/logging. Проектные (loading_progress по чанкам, perf_stats) — ниже.
    simul::install_resource_lua_bindings(
      L,
      env,
      nullptr,
      c.assets_sim != nullptr ? c.assets_sim->resources() : nullptr,
      c.ui_resource_scope);
    simul::install_sound_lua_bindings(app, c, systems().sound);
    simul::install_window_lua_bindings(app, c, bootstrap()->settings, systems().sound, [this]() { request_quit(); });
    simul::install_image_lua_binding(app, c);
    simul::install_app_lifecycle_bindings(app, *this);

    // Проектный биндинг: прогресс загрузки (зависит от чанков мира — считается локально в проекте).
    app.set_function("loading_progress", [cptr]() -> double {
      return loading_progress(*cptr);
    });

    // perf-статистика фаз апдейта актора (catalogue). Актор-сим и UI — один поток, читаем напрямую.
    // Возвращает массив { name, avg, min, max, last, count, samples={...} }; samples — последние
    // замеры в хроно-порядке (для nk.plot). Round-trip значений в lua при 20fps main дешёв (~1%),
    // отдельного C++-пути рисования не нужно.
    app.set_function("perf_stats", [cptr]() -> sol::table {
      auto& c = *cptr;
      auto& lua = c.ui->script_state();
      sol::table out = lua.create_table();
      std::vector<uint64_t> samples;
      int32_t i = 0;
      core::actor_perf_statistics().for_each(
        [&](const catalogue::statistics_store::function_record& r) {
          sol::table e = lua.create_table();
          e["name"] = std::string(r.name);
          e["avg"] = r.average_mcs();
          e["min"] = double(r.min_mcs);
          e["max"] = double(r.max_mcs);
          e["last"] = double(r.last_mcs);
          e["count"] = double(r.call_count);
          r.ordered_samples(samples);
          sol::table s = lua.create_table(int32_t(samples.size()), 0);
          for (size_t k = 0; k < samples.size(); ++k) {
            s[k + 1] = double(samples[k]);
          }
          e["samples"] = s;
          out[++i] = e;
        });
      return out;
    });
  }

  {
    sol::environment env = c.ui->script_env();
    sol::protected_function require = env["require"];
    const auto ret = require(c.ui_entry_script);
    if (!ret.valid()) {
      const sol::error err = ret;
      utils::error{}("visage: could not require entry module '{}': {}", c.ui_entry_script, err.what());
    }

    sol::object entry = ret.return_count() > 0 ? ret.get<sol::object>() : sol::nil;
    c.ui->set_entry_point(entry);
    DE_LOG(catalogue::log_domain::ui, flow, "visage: entry point loaded from demiurg resource '{}'", c.ui_entry_script);
  }
}

// begin_project_loading зовётся движковым game_host после standard_begin_loading + создания окна:
// сюда — только проектная сцена (текстуры/звуки/чанки/сетка/камера/батчи/мозги/актёры).
void simulation::begin_project_loading() {
  auto& c = state();
  c.sound_by_name.clear();

  const char* texture_resources[] = {
    "textures/grass",
    "textures/grass1_0",
    "textures/grass3",
    "textures/grad1",
    "textures/grad2",
    "textures/quad",
  };
  for (const char* res_id : texture_resources) {
    const auto tex_handle = c.assets_sim->resources()->handle(res_id);
    if (tex_handle.get<painter::gpu_texture_resource>() != nullptr) {
      // Временный mock-scene manifest: scene-ресурсы видимы UI сразу, readiness проверяет сам UI.
      c.pending_ui_scope->grant(tex_handle);
      if (systems().render) {
        const resource_ref ref = resource_ref::from_handle(tex_handle);
        c.br->load_resource.try_push(command_load_resource{ref, static_cast<int32_t>(demiurg::state::hot)});
        c.startup_resources.push_back(ref);
        DE_LOG(catalogue::log_domain::resource, flow, "main: requested texture '{}' -> hot", res_id);
      }
    } else {
      utils::warn("main: texture resource '{}' not found in registry", res_id);
    }
  }

  const std::pair<const char*, const char*> named_sounds[] = {
    {"eating", "sounds/eating/freesound_community-chomp-chew-bite-102031"},
    {"fleeing", "sounds/fleeing/freesound_community-escaping-downstairs-104907"},
    {"walking", "sounds/walking/freesound_community-walking-46245"},
    {"ambient", "sounds/ambient/soundreality-ambient-spring-forest-323801"},
  };
  for (const auto& [name, res_id] : named_sounds) {
    const auto snd_handle = c.assets_sim->resources()->handle(res_id);
    if (snd_handle.get<sound::sound_resource>() == nullptr) {
      utils::warn("main: sound resource '{}' not found in registry", res_id);
      continue;
    }
    c.pending_ui_scope->grant(snd_handle);
    c.br->load_resource.try_push(command_load_resource{
      resource_ref::from_handle(snd_handle), static_cast<int32_t>(demiurg::state::warm)});
    c.sound_by_name.emplace(utils::string_hash(name), snd_handle);
  }
  DE_LOG(catalogue::log_domain::resource, flow, "main: requested {} sounds -> warm", c.sound_by_name.size());

  const uint32_t tex_count = c.textures.gather(*c.assets_sim->resources(), "textures/grass");
  DE_LOG(catalogue::log_domain::resource, flow, "main: gathered {} tile textures by 'textures/grass'", tex_count);

  c.grid.tile_size = 1.0f;
  c.grid.resize(c.chunks_x * c.chunk_size, c.chunks_y * c.chunk_size);
  c.chunks_requested.assign(size_t(c.chunks_x) * c.chunks_y, false);
  c.chunks_loaded.assign(size_t(c.chunks_x) * c.chunks_y, false);
  c.chunks_loaded_count = 0;
  c.chunks_logged = false;
  c.tiles_logged = false;
  c.actors_logged = false;

  if (systems().assets) {
    for (uint32_t cy = 0; cy < c.chunks_y; ++cy) {
      for (uint32_t cx = 0; cx < c.chunks_x; ++cx) {
        const size_t idx = size_t(cy) * c.chunks_x + cx;
        command_load_chunk cmd;
        cmd.generation = c.state_generation;
        cmd.x = int32_t(cx);
        cmd.y = int32_t(cy);
        cmd.size = c.chunk_size;
        cmd.textures.assign(c.textures.handles().begin(), c.textures.handles().end());
        c.br->load_chunk.try_push(std::move(cmd));
        c.chunks_requested[idx] = true;
      }
    }
    DE_LOG(catalogue::log_domain::gameplay, flow, "main: requested {} mock world chunks via assets", c.chunks_requested.size());
  }

  const glm::vec2 extent = c.grid.world_extent();
  c.cam.center = extent * 0.5f;
  c.cam.half_width = 8.0f;
  c.cam.aspect = float(bootstrap()->settings.window.width) / float(std::max(bootstrap()->settings.window.height, 1u));

  if (const auto r = c.batch.bind("v2ui1"); !r) {
    utils::error{}("tile_instance layout mismatch vs 'v2ui1': {} (attr {}, expected {}, actual {})",
                   instance_layout::match_error::to_string(r.error), r.where, r.expected, r.actual);
  }
  if (const auto r = c.actors_batch.bind("v2ui1c4v1"); !r) {
    utils::error{}("actor_instance layout mismatch vs 'v2ui1c4v1': {} (attr {}, expected {}, actual {})",
                   instance_layout::match_error::to_string(r.error), r.where, r.expected, r.actual);
  }

  // Конфиги «мозга» актора из tavl: синхронно доводим до usable (как startup/entry в begin_boot) и
  // передаём в слайс. Отсутствие ресурса ⇒ соответствующее поле nullptr ⇒ нативный/хардкод фолбэк.
  core::brain_config brains;
  std::vector<core::prefab_def> prefab_defs; // владелец текстов префабов на время init (brains.prefabs → сюда)
  if (auto* reg = c.assets_sim != nullptr ? c.assets_sim->resources() : nullptr) {
    if (auto* sr = reg->get<script_resource>("scripts/actor_is_hungry")) {
      while (!sr->usable()) {
        sr->load(utils::safe_handle_t{});
      }
      brains.is_hungry_program = sr->program();
      DE_LOG(catalogue::log_domain::gameplay, flow, "main: actor.is_hungry <- скрипт 'scripts/actor_is_hungry'");
    } else {
      utils::warn("main: скрипт 'scripts/actor_is_hungry' не найден в реестре — нативный is_hungry");
    }
    if (auto* fr = reg->get<fsm_resource>("fsm/actor")) {
      while (!fr->usable()) {
        fr->load(utils::safe_handle_t{});
      }
      brains.fsm_transitions = &fr->transitions();
      DE_LOG(catalogue::log_domain::gameplay, flow, "main: mood FSM <- конфиг 'fsm/actor' ({} переходов)", fr->transitions().size());
    } else {
      utils::warn("main: конфиг 'fsm/actor' не найден в реестре — хардкод FSM");
    }
    if (reg->get<goap_resource>("goap/actor") != nullptr) {
      brains.goap = std::make_shared<goap_config>(resolve_goap_config(*reg, "goap/actor"));
      DE_LOG(catalogue::log_domain::gameplay, flow, "main: GOAP <- конфиг 'goap/actor' ({} метрик, {} действий)",
             brains.goap->metrics.size(), brains.goap->actions.size());
    } else {
      utils::warn("main: конфиг 'goap/actor' не найден в реестре — хардкод GOAP");
    }
    // Префабы из prefab/*.tavl: собираем имя+текст всех prefab_resource → в brain_config (слайс
    // регистрирует специи компонентов в C++ и скармливает текст в prefab_registry). Потребляется в
    // init (текст копируется), поэтому prefab_defs может жить локально до конца init.
    std::vector<core::prefab_resource*> prefab_res;
    reg->filter<core::prefab_resource>("prefab/", prefab_res);
    for (auto* pr : prefab_res) {
      while (!pr->usable()) {
        pr->load(utils::safe_handle_t{});
      }
      prefab_defs.push_back(core::prefab_def{std::string(pr->prefab_name()), std::string(pr->text())});
    }
    if (!prefab_defs.empty()) {
      brains.prefabs = &prefab_defs;
      DE_LOG(catalogue::log_domain::gameplay, flow, "main: prefab <- {} префабов из prefab/*.tavl", prefab_defs.size());
    } else {
      utils::warn("main: префабы 'prefab/*' не найдены в реестре — хардкод food");
    }
  }

  c.actors.init(
    initial_actor_count,
    glm::vec2{0.5f, 0.5f},
    glm::max(extent - glm::vec2{0.5f, 0.5f}, glm::vec2{0.5f, 0.5f}),
    std::max(tex_count, 1u),
    brains);
  c.metrics_last_log = std::chrono::steady_clock::now();
  DE_LOG(catalogue::log_domain::gameplay, flow, "main: spawned {} lightweight actors in aesthetics world", initial_actor_count);
}

// on_framebuffer_resize — коллбэк ресайза окна от движкового begin_main_frame: проекция/aspect
// считаются от ЖИВОГО размера фреймбуфера (иначе картинка искажается на реальном размере окна).
void simulation::on_framebuffer_resize(const uint32_t w, const uint32_t h) {
  state().cam.aspect = float(w) / float(std::max(h, 1u));
}

// project_loading_complete — проектное условие готовности (AND-ится движком к готовности startup-набора):
// все mock-чанки мира применены.
bool simulation::project_loading_complete() const {
  const auto& c = state();
  return c.chunks_loaded_count == c.chunks_loaded.size();
}

// update_gameplay — середина кадра. Движковый game_host уже сделал lifecycle tick, часы (pause/advance),
// begin_main_frame (окно/ввод/ресайз) и передал масштабированную дельту game-часов game_delta_ticks;
// run_visage_frame он вызовет после. Внутренние проверки фазы/паузы пока остаются здесь (шаг 4 их вынесет).
void simulation::update_gameplay(const size_t time, const uint64_t game_delta_ticks) {
  auto& c = state();

  // Демо п.2/п.3: периодически переключаем активный render graph graph<->menu_graph, чтобы проверить
  // мгновенный своп без пересоздания ресурсов. Управляется render.demo_graph_toggle_ms (0 ⇒ выкл).
  if (systems().render) {
    const auto& rc = bootstrap()->settings.render;
    if (rc.demo_graph_toggle_ms > 0 && !rc.menu_graph.empty() && rc.menu_graph != rc.graph) {
      const uint64_t period = std::max<uint64_t>(1,
                                                 uint64_t(rc.demo_graph_toggle_ms) * uint64_t(bootstrap()->settings.simulation.main_fps) / 1000ull);
      if (c.tick % period == 0) {
        const bool to_menu = (c.tick / period) % 2 == 1;
        if (c.br) {
          c.br->set_active_graph.write_slot().name = to_menu ? rc.menu_graph : rc.graph;
          c.br->set_active_graph.publish();
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
    c.sound_devices_requested &&
    !c.sound_devices_logged &&
    c.sound_devices_ready.load(std::memory_order_acquire)) {
    DE_LOG(catalogue::log_domain::sound, flow, "main: sound playback devices count {}", c.sound_devices.size());
    for (size_t i = 0; i < c.sound_devices.size(); ++i) {
      DE_LOG(catalogue::log_domain::sound, flow, "main: sound device[{}] '{}'", i, c.sound_devices[i]);
    }
    c.sound_devices_logged = true;
  }

  // (бывший тест test.mp3 удалён: test.mp3 больше нет, звук теперь грузится именованным набором
  //  и играется по событиям через мост sim→sound ниже + из UI в фазе D-UI.)

  // атлас шрифта доехал на GPU: фиксируем слот в шрифте (nuklear зашьёт его в texture.id
  // draw-команд текста; шейдер UI по нему сэмплит атлас). gpu_index записан рендером.
  for (auto& [h, done] : c.ui_fonts) {
    if (done) {
      continue;
    }
    auto* fr = h.get<visage::font_resource>();
    if (fr == nullptr || !fr->usable()) {
      continue;
    }
    const uint32_t slot = fr->gpu_index;
    if (auto* font = fr->font()) {
      font->set_texture_id(slot);
    }
    DE_LOG(catalogue::log_domain::ui, flow, "main: font atlas '{}' reached GPU (usable), texture slot={}", fr->id, slot);
    done = true;
  }

  if (c.br) {
    command_chunk_loaded cmd{};
    while (c.br->chunk_loaded.try_pop(cmd)) {
      if (cmd.generation != c.state_generation) {
        DE_LOG(catalogue::log_domain::gameplay, flow,
               "main: dropped stale chunk ({},{}) generation={} current={}",
               cmd.x, cmd.y, cmd.generation, c.state_generation);
        continue;
      }
      tile_chunk chunk;
      chunk.coord = chunk_coord{cmd.x, cmd.y};
      chunk.size = cmd.size;
      chunk.tiles.resize(cmd.textures.size());
      for (size_t i = 0; i < cmd.textures.size(); ++i) {
        chunk.tiles[i].texture = cmd.textures[i];
      }

      apply_chunk(c.grid, chunk);

      if (cmd.x >= 0 && cmd.y >= 0 && uint32_t(cmd.x) < c.chunks_x && uint32_t(cmd.y) < c.chunks_y) {
        const size_t idx = size_t(cmd.y) * c.chunks_x + size_t(cmd.x);
        if (!c.chunks_loaded[idx]) {
          c.chunks_loaded[idx] = true;
          c.chunks_loaded_count += 1;
        }
      }
    }

    if (!c.chunks_logged && c.chunks_loaded_count == c.chunks_loaded.size()) {
      DE_LOG(catalogue::log_domain::gameplay, flow, "main: all {} mock world chunks loaded", c.chunks_loaded_count);
      c.chunks_logged = true;
    }
  }

  // --- пайплайн тайловой карты: фрустум камеры -> срез сетки -> инстансы -> сообщение ---
  if (c.batch.valid()) {
    // собираем сообщение в рендер: метаданные + упакованные байты инстансов ("v2ui1").
    const glm::mat4 vp = c.cam.view_proj();

    // Контракт записи в буфер: шлём общий UBO (view_proj + ui_proj + misc) в host-visible
    // camera_buffer. ВСЕГДА (не только в game): ui_proj нужен UI на splash/loading тоже.
    // ui_proj — ortho «пиксели окна -> clip» (Vulkan: y вниз, начало слева-сверху). misc=screen_size/px_range.
    if (systems().render) {
      // ЖИВОЙ размер фреймбуфера, а не статичный config (иначе проекция искажается на реальном
      // размере окна). cam.aspect уже обновлён коллбэком ресайза, так что и view_proj корректен.
      const float w = float(std::max(c.fb_width, 1u));
      const float h = float(std::max(c.fb_height, 1u));

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
      if (c.br) {
        c.br->write_buffer.write(camera_buffer_hash,
                                 std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&ubo), sizeof(global_ubo_t)));
      }
    }

    // Тайлы карты публикуем ТОЛЬКО в game (шаг 3d): на splash/loading карта не рисуется, поэтому
    // ничего не мигает и null-текстуры не вылезают (стартовый набор уже usable() к моменту game).
    // Срез сетки строим тут же (только когда реально публикуем — не тратим CPU на splash/loading).
    if (c.lifecycle.phase() == simul::app_state::game && systems().render && c.br) {
      const tile_span span = visible_tiles(c.cam, c.grid, 1.0f);
      c.batch.build(c.grid, span, c.textures);

      auto& slot = c.br->draw_tiles.write_slot();
      std::memcpy(slot.view_proj.data(), &vp[0][0], sizeof(float) * 16);
      slot.count = c.batch.count();
      slot.stride = tile_batch::stride();
      slot.bytes.resize(size_t(slot.count) * slot.stride);
      c.batch.blit(std::span<uint8_t>(slot.bytes));

      if (!c.tiles_logged) {
        DE_TRACE(catalogue::log_domain::gameplay,
                 "tile slice [{},{})x[{},{}) = {} instances, {} B/inst, {} B payload",
                 span.x0, span.x1, span.y0, span.y1, slot.count, slot.stride, slot.bytes.size());
        c.tiles_logged = true;
      }

      c.br->draw_tiles.publish();
    }
  }

  // --- actor simulation slice: simple AI -> move intents -> aesthetics components -> GPU batch ---
  // Только в game (шаг 3d): на splash/loading акторов не считаем и не публикуем.
  if (c.lifecycle.phase() == simul::app_state::game &&
      !c.pause.paused(simul::pause_domain::gameplay) && c.actors_batch.valid()) {
    const auto t0 = std::chrono::steady_clock::now();
    c.actors_last_metrics = c.actors.update(
      float(game_delta_ticks) / float(utils::global_time_resolution),
      c.actors_batch,
      *bootstrap()->pool);
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t update_us = uint64_t(std::max<int64_t>(utils::count_mcs(t0, t1), 0));

    // Снапшот акторов — latest-wins мейлбокс: заполняем слот-продюсер НА МЕСТЕ (bytes/ids
    // переиспользуют ёмкость между кадрами), затем publish. Строим только когда рендер включён.
    if (systems().render && c.br) {
      auto& slot = c.br->draw_actors.write_slot();
      slot.count = c.actors_batch.count();
      slot.stride = actor_batch::stride();
      slot.sim_frame_time = time;
      slot.bytes.resize(size_t(slot.count) * slot.stride);
      c.actors_batch.blit(std::span<uint8_t>(slot.bytes));
      slot.ids.assign(c.actors_batch.ids().begin(), c.actors_batch.ids().end());

      if (!c.actors_logged) {
        DE_TRACE(catalogue::log_domain::gameplay,
                 "actor slice {} actors, {} intents, {} instances, {} B/inst, {} B payload",
                 c.actors_last_metrics.actors,
                 c.actors_last_metrics.intents,
                 slot.count,
                 slot.stride,
                 slot.bytes.size());
        c.actors_logged = true;
      }

      c.br->draw_actors.publish();
    }

    // презентационный мост sim→sound: эмиты звука (вход в состояние FSM) → звуковой актор.
    // Куллинг по близости к слушателю (камере) + кап на тик (ограничение голосов). Звук
    // эфемерен и НЕ реплицируется — здесь решается лишь что РЕАЛЬНО проиграть.
    if (systems().sound) {
      const auto emits = c.actors.sound_events();
      const glm::vec2 listener = c.cam.center;
      const float audible = c.cam.half_width * 1.5f;
      const float audible2 = audible * audible;
      constexpr uint32_t max_sounds_per_tick = 8;
      uint32_t sent = 0;
      for (const auto& e : emits) {
        if (sent >= max_sounds_per_tick) {
          break;
        }
        const glm::vec2 d = e.pos - listener;
        if (d.x * d.x + d.y * d.y > audible2) {
          continue;
        }
        // резолв хеш-имя события → demiurg-ресурс; неизвестный звук пропускаем
        const auto snd_it = c.sound_by_name.find(e.name);
        if (snd_it == c.sound_by_name.end()) {
          continue;
        }
        command_sound_play play{};
        play.taskid = generate_task_id();
        play.after = SIZE_MAX; // без секвенсинга
        play.res = resource_ref::from_handle(snd_it->second);
        play.start = 0.0;
        c.br->sound_play.try_push(play);
        ++sent;
        DE_TRACE(catalogue::log_domain::sound, "sim-sound send task={} at ({:.1f},{:.1f})", play.taskid, e.pos.x, e.pos.y);
      }
      if (sent > 0) {
        DE_LOG(catalogue::log_domain::sound, flow, "sim-sounds sent {} (of {} emits)", sent, emits.size());
      }
    }

    c.metrics_frames += 1;
    c.metrics_actor_ticks += 1;
    c.metrics_intents += c.actors_last_metrics.intents;
    c.metrics_instances += c.actors_last_metrics.instances;
    c.metrics_actor_update_us += update_us;

    if (bootstrap()->settings.metrics.enabled) {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.metrics_last_log).count();
      if (elapsed_ms >= bootstrap()->settings.metrics.log_interval_ms && c.metrics_frames != 0) {
        const double seconds = double(elapsed_ms) / 1000.0;
        const double fps = double(c.metrics_frames) / seconds;
        const double intent_rate = double(c.metrics_intents) / seconds;
        const double instance_rate = double(c.metrics_instances) / seconds;
        const double avg_actor_us = double(c.metrics_actor_update_us) / double(c.metrics_actor_ticks);
        c.ui_main_fps = fps;
        c.ui_intents_per_sec = intent_rate;
        c.ui_instances_per_sec = instance_rate;
        c.ui_actor_update_avg_us = avg_actor_us;
        DE_LOG(catalogue::log_domain::main, flow,
               "metrics: main_fps={:.1f}, actors={}, intents/s={:.0f}, actor_instances/s={:.0f}, actor_update_avg_us={:.1f}",
               fps,
               c.actors_last_metrics.actors,
               intent_rate,
               instance_rate,
               avg_actor_us);

        c.metrics_last_log = now;
        c.metrics_frames = 0;
        c.metrics_actor_ticks = 0;
        c.metrics_intents = 0;
        c.metrics_instances = 0;
        c.metrics_actor_update_us = 0;
      }
    }
  }

}

// on_visage_before_update — before_update-хук визажа (движковый game_host сам зовёт run_visage_frame):
// проектные env-числа UI + слияние оптимистичного sound_state перед кадром интерфейса. sound-state
// merge остаётся тут: это API звукового плеера tile_frontier, а не базовая обработка visage кадра.
void simulation::on_visage_before_update() {
  auto& c = state();
  c.ui->set_env_number("tf_main_fps", c.ui_main_fps);
  c.ui->set_env_number("tf_actor_count", double(c.actors_last_metrics.actors));
  c.ui->set_env_number("tf_actor_intents", double(c.actors_last_metrics.intents));
  c.ui->set_env_number("tf_actor_instances", double(c.actors_last_metrics.instances));
  c.ui->set_env_number("tf_actor_ticks", double(c.actors_last_metrics.ticks));
  c.ui->set_env_number("tf_intents_per_sec", c.ui_intents_per_sec);
  c.ui->set_env_number("tf_instances_per_sec", c.ui_instances_per_sec);
  c.ui->set_env_number("tf_actor_update_avg_us", c.ui_actor_update_avg_us);

  // Движковое слияние оптимистичного sound_state с публикацией звукового треда.
  simul::advance_sound_state(c);
}

} // namespace core
} // namespace tile_frontier
