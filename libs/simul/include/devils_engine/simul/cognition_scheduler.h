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
// в пределах бюджета созревших (приоритет — давность решения), раскидывает их перебор по потокам
// пула и детерминированно сливает результат. Это переиспользуемая MT-машинерия, вынесенная из
// прототипа actor-слайса (см. ROADMAP п.11 / docs/simul-extraction-design.md, шаг 1): подсистема
// поставляет свою политику (skip/maturity через колбэк enumerate, сам think через decide) и знание
// формы сущности/интента (index_of/intent_key), а планировщик владеет только generic-частью —
// budget-clamp, per-thread scratch-полосами и детерминированным merge.
//
// Обязанности:
//   - отбор + обрезка бюджетом (nth_element по overdue↓, детерминированный тай-брейк по index_of);
//   - per-thread scratch-полосы (slots = pool.size()+1, индекс = pool.thread_index; 0 = вызывающий
//     поток, 1..size = воркеры) — переиспользуемые между тиками, реаллокация лишь при смене
//     числа потоков;
//   - distribute1/compute/wait (вызывающий поток обрабатывает свой чанк, а не простаивает) +
//     слияние per-thread буферов в выходной вектор + стабильная сортировка по intent_key ⇒ apply
//     не зависит от раскладки по потокам.
//
// Инварианты: параметры и алгоритм подобраны под детерминизм лог-реплея — та же входная выборка
// даёт тот же отсортированный выход независимо от числа воркеров. Scratch-тип (`Scratch`) —
// параметр шаблона: у GOAP-актора это acumen::execution_scratch (A*+cache+ds VM), у FSM-only
// «мозга» — свой lane, планировщику всё равно. Intent-тип тоже параметр: планировщик не зависит
// от libs/act.

namespace devils_engine {
namespace simul {

template <typename EntityId, typename Scratch, typename Intent>
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
  //   decide(EntityId, Scratch&, std::vector<Intent>&): восприятие/планирование ОДНОЙ сущности в
  //     свой per-thread scratch и выходной буфер (эксклюзивны на поток ⇒ гонок нет).
  //   index_of(EntityId) -> uint64_t: детерминированный ключ (тай-брейк бюджета).
  //   intent_key(const Intent&) -> uint64_t: ключ финальной сортировки выхода.
  // out очищается и заполняется отсортированными интентами.
  template <typename Enumerate, typename Decide, typename IndexOf, typename IntentKey>
  void run(const uint64_t /*tick*/, thread::atomic_pool& pool,
           Enumerate&& enumerate, Decide&& decide, IndexOf&& index_of, IntentKey&& intent_key,
           std::vector<Intent>& out) {
    out.clear();
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

    // (2) per-thread scratch/буферы: слот = pool.thread_index. Реаллокация только при смене числа
    //     потоков (обычно один раз). deque для scratch — стабильные/non-movable VM-контексты.
    const size_t slots = pool.size() + 1;
    if (lanes_.size() != slots) {
      lanes_.resize(slots);
    }
    if (buffers_.size() != slots) {
      buffers_.resize(slots);
    }
    for (auto& b : buffers_) {
      b.clear();
    }

    // (3) раскидать по потокам ВКЛЮЧАЯ вызывающий (distribute1 делит на size()+1, слот 0 = caller):
    //     нарезающий поток не простаивает, а сам берёт свой чанк через compute(). Лямбда — lvalue
    //     (distribute1 копирует её на каждый чанк). Чанк зовётся как f(start, job_count).
    auto job = [this, &pool, &decide](const size_t start, const size_t count) {
      const uint32_t slot = pool.thread_index(std::this_thread::get_id());
      auto& scratch = lanes_[slot];
      auto& out_buf = buffers_[slot];
      for (size_t i = start; i < start + count; ++i) {
        decide(due_[i].entity, scratch, out_buf);
      }
    };
    pool.distribute1(due_.size(), job);
    pool.compute(); // вызывающий поток обрабатывает свой чанк (слот 0), а не ждёт вхолостую
    pool.wait();    // затем ждём опустошения очереди И завершения задач в работе на воркерах

    // (4) слить выходы потоков и упорядочить детерминированно.
    for (auto& b : buffers_) {
      out.insert(out.end(), b.begin(), b.end());
    }
    std::sort(out.begin(), out.end(), [&intent_key](const Intent& a, const Intent& b) {
      return intent_key(a) < intent_key(b);
    });
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
  std::deque<Scratch> lanes_;               // per-thread scratch (0=вызывающий, 1..=воркеры)
  std::vector<std::vector<Intent>> buffers_; // per-thread выходные буферы интентов
  std::vector<request> due_;                 // переиспользуемый скретч отбора
};

} // namespace simul
} // namespace devils_engine

#endif
