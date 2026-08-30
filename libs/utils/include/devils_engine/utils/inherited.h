#ifndef DEVILS_ENGINE_UTILS_INHERITED_H
#define DEVILS_ENGINE_UTILS_INHERITED_H

// Две мелочи, которые в игровом коде появляются снова и снова и каждый раз пишутся заново.
//
// ПЕРВАЯ — наследование ответа по иерархии. У комнаты нет своего уровня преступности; он есть у района, и
// комната его наследует. У района нет своей торговли; она есть у города. Общее правило: ответ несёт
// РОВНО ОДИН уровень, и не тот, на котором спрашивают, а запрос — подъём до ближайшего носителя.
//
// Соблазн — держать поле у каждого узла и «просто обновлять сверху вниз». Это и есть ошибка: десять тысяч
// чисел, обязанных меняться вместе, однажды разойдутся, и найти, где именно, будет нечем. Носитель один,
// остальные наследуют — тогда расходиться нечему.
//
// ВТОРАЯ — таблица рантайм-подмен поверх данных с диска. Дверь заперли, район отжали, титул узурпировали:
// файл говорит НАЧАЛЬНОЕ состояние, игра — текущее, и второе обязано переживать выгрузку сектора, в
// котором лежит первое. Подмен всегда мало относительно данных, поэтому это отсортированный вектор, а не
// хеш-таблица: меньше памяти, лучше локальность, и порядок обхода детерминирован — что важно, если по
// состоянию считается контрольная сумма.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace devils_engine {
namespace utils {

// Ближайший вверх по иерархии узел, который НЕСЁТ ответ. `parent_of` возвращает родителя (или тот же
// `missing`, если родителя нет), `carries` отвечает, есть ли у узла собственный ответ.
//
// Подъём ОГРАНИЧЕН по числу шагов намеренно: цепочка вложенности коротка по построению, а неограниченный
// обход означал бы готовность крутиться в кольце, если данные окажутся порчеными. Порченые данные бывают;
// зависший на них поток — необязательно.
template <typename Key, typename ParentOf, typename Carries>
[[nodiscard]] Key nearest_carrier(const Key start, const Key missing, ParentOf parent_of, Carries carries,
                                  const uint32_t max_hops = 8) {
  auto current = start;
  for (uint32_t hop = 0; hop < max_hops; ++hop) {
    if (current == missing) return missing;
    if (carries(current)) return current;
    current = parent_of(current);
  }
  return missing;
}

// Разреженные рантайм-подмены: ключ -> значение поверх того, что лежит в данных.
template <typename Key, typename Value>
class override_table {
public:
  struct entry {
    Key key{};
    Value value{};
  };

  void set(const Key key, const Value& value) {
    const auto place = lower(key);
    if (place != entries_.end() && place->key == key) {
      place->value = value;
      return;
    }
    entries_.insert(place, entry{key, value});
  }

  // Возвращается УКАЗАТЕЛЬ, а не значение с флагом: «подмены нет» и «подменено значением по умолчанию» —
  // разные утверждения, и вызывающий обязан их различать. Дверь без записи открыта потому, что так
  // сказано в файле; дверь с записью `false` открыта потому, что её ОТКРЫЛИ.
  [[nodiscard]] const Value* find(const Key key) const {
    const auto place = lower(key);
    return place != entries_.end() && place->key == key ? &place->value : nullptr;
  }

  bool erase(const Key key) {
    const auto place = lower(key);
    if (place == entries_.end() || place->key != key) return false;
    entries_.erase(place);
    return true;
  }

  [[nodiscard]] Value value_or(const Key key, const Value& fallback) const {
    const auto* found = find(key);
    return found != nullptr ? *found : fallback;
  }

  [[nodiscard]] size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  void clear() noexcept { entries_.clear(); }

  [[nodiscard]] auto begin() const noexcept { return entries_.begin(); }
  [[nodiscard]] auto end() const noexcept { return entries_.end(); }

private:
  auto lower(const Key key) {
    return std::lower_bound(entries_.begin(), entries_.end(), key,
                            [](const entry& item, const Key& value) { return item.key < value; });
  }
  auto lower(const Key key) const {
    return std::lower_bound(entries_.begin(), entries_.end(), key,
                            [](const entry& item, const Key& value) { return item.key < value; });
  }

  std::vector<entry> entries_;
};

} // namespace utils
} // namespace devils_engine

#endif
