#ifndef DEVILS_ENGINE_SIMUL_COGNITION_SCHEDULER_H
#define DEVILS_ENGINE_SIMUL_COGNITION_SCHEDULER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <thread>
#include <vector>

#include <devils_engine/thread/atomic_pool.h>

// Общий планировщик «когниции» (think-фазы) для систем над ECS: из множества сущностей отбирает
// в пределах бюджета созревших (приоритет — давность решения) и раскидывает их перебор по потокам
// пула, отдавая каждому потоку свою scratch-полосу. Это переиспользуемая MT-машинерия, вынесенная
// из прототипа actor-слайса (см. ROADMAP п.11 / docs/simul-extraction-design.md, шаг 1): подсистема
// поставляет свою политику (skip/maturity через колбэк enumerate, сам think через decide) и знание
// формы сущности (index_of для детерминированного тай-брейка бюджета), а планировщик владеет только
// generic-частью — budget-clamp, per-thread scratch-полосами и distribute/compute/wait.
//
// ВЫХОД — на стороне decide. Планировщик НЕ собирает и НЕ сортирует результаты: сброс думанья в
// сообщения делает сам decide, обычно записью в aesthetics::message_buffer, где слот адресуется
// индексом сущности (разные потоки думают разные сущности ⇒ пишут непересекающиеся слоты без локов,
// а обход буфера в порядке индекса даёт детерминированный apply без явной сортировки). Так
// планировщик не зависит ни от libs/act, ни от libs/aesthetics — только знает entity + scratch.
// Это «select/work-list шаг» целевого пайплайна; map-часть think = distribute поверх отобранных.
//
// Обязанности:
//   - отбор + обрезка бюджетом (nth_element по overdue↓, детерминированный тай-брейк по index_of);
//   - per-thread scratch-полосы (slots = pool.size()+1, индекс = pool.thread_index; 0 = вызывающий
//     поток, 1..size = воркеры) — переиспользуемые между тиками, реаллокация лишь при смене
//     числа потоков;
//   - distribute1/compute/wait (вызывающий поток обрабатывает свой чанк, а не простаивает).
//
// Инварианты: параметры и алгоритм подобраны под детерминизм лог-реплея — та же входная выборка
// даёт тот же отобранный набор независимо от числа воркеров, а детерминизм ПОРЯДКА выхода
// обеспечивает потребитель (message_buffer). Scratch-тип (`Scratch`) — параметр шаблона: у GOAP-
// актора это acumen::execution_scratch (A*+cache+ds VM), у FSM-only «мозга» — свой lane,
// планировщику всё равно.

namespace devils_engine {
namespace simul {

template <typename EntityId, typename Scratch>
class cognition_scheduler {
public:
  struct request {
    uint64_t overdue; // давность решения (tick - last_think); больше ⇒ приоритетнее
    EntityId entity;
  };

  // окно коммита: сущность держится решения commit_ticks тиков и переобдумывает не чаще. Спрос
  // ≈ N/commit_ticks, лаг ≈ commit_ticks. Подсистема использует matured() при заполнении due.
  size_t commit_ticks = 3;
  // потолок обдумываний за тик (спайк-сейфти). ДОЛЖЕН быть ≥ спроса (N/commit_ticks), иначе узким
  // местом станет бюджет, а не коммит. count-budget (а не time) ⇒ детерминированно.
  size_t think_budget = 2048;

  // «Созрел» = ещё не думал (last_think==0) ИЛИ истёк commit_ticks. Хелпер для колбэка enumerate.
  bool matured(const uint64_t last_think, const uint64_t tick) const noexcept {
    return last_think == 0 || (tick - last_think) >= commit_ticks;
  }

  // Один проход think-фазы.
  //   enumerate(std::vector<request>&): подсистема заполняет due созревшими (свой skip-предикат +
  //     matured()); overdue = tick - last_think.
  //   decide(EntityId, Scratch&): восприятие/планирование ОДНОЙ сущности в свой per-thread scratch;
  //     сброс результата (обычно store в message_buffer по индексу сущности) — на стороне decide.
  //     Полосы эксклюзивны на поток ⇒ гонок по scratch нет; писать decide обязан только «свою»
  //     сущность (её слот) ⇒ гонок по выходу тоже нет.
  //   index_of(EntityId) -> uint64_t: детерминированный ключ (тай-брейк бюджета).
  template <typename Enumerate, typename Decide, typename IndexOf>
  void run(const uint64_t /*tick*/, thread::atomic_pool& pool,
           Enumerate&& enumerate, Decide&& decide, IndexOf&& index_of) {
    due_.clear();
    enumerate(due_);

    // (1) обрезка бюджетом: дольше ждавшие впереди, тай-брейк по index_of (детерминированно).
    if (due_.size() > think_budget) {
      std::nth_element(due_.begin(), due_.begin() + think_budget, due_.end(),
                       [&index_of](const request& a, const request& b) {
                         if (a.overdue != b.overdue) {
                           return a.overdue > b.overdue;
                         }
                         return index_of(a.entity) < index_of(b.entity);
                       });
      due_.resize(think_budget);
    }
    if (due_.empty()) {
      return;
    }

    // (2) per-thread scratch: слот = pool.thread_index. Реаллокация только при смене числа потоков
    //     (обычно один раз). deque — стабильные/non-movable VM-контексты.
    const size_t slots = pool.size() + 1;
    if (lanes_.size() != slots) {
      lanes_.resize(slots);
    }

    // (3) раскидать по потокам ВКЛЮЧАЯ вызывающий (distribute1 делит на size()+1, слот 0 = caller):
    //     нарезающий поток не простаивает, а сам берёт свой чанк через compute(). Лямбда — lvalue
    //     (distribute1 копирует её на каждый чанк). Чанк зовётся как f(start, job_count).
    auto job = [this, &pool, &decide](const size_t start, const size_t count) {
      const uint32_t slot = pool.thread_index(std::this_thread::get_id());
      auto& scratch = lanes_[slot];
      for (size_t i = start; i < start + count; ++i) {
        decide(due_[i].entity, scratch);
      }
    };
    pool.distribute1(due_.size(), job);
    pool.compute(); // вызывающий поток обрабатывает свой чанк (слот 0), а не ждёт вхолостую
    pool.wait();    // затем ждём опустошения очереди И завершения задач в работе на воркерах
  }

  // Scratch-полосы. Слот 0 (вызывающий поток) подсистема может переиспользовать в apply-фазе
  // (когниция к тому моменту завершена). Пусто, пока не было ни одного непустого run().
  std::deque<Scratch>& lanes() noexcept {
    return lanes_;
  }
  const std::deque<Scratch>& lanes() const noexcept {
    return lanes_;
  }

private:
  std::deque<Scratch> lanes_;    // per-thread scratch (0=вызывающий, 1..=воркеры)
  std::vector<request> due_;     // переиспользуемый скретч отбора
};

} // namespace simul
} // namespace devils_engine

#endif
