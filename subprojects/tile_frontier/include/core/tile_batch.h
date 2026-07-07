#ifndef TILE_FRONTIER_CORE_TILE_BATCH_H
#define TILE_FRONTIER_CORE_TILE_BATCH_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "tile_map.h"
#include "draw_intent.h"

namespace tile_frontier {
namespace core {

// Производитель инстансов тайлов на ГЛАВНОМ потоке. Собирает видимый срез сетки в
// переиспользуемый буфер (capacity держится между кадрами -> амортизированно zero-alloc),
// раскладку tile_instance валидирует один раз через draw_intent против layout-строки "v2ui1".
class tile_batch {
public:
  // привязать tile_instance к layout draw_group (валидация раскладки по токенам + страйду)
  instance_layout::match_result bind(const std::string_view& layout = "v2ui1") {
    return intent_.bind(layout);
  }
  bool valid() const noexcept { return intent_.valid(); }

  // собрать инстансы из среза span сетки grid: tile(x,y) -> {мировой центр, индекс текстуры}
  void build(const tile_grid& grid, const tile_span& span) {
    instances_.clear();
    instances_.reserve(span.count());
    for (uint32_t y = span.y0; y < span.y1; ++y) {
      for (uint32_t x = span.x0; x < span.x1; ++x) {
        instances_.push_back(tile_instance{grid.world_center(x, y), grid.at(x, y).texture});
      }
    }
  }

  std::span<const tile_instance> instances() const noexcept { return instances_; }
  uint32_t count() const noexcept { return uint32_t(instances_.size()); }
  static constexpr uint32_t stride() noexcept { return draw_intent<tile_instance>::stride(); }

  // упаковать собранные инстансы в буфер вызывающего (payload сообщения); intent не аллоцирует.
  std::size_t blit(const std::span<uint8_t>& dst) const {
    return intent_.blit(std::span<const tile_instance>(instances_), dst);
  }

private:
  draw_intent<tile_instance> intent_;
  std::vector<tile_instance> instances_;
};

} // namespace core
} // namespace tile_frontier

#endif
