#include <cmath>

#include <devils_engine/utils/core.h>

#include "actor_batch.h"

// Презентационный читатель хвоста тика. Здесь лежит ровно то, что раньше делало причинный
// заголовок зависимым от libs/painter: GPU-раскладка инстанса и ВЫВОДИМАЯ анимация масштаба.
// Ни то, ни другое не является состоянием и не едет ни в checkpoint, ни по проводу.

using namespace devils_engine;

namespace frontier_online {
namespace core {

namespace {

// Анимация масштаба = синусоида, частота/амплитуда зависят от состояния FSM. ВЫВОДИМАЯ
// величина (из state + tick + пер-акторной фазы) — НЕ хранится, не пишется в save. Частота в
// циклах/тик (на main_fps≈20: 0.03 ц/т ≈ 0.6Гц медленно … 0.28 ц/т ≈ 5.6Гц паника).
// «Думаю — медленная синусоида, гонюсь/ем — быстрая» (как просил автор).
static float animation_scale(const uint64_t state, const uint64_t tick, const uint32_t phase) noexcept {
  static const uint64_t h_think = utils::string_hash("think");
  static const uint64_t h_wander = utils::string_hash("wander");
  static const uint64_t h_seek = utils::string_hash("seek_food");
  static const uint64_t h_chase = utils::string_hash("chase");
  static const uint64_t h_flee = utils::string_hash("flee");
  static const uint64_t h_eating = utils::string_hash("eating");
  static const uint64_t h_eaten = utils::string_hash("eaten");

  float freq = 0.05f, amp = 0.10f; // дефолт
  if (state == h_think) {
    freq = 0.03f;
    amp = 0.12f;
  } // медленное «дыхание» раздумья
  else if (state == h_wander) {
    freq = 0.07f;
    amp = 0.10f;
  } else if (state == h_seek) {
    freq = 0.12f;
    amp = 0.12f;
  } else if (state == h_chase) {
    freq = 0.20f;
    amp = 0.16f;
  } // возбуждённо
  else if (state == h_flee) {
    freq = 0.28f;
    amp = 0.18f;
  } // паника
  else if (state == h_eating) {
    freq = 0.40f;
    amp = 0.22f;
  } // быстрая синусоида — кушаем
  else if (state == h_eaten) {
    freq = 0.55f;
    amp = 0.30f;
  } // жертва бьётся в захвате

  const float t = float(tick + uint64_t(phase));
  return 1.0f + amp * std::sin(6.28318530718f * freq * t);
}

} // namespace

void actor_batch::read(const aesthetics::world& world, const uint64_t tick) {
  instances_.clear();
  ids_.clear();
  instances_.reserve(world.count<actor_visual>());
  ids_.reserve(world.count<actor_visual>());

  // Один проход по всему рисуемому (актёры И будущие предметы). У кого есть actor_state —
  // масштаб анимируется синусоидой по состоянию; у предметов/препятствий (нет state) —
  // базовый размер. get<> по id — O(1) sparse-set, проход не децимируется, цена приемлема.
  for (auto [id, pos, visual] : world.view<actor_position, actor_visual>()) {
    float scale = visual->size;
    if (const auto* st = world.get<actor_state>(id); st != nullptr) {
      const auto* brain = world.get<actor_brain>(id);
      scale *= animation_scale(st->state, tick, brain != nullptr ? brain->phase : 0u);
    }
    ids_.push_back(uint32_t(id));
    instances_.push_back(actor_instance{
      pos->value, visual->texture, instance_layout::rgba8_color{visual->color.value}, scale});
  }
}

} // namespace core
} // namespace frontier_online
