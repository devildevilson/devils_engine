#ifndef DEVILS_ENGINE_AESTHETICS_COLLECT_BUFFER_H
#define DEVILS_ENGINE_AESTHETICS_COLLECT_BUFFER_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

#include "devils_engine/utils/core.h"

// collect_buffer<Msg> — примитив АГРЕГАЦИИ (reduce типа collect, ROADMAP п.16): плоский конкурентный
// буфер сообщений ОДНОГО типа. В отличие от elect_buffer (atomic-min победитель на цель) и message_buffer
// (слот на сущность), сбор здесь НЕ ключуется сущностью — все продюсеры дописывают в ОДИН буфер через
// atomic fetch_add (lock-free), а группировка/детерминизм достигаются ОТДЕЛЬНЫМИ шагами: sort(cmp) по
// ключу сообщения → for_each_group скользящим окном по отсортированному буферу (соседи одной группы идут
// подряд). Так снимается ограничение per-entity Cap: одна общая ёмкость на тип.
//
// Требование collect (отличается от elect): НИЧЕГО НЕ ТЕРЯТЬ. Переполнение ёмкости = ГРОМКАЯ ОШИБКА
// (не тихий дроп) — потеря кумулятивного сообщения (урон/провенанс) это баг сайзинга, не штатное состояние.
//
// Детерминизм: порядок fetch_add НЕдетерминирован (гонка потоков) ⇒ и порядок буфера после push; его
// восстанавливает sort(cmp). Для побайтовой воспроизводимости cmp должен задавать ПОЛНЫЙ порядок
// (уникальный ключ на сообщение), либо потребитель обязан быть порядок-независимым внутри группы (сумма).
//
// Контракт: reset(capacity) на главном потоке ДО параллельной фазы (ёмкость с запасом на max сообщений
// за тик); push() из воркеров безопасен (atomic индекс, реаллокаций нет). sort/data/for_each_group — ПОСЛЕ
// барьера пула. count_ — plain size_t, конкурентно трогается ТОЛЬКО через atomic_ref в push (буфер movable).

namespace devils_engine {
namespace aesthetics {

template <typename Msg>
class collect_buffer {
public:
  size_t capacity() const noexcept {
    return storage_.size();
  }
  size_t size() const noexcept {
    return count_; // plain-чтение после барьера
  }
  bool empty() const noexcept {
    return count_ == 0;
  }

  // Задать ёмкость (с запасом) и очистить. Storage переиспользуется между тиками (растёт по необходимости).
  void reset(const size_t capacity) {
    if (storage_.size() < capacity) {
      storage_.resize(capacity);
    }
    count_ = 0;
  }

  void clear() noexcept {
    count_ = 0;
  }

  // Дописать сообщение. Потокобезопасно (atomic индекс). Переполнение ёмкости — ГРОМКАЯ ОШИБКА.
  void push(const Msg& m) {
    const size_t i = std::atomic_ref<size_t>(count_).fetch_add(1, std::memory_order_relaxed);
    if (i >= storage_.size()) {
      utils::error{}("collect_buffer: capacity {} exceeded — reset() with enough headroom; collect must not drop messages", storage_.size());
    }
    storage_[i] = m;
  }

  // Собранные сообщения [0, size). До sort() порядок = порядок push (недетерминирован). Чтение после барьера.
  std::span<Msg> data() noexcept {
    return {storage_.data(), count_};
  }
  std::span<const Msg> data() const noexcept {
    return {storage_.data(), count_};
  }

  // Отсортировать собранное по ключу сообщения (шаг детерминизации). Однопоточно, после барьера.
  template <typename Cmp>
  void sort(Cmp cmp) {
    std::sort(storage_.begin(), storage_.begin() + count_, cmp);
  }

  // Скользящее окно по ОТСОРТИРОВАННОМУ буферу: fn(std::span<const Msg>) на каждый максимальный ран
  // соседей, для которых same(a, b) истинно (одна группа — напр. одна цель). Требует sort по тому же ключу.
  template <typename Same, typename Fn>
  void for_each_group(Same same, Fn fn) const {
    const size_t n = count_;
    size_t i = 0;
    while (i < n) {
      size_t j = i + 1;
      while (j < n && same(storage_[i], storage_[j])) {
        ++j;
      }
      fn(std::span<const Msg>(storage_.data() + i, j - i));
      i = j;
    }
  }

private:
  std::vector<Msg> storage_;
  size_t count_ = 0; // число push; конкурентно — только через atomic_ref в push
};

} // namespace aesthetics
} // namespace devils_engine

#endif
