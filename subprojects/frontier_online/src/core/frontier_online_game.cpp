#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <span>

#include <devils_engine/bindings/critical_error.h>
#include <devils_engine/bindings/lua_header.h>
#include <devils_engine/catalogue/introspection.h>
#include <devils_engine/catalogue/logging.h>
#include <devils_engine/input/events.h>
#include <devils_engine/simul/camera_input.h> // named actions camera_* — pan камеры
#include <devils_engine/painter/gpu_texture_resource.h>
#include <devils_engine/sound/sound_resource.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/log.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/time-utils.hpp>
#include <devils_engine/visage/system.h>

#include "core/brain_config_loader.h"
#include "core/causal_content.h"
#include "core/broker.h"
#include "core/messages.h"
#include "core/frontier_online_game.h"
#include "core/net_client.h"

namespace frontier_online {
namespace core {

using namespace devils_engine;

frontier_online_game::frontier_online_game() = default;
frontier_online_game::~frontier_online_game() = default;

void frontier_online_game::begin_scene(const scene_start_context& context) {
  sound_by_name_.clear();

  std::vector<demiurg::resource_handle> tile_textures;
  for (const auto& binding : context.resources) {
    if (binding.group == context.config.tile_texture_group) {
      if (binding.resource.handle.get<painter::gpu_texture_resource>() == nullptr) {
        utils::error{}("frontier_online: resource in tile texture group is not a GPU texture");
      }
      tile_textures.push_back(binding.resource.handle);
    } else if (binding.group == context.config.sound_group && !binding.alias.empty()) {
      if (binding.resource.handle.get<sound::sound_resource>() == nullptr) {
        utils::error{}("frontier_online: resource for sound alias '{}' is not a sound", binding.alias);
      }
      const auto [it, inserted] = sound_by_name_.emplace(
        utils::string_hash(binding.alias), binding.resource.handle);
      if (!inserted) {
        utils::error{}("frontier_online: duplicate sound alias '{}' in scene '{}'", binding.alias, context.scene_id);
      }
    }
  }

  const uint32_t texture_count = textures_.assign(tile_textures);
  if (texture_count == 0) {
    utils::error{}("frontier_online: scene '{}' has no textures in group '{}'",
                   context.scene_id, context.config.tile_texture_group);
  }
  DE_LOG(catalogue::log_domain::resource, flow,
         "main: scene manifest '{}' selected {} tile textures and {} named sounds",
         context.scene_id, texture_count, sound_by_name_.size());

  scene_generation_ = context.generation;
  assets_available_ = context.assets_available;
  camera_config_ = context.config.camera;
  camera_.half_width = camera_config_.half_width;
  camera_.aspect = float(context.viewport_width) / float(std::max(context.viewport_height, 1u));
  grid_.tile_size = context.config.tile_size;
  join_ = context.join;
  networked_ = !join_.endpoint.empty();
  net_world_built_ = false;
  if (networked_) {
    // Земля и бокс сессии придут от авторитета (объявление мира + checkpoint): сетку строит
    // update_network, когда ведомый загрузит мир. Свой chunks_x/y и world_seed здесь не годятся.
    build_world(context.messages, 0, 0, 0, 0, context.config.terrain_generator, false);
  } else {
    build_world(context.messages, context.config.chunk_size, context.config.chunks_x,
                context.config.chunks_y, context.config.world_seed,
                context.config.terrain_generator, context.assets_available);
  }

  if (const auto result = tile_batch_.bind("v2ui1"); !result) {
    utils::error{}("tile_instance layout mismatch vs 'v2ui1': {} (attr {}, expected {}, actual {})",
                   devils_engine::simul::instance_layout::match_error::to_string(result.error), result.where,
                   result.expected, result.actual);
  }
  if (const auto result = actor_batch_.bind("v2ui1c4v1"); !result) {
    utils::error{}("actor_instance layout mismatch vs 'v2ui1c4v1': {} (attr {}, expected {}, actual {})",
                   devils_engine::simul::instance_layout::match_error::to_string(result.error), result.where,
                   result.expected, result.actual);
  }

  auto brains = load_brain_config(context.asset_registry);
  DE_LOG(catalogue::log_domain::gameplay, flow,
         "main: brain config <- {} FSM brains, {} GOAP brains, {} prefabs", brains.fsms.size(),
         brains.goaps.size(), brains.prefabs.size());

  net_.reset();
  net_refused_ = false;
  net_shown_tick_ = 0;
  if (networked_) {
    // Мир придёт checkpoint'ом авторитета; локальный спавн только тратил бы память и время.
    reset_metrics();
    DE_LOG(catalogue::log_domain::gameplay, flow, "main: networked scene, joining {}",
           join_.endpoint);
    return;
  }

  actors_.init(
    context.config.actor_count,
    glm::vec2{0.5f, 0.5f},
    glm::max(world_extent_ - glm::vec2{0.5f, 0.5f}, glm::vec2{0.5f, 0.5f}),
    texture_count,
    brains,
    context.config.actor_prefab_cycle);
  reset_metrics();
  DE_LOG(catalogue::log_domain::gameplay, flow,
         "main: spawned {} lightweight actors in aesthetics world", context.config.actor_count);
}

void frontier_online_game::framebuffer_resized(const uint32_t width, const uint32_t height) noexcept {
  camera_.aspect = float(width) / float(std::max(height, 1u));
}

void frontier_online_game::build_world(broker& messages, const uint32_t chunk_size,
                                       const uint32_t chunks_x, const uint32_t chunks_y,
                                       const uint64_t world_seed, const std::string& generator,
                                       const bool request) {
  chunk_size_ = chunk_size;
  chunks_x_ = chunks_x;
  chunks_y_ = chunks_y;
  grid_.resize(chunks_x_ * chunk_size_, chunks_y_ * chunk_size_);
  chunks_loaded_.assign(size_t(chunks_x_) * chunks_y_, false);
  chunks_loaded_count_ = 0;
  chunks_logged_ = false;
  tiles_logged_ = false;
  actors_logged_ = false;

  ++world_build_;
  next_chunk_request_ = 0;
  chunk_request_count_ = request ? chunks_loaded_.size() : 0;
  chunk_request_seed_ = world_seed;
  chunk_request_generator_ = generator;
  if (request) {
    DE_LOG(catalogue::log_domain::gameplay, flow,
           "main: requesting {} world chunks from generator '{}' (seed {})",
           chunk_request_count_, generator, world_seed);
    request_chunks(messages);
  }

  world_extent_ = grid_world_extent(grid_);
  camera_.center = world_extent_ * 0.5f;
}

// Сетка сессии покрывает бокс авторитета целыми чанками: край бокса не обязан делиться на чанк.
void frontier_online_game::build_session_world(broker& messages, const session_world& world) {
  const float chunk_extent = float(world.chunk_size) * grid_.tile_size;
  if (world.chunk_size == 0 || !(chunk_extent > 0.0f) || !(world.max_x > 0.0f) ||
      !(world.max_y > 0.0f)) {
    utils::error{}("frontier_online: session world from the authority is unusable: chunk {} x "
                   "tile {}, box max ({}, {})",
                   world.chunk_size, grid_.tile_size, world.max_x, world.max_y);
  }
  const auto chunks = chunks_covering({world.max_x, world.max_y}, world.chunk_size, grid_.tile_size);
  build_world(messages, world.chunk_size, chunks.x, chunks.y, world.world_seed, world.generator,
              assets_available_);
  net_world_built_ = true;
  utils::info("frontier_online: session world {}x{} chunks of {} (box [{}, {}]x[{}, {}], "
              "generator '{}', seed {})",
              chunks_x_, chunks_y_, chunk_size_, world.min_x, world.max_x, world.min_y,
              world.max_y, world.generator, world.world_seed);
}

bool frontier_online_game::loading_complete() const noexcept {
  return chunks_loaded_count_ == chunks_loaded_.size();
}

std::pair<std::size_t, std::size_t> frontier_online_game::loading_progress() const noexcept {
  return {chunks_loaded_count_, chunks_loaded_.size()};
}

void frontier_online_game::begin_frame(const presentation_context& context) {
  request_chunks(context.messages);
  drain_loaded_chunks(context.messages, context.generation);
  if (context.render_available) {
    auto& state = context.messages.active_gameplay_state.write_slot();
    state.active = context.gate.run_gameplay;
    context.messages.active_gameplay_state.publish();
  }
  move_camera(context);
  collect_player_intents(context);
  publish_sound_listener(context.messages, context.sound_available);
}

void frontier_online_game::update_simulation(const simulation_context& context) {
  if (networked_) {
    update_network(context);
    return;
  }
  if (context.run_gameplay && actor_batch_.valid()) {
    update_actors(context);
  }
}

// Сетевой круг идёт и на локальной паузе: авторитет не ждёт, а отставший за удерживаемую
// историю участник отключается. Тик клиента сюда не входит — мир шагает bundles авторитета.
void frontier_online_game::update_network(const simulation_context& context) {
  if (!net_) {
    if (net_refused_) return;
    // Тот же корень модулей, что у причинного содержимого сервера: отпечаток считается по нему.
    net_ = net_client::connect(join_, project_modules_root(), context.pool);
    if (!net_) {
      // Причина уже в логе. Окно остаётся с пустым миром: локальный слайс в сетевой сцене не
      // создан, и переключаться на него нельзя.
      net_refused_ = true;
      return;
    }
  }
  net_->service();
  if (session_world layout; net_->layout(layout) && (!net_world_built_ || *net_world_ != layout)) {
    build_session_world(context.messages, layout);
    net_world_ = std::make_unique<session_world>(std::move(layout));
  }
  const auto* world = net_->world();
  if (world == nullptr || !actor_batch_.valid() || net_->tick() == net_shown_tick_) {
    return;
  }
  const auto begin = std::chrono::steady_clock::now();
  actor_batch_.read(world->ecs(), net_->tick());
  const auto end = std::chrono::steady_clock::now();
  net_shown_tick_ = net_->tick();
  publish_actor_sounds(context.messages, context.sound_available, world->sound_events());
  // Метрики показа: мир чужой, решения в нём считал ведомый, поэтому intents здесь не считаются.
  actors_last_metrics_ = actor_metrics{
    uint32_t(world->ecs().count<actor_position>()), 0u, actor_batch_.produced(),
    uint32_t(world->ecs().count<actor_eating>()), net_->tick()};
  metrics_actor_ticks_ += 1;
  metrics_instances_ += actor_batch_.produced();
  metrics_actor_update_us_ += uint64_t(std::max<int64_t>(utils::count_mcs(begin, end), 0));
}

void frontier_online_game::end_frame(const presentation_context& context) {
  publish_camera_and_tiles(context);
  publish_actor_snapshot(context);
  update_metrics(context.settings.metrics, context.gate.run_gameplay);
}

void frontier_online_game::collect_player_intents(const presentation_context& context) {
  if (!context.gate.run_gameplay) {
    return;
  }
  static const auto spawn_food = input::events::make_event_id("spawn_food");
  if (!input::events::check_event(spawn_food, input::event_state::click_mask)) {
    return;
  }

  const glm::vec2 point = glm::clamp(
    screen_to_world(
      camera_, glm::vec2{context.mouse_x, context.mouse_y},
      glm::vec2{float(context.window_width), float(context.window_height)}),
    glm::vec2{0.0f}, world_extent_);

  if (networked_) {
    // Предложение авторитету: в мире еда появится, когда он запишет её в canonical bundle.
    if (net_) static_cast<void>(net_->propose_food(point.x, point.y));
    return;
  }
  act::intent intent;
  intent.kind = act::intent_kind::spawn_prefab;
  intent.payload.spawn.prefab = utils::string_hash("food");
  intent.payload.spawn.target = act::vec3{point.x, point.y, 0.0};
  intent.source_action = utils::string_hash("spawn_food");
  static_cast<void>(actors_.enqueue_player_intent(intent));
}

// Первый живой player-input: named actions camera_* (WASD, standard key bindings) двигают
// камеру со скоростью, пропорциональной видимой полуширине (одинаково ощущается на любом зуме).
// Presentation-плоскость: скорость задаётся в понятном человеку real-time, но active-gameplay gate
// замораживает камеру вместе с миром на локальной паузе. Точка не выходит за мировой бокс тайлов.
void frontier_online_game::move_camera(const presentation_context& context) {
  if (!context.gate.run_gameplay) {
    return;
  }
  const float dt = float(context.time) / float(utils::global_time_resolution);
  simul::step_camera(camera_, camera_config_, simul::camera_pan_direction(), dt, glm::vec2{0.0f, 0.0f},
                     world_extent_);
}

void frontier_online_game::request_chunks(broker& messages) {
  while (next_chunk_request_ < chunk_request_count_) {
    command_load_chunk command;
    command.generation = scene_generation_;
    command.world_build = world_build_;
    command.x = int32_t(next_chunk_request_ % chunks_x_);
    command.y = int32_t(next_chunk_request_ / chunks_x_);
    command.chunk_size = chunk_size_;
    command.world_seed = chunk_request_seed_;
    command.generator = chunk_request_generator_;
    if (!messages.load_chunk.try_push(std::move(command))) return; // очередь полна: следующий кадр
    ++next_chunk_request_;
  }
}

void frontier_online_game::drain_loaded_chunks(broker& messages, const uint64_t generation) {
  command_chunk_loaded command{};
  while (messages.chunk_loaded.try_pop(command)) {
    if (command.generation != generation || command.world_build != world_build_) {
      DE_LOG(catalogue::log_domain::gameplay, flow,
             "main: dropped stale chunk ({},{}) generation={}/{} world build={}/{}",
             command.x, command.y, command.generation, generation, command.world_build,
             world_build_);
      continue;
    }

    tile_chunk chunk;
    chunk.coord = chunk_coord{command.x, command.y};
    chunk.size = command.size;
    chunk.tiles.resize(command.terrain.size());
    for (size_t i = 0; i < command.terrain.size(); ++i) {
      chunk.tiles[i].terrain = command.terrain[i];
    }
    apply_chunk(grid_, chunk);

    if (command.x >= 0 && command.y >= 0 &&
        uint32_t(command.x) < chunks_x_ && uint32_t(command.y) < chunks_y_) {
      const size_t index = size_t(command.y) * chunks_x_ + size_t(command.x);
      if (!chunks_loaded_[index]) {
        chunks_loaded_[index] = true;
        chunks_loaded_count_ += 1;
      }
    }
  }

  if (!chunks_logged_ && chunks_loaded_count_ == chunks_loaded_.size()) {
    DE_LOG(catalogue::log_domain::gameplay, flow,
           "main: all {} world chunks generated", chunks_loaded_count_);
    chunks_logged_ = true;
  }
}

void frontier_online_game::publish_camera_and_tiles(const presentation_context& context) {
  if (!tile_batch_.valid()) {
    return;
  }

  // Камера уезжает в рендер снапшотом состояния, а не готовой матрицей: global_ubo («camera_buffer»)
  // собирает рендер-поток, интерполируя prev→cur по реальным часам (см. render_system.cpp) — иначе
  // камера шагала бы с частотой main-тика на фоне плавных акторов.
  // На паузе камера стоит, но окно может смениться (ресайз, полный экран): новый aspect обязан уйти
  // в рендер, иначе мир растянут старым, а тайлы уже отобраны по новому.
  const bool camera_changed = camera_.center != last_published_camera_.center ||
                              camera_.half_width != last_published_camera_.half_width ||
                              camera_.aspect != last_published_camera_.aspect;
  if (context.render_available && (context.gate.run_gameplay || camera_changed)) {
    last_published_camera_ = camera_;
    auto& cam = context.messages.draw_camera.write_slot();
    cam.center_x = camera_.center.x;
    cam.center_y = camera_.center.y;
    cam.half_width = camera_.half_width;
    cam.aspect = camera_.aspect;
    cam.frame_time = context.time;
    context.messages.draw_camera.publish();
    published_cameras_.push(camera_);
  }

  if (!context.gate.in_game || !context.render_available) {
    return;
  }

  // Камера двигается до 1.5 полуширины экрана в секунду: при полуширине 28 это больше тайла за
  // шаг, и запаса в один тайл вокруг ТЕКУЩЕЙ камеры не хватало на ту, что рисует рендер.
  const tile_span span = visible_tiles(published_cameras_.bounds(camera_), grid_, 1.0f);
  tile_batch_.build(grid_, span, textures_);

  auto& slot = context.messages.draw_tiles.write_slot();
  slot.count = tile_batch_.count();
  slot.stride = tile_batch::stride();
  slot.bytes.resize(size_t(slot.count) * slot.stride);
  tile_batch_.blit(std::span<uint8_t>(slot.bytes));

  if (!tiles_logged_) {
    DE_TRACE(catalogue::log_domain::gameplay,
             "tile slice [{},{})x[{},{}) = {} instances, {} B/inst, {} B payload",
             span.x0, span.x1, span.y0, span.y1, slot.count, slot.stride, slot.bytes.size());
    tiles_logged_ = true;
  }
  context.messages.draw_tiles.publish();
}

void frontier_online_game::update_actors(const simulation_context& context) {
  const auto begin = std::chrono::steady_clock::now();
  actors_last_metrics_ = actors_.update(context.tick, context.game_delta, &actor_batch_, context.pool);
  const auto end = std::chrono::steady_clock::now();
  const uint64_t update_us = uint64_t(std::max<int64_t>(utils::count_mcs(begin, end), 0));

  publish_actor_sounds(context.messages, context.sound_available, actors_.sound_events());
  metrics_actor_ticks_ += 1;
  metrics_intents_ += actors_last_metrics_.intents;
  metrics_instances_ += actors_last_metrics_.instances;
  metrics_actor_update_us_ += update_us;
}

void frontier_online_game::publish_actor_snapshot(const presentation_context& context) {
  if (!context.gate.run_gameplay || !context.render_available || !actor_batch_.valid()) {
    return;
  }

  auto& slot = context.messages.draw_actors.write_slot();
  slot.count = actor_batch_.count();
  slot.stride = actor_batch::stride();
  // One snapshot publication per presentation frame. Several fixed simulation steps may have
  // contributed to this final state; render interpolation remains wall-time presentation work.
  slot.sim_frame_time = context.time;
  slot.bytes.resize(size_t(slot.count) * slot.stride);
  actor_batch_.blit(std::span<uint8_t>(slot.bytes));
  slot.ids.assign(actor_batch_.ids().begin(), actor_batch_.ids().end());

  if (!actors_logged_) {
    DE_TRACE(catalogue::log_domain::gameplay,
             "actor slice {} actors, {} intents, {} instances, {} B/inst, {} B payload",
             actors_last_metrics_.actors, actors_last_metrics_.intents, slot.count,
             slot.stride, slot.bytes.size());
    actors_logged_ = true;
  }
  context.messages.draw_actors.publish();
}

// Слушатель = точка камеры (top-down дефолтная ориентация из command_sound_listener: панорама
// следует +x экрана). Публикуется каждый кадр (latest-wins) — работает и на gameplay-паузе.
void frontier_online_game::publish_sound_listener(broker& messages, const bool sound_available) {
  if (!sound_available) {
    return;
  }
  auto& slot = messages.sound_listener.write_slot();
  slot = {};
  slot.pos[0] = camera_.center.x;
  slot.pos[1] = camera_.center.y;
  messages.sound_listener.publish();
}

// Звуки тика показываемого мира: своего (одиночная игра) или проверенного мира ведомого (сеть).
// Звучит последний показанный тик; тики догона, пройденные за один круг, молчат — проиграть их
// задним числом значило бы озвучить прошлое.
void frontier_online_game::publish_actor_sounds(broker& messages, const bool sound_available,
                                                const std::span<const sound_emit> emits) {
  if (!sound_available) {
    return;
  }

  const glm::vec2 listener = camera_.center;
  // Радиус слышимости привязан к зуму; тот же радиус уходит в max_distance ⇒ отсечка ниже —
  // просто бюджет (не слать заведомо неслышимое), а реальную громкость/панораму считает
  // sound-система по позиции источника относительно слушателя (linear в [min, max]).
  const float audible = camera_.half_width * 1.5f;
  const float audible_squared = audible * audible;
  constexpr uint32_t max_sounds_per_tick = 8;
  uint32_t sent = 0;
  for (const auto& event : emits) {
    if (sent >= max_sounds_per_tick) {
      break;
    }
    const glm::vec2 distance = event.pos - listener;
    if (distance.x * distance.x + distance.y * distance.y > audible_squared) {
      continue;
    }
    const auto sound_it = sound_by_name_.find(event.name);
    if (sound_it == sound_by_name_.end()) {
      continue;
    }

    command_sound_play play{};
    play.taskid = generate_task_id();
    play.after = SIZE_MAX;
    play.res = resource_ref::from_handle(sound_it->second);
    auto* sound_resource = sound_it->second.get<sound::sound_resource>();
    play.resource_pin = sound_resource == nullptr ? nullptr : sound_resource->pin();
    if (!play.resource_pin) {
      continue;
    }
    play.start = 0.0;
    play.pos[0] = event.pos.x;
    play.pos[1] = event.pos.y;
    play.min_distance = audible * 0.25f; // внутри четверти радиуса — полная громкость
    play.max_distance = audible;
    // Канал звука lossy: полная очередь — звук пропущен, и в счёт отправленных он не идёт.
    if (!messages.sound_play.try_push(play)) continue;
    ++sent;
    DE_TRACE(catalogue::log_domain::sound,
             "sim-sound send task={} at ({:.1f},{:.1f})", play.taskid, event.pos.x, event.pos.y);
  }
  if (sent > 0) {
    DE_LOG(catalogue::log_domain::sound, flow,
           "sim-sounds sent {} (of {} emits)", sent, emits.size());
  }
}

void frontier_online_game::update_metrics(const metrics_config& config, const bool active) {
  const auto now = std::chrono::steady_clock::now();
  if (!active || !config.enabled) {
    // FPS/CPU costs are real-time measurements by definition, but their sampling window is an
    // active-gameplay window: menu pause must not dilute the first result after resume.
    metrics_last_log_ = now;
    metrics_frames_ = 0;
    metrics_actor_ticks_ = 0;
    metrics_intents_ = 0;
    metrics_instances_ = 0;
    metrics_actor_update_us_ = 0;
    return;
  }
  metrics_frames_ += 1;
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    now - metrics_last_log_).count();
  if (elapsed_ms < config.log_interval_ms || metrics_frames_ == 0) {
    return;
  }

  const double seconds = double(elapsed_ms) / 1000.0;
  ui_main_fps_ = double(metrics_frames_) / seconds;
  ui_intents_per_sec_ = double(metrics_intents_) / seconds;
  ui_instances_per_sec_ = double(metrics_instances_) / seconds;
  ui_actor_update_avg_us_ = metrics_actor_ticks_ == 0
    ? 0.0
    : double(metrics_actor_update_us_) / double(metrics_actor_ticks_);
  DE_LOG(catalogue::log_domain::main, flow,
         "metrics: main_fps={:.1f}, actors={}, intents/s={:.0f}, actor_instances/s={:.0f}, actor_update_avg_us={:.1f}",
         ui_main_fps_, actors_last_metrics_.actors, ui_intents_per_sec_,
         ui_instances_per_sec_, ui_actor_update_avg_us_);

  metrics_last_log_ = now;
  metrics_frames_ = 0;
  metrics_actor_ticks_ = 0;
  metrics_intents_ = 0;
  metrics_instances_ = 0;
  metrics_actor_update_us_ = 0;
}

void frontier_online_game::reset_metrics() noexcept {
  actors_last_metrics_ = {};
  metrics_frames_ = 0;
  metrics_actor_ticks_ = 0;
  metrics_intents_ = 0;
  metrics_instances_ = 0;
  metrics_actor_update_us_ = 0;
  ui_main_fps_ = 0.0;
  ui_intents_per_sec_ = 0.0;
  ui_instances_per_sec_ = 0.0;
  ui_actor_update_avg_us_ = 0.0;
  metrics_last_log_ = std::chrono::steady_clock::now();
}

// Lua-биндинги игры: реализация — именованные методы (не лямбды: одинаковые сигнатуры лямбд одной
// функции делят метатаблицу __gc в sol2), регистрация — в register_ui_bindings.
struct frontier_online_game::lua_bindings {
  frontier_online_game* game = nullptr;
  visage::system* ui = nullptr;

  sol::table perf_stats() const {
    auto& lua = ui->script_state();
    sol::table out = lua.create_table();
    std::vector<uint64_t> samples;
    int32_t index = 0;
    actor_perf_statistics().for_each(
      [&](const catalogue::statistics_store::function_record& record) {
        sol::table entry = lua.create_table();
        entry["name"] = std::string(record.name);
        entry["avg"] = record.average_mcs();
        entry["min"] = double(record.min_mcs);
        entry["max"] = double(record.max_mcs);
        entry["last"] = double(record.last_mcs);
        entry["count"] = double(record.call_count);
        record.ordered_samples(samples);
        sol::table ordered = lua.create_table(int32_t(samples.size()), 0);
        for (size_t i = 0; i < samples.size(); ++i) {
          ordered[i + 1] = double(samples[i]);
        }
        entry["samples"] = ordered;
        out[++index] = entry;
      });
    return out;
  }
  sol::optional<bool> act_predicate(const std::string& name, const uint32_t id) const {
    const auto v = game->actors_.ui_predicate(name, aesthetics::entityid_t(id));
    return v.has_value() ? sol::optional<bool>(*v) : sol::optional<bool>(sol::nullopt);
  }
  sol::optional<double> act_number(const std::string& name, const uint32_t id) const {
    const auto v = game->actors_.ui_number(name, aesthetics::entityid_t(id));
    return v.has_value() ? sol::optional<double>(double(*v)) : sol::optional<double>(sol::nullopt);
  }
  sol::optional<int64_t> act_string(const std::string& name, const uint32_t id) const {
    const auto v = game->actors_.ui_string(name, aesthetics::entityid_t(id));
    return v.has_value() ? sol::optional<int64_t>(int64_t(*v)) : sol::optional<int64_t>(sol::nullopt);
  }
  sol::object act_describe(const std::string& name, const uint32_t id, sol::optional<sol::protected_function> callback) const {
    auto& lua = ui->script_state();
    bool ok = false;
    bool callback_failed = false;
    if (callback.has_value()) {
      ok = game->actors_.ui_describe(name, aesthetics::entityid_t(id), [&](const std::string_view node) {
        if (callback_failed) return;
        const auto result = (*callback)(std::string(node));
        // Критическая ошибка внутри коллбека переносится через Lua и бросается дальше, а не
        // превращается в предупреждение; обычная ошибка Lua — отказ всего describe (nil), а не
        // успешный ответ с потерянными узлами.
        bindings::rethrow_critical_error(lua.lua_state());
        if (!result.valid()) {
          const sol::error err = result;
          utils::warn("act_describe callback error: {}", err.what());
          callback_failed = true;
        }
      });
      return ok && !callback_failed ? sol::make_object(lua, true) : sol::make_object(lua, sol::nil);
    }
    std::string text;
    ok = game->actors_.ui_describe(name, aesthetics::entityid_t(id), [&](const std::string_view node) {
      if (!text.empty()) {
        text.push_back('\n');
      }
      text.append(node);
    });
    return ok ? sol::make_object(lua, text) : sol::make_object(lua, sol::nil);
  }
  // Сеть для интерфейса: nil — локальная симуляция (клиент запущен без --join).
  sol::object net_status() const {
    auto& lua = ui->script_state();
    if (!game->networked_ || !game->net_) {
      return sol::nil;
    }
    const auto status = game->net_->status();
    sol::table out = lua.create_table();
    out["phase"] = std::string(status.phase);
    out["endpoint"] = game->join_.endpoint;
    out["peer"] = double(status.peer);
    out["tick"] = double(status.tick);
    out["catchup_tick"] = double(status.catchup_tick);
    out["silence_ms"] = double(status.silence_ms);
    out["players"] = double(status.players);
    out["sessions"] = double(status.sessions);
    out["resumes"] = double(status.resumes);
    out["attempts"] = double(status.attempts);
    out["own_inputs"] = double(status.own_inputs);
    out["rejected_inputs"] = double(status.rejected_inputs);
    return out;
  }
};

void frontier_online_game::register_ui_bindings(visage::system& ui) {
  sol::environment env = ui.script_env();
  sol::table app = env["app"].get_or_create<sol::table>();
  const lua_bindings bindings{this, &ui};
  app.set_function("perf_stats", &lua_bindings::perf_stats, bindings);
  // ── read-only шов к act-функциям (UI-проекция игрового состояния, ROADMAP п.13) ──
  // Lua зовёт pure-категории по имени над entityid; effect-категории здесь НЕТ — Lua не мутирует
  // геймплей. nil = нет функции / не та категория / нет сущности. string-категория = хеш loc-ключа.
  app.set_function("act_predicate", &lua_bindings::act_predicate, bindings);
  app.set_function("act_number", &lua_bindings::act_number, bindings);
  app.set_function("act_string", &lua_bindings::act_string, bindings);
  app.set_function("act_describe", &lua_bindings::act_describe, bindings);
  app.set_function("net_status", &lua_bindings::net_status, bindings);
}

void frontier_online_game::before_ui_update(visage::system& ui) const {
  ui.set_env_number("tf_main_fps", ui_main_fps_);
  ui.set_env_number("tf_actor_count", double(actors_last_metrics_.actors));
  ui.set_env_number("tf_actor_intents", double(actors_last_metrics_.intents));
  ui.set_env_number("tf_actor_instances", double(actors_last_metrics_.instances));
  ui.set_env_number("tf_actor_ticks", double(actors_last_metrics_.ticks));
  ui.set_env_number("tf_intents_per_sec", ui_intents_per_sec_);
  ui.set_env_number("tf_instances_per_sec", ui_instances_per_sec_);
  ui.set_env_number("tf_actor_update_avg_us", ui_actor_update_avg_us_);
}

} // namespace core
} // namespace frontier_online
