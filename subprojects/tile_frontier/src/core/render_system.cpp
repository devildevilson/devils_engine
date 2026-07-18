#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

#include <devils_engine/catalogue/logging.h> // доменные логи (render)
#include <devils_engine/simul/render_runtime.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/time-utils.hpp>

#include "actor_simulation.h"
#include "broker.h"
#include "draw_intent.h"
#include "global_ubo.h"
#include "interpolation.h"
#include "messages.h"
#include "render_system.h"
#include "tile_batch.h"
#include "tile_map.h"
#include "write_buffer_channel.h"

// Политика смешивания инстанса актёра (см. blend_traits в simul/interpolation.h). Позиция и размер —
// непрерывные (лерп), texture/color — дискретные (снап к новейшему b). teleport-guard выключен
// (snap_dist2 == 0): актёры движутся плавно; выставь порог, чтобы варп/телепорт не «ехал через
// экран», а прыгал мгновенно. Специализация живёт в namespace движка: специализировать шаблон
// через using-алиас (core::blend_traits) нельзя.
namespace devils_engine {
namespace simul {

template <>
struct blend_traits<::tile_frontier::core::actor_instance> {
  using actor_instance = ::tile_frontier::core::actor_instance;
  static constexpr float snap_dist2 = 0.0f; // 0 = guard выключен
  static actor_instance mix(const actor_instance& a, const actor_instance& b, const float t) noexcept {
    actor_instance o = b; // discrete: texture, color берём у новейшего снапшота
    if constexpr (snap_dist2 > 0.0f) {
      const glm::vec2 d = b.pos - a.pos;
      if (d.x * d.x + d.y * d.y > snap_dist2) {
        return o; // телепорт: позицию не лерпим
      }
    }
    o.pos = a.pos + (b.pos - a.pos) * t;
    o.size = a.size + (b.size - a.size) * t;
    return o;
  }
};

} // namespace simul
} // namespace devils_engine

namespace tile_frontier {
namespace core {

using namespace devils_engine;

static uint32_t make_color(const float r, const float g, const float b, const float a) {
  const auto pack = [](const float v) {
    return uint32_t(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  return (pack(r) << 0) | (pack(g) << 8) | (pack(b) << 16) | (pack(a) << 24);
}

struct render_simulation_init : public simul::standard_render_state<broker> {
  uint32_t tile_pair_index = painter::invalid_resource_slot;
  uint32_t actor_pair_index = painter::invalid_resource_slot;
  bool tiles_ready = false;
  bool actors_ready = false;
  bool actor_draw_ready = false;
  snapshot_interpolator<actor_instance> actor_interp;         // prev/cur + timing + blend (см. interpolation.h)
  std::vector<uint8_t> actor_interp_bytes;                    // переиспользуемый выход resolve() -> GPU
  std::chrono::steady_clock::time_point actor_draw_last_tp{}; // для РЕАЛЬНОГО wall-time между кадрами (п.①)
  bool actor_draw_tp_valid = false;

  // Камера — тот же трёхконцерновый рецепт, что и акторы: timing = nominal_clock, identity
  // тривиальна (снапшот один), blend локальный (center/half_width лерп, aspect/fb_* снап).
  command_draw_camera cam_prev{}, cam_cur{};
  bool cam_have_prev = false, cam_have_cur = false;
  nominal_clock cam_clock;
  std::chrono::steady_clock::time_point cam_last_tp{};
  bool cam_tp_valid = false;

  ~render_simulation_init() noexcept = default;
};

static void render_reset_project_draw_state(render_simulation_init& c) {
  c.tiles_ready = false;
  c.actors_ready = false;
  c.actor_draw_ready = false;
  c.actor_draw_tp_valid = false; // сброс wall-clock базы: после detach/shutdown не считаем гигантский dt
  c.cam_tp_valid = false;
  c.cam_have_prev = false; // последний снапшот камеры оставляем (UBO перезапишется после пересборки графа)
  c.tile_pair_index = painter::invalid_resource_slot;
  c.actor_pair_index = painter::invalid_resource_slot;
}

static void render_shutdown(render_simulation_init& c) {
  simul::standard_render_shutdown(c);
  render_reset_project_draw_state(c);
}

static void render_create_tile_draw(render_simulation_init& c) {
  if (c.tiles_ready || !c.graph_ready) {
    return;
  }

  const uint32_t dg_index = c.base->find_draw_group("dg_tiles");
  if (dg_index == painter::invalid_resource_slot) {
    utils::warn("render tiles: draw group 'dg_tiles' not found");
    return;
  }

  draw_intent<tile_instance> intent;
  if (const auto r = intent.bind(std::span<const painter::format::values>(c.base->draw_groups[dg_index].instance_layout), uint32_t(c.base->draw_groups[dg_index].stride)); !r) {
    utils::warn("render tiles: dg_tiles instance layout mismatch: {} (attr {}, expected {}, actual {})",
                core::instance_layout::match_error::to_string(r.error), r.where, r.expected, r.actual);
    return;
  }

  const auto quad_h = c.assets->register_buffer_storage("tile_quad");
  painter::buffer_create_info bci{"g1", 6, 0};
  c.assets->create_buffer_storage(quad_h, bci);

  struct vertex_data {
    float x, y, z;
    uint32_t c;
  };
  const uint32_t white = make_color(1.0f, 1.0f, 1.0f, 1.0f);
  const vertex_data vertices[] = {
    {-0.5f, -0.5f, 0.0f, white},
    {0.5f, -0.5f, 0.0f, white},
    {0.5f, 0.5f, 0.0f, white},
    {-0.5f, -0.5f, 0.0f, white},
    {0.5f, 0.5f, 0.0f, white},
    {-0.5f, 0.5f, 0.0f, white},
  };

  const auto vertex_bytes = std::span(reinterpret_cast<const uint8_t*>(vertices), sizeof(vertices));
  c.assets->populate_buffer_storage(quad_h, vertex_bytes, std::span<const uint8_t>());
  c.assets->mark_ready_buffer_slot(quad_h);

  c.tile_pair_index = c.base->register_pair(dg_index, quad_h, 5000);

  for (uint32_t off = 0; off < 2; ++off) {
    const auto indi = c.base->get_current_indirect_resource_frame(c.tile_pair_index, off);
    auto cmd = reinterpret_cast<VkDrawIndirectCommand*>(&reinterpret_cast<uint8_t*>(indi.mapped)[indi.sub.offset]);
    cmd[0].vertexCount = 6;
    cmd[0].instanceCount = 0;
    cmd[0].firstVertex = 0;
    cmd[0].firstInstance = 0;
  }

  c.tiles_ready = true;
  DE_LOG(catalogue::log_domain::render, flow, "render tiles: registered tile quad draw pair");
}

static void render_create_actor_draw(render_simulation_init& c) {
  if (c.actors_ready || !c.graph_ready) {
    return;
  }

  const uint32_t dg_index = c.base->find_draw_group("dg_actors");
  if (dg_index == painter::invalid_resource_slot) {
    utils::warn("render actors: draw group 'dg_actors' not found");
    return;
  }

  draw_intent<actor_instance> intent;
  if (const auto r = intent.bind(std::span<const painter::format::values>(c.base->draw_groups[dg_index].instance_layout), uint32_t(c.base->draw_groups[dg_index].stride)); !r) {
    utils::warn("render actors: dg_actors instance layout mismatch: {} (attr {}, expected {}, actual {})",
                core::instance_layout::match_error::to_string(r.error), r.where, r.expected, r.actual);
    return;
  }

  const auto tri_h = c.assets->register_buffer_storage("actor_triangle");
  painter::buffer_create_info bci{"g1", 3, 0};
  c.assets->create_buffer_storage(tri_h, bci);

  struct vertex_data {
    float x, y, z;
    uint32_t c;
  };
  const uint32_t white = make_color(1.0f, 1.0f, 1.0f, 1.0f);
  const vertex_data vertices[] = {
    {0.0f, 0.58f, 0.0f, white},
    {-0.50f, -0.35f, 0.0f, white},
    {0.50f, -0.35f, 0.0f, white},
  };

  const auto vertex_bytes = std::span(reinterpret_cast<const uint8_t*>(vertices), sizeof(vertices));
  c.assets->populate_buffer_storage(tri_h, vertex_bytes, std::span<const uint8_t>());
  c.assets->mark_ready_buffer_slot(tri_h);

  c.actor_pair_index = c.base->register_pair(dg_index, tri_h, 5000);

  for (uint32_t off = 0; off < 2; ++off) {
    const auto indi = c.base->get_current_indirect_resource_frame(c.actor_pair_index, off);
    auto cmd = reinterpret_cast<VkDrawIndirectCommand*>(&reinterpret_cast<uint8_t*>(indi.mapped)[indi.sub.offset]);
    cmd[0].vertexCount = 3;
    cmd[0].instanceCount = 0;
    cmd[0].firstVertex = 0;
    cmd[0].firstInstance = 0;
  }

  c.actors_ready = true;
  DE_LOG(catalogue::log_domain::render, flow,
         "render actors: registered actor triangle draw pair {} (dg '{}', layout '{}', stride {}, max {})",
         c.actor_pair_index,
         c.base->draw_groups[dg_index].name,
         c.base->draw_groups[dg_index].layout_str,
         c.base->draw_groups[dg_index].stride,
         c.base->pairs[c.actor_pair_index].max_size);
}

static void render_update_tile_draw(render_simulation_init& c, const command_draw_tiles& msg) {
  if (!c.tiles_ready || c.tile_pair_index == painter::invalid_resource_slot) {
    return;
  }
  if (msg.stride != tile_batch::stride()) {
    utils::warn("render tiles: bad instance stride {}, expected {}", msg.stride, tile_batch::stride());
    return;
  }

  const auto& pair = c.base->pairs[c.tile_pair_index];
  const uint32_t count = std::min(msg.count, pair.max_size);
  const size_t bytes = std::min(msg.bytes.size(), size_t(count) * msg.stride);

  for (uint32_t off = 0; off < 2; ++off) {
    const auto inst = c.base->get_current_instance_resource_frame(c.tile_pair_index, off);
    const auto indi = c.base->get_current_indirect_resource_frame(c.tile_pair_index, off);
    if (bytes != 0) {
      std::memcpy(static_cast<uint8_t*>(inst.mapped) + inst.sub.offset, msg.bytes.data(), bytes);
    }

    auto cmd = reinterpret_cast<VkDrawIndirectCommand*>(&reinterpret_cast<uint8_t*>(indi.mapped)[indi.sub.offset]);
    cmd[0].vertexCount = 6;
    cmd[0].instanceCount = count;
    cmd[0].firstVertex = 0;
    cmd[0].firstInstance = 0;
  }
}

static void render_update_actor_draw(render_simulation_init& c) {
  if (!c.actors_ready || c.actor_pair_index == painter::invalid_resource_slot) {
    return;
  }
  if (!c.actor_interp.has_data()) {
    return;
  }

  const auto& pair = c.base->pairs[c.actor_pair_index];
  c.actor_interp.resolve(c.actor_interp_bytes); // интерполяция prev->cur по alpha реальных часов
  const uint32_t count = std::min(c.actor_interp.count(), pair.max_size);
  const size_t bytes = std::min(c.actor_interp_bytes.size(), size_t(count) * sizeof(actor_instance));

  for (uint32_t off = 0; off < 2; ++off) {
    const auto inst = c.base->get_current_instance_resource_frame(c.actor_pair_index, off);
    const auto indi = c.base->get_current_indirect_resource_frame(c.actor_pair_index, off);
    if (bytes != 0) {
      std::memcpy(static_cast<uint8_t*>(inst.mapped) + inst.sub.offset, c.actor_interp_bytes.data(), bytes);
    }

    auto cmd = reinterpret_cast<VkDrawIndirectCommand*>(&reinterpret_cast<uint8_t*>(indi.mapped)[indi.sub.offset]);
    cmd[0].vertexCount = 3;
    cmd[0].instanceCount = count;
    cmd[0].firstVertex = 0;
    cmd[0].firstInstance = 0;
  }
}

static void render_on_graph_ready(render_simulation_init& c) {
  render_create_tile_draw(c);
  render_create_actor_draw(c);
}

// Собрать global_ubo («camera_buffer») из интерполированного prev→cur состояния камеры — на КАЖДЫЙ
// рендер-кадр, а не на main-тик. Камера рисуется с тем же лагом в один снапшот, что и акторы, поэтому
// они визуально синхронны. aspect и размер фреймбуфера дискретны (resize) — снап к новейшему.
static void render_update_camera(render_simulation_init& c) {
  if (!c.graph_ready || !c.cam_have_cur) {
    return;
  }
  const auto& a = c.cam_have_prev ? c.cam_prev : c.cam_cur;
  const auto& b = c.cam_cur;
  const float t = c.cam_clock.alpha();

  camera2d cam;
  cam.center = {a.center_x + (b.center_x - a.center_x) * t,
                a.center_y + (b.center_y - a.center_y) * t};
  cam.half_width = a.half_width + (b.half_width - a.half_width) * t;
  cam.aspect = b.aspect;

  global_ubo_t ubo{};
  ubo.view_proj = cam.view_proj();
  ubo.ui_proj = glm::mat4(1.0f);
  ubo.ui_proj[0][0] = 2.0f / b.fb_width;
  ubo.ui_proj[1][1] = 2.0f / b.fb_height;
  ubo.ui_proj[2][2] = -1.0f;
  ubo.ui_proj[3][0] = -1.0f;
  ubo.ui_proj[3][1] = -1.0f;
  ubo.misc = glm::vec4(b.fb_width, b.fb_height, 4.0f, 0.0f);

  static const uint64_t camera_buffer_hash = utils::string_hash("camera_buffer");
  simul::standard_render_write_buffer_bytes(
    c, camera_buffer_hash,
    std::span<const std::byte>(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
}

render_simulation::render_simulation(const size_t frame_time, render_simulation_config config) noexcept : simul::render_system<::tile_frontier::core::broker>(frame_time),
                                                                                                          container(std::make_unique<render_simulation_init>()) {
  container->config = std::move(config);
}

render_simulation::~render_simulation() noexcept {
  if (container) {
    render_shutdown(*container);
  }
}

void render_simulation::init() {
  if (container->config.create_vulkan_on_init) {
    simul::standard_render_create_instance(*container);
    simul::standard_render_create_device(*container);
    simul::standard_render_create_base_resources(*container);
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

bool render_simulation::stop_predicate() const {
  return false;
}

void render_simulation::update([[maybe_unused]] const size_t time) {
  // тут че вообще делаем? пробегаем события
  // заходим в рендер граф, обрабатываем
  // вообще ничего особо сложного

  // событие изменения размеров окна
  // событиe обновления данных
  // событиe обновления настроек
  // ...

  // в конце заходим в рендер граф и рисуем все подряд

  if (container->br == nullptr) {
    return; // broker ещё не задан — нечего обрабатывать/рисовать
  }
  auto& br = *container->br;

  simul::standard_render_drain_commands(
    *container,
    br,
    [this](const command_window_recreation& cmd) {
      simul::standard_render_attach_window(
        *container,
        cmd,
        [this] {
          render_reset_project_draw_state(*container);
        },
        [this] {
          render_on_graph_ready(*container);
        });
    },
    [this] {
      simul::standard_render_try_create_graph(*container, [this] {
        render_on_graph_ready(*container);
      });
    });

  // Снапшот камеры: prev <- cur, часы на ноль; между снапшотами alpha гоним по реальному wall-time
  // рендер-кадра (та же дисциплина, что у акторов ниже). UBO пересобирается каждый рендер-кадр.
  bool camera_snapshot = false;
  if (const command_draw_camera* cmd = br.draw_camera.consume()) {
    container->cam_prev = container->cam_cur;
    container->cam_have_prev = container->cam_have_cur;
    container->cam_cur = *cmd;
    container->cam_have_cur = true;
    container->cam_clock.on_snapshot(cmd->frame_time);
    camera_snapshot = true;
  }
  if (container->cam_have_cur) {
    const auto now = std::chrono::steady_clock::now();
    if (!camera_snapshot && container->cam_tp_valid) {
      container->cam_clock.advance(size_t(std::max<int64_t>(utils::count_mcs(container->cam_last_tp, now), 0)));
    }
    container->cam_last_tp = now;
    container->cam_tp_valid = true;
    render_update_camera(*container);
  }

  if (container->graph_ready) {
    if (const command_draw_tiles* cmd = br.draw_tiles.consume()) {
      render_update_tile_draw(*container, *cmd);
    }
  }

  bool actor_snapshot = false;
  if (const command_draw_actors* cmd = br.draw_actors.consume()) {
    if (cmd->stride != actor_batch::stride()) {
      utils::warn("render actors: bad instance stride {}, expected {}", cmd->stride, actor_batch::stride());
    } else {
      container->actor_interp.push(
        std::span<const uint8_t>(cmd->bytes),
        std::span<const uint32_t>(cmd->ids),
        cmd->sim_frame_time);
      container->actor_draw_ready = true;
      actor_snapshot = true;
    }
  }

  if (container->graph_ready && container->actor_draw_ready) {
    // alpha гоним по РЕАЛЬНОМУ прошедшему времени рендер-кадра, а не по номинальному шагу (п.①).
    // На кадре прихода снапшота elapsed сброшен в push() -> не продвигаем (alpha=0 -> показываем prev).
    const auto now = std::chrono::steady_clock::now();
    if (!actor_snapshot) {
      const size_t real_dt = container->actor_draw_tp_valid
                               ? size_t(std::max<int64_t>(utils::count_mcs(container->actor_draw_last_tp, now), 0))
                               : 0;
      container->actor_interp.advance(real_dt);
    }
    container->actor_draw_last_tp = now;
    container->actor_draw_tp_valid = true;
    render_update_actor_draw(*container);
  }

  if (container->graph_ready && container->draw_active && container->base->can_draw()) {
    container->base->prepare_frame();
    container->ctx.prepare();
    container->ctx.draw();
    container->base->submit_frame();
  }
}

void render_simulation::set_broker(struct broker* b) {
  simul::render_system<::tile_frontier::core::broker>::set_broker(b);
  if (!container) {
    return;
  }
  container->br = b;
  simul::standard_render_try_create_graph(*container, [this] {
    render_on_graph_ready(*container);
  }); // триггер сборки графа
}

} // namespace core
} // namespace tile_frontier
