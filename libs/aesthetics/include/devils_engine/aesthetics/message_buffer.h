#ifndef DEVILS_ENGINE_AESTHETICS_MESSAGE_BUFFER_H
#define DEVILS_ENGINE_AESTHETICS_MESSAGE_BUFFER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "common.h"
#include "devils_engine/utils/core.h"

// message_buffer — плотный по-энтити канал сообщений одного типа: примитив коммуникации между
// фазами декларативного пайплайна (process query_t<> → message_buffer → process query_t<> → …,
// см. ROADMAP п.11). Слот адресуется get_entityid_index(entity);
// на слот приходится МАЛЕНЬКИЙ ФИКС-БАКЕТ std::array<Msg, Cap> + счётчик, т.е. на одну сущность можно
// сложить до Cap сообщений одного типа за проход (актор может выдать несколько intent'ов за кадр;
// звук — основной + follow-up и т.п.). Отсюда свойства:
//   - ЗАПИСЬ БЕЗ ЛОКОВ: если разные потоки пишут разным сущностям (инвариант map-фазы — каждую
//     сущность обрабатывает ровно один поток), они трогают непересекающиеся бакеты ⇒ гонок нет,
//     per-thread staging и merge не нужны; push в свой бакет из одного потока последователен;
//   - ДЕТЕРМИНИРОВАННЫЙ ОБХОД БЕЗ SORT: for_each идёт по сущностям в порядке возрастания индекса, а
//     внутри бакета — в порядке push ⇒ потребитель видит стабильный порядок без сортировки выхода.
// «Индексация по (message_type, entity)» из дизайна: один message_buffer<Msg> = один канал типа Msg,
// ось entity — слот, а несколько сообщений на сущность — бакет.
//
// Ёмкость бакета Cap задаётся ПО ТИПУ через трейт message_capacity<Msg> (дефолт 1 = одно сообщение на
// сущность). Типы, которых бывает несколько на актора, специализируют трейт (напр. 4). message_registry
// подхватывает Cap из трейта автоматически (channel<Msg>() → message_buffer<Msg>).
//
// Контракт: буфер сайзится reset() на главном потоке ДО параллельной фазы (ёмкость покрывает макс.
// индекс сущности — обычно world::index_capacity()); тогда push() из воркеров безопасен (реаллокаций
// нет). push сверх Cap отбрасывает сообщение и возвращает false (переполнение бакета — не гонка).

namespace devils_engine {
namespace aesthetics {

// Ёмкость бакета на сущность для типа Msg (сколько сообщений одного типа можно сложить одной сущности
// за проход). Специализируй для типов, которых бывает несколько на актора.
template <typename Msg>
struct message_capacity {
  static constexpr size_t value = 1;
};

template <typename Msg, size_t Cap = message_capacity<Msg>::value>
class message_buffer {
public:
  static_assert(Cap >= 1 && Cap <= 255, "message_buffer bucket capacity must be in [1, 255]");

  // Число слотов = ёмкость по индексу сущности (не число сообщений).
  size_t capacity() const noexcept {
    return counts_.size();
  }
  static constexpr size_t bucket_capacity() noexcept {
    return Cap;
  }

  // Задать ёмкость (покрыть индексы < capacity) и очистить бакеты. Storage переиспользуется между
  // тиками: capacity только растёт, повторный reset той же величины — просто clear().
  void reset(const size_t capacity) {
    if (slots_.size() < capacity) {
      slots_.resize(capacity);
      keys_.resize(capacity, invalid_entityid);
    }
    counts_.assign(capacity, 0);
  }

  // Очистить бакеты, сохранив ёмкость/память.
  void clear() noexcept {
    for (auto& c : counts_) {
      c = 0;
    }
  }

  // Дописать сообщение в бакет сущности. Потокобезопасно ТОЛЬКО при непересекающихся сущностях у
  // разных потоков (инвариант map-фазы). Возвращает false, если бакет полон (> Cap на сущность).
  bool push(const entityid_t entity, const Msg& msg) {
    const size_t idx = checked_index(entity);
    if (counts_[idx] >= Cap) {
      return false;
    }
    slots_[idx][counts_[idx]] = msg;
    counts_[idx] += 1;
    keys_[idx] = entity;
    return true;
  }

  bool has(const entityid_t entity) const noexcept {
    const size_t idx = get_entityid_index(entity);
    return idx < counts_.size() && counts_[idx] != 0;
  }

  uint8_t count(const entityid_t entity) const noexcept {
    const size_t idx = get_entityid_index(entity);
    return idx < counts_.size() ? counts_[idx] : uint8_t(0);
  }

  // Бакет сущности как span (в порядке push) или пустой span, если сообщений нет.
  std::span<const Msg> find(const entityid_t entity) const noexcept {
    const size_t idx = get_entityid_index(entity);
    if (idx >= counts_.size() || counts_[idx] == 0) {
      return {};
    }
    return std::span<const Msg>(slots_[idx].data(), counts_[idx]);
  }

  // Число присутствующих сущностей (с непустым бакетом). Скан присутствия; вызывается редко.
  size_t size() const noexcept {
    size_t n = 0;
    for (const auto c : counts_) {
      n += (c != 0);
    }
    return n;
  }

  // Суммарное число сообщений во всех бакетах.
  size_t total() const noexcept {
    size_t n = 0;
    for (const auto c : counts_) {
      n += c;
    }
    return n;
  }

  bool empty() const noexcept {
    for (const auto c : counts_) {
      if (c != 0) {
        return false;
      }
    }
    return true;
  }

  // Детерминированный обход: сущности по возрастанию индекса, внутри — бакет в порядке push.
  // fn(entityid_t, const Msg&) вызывается на КАЖДОЕ сообщение. Заменяет merge+sort выхода фазы.
  template <typename Fn>
  void for_each(Fn&& fn) const {
    for (size_t i = 0; i < counts_.size(); ++i) {
      for (uint8_t j = 0; j < counts_[i]; ++j) {
        fn(keys_[i], slots_[i][j]);
      }
    }
  }

private:
  size_t checked_index(const entityid_t entity) const {
    const size_t idx = get_entityid_index(entity);
    if (idx >= counts_.size()) {
      utils::error{}("message_buffer: entity index {} out of capacity {} — reset() the buffer to cover all live entity indices before the parallel write phase", idx, counts_.size());
    }
    return idx;
  }

  std::vector<std::array<Msg, Cap>> slots_; // бакеты сообщений, адресуемые индексом сущности
  std::vector<uint8_t> counts_;             // размер бакета каждой сущности (0 = пусто)
  std::vector<entityid_t> keys_;            // полный entityid слота (для for_each)
};

} // namespace aesthetics
} // namespace devils_engine

#endif
