#ifndef FRONTIER_ONLINE_CORE_ACTOR_BATCH_H
#define FRONTIER_ONLINE_CORE_ACTOR_BATCH_H

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "actor_simulation.h" // компоненты + tick_tail_reader (причинная сторона)
#include "draw_intent.h"      // упаковщик инстансов в сырые байты (презентационная сторона)

// ПРЕЗЕНТАЦИОННАЯ сторона хвоста тика: сборка GPU-инстансов актёров.
//
// Раньше это жило прямо в actor_simulation.h, и из-за одного `instance_layout::rgba8_color`
// причинный заголовок тянул libs/painter. Теперь причинное ядро объявляет только
// `tick_tail_reader`, а всё, что знает про GPU-раскладку и страйд, лежит здесь и линкуется
// исключительно в клиент. Headless-авторитет этот заголовок не включает вовсе.

namespace frontier_online {
namespace core {

// GPU instance for actor draw group. Layout: "v2ui1c4v1".
struct actor_instance {
  glm::vec2 pos;
  uint32_t texture;
  instance_layout::rgba8_color color;
  float size;
};

class actor_batch final : public tick_tail_reader {
public:
  instance_layout::match_result bind(const std::string_view& layout = "v2ui1c4v1") {
    return intent_.bind(layout);
  }
  bool valid() const noexcept {
    return intent_.valid();
  }

  // tick нужен для анимации (синусоида масштаба по текущему состоянию FSM, выводимая, не хранимая).
  void read(const devils_engine::aesthetics::world& world, uint64_t tick) override;

  uint32_t produced() const noexcept override {
    return count();
  }

  std::span<const actor_instance> instances() const noexcept {
    return instances_;
  }
  std::span<const uint32_t> ids() const noexcept {
    return ids_;
  }
  uint32_t count() const noexcept {
    return uint32_t(instances_.size());
  }
  static constexpr uint32_t stride() noexcept {
    return draw_intent<actor_instance>::stride();
  }

  std::size_t blit(const std::span<uint8_t>& dst) const {
    return intent_.blit(std::span<const actor_instance>(instances_), dst);
  }

private:
  draw_intent<actor_instance> intent_;
  std::vector<actor_instance> instances_;
  std::vector<uint32_t> ids_;
};

} // namespace core
} // namespace frontier_online

#endif
