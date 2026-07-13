#ifndef TILE_FRONTIER_CORE_GLOBAL_UBO_H
#define TILE_FRONTIER_CORE_GLOBAL_UBO_H

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

// Общий uniform-буфер (UBO), один на все шейдеры. std140-совместимая раскладка: mat4 и vec4
// выровнены по 16 байт, поэтому C++-структура совпадает с std140 без доп. паддинга.
// Зеркалит `global_ubo` в шейдерах (см. tests/shaders/ui.vert.glsl). Ресурс рендер-графа —
// host-visible буфер "camera_buffer" (см. resources/res1.tavl), заполняется главным потоком
// через command_write_buffer. Поля доращиваются по мере надобности (обратные матрицы и т.п.).

namespace tile_frontier {
namespace core {

struct global_ubo_t {
  glm::mat4 view_proj;   // мир -> clip (камера)
  glm::mat4 ui_proj;     // пиксели окна -> clip (UI, ortho)
  glm::vec4 misc;        // x=screen_w, y=screen_h, z=sdf_px_range, w=reserved
  glm::vec4 reserved[3]; // паддинг до 192 = кратно minUniformBufferOffsetAlignment (64);
                         // doublebuffer кладёт копии встык, поэтому размер копии обязан быть
                         // выровнен. Заодно задел под обратные матрицы и прочую мишуру.
};

static_assert(sizeof(global_ubo_t) == 192, "global_ubo_t: 12*v4, кратно 64 (UBO offset alignment)");

} // namespace core
} // namespace tile_frontier

#endif
