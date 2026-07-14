#ifndef DEVILS_ENGINE_AESTHETICS_MESSAGE_BUFFER_H
#define DEVILS_ENGINE_AESTHETICS_MESSAGE_BUFFER_H

#include <cstddef>
#include <vector>

#include "common.h"
#include "devils_engine/utils/core.h"

// message_buffer — плотный по-энтити канал сообщений одного типа: примитив коммуникации между
// фазами декларативного пайплайна (process query_t<> → message_buffer → process query_t<> → …,
// см. ROADMAP п.11 / docs/simul-extraction-design.md). Слот адресуется get_entityid_index(entity),
// хранилище — плоский массив во всю ёмкость индексов мира. Отсюда два ключевых свойства:
//   - ЗАПИСЬ БЕЗ ЛОКОВ: если разные потоки пишут разным сущностям (инвариант map-фазы — каждую
//     сущность обрабатывает ровно один поток), они трогают непересекающиеся слоты ⇒ гонок нет,
//     per-thread staging и последующий merge не нужны;
//   - ДЕТЕРМИНИРОВАННЫЙ ОБХОД БЕЗ SORT: for_each идёт по слотам в порядке возрастания индекса ⇒
//     потребитель (apply-фаза) видит сообщения в стабильном порядке независимо от раскладки по
//     потокам, без явной сортировки выхода.
// «Индексация по (message_type, entity)» из дизайна реализуется так: один message_buffer<Msg> =
// один канал типа Msg, ось entity — слот внутри. keys_[i] == invalid_entityid помечает пустой слот
// (и заодно несёт полный entityid для for_each, не требуя, чтобы сам Msg хранил свою сущность).
//
// Контракт использования: буфер сайзится reset()/на главном потоке ДО параллельной фазы (ёмкость
// должна покрывать максимальный индекс, в который будут писать — обычно world::index_capacity()),
// после чего store() из воркеров безопасен, т.к. реаллокации нет. Одно сообщение на сущность за
// проход (повторный store перезаписывает — last-write-wins).

namespace devils_engine {
namespace aesthetics {

template <typename Msg>
class message_buffer {
public:
  // Число слотов = ёмкость по индексу сущности (не число присутствующих сообщений).
  size_t capacity() const noexcept {
    return keys_.size();
  }

  // Задать ёмкость (покрыть индексы < capacity) и очистить присутствие. Storage переиспользуется
  // между тиками: capacity только растёт, повторный reset той же величины — просто clear().
  void reset(const size_t capacity) {
    if (slots_.size() < capacity) {
      slots_.resize(capacity);
    }
    keys_.assign(capacity, invalid_entityid);
  }

  // Очистить присутствие, сохранив ёмкость/память.
  void clear() noexcept {
    for (auto& k : keys_) {
      k = invalid_entityid;
    }
  }

  // Записать сообщение сущности. Потокобезопасно ТОЛЬКО при непересекающихся сущностях у разных
  // потоков (инвариант map-фазы). Ёмкость должна быть задана заранее — иначе это ошибка сайзинга.
  void store(const entityid_t entity, const Msg& msg) {
    slots_[checked_index(entity)] = msg;
    keys_[get_entityid_index(entity)] = entity;
  }

  // Занять слот сущности под запись на месте (пометив присутствие) и вернуть ссылку.
  Msg& emplace(const entityid_t entity) {
    const size_t idx = checked_index(entity);
    keys_[idx] = entity;
    return slots_[idx];
  }

  bool has(const entityid_t entity) const noexcept {
    const size_t idx = get_entityid_index(entity);
    return idx < keys_.size() && keys_[idx] != invalid_entityid;
  }

  const Msg* find(const entityid_t entity) const noexcept {
    const size_t idx = get_entityid_index(entity);
    if (idx >= keys_.size() || keys_[idx] == invalid_entityid) {
      return nullptr;
    }
    return &slots_[idx];
  }

  // Число присутствующих сообщений (скан присутствия). Дёшево на масштабах тика; вызывается редко
  // (метрики). Отдельный счётчик не держим — его инкремент из воркеров был бы гонкой.
  size_t size() const noexcept {
    size_t n = 0;
    for (const auto k : keys_) {
      n += (k != invalid_entityid);
    }
    return n;
  }

  bool empty() const noexcept {
    for (const auto k : keys_) {
      if (k != invalid_entityid) {
        return false;
      }
    }
    return true;
  }

  // Детерминированный обход присутствующих сообщений в порядке возрастания индекса сущности.
  // fn(entityid_t, const Msg&). Заменяет merge+sort выхода фазы.
  template <typename Fn>
  void for_each(Fn&& fn) const {
    for (size_t i = 0; i < keys_.size(); ++i) {
      if (keys_[i] != invalid_entityid) {
        fn(keys_[i], slots_[i]);
      }
    }
  }

private:
  size_t checked_index(const entityid_t entity) const {
    const size_t idx = get_entityid_index(entity);
    if (idx >= keys_.size()) {
      utils::error{}("message_buffer: entity index {} out of capacity {} — reset() the buffer to cover all live entity indices before the parallel write phase", idx, keys_.size());
    }
    return idx;
  }

  std::vector<Msg> slots_;         // сообщения, адресуемые индексом сущности
  std::vector<entityid_t> keys_;   // полный entityid слота; invalid_entityid = пусто
};

} // namespace aesthetics
} // namespace devils_engine

#endif
