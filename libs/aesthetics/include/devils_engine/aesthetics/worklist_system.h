#ifndef DEVILS_ENGINE_AESTHETICS_WORKLIST_SYSTEM_H
#define DEVILS_ENGINE_AESTHETICS_WORKLIST_SYSTEM_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <thread>
#include <vector>

#include "common.h"
#include "devils_engine/thread/atomic_pool.h"
#include "system_runner.h"

// worklist_system — параллельный map по ЯВНОМУ списку сущностей (work-list) с per-thread scratch.
// В отличие от template_system (обход всего query по компонентам) обходит произвольное ПОДМНОЖЕСТВО,
// заранее отобранное вызывающим (select/budget-шаг). Это think-примитив бюджетируемых систем:
// [select → worklist] → [aesthetics::run → сообщения]. Каждому потоку — своя scratch-полоса
// (slot = pool.thread_index; 0 = вызывающий поток, он тоже берёт чанк), полосы переиспользуются между
// тиками (реаллокация лишь при смене числа потоков). process пишет ТОЛЬКО «свою» сущность (её слот в
// message_buffer / её компонент) ⇒ потоки трогают непересекающуюся память, локов нет. Детерминизм
// ПОРЯДКА выхода обеспечивает потребитель (message_buffer, обход по индексу), а не порядок work-list.
//
// Рядом — budget_clamp: generic обрезка списка кандидатов по приоритету (с детерминированным тай-
// брейком в переданном less) до заполнения work-list. Вместе они заменяют bespoke-планировщик
// «select + scratch-map» переиспользуемыми примитивами (см. ROADMAP п.11).

namespace devils_engine {
namespace aesthetics {

// Оставить budget самых приоритетных кандидатов (less(a,b)==true ⇒ a приоритетнее/раньше b), остальных
// отбросить. nth_element ⇒ частичная сортировка O(n). Детерминизм — забота less (тай-брейк). Мутирует due.
template <typename Request, typename Less>
void budget_clamp(std::vector<Request>& due, const size_t budget, Less&& less) {
  if (due.size() <= budget) {
    return;
  }
  std::nth_element(due.begin(), due.begin() + budget, due.end(), less);
  due.resize(budget);
}

template <typename Scratch>
class worklist_system {
public:
  worklist_system(thread::atomic_pool* pool) noexcept : pool_(pool) {}
  virtual ~worklist_system() noexcept = default;

  // Список сущностей на обработку (заполняет вызывающий из select-шага, очищает перед заполнением).
  std::vector<entityid_t>& worklist() noexcept {
    return worklist_;
  }
  const std::vector<entityid_t>& worklist() const noexcept {
    return worklist_;
  }

  // Полосы scratch (0 = вызывающий поток). Слот 0 можно переиспользовать в apply-фазе после run().
  std::deque<Scratch>& lanes() noexcept {
    return lanes_;
  }
  const std::deque<Scratch>& lanes() const noexcept {
    return lanes_;
  }

  // Добавить work-list в общую фазу без barrier. Вызывается aesthetics::run; публичен также как seam
  // для других внешних phase runners. Каждый элемент → process(entity, своя scratch-полоса, time).
  void enqueue(thread::atomic_pool& target_pool, const size_t time) {
    if (worklist_.empty()) {
      return;
    }
    const size_t slots = target_pool.size() + 1;
    if (lanes_.size() != slots) {
      lanes_.resize(slots);
    }
    target_pool.distribute1(
      worklist_.size(),
      [this, &target_pool](const size_t start, const size_t count, const size_t tick) {
        const uint32_t slot = target_pool.thread_index(std::this_thread::get_id());
        auto& scratch = lanes_[size_t(slot)];
        for (size_t i = start; i < start + count; ++i) {
          process(worklist_[i], scratch, tick);
        }
      },
      time);
  }

  // Совместимость старого одиночного вызова: lifecycle всё равно принадлежит общему runner.
  void run(const size_t time) {
    aesthetics::run(*pool_, time, *this);
  }

  // Обработать одну сущность в свою per-thread scratch-полосу. Реализует подкласс.
  virtual void process(entityid_t entity, Scratch& scratch, size_t time) = 0;

protected:
  thread::atomic_pool* pool_;
  std::vector<entityid_t> worklist_;
  std::deque<Scratch> lanes_;
};

} // namespace aesthetics
} // namespace devils_engine

#endif
