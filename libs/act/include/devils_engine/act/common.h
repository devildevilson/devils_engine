#ifndef DEVILS_ENGINE_ACT_COMMON_H
#define DEVILS_ENGINE_ACT_COMMON_H

#include <cstdint>

namespace devils_engine {
namespace act {
// Базовые типы геймплейного слоя. В ЛОГ/сеть/диск едет ГОЛЫЙ id (entity_id), «тип»
// различается КОМПОНЕНТАМИ в ECS, не тегом на хэндле. Есть invoke-time fat-handle
// (act::entity_handle в packer.h) — склейка {world, id} только на ВРЕМЯ вызова, чтобы
// эффект писался без exec_context; он НЕ хранится и НЕ сериализуется (указатель
// непереносим), поэтому это не противоречит «по сети едет голый id».

struct entity_id {
  uint32_t id = 0xffffffff;
  bool valid() const noexcept {
    return id != 0xffffffff;
  }
  bool operator==(const entity_id&) const noexcept = default;
};

// Число симуляции. СЕЙЧАС double; когда придёт детерминизм — поменять на fixed
// (devils_script уже умеет кастомный арифметический тип). Один typedef — одна правка.
using real_t = double;

struct vec3 {
  real_t x = 0, y = 0, z = 0;
};

} // namespace act
} // namespace devils_engine

#endif
