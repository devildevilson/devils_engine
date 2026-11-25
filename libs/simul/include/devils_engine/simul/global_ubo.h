#ifndef DEVILS_ENGINE_SIMUL_GLOBAL_UBO_H
#define DEVILS_ENGINE_SIMUL_GLOBAL_UBO_H

// Общий uniform-буфер рендер-графа: ресурс `camera_buffer` (формат mat4mat4v4v4v4v4, копия на кадр в
// полёте) и блок `global_ubo` (set 0, binding 0) в шейдерах. Его читает шейдер интерфейса visage
// (`ui.vert.glsl`: `ui_proj`), поэтому раскладка — контракт движка, а не проекта. Проект даёт только
// `view_proj` своей камеры; проекцию интерфейса и размер кадра берёт сам рендер из текущего
// фреймбуфера, и пишет UBO каждый рендер-кадр — интерфейс рисуется и на загрузке, и на паузе после
// ресайза, когда снапшотов игровой камеры нет.

#include <cstddef>
#include <cstdint>
#include <span>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "devils_engine/utils/string_id.h"
#include "render_runtime.h"

namespace devils_engine {
namespace simul {

// std140: mat4 и vec4 выровнены по 16 байт, поэтому структура совпадает с std140 без доп. паддинга.
struct global_ubo_t {
  glm::mat4 view_proj;   // мир -> clip (камера проекта)
  glm::mat4 ui_proj;     // пиксели окна -> clip (интерфейс, ortho)
  glm::vec4 misc;        // x = ширина кадра, y = высота кадра, z = sdf px range шрифтов, w = 0
  glm::vec4 reserved[3]; // до 192 байт: копии на кадр лежат встык, размер копии кратен
                         // minUniformBufferOffsetAlignment (до 64 у всех целевых устройств)
};
static_assert(sizeof(global_ubo_t) == 192, "global_ubo_t: 12 x vec4, multiple of 64 (UBO offset alignment)");

// Диапазон SDF шрифтов visage (`font_resource`: pixel_range = 4).
inline constexpr float ui_sdf_px_range = 4.0f;

// Пиксели окна (начало в левом верхнем углу, y вниз — как у вершин visage) -> clip Vulkan.
inline glm::mat4 ui_projection(const float width, const float height) {
  glm::mat4 m(1.0f);
  m[0][0] = 2.0f / width;
  m[1][1] = 2.0f / height;
  m[2][2] = -1.0f;
  m[3][0] = -1.0f;
  m[3][1] = -1.0f;
  return m;
}

// Собрать UBO из камеры проекта и текущего фреймбуфера и отдать в `camera_buffer` (запись отложена
// до standard_render_flush_buffer_writes, как у любого буфера с копией на кадр). Звать каждый
// рендер-кадр, пока граф готов.
template <typename State>
void standard_render_write_global_ubo(State& c, const glm::mat4& view_proj) {
  if (!c.graph_ready) {
    return;
  }
  const float width = float(c.pending_graph_width == 0 ? 1u : c.pending_graph_width);
  const float height = float(c.pending_graph_height == 0 ? 1u : c.pending_graph_height);
  global_ubo_t ubo{};
  ubo.view_proj = view_proj;
  ubo.ui_proj = ui_projection(width, height);
  ubo.misc = glm::vec4(width, height, ui_sdf_px_range, 0.0f);
  static const uint64_t camera_buffer_hash = utils::string_hash("camera_buffer");
  standard_render_write_buffer_bytes(
    c, camera_buffer_hash, std::span<const std::byte>(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
}

} // namespace simul
} // namespace devils_engine

#endif
