#ifndef DEVILS_ENGINE_AESTHETICS_ELECT_BUFFER_H
#define DEVILS_ENGINE_AESTHETICS_ELECT_BUFFER_H

#include <atomic>
#include <cstddef>
#include <vector>

#include "common.h"
#include "devils_engine/utils/core.h"

// elect_buffer — примитив АРБИТРАЖА взаимодействий: на каждую ЦЕЛЬ выбирает единственного победителя
// среди претендентов lock-free минимумом по entityid (ROADMAP п.16, память entity-interaction-model).
// Слот адресуется get_entityid_index(target); значение слота — наименьший claimant.id, обновляемый
// атомарным минимумом. Отсюда свойства:
//   - ЗАПИСЬ БЕЗ ЛОКОВ many→one: в отличие от message_buffer (каждый пишет СВОЙ слот), сюда МНОГО
//     претендентов пишут слот ОДНОЙ цели — поэтому это atomic-min (std::atomic_ref), а не append;
//   - ДЕТЕРМИНИЗМ БЕЗ СОРТИРОВКИ: финальный минимум не зависит от порядка потоков ⇒ победитель
//     воспроизводим побайтово. Полный entityid несёт индекс в старших битах ⇒ min по id ≈ min по
//     индексу («младший id побеждает» — то же правило, что раньше давал порядок apply, но теперь явное).
//
// Правило победы СОЗНАТЕЛЬНО техническое (min по id, без приоритетов) — это «закон природы» движка;
// «кто именно вправе победить» (сила/размер) — это eligibility конкретного эффекта, примитив её не знает.
//
// Контракт: reset() на главном потоке ДО параллельной фазы (ёмкость покрывает макс. индекс цели —
// обычно world::index_capacity()); тогда claim() из воркеров безопасен (реаллокаций нет). Чтение
// winner()/won()/for_each_winner() — ПОСЛЕ барьера пула (happens-after всех claim).

namespace devils_engine {
namespace aesthetics {

class elect_buffer {
public:
  // Число слотов = ёмкость по индексу цели.
  size_t capacity() const noexcept {
    return slots_.size();
  }

  // Задать ёмкость (покрыть индексы < capacity) и сбросить выборы. Storage переиспользуется между
  // тиками: capacity растёт по необходимости, слоты каждый тик инициализируются нейтралью для min.
  void reset(const size_t capacity) {
    if (keys_.size() < capacity) {
      keys_.resize(capacity);
    }
    slots_.assign(capacity, invalid_entityid); // invalid = UINT32_MAX = нейтраль для минимума
  }

  // Сбросить выборы, сохранив ёмкость/память.
  void clear() noexcept {
    for (auto& s : slots_) {
      s = invalid_entityid;
    }
  }

  // Заявить претензию claimant на target. Атомарный минимум: побеждает наименьший claimant.id.
  // Потокобезопасно при непересекающихся claimant'ах у разных потоков (инвариант map-фазы); слот
  // цели могут атаковать МНОГО потоков — на то и atomic-min. Ёмкость должна покрывать индекс цели.
  void claim(const entityid_t target, const entityid_t claimant) {
    const size_t idx = checked_index(target);
    std::atomic_ref<entityid_t> slot(slots_[idx]);
    entityid_t cur = slot.load(std::memory_order_relaxed);
    while (claimant < cur && !slot.compare_exchange_weak(cur, claimant, std::memory_order_relaxed)) {
      // compare_exchange_weak обновляет cur при провале ⇒ перепроверяем условие минимума
    }
    // полный id цели для for_each_winner: все претенденты пишут ОДНО значение ⇒ benign, но релакс-атомик
    // ради чистоты модели памяти (конкурентные записи одной ячейки).
    std::atomic_ref<entityid_t> key(keys_[idx]);
    key.store(target, std::memory_order_relaxed);
  }

  // Победитель цели (наименьший заявившийся claimant.id) или invalid_entityid, если претензий не было.
  entityid_t winner(const entityid_t target) const noexcept {
    const size_t idx = get_entityid_index(target);
    return idx < slots_.size() ? slots_[idx] : invalid_entityid;
  }

  bool has_winner(const entityid_t target) const noexcept {
    return !is_invalid_entityid(winner(target));
  }

  // Победил ли именно этот claimant за эту цель (типовая проверка на commit-фазе).
  bool won(const entityid_t target, const entityid_t claimant) const noexcept {
    return winner(target) == claimant;
  }

  // Число цепей с победителем (оспариваемых целей). Скан; вызывается редко.
  size_t size() const noexcept {
    size_t n = 0;
    for (const auto s : slots_) {
      n += !is_invalid_entityid(s);
    }
    return n;
  }

  bool empty() const noexcept {
    for (const auto s : slots_) {
      if (!is_invalid_entityid(s)) {
        return false;
      }
    }
    return true;
  }

  // Детерминированный обход победителей по возрастанию индекса цели. fn(entityid_t target, entityid_t winner).
  template <typename Fn>
  void for_each_winner(Fn&& fn) const {
    for (size_t i = 0; i < slots_.size(); ++i) {
      if (!is_invalid_entityid(slots_[i])) {
        fn(keys_[i], slots_[i]);
      }
    }
  }

private:
  size_t checked_index(const entityid_t entity) const {
    const size_t idx = get_entityid_index(entity);
    if (idx >= slots_.size()) {
      utils::error{}("elect_buffer: target index {} out of capacity {} — reset() the buffer to cover all live entity indices before the parallel claim phase", idx, slots_.size());
    }
    return idx;
  }

  std::vector<entityid_t> slots_; // победитель (наименьший claimant.id) на индекс цели; invalid = нет претензий
  std::vector<entityid_t> keys_;  // полный entityid цели (для for_each_winner)
};

} // namespace aesthetics
} // namespace devils_engine

#endif
