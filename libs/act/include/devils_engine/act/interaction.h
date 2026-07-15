#ifndef DEVILS_ENGINE_ACT_INTERACTION_H
#define DEVILS_ENGINE_ACT_INTERACTION_H

#include <cstdint>

namespace devils_engine {
namespace act {
// interaction — ДЕСКРИПТОР арбитража эффекта-взаимодействия (ROADMAP п.16). Тегирует эффект в реестре:
// как разрешать конкуренцию за участника (reduce) и какой участник scope — оспариваемая цель. Сам reduce
// (elect/collect) и контейнер вызовов живут в других слоях (aesthetics/catalogue); здесь только «рецепт»
// рядом с эффектом, чтобы generic record/commit не хардкодили конкретные функции.
enum class arbitration : uint8_t {
  exclusive, // один победитель на цель (atomic-min по id) — eat/grab; reduce = elect
  // cumulative — collect (урон/атака), появится вместе с примитивом collect
};

struct interaction {
  arbitration rule = arbitration::exclusive;
  uint8_t target_scope = 1; // индекс участника scope, за которого конкуренция (обычно secondary)
  bool self_claim = true;   // помечать инициатора (правило «intent бьёт grab»: снимает каскад/симметрию)
};

} // namespace act
} // namespace devils_engine

#endif
