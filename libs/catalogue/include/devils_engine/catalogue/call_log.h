#ifndef DEVILS_ENGINE_CATALOGUE_CALL_LOG_H
#define DEVILS_ENGINE_CATALOGUE_CALL_LOG_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "devils_engine/utils/core.h"

// call_log — контейнер ОТЛОЖЕННЫХ ВЫЗОВОВ: ровно две задачи — (а) сложить вызов (id функции + участники)
// и (б) проиграть контейнер позже. Ядро deferred-call механизма взаимодействий (ROADMAP п.16): think-фаза
// записывает вызовы (record), commit-фаза их проигрывает (replay); ГЕЙТ победителей и сам reduce живут в
// вызывающем слое (catalogue ниже act/aesthetics по зависимостям и НАМЕРЕННО про них не знает — участники
// приезжают сырыми uint, их смысл (entity_id/скаляр) задаёт вызывающий слой).
//
// Хранилище — плотный per-index массив: слот = индекс инициатора (задаёт вызывающий). Разные инициаторы
// пишут разные слоты ⇒ MT-запись БЕЗ ЛОКОВ (инвариант map-фазы), обход replay по индексу ⇒ детерминизм
// без сортировки. Тот же принцип, что у aesthetics::message_buffer/elect_buffer, но generic и без ECS.
//
// Контракт: reset(capacity) на главном потоке ДО параллельной записи (ёмкость покрывает макс. индекс
// инициатора); тогда record() из воркеров безопасен (реаллокаций нет). Один вызов на слот (last-write-wins).

namespace devils_engine {
namespace catalogue {

// Одна запись отложенного вызова: id функции + до двух участников. Минимально под текущую нужду
// (взаимодействие = эффект + primary/target); расширится generic-аргументами при необходимости.
struct call_record {
  uint64_t fn = 0;      // id геймплейной функции (напр. act::fn_id = string_hash имени)
  uint32_t primary = 0; // главный участник (scope[0])
  uint32_t target = 0;  // вторичный участник (scope[1]) или invalid, если не нужен
};

class call_log {
public:
  size_t capacity() const noexcept {
    return present_.size();
  }

  // Задать ёмкость по индексу инициатора и очистить. Переиспользуется между тиками (capacity растёт).
  void reset(const size_t capacity) {
    if (slots_.size() < capacity) {
      slots_.resize(capacity);
    }
    present_.assign(capacity, 0);
  }

  void clear() noexcept {
    for (auto& p : present_) {
      p = 0;
    }
  }

  // Записать вызов в слот index (обычно индекс инициатора). Потокобезопасно при непересекающихся index
  // у разных потоков. Ёмкость должна покрывать index — иначе ошибка сайзинга. last-write-wins.
  void record(const size_t index, const call_record& rec) {
    if (index >= present_.size()) {
      utils::error{}("call_log: index {} out of capacity {} — reset() before the parallel record phase", index, present_.size());
    }
    slots_[index] = rec;
    present_[index] = 1;
  }

  bool has(const size_t index) const noexcept {
    return index < present_.size() && present_[index] != 0;
  }

  size_t size() const noexcept {
    size_t n = 0;
    for (const auto p : present_) {
      n += (p != 0);
    }
    return n;
  }

  bool empty() const noexcept {
    for (const auto p : present_) {
      if (p != 0) {
        return false;
      }
    }
    return true;
  }

  // Детерминированный обход записанных вызовов по возрастанию индекса. fn(const call_record&).
  template <typename Fn>
  void replay(Fn&& fn) const {
    for (size_t i = 0; i < present_.size(); ++i) {
      if (present_[i]) {
        fn(slots_[i]);
      }
    }
  }

private:
  std::vector<call_record> slots_;
  std::vector<uint8_t> present_;
};

} // namespace catalogue
} // namespace devils_engine

#endif
