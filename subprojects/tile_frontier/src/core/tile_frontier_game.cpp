#include <algorithm>
#include <chrono>
#include <cstring>
#include <span>

#include <devils_engine/bindings/lua_header.h>
#include <devils_engine/catalogue/introspection.h>
#include <devils_engine/catalogue/logging.h>
#include <devils_engine/input/events.h> // named actions camera_* — WASD-движение камеры
#include <devils_engine/painter/gpu_texture_resource.h>
#include <devils_engine/sound/sound_resource.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/time-utils.hpp>
#include <devils_engine/visage/system.h>

#include "brain_config_loader.h"
#include "broker.h"
#include "instance_layout.h"
#include "messages.h"
#include "tile_frontier_game.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

void tile_frontier_game::begin_scene(const scene_start_context& context) {
  sound_by_name_.clear();

  std::vector<demiurg::resource_handle> tile_textures;
  for (const auto& binding : context.resources) {
    if (binding.group == context.config.tile_texture_group) {
      if (binding.resource.handle.get<painter::gpu_texture_resource>() == nullptr) {
        utils::error{}("tile_frontier: resource in tile texture group is not a GPU texture");
      }
      tile_textures.push_back(binding.resource.handle);
    } else if (binding.group == context.config.sound_group && !binding.alias.empty()) {
      if (binding.resource.handle.get<sound::sound_resource>() == nullptr) {
        utils::error{}("tile_frontier: resource for sound alias '{}' is not a sound", binding.alias);
      }
      const auto [it, inserted] = sound_by_name_.emplace(
        utils::string_hash(binding.alias), binding.resource.handle);
      if (!inserted) {
        utils::error{}("tile_frontier: duplicate sound alias '{}' in scene '{}'", binding.alias, context.scene_id);
      }
    }
  }

  const uint32_t texture_count = textures_.assign(tile_textures);
  if (texture_count == 0) {
    utils::error{}("tile_frontier: scene '{}' has no textures in group '{}'",
                   context.scene_id, context.config.tile_texture_group);
  }
  DE_LOG(catalogue::log_domain::resource, flow,
         "main: scene manifest '{}' selected {} tile textures and {} named sounds",
         context.scene_id, texture_count, sound_by_name_.size());

  chunk_size_ = context.config.chunk_size;
  chunks_x_ = context.config.chunks_x;
  chunks_y_ = context.config.chunks_y;
  grid_.tile_size = context.config.tile_size;
  grid_.resize(chunks_x_ * chunk_size_, chunks_y_ * chunk_size_);
  chunks_requested_.assign(size_t(chunks_x_) * chunks_y_, false);
  chunks_loaded_.assign(size_t(chunks_x_) * chunks_y_, false);
  chunks_loaded_count_ = 0;
  chunks_logged_ = false;
  tiles_logged_ = false;
  actors_logged_ = false;

  if (context.assets_available) {
    for (uint32_t cy = 0; cy < chunks_y_; ++cy) {
      for (uint32_t cx = 0; cx < chunks_x_; ++cx) {
        const size_t index = size_t(cy) * chunks_x_ + cx;
        command_load_chunk command;
        command.generation = context.generation;
        command.x = int32_t(cx);
        command.y = int32_t(cy);
        command.size = chunk_size_;
        command.textures.assign(textures_.handles().begin(), textures_.handles().end());
        context.messages.load_chunk.try_push(std::move(command));
        chunks_requested_[index] = true;
      }
    }
    DE_LOG(catalogue::log_domain::gameplay, flow,
           "main: requested {} mock world chunks via assets", chunks_requested_.size());
  }

  const glm::vec2 extent = grid_.world_extent();
  world_extent_ = extent;
  camera_.center = extent * 0.5f;
  camera_.half_width = context.config.camera_half_width;
  camera_.aspect = float(context.viewport_width) / float(std::max(context.viewport_height, 1u));

  if (const auto result = tile_batch_.bind("v2ui1"); !result) {
    utils::error{}("tile_instance layout mismatch vs 'v2ui1': {} (attr {}, expected {}, actual {})",
                   instance_layout::match_error::to_string(result.error), result.where,
                   result.expected, result.actual);
  }
  if (const auto result = actor_batch_.bind("v2ui1c4v1"); !result) {
    utils::error{}("actor_instance layout mismatch vs 'v2ui1c4v1': {} (attr {}, expected {}, actual {})",
                   instance_layout::match_error::to_string(result.error), result.where,
                   result.expected, result.actual);
  }

  auto brains = load_required_brain_config(
    context.asset_registry, context.config.actor_script, context.config.fsm_prefix,
    context.config.goap_prefix, context.config.prefab_prefix);
  DE_LOG(catalogue::log_domain::gameplay, flow,
         "main: required brain config <- script '{}', {} FSM brains ('{}'), {} GOAP brains ('{}'), {} prefabs",
         context.config.actor_script, brains.fsms.size(), context.config.fsm_prefix,
         brains.goaps.size(), context.config.goap_prefix, brains.prefabs.size());

  actors_.init(
    context.config.actor_count,
    glm::vec2{0.5f, 0.5f},
    glm::max(extent - glm::vec2{0.5f, 0.5f}, glm::vec2{0.5f, 0.5f}),
    texture_count,
    brains,
    context.config.actor_prefab_cycle);
  reset_metrics();
  DE_LOG(catalogue::log_domain::gameplay, flow,
         "main: spawned {} lightweight actors in aesthetics world", context.config.actor_count);
}

void tile_frontier_game::framebuffer_resized(const uint32_t width, const uint32_t height) noexcept {
  camera_.aspect = float(width) / float(std::max(height, 1u));
}

bool tile_frontier_game::loading_complete() const noexcept {
  return chunks_loaded_count_ == chunks_loaded_.size();
}

std::pair<std::size_t, std::size_t> tile_frontier_game::loading_progress() const noexcept {
  return {chunks_loaded_count_, chunks_loaded_.size()};
}

void tile_frontier_game::update(const frame_context& context) {
  drain_loaded_chunks(context.messages, context.generation);
  move_camera(context); // presentation-контрол: двигается и при gameplay-паузе
  collect_player_intents(context);
  publish_camera_and_tiles(context);
  publish_sound_listener(context.messages, context.sound_available); // слушатель = камера, и на паузе
  if (context.gate.run_gameplay && actor_batch_.valid()) {
    update_actors(context);
  }
}

void tile_frontier_game::collect_player_intents(const frame_context& context) {
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

  act::intent intent;
  intent.kind = act::intent_kind::spawn_prefab;
  intent.payload.spawn.prefab = utils::string_hash("food");
  intent.payload.spawn.target = act::vec3{point.x, point.y, 0.0};
  intent.source_action = utils::string_hash("spawn_food");
  static_cast<void>(actors_.enqueue_player_intent(intent));
}

// Первый живой player-input: named actions camera_* (WASD, standard key bindings) двигают
// камеру со скоростью, пропорциональной видимой полуширине (одинаково ощущается на любом зуме).
// Presentation-плоскость: реальное время кадра (context.time), game pause/scale не влияют; точка
// камеры не выходит за мировой бокс тайлов.
void tile_frontier_game::move_camera(const frame_context& context) {
  if (!context.gate.in_game) {
    return;
  }
  static const auto up = input::events::make_event_id("camera_up");
  static const auto left = input::events::make_event_id("camera_left");
  static const auto down = input::events::make_event_id("camera_down");
  static const auto right = input::events::make_event_id("camera_right");

  // По живому тесту экранный «верх» = -y мира (Vulkan clip-Y направлен вниз, ortho это не компенсирует).
  glm::vec2 dir{0.0f, 0.0f};
  dir.y -= input::events::is_pressed(up) ? 1.0f : 0.0f;
  dir.y += input::events::is_pressed(down) ? 1.0f : 0.0f;
  dir.x -= input::events::is_pressed(left) ? 1.0f : 0.0f;
  dir.x += input::events::is_pressed(right) ? 1.0f : 0.0f;
  if (dir.x == 0.0f && dir.y == 0.0f) {
    return;
  }

  const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
  const float dt = float(context.time) / float(utils::global_time_resolution);
  const float speed = camera_.half_width * 1.5f; // мировых единиц/сек — полтора экрана-полуширины
  camera_.center = glm::clamp(camera_.center + dir * (speed * dt / len),
                              glm::vec2{0.0f, 0.0f}, world_extent_);
}

void tile_frontier_game::drain_loaded_chunks(broker& messages, const uint64_t generation) {
  command_chunk_loaded command{};
  while (messages.chunk_loaded.try_pop(command)) {
    if (command.generation != generation) {
      DE_LOG(catalogue::log_domain::gameplay, flow,
             "main: dropped stale chunk ({},{}) generation={} current={}",
             command.x, command.y, command.generation, generation);
      continue;
    }

    tile_chunk chunk;
    chunk.coord = chunk_coord{command.x, command.y};
    chunk.size = command.size;
    chunk.tiles.resize(command.textures.size());
    for (size_t i = 0; i < command.textures.size(); ++i) {
      chunk.tiles[i].texture = command.textures[i];
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
           "main: all {} mock world chunks loaded", chunks_loaded_count_);
    chunks_logged_ = true;
  }
}

void tile_frontier_game::publish_camera_and_tiles(const frame_context& context) {
  if (!tile_batch_.valid()) {
    return;
  }

  // Камера уезжает в рендер снапшотом состояния, а не готовой матрицей: global_ubo («camera_buffer»)
  // собирает рендер-поток, интерполируя prev→cur по реальным часам (см. render_system.cpp) — иначе
  // камера шагала бы с частотой main-тика на фоне плавных акторов.
  const glm::mat4 view_projection = camera_.view_proj();
  if (context.render_available) {
    auto& cam = context.messages.draw_camera.write_slot();
    cam.center_x = camera_.center.x;
    cam.center_y = camera_.center.y;
    cam.half_width = camera_.half_width;
    cam.aspect = camera_.aspect;
    cam.fb_width = float(std::max(context.framebuffer_width, 1u));
    cam.fb_height = float(std::max(context.framebuffer_height, 1u));
    cam.frame_time = context.time;
    context.messages.draw_camera.publish();
  }

  if (!context.gate.in_game || !context.render_available) {
    return;
  }

  const tile_span span = visible_tiles(camera_, grid_, 1.0f);
  tile_batch_.build(grid_, span, textures_);

  auto& slot = context.messages.draw_tiles.write_slot();
  std::memcpy(slot.view_proj.data(), &view_projection[0][0], sizeof(float) * 16);
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

void tile_frontier_game::update_actors(const frame_context& context) {
  const auto begin = std::chrono::steady_clock::now();
  actors_last_metrics_ = actors_.update(context.game_delta_ticks, actor_batch_, context.pool);
  const auto end = std::chrono::steady_clock::now();
  const uint64_t update_us = uint64_t(std::max<int64_t>(utils::count_mcs(begin, end), 0));

  if (context.render_available) {
    auto& slot = context.messages.draw_actors.write_slot();
    slot.count = actor_batch_.count();
    slot.stride = actor_batch::stride();
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

  publish_actor_sounds(context.messages, context.sound_available);
  update_metrics(context.settings.metrics, update_us);
}

// Слушатель = точка камеры (top-down дефолтная ориентация из command_sound_listener: панорама
// следует +x экрана). Публикуется каждый кадр (latest-wins) — работает и на gameplay-паузе.
void tile_frontier_game::publish_sound_listener(broker& messages, const bool sound_available) {
  if (!sound_available) {
    return;
  }
  auto& slot = messages.sound_listener.write_slot();
  slot = {};
  slot.pos[0] = camera_.center.x;
  slot.pos[1] = camera_.center.y;
  messages.sound_listener.publish();
}

void tile_frontier_game::publish_actor_sounds(broker& messages, const bool sound_available) {
  if (!sound_available) {
    return;
  }

  const auto emits = actors_.sound_events();
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
    const auto sound = sound_by_name_.find(event.name);
    if (sound == sound_by_name_.end()) {
      continue;
    }

    command_sound_play play{};
    play.taskid = generate_task_id();
    play.after = SIZE_MAX;
    play.res = resource_ref::from_handle(sound->second);
    play.start = 0.0;
    play.pos[0] = event.pos.x;
    play.pos[1] = event.pos.y;
    play.min_distance = audible * 0.25f; // внутри четверти радиуса — полная громкость
    play.max_distance = audible;
    messages.sound_play.try_push(play);
    ++sent;
    DE_TRACE(catalogue::log_domain::sound,
             "sim-sound send task={} at ({:.1f},{:.1f})", play.taskid, event.pos.x, event.pos.y);
  }
  if (sent > 0) {
    DE_LOG(catalogue::log_domain::sound, flow,
           "sim-sounds sent {} (of {} emits)", sent, emits.size());
  }
}

void tile_frontier_game::update_metrics(const metrics_config& config, const uint64_t update_us) {
  metrics_frames_ += 1;
  metrics_actor_ticks_ += 1;
  metrics_intents_ += actors_last_metrics_.intents;
  metrics_instances_ += actors_last_metrics_.instances;
  metrics_actor_update_us_ += update_us;

  if (!config.enabled) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    now - metrics_last_log_).count();
  if (elapsed_ms < config.log_interval_ms || metrics_frames_ == 0) {
    return;
  }

  const double seconds = double(elapsed_ms) / 1000.0;
  ui_main_fps_ = double(metrics_frames_) / seconds;
  ui_intents_per_sec_ = double(metrics_intents_) / seconds;
  ui_instances_per_sec_ = double(metrics_instances_) / seconds;
  ui_actor_update_avg_us_ = double(metrics_actor_update_us_) / double(metrics_actor_ticks_);
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

void tile_frontier_game::reset_metrics() noexcept {
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

void tile_frontier_game::register_ui_bindings(visage::system& ui) {
  auto* ui_ptr = &ui;
  sol::environment env = ui.script_env();
  sol::table app = env["app"].get_or_create<sol::table>();
  app.set_function("perf_stats", [ui_ptr]() -> sol::table {
    auto& lua = ui_ptr->script_state();
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
  });

  // ── read-only шов к act-функциям (UI-проекция игрового состояния, ROADMAP п.13) ──
  // Lua зовёт pure-категории по имени над entityid; effect-категории здесь НЕТ — Lua не мутирует
  // геймплей (будущая кнопка атаки пойдёт через очередь интенций с анти-спамом, не через act).
  // nil = нет функции / не та категория / нет сущности.
  app.set_function("act_predicate", [this](const std::string& name, const uint32_t id) -> sol::optional<bool> {
    const auto v = actors_.ui_predicate(name, aesthetics::entityid_t(id));
    return v.has_value() ? sol::optional<bool>(*v) : sol::optional<bool>(sol::nullopt);
  });
  app.set_function("act_number", [this](const std::string& name, const uint32_t id) -> sol::optional<double> {
    const auto v = actors_.ui_number(name, aesthetics::entityid_t(id));
    return v.has_value() ? sol::optional<double>(double(*v)) : sol::optional<double>(sol::nullopt);
  });
  // string-категория = хеш loc-ключа (utils::id); Lua 5.4 integer 64-битный, точности хватает.
  app.set_function("act_string", [this](const std::string& name, const uint32_t id) -> sol::optional<int64_t> {
    const auto v = actors_.ui_string(name, aesthetics::entityid_t(id));
    return v.has_value() ? sol::optional<int64_t>(int64_t(*v)) : sol::optional<int64_t>(sol::nullopt);
  });
  // describe: с коллбеком — вызывается по каждому узлу исполнения (Lua строит граф/тултип);
  // без коллбека — простая строка узлов через перевод строки. nil = функции/сущности нет.
  app.set_function("act_describe", [this, ui_ptr](const std::string& name, const uint32_t id,
                                                  sol::optional<sol::protected_function> callback) -> sol::object {
    auto& lua = ui_ptr->script_state();
    bool ok = false;
    if (callback.has_value()) {
      ok = actors_.ui_describe(name, aesthetics::entityid_t(id), [&](const std::string_view node) {
        const auto result = (*callback)(std::string(node));
        if (!result.valid()) {
          const sol::error err = result;
          utils::warn("act_describe callback error: {}", err.what());
        }
      });
      return ok ? sol::make_object(lua, true) : sol::make_object(lua, sol::nil);
    }
    std::string text;
    ok = actors_.ui_describe(name, aesthetics::entityid_t(id), [&](const std::string_view node) {
      if (!text.empty()) {
        text.push_back('\n');
      }
      text.append(node);
    });
    return ok ? sol::make_object(lua, text) : sol::make_object(lua, sol::nil);
  });
}

void tile_frontier_game::before_ui_update(visage::system& ui) const {
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
} // namespace tile_frontier
