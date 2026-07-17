#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <span>

#include <devils_engine/bindings/lua_header.h>
#include <devils_engine/catalogue/introspection.h> // catalogue::statistics_store (perf UI)
#include <devils_engine/catalogue/logging.h>       // доменное логгирование (DE_LOG) + init_logging
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/painter/gpu_texture_resource.h>
#include <devils_engine/simul/loading_runtime.h>
#include <devils_engine/simul/window_runtime.h>
#include <devils_engine/sound/sound_resource.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/time-utils.hpp>
#include <gtl/phmap.hpp>

#include "actor_simulation.h"
#include "app_config_resource.h"
#include "assets_system.h"
#include "brain_config_loader.h"
#include "broker.h"
#include "config.h"
#include "global_ubo.h"
#include "messages.h"
#include "render_system.h"
#include "runtime.h"
#include "texture_set.h"
#include "tile_batch.h"
#include "tile_map.h"
#include "world_scene_resource.h"

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
// Общий main/game-thread runtime state (окно/UI/часы/pause/broker/sound/loading) живёт в
// simul::standard_game_state. Здесь остаются только ссылки на project workers и состояние мира.
struct simulation_init : public simul::standard_game_state<broker> {
  assets_simulation* assets_sim = nullptr;

  // Звуки — demiurg-ресурсы в потоке ассетов. main держит name_hash → stable handle (для резолва в
  // command_sound_play из UI/геймплея) и запрашивает их до warm. Сам звук-актор ресурсы не хранит.
  gtl::flat_hash_map<uint64_t, demiurg::resource_handle> sound_by_name;

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
};

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

// project_init зовётся движковым game_host в начале init(): аллоцирует проектное состояние (его тип
// известен только тут), резолвит нужный проекту assets worker и ставит проектный календарь. Broker,
// frame_time, game_scale, presence и стартовый fb-размер настраивает сам game_host после этого хука.
void simulation::project_init() {
  container.reset(new simulation_init);
  auto& c = *container;
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

// Стандартный UI lifecycle и engine bindings ставит game_host. Проект добавляет только API своего
// gameplay slice; require(entry) host выполнит после этого hook.
void simulation::register_project_ui_bindings() {
  auto& c = state();
  auto* cptr = &c;
  sol::environment env = c.ui->script_env();
  sol::table app = env["app"].get_or_create<sol::table>();

  // Проектный биндинг: perf-статистика фаз апдейта актора (catalogue). Актор-сим и UI — один поток.
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

// begin_project_loading зовётся движковым game_host после standard_begin_loading + создания окна:
// сюда — только проектная сцена (текстуры/звуки/чанки/сетка/камера/батчи/мозги/актёры).
void simulation::begin_project_loading() {
  auto& c = state();
  c.sound_by_name.clear();
  auto* scene_resource = c.pending_project_scene.get<world_scene_resource>();
  if (scene_resource == nullptr || !scene_resource->usable()) {
    utils::error{}("tile_frontier: scene manifest '{}' has no usable world descriptor", c.pending_scene);
  }
  const auto& scene = scene_resource->config();

  std::vector<demiurg::resource_handle> tile_textures;
  for (const auto& binding : c.pending_scene_resources) {
    if (binding.group == scene.tile_texture_group) {
      if (binding.resource.handle.get<painter::gpu_texture_resource>() == nullptr) {
        utils::error{}("tile_frontier: resource in tile texture group is not a GPU texture");
      }
      tile_textures.push_back(binding.resource.handle);
    } else if (binding.group == scene.sound_group && !binding.alias.empty()) {
      if (binding.resource.handle.get<sound::sound_resource>() == nullptr) {
        utils::error{}("tile_frontier: resource for sound alias '{}' is not a sound", binding.alias);
      }
      const auto [it, inserted] = c.sound_by_name.emplace(utils::string_hash(binding.alias), binding.resource.handle);
      if (!inserted) {
        utils::error{}("tile_frontier: duplicate sound alias '{}' in scene '{}'", binding.alias, c.pending_scene);
      }
    }
  }
  const uint32_t tex_count = c.textures.assign(tile_textures);
  if (tex_count == 0) {
    utils::error{}("tile_frontier: scene '{}' has no textures in group '{}'", c.pending_scene, scene.tile_texture_group);
  }
  DE_LOG(catalogue::log_domain::resource, flow,
         "main: scene manifest '{}' selected {} tile textures and {} named sounds",
         c.pending_scene, tex_count, c.sound_by_name.size());

  c.chunk_size = scene.chunk_size;
  c.chunks_x = scene.chunks_x;
  c.chunks_y = scene.chunks_y;
  c.grid.tile_size = scene.tile_size;
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
  c.cam.half_width = scene.camera_half_width;
  c.cam.aspect = float(bootstrap()->settings.window.width) / float(std::max(bootstrap()->settings.window.height, 1u));

  if (const auto r = c.batch.bind("v2ui1"); !r) {
    utils::error{}("tile_instance layout mismatch vs 'v2ui1': {} (attr {}, expected {}, actual {})",
                   instance_layout::match_error::to_string(r.error), r.where, r.expected, r.actual);
  }
  if (const auto r = c.actors_batch.bind("v2ui1c4v1"); !r) {
    utils::error{}("actor_instance layout mismatch vs 'v2ui1c4v1': {} (attr {}, expected {}, actual {})",
                   instance_layout::match_error::to_string(r.error), r.where, r.expected, r.actual);
  }

  if (c.assets_sim == nullptr || c.assets_sim->resources() == nullptr) {
    utils::error{}("tile_frontier: gameplay config requires the assets subsystem");
  }
  auto brains = core::load_required_brain_config(
    *c.assets_sim->resources(), scene.actor_script, scene.actor_fsm,
    scene.actor_goap, scene.prefab_prefix);
  DE_LOG(catalogue::log_domain::gameplay, flow,
         "main: required brain config <- script '{}', FSM '{}' ({} transitions), GOAP '{}' ({} metrics, {} actions), {} prefabs",
         scene.actor_script, scene.actor_fsm, brains.fsm_transitions->size(), scene.actor_goap,
         brains.goap->metrics.size(), brains.goap->actions.size(), brains.prefabs.size());

  c.actors.init(
    scene.actor_count,
    glm::vec2{0.5f, 0.5f},
    glm::max(extent - glm::vec2{0.5f, 0.5f}, glm::vec2{0.5f, 0.5f}),
    std::max(tex_count, 1u),
    brains);
  c.metrics_last_log = std::chrono::steady_clock::now();
  DE_LOG(catalogue::log_domain::gameplay, flow, "main: spawned {} lightweight actors in aesthetics world", scene.actor_count);
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

// Вклад проекта в общий прогресс загрузки: mock-чанки мира (движок добавит долю startup-ресурсов).
std::pair<std::size_t, std::size_t> simulation::project_loading_progress() const {
  const auto& c = state();
  return {c.chunks_loaded_count, c.chunks_loaded.size()};
}

// update_gameplay — середина кадра. Движковый game_host уже сделал lifecycle tick, часы (pause/advance),
// begin_main_frame (окно/ввод/ресайз) и передал масштабированную дельту game-часов game_delta_ticks;
// run_visage_frame он вызовет после. Проект только применяет готовый phase_gate к своим фазам.
void simulation::update_gameplay(const size_t time, const uint64_t game_delta_ticks, const simul::phase_gate& gate) {
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

  // (бывший тест test.mp3 удалён: test.mp3 больше нет, звук теперь грузится именованным набором
  //  и играется по событиям через мост sim→sound ниже + из UI в фазе D-UI.)

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

    // Тайлы карты публикуем ТОЛЬКО в game (ворота от движка): на splash/loading карта не рисуется,
    // поэтому ничего не мигает и null-текстуры не вылезают (стартовый набор уже usable() к моменту game).
    // Срез сетки строим тут же (только когда реально публикуем — не тратим CPU на splash/loading).
    if (gate.in_game && systems().render && c.br) {
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
  // Мутирующая gameplay-фаза: только когда движок открыл ворота run_gameplay (game И не на паузе).
  if (gate.run_gameplay && c.actors_batch.valid()) {
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

// on_visage_before_update — только project env-числа. Engine sound-state game_host уже слил.
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
}

} // namespace core
} // namespace tile_frontier
