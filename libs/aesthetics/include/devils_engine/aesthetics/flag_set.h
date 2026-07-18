#ifndef DEVILS_ENGINE_AESTHETICS_FLAG_SET_H
#define DEVILS_ENGINE_AESTHETICS_FLAG_SET_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "devils_engine/utils/string_id.h" // utils::id — флаг = string_hash имени
#include "devils_engine/utils/timeline.h"  // utils::game_duration — остаток жизни флага

// flag_set — generic per-entity флаги с опциональным сроком годности (ROADMAP п.8 / B-е).
// Флаг = строка, привязанная к сущности: хранится хешем (utils::id), опционально с ОСТАТКОМ
// времени жизни (typed game_duration). Компонент — обычные данные ECS: движок даёт контейнер и
// семантику, проект решает, на каких сущностях он живёт, и регистрирует его в serial/ds сам.
// Тип намеренно агрегат с ПУБЛИЧНЫМ полем entries (dumb store): reflect/сериализация видят его
// напрямую, методы — только конвенция доступа поверх сортированного вектора.
//
// Модель времени — ОБРАТНЫЙ ОТСЧЁТ, не абсолютный deadline (тот же выбор, что actor_eating):
// писателю и читателю НЕ нужен доступ к текущему времени — критично для ds building blocks, чьи
// тела исполняются на commit-фазе без контекста времени. Проект гоняет advance(game_dt) явной
// sweep-фазой (map-система: self-мутация, категория A); дельта уже отмасштабирована game-часами,
// поэтому пауза (dt=0) и замедление/ускорение (game_time_scale) действуют на сроки автоматически.
// Абсолютная дата окончания для UI — проекция now + remaining на месте.
//
// Детерминизм: записи сортированы по хешу флага (вставка/поиск O(log n) + memmove на крошечном
// векторе), порядок обхода entries стабилен и не зависит от порядка set(). Повторный set()
// ПЕРЕЗАПИСЫВАЕТ срок (refresh). MT: мутация — только своей сущности в map-фазе или через
// deferred collect/elect; сам контейнер потокобезопасностью не занимается.

namespace devils_engine {
namespace aesthetics {

struct flag_entry {
  utils::id flag = 0;
  utils::game_duration remaining{};
};

struct flag_set {
  // Бессрочный флаг: max-остаток на практике не истекает (advance вычитает насыщенно).
  static constexpr utils::game_duration no_expiry{UINT64_MAX};

  std::vector<flag_entry> entries; // сортирован по flag; крошечный (единицы записей)

  // Ставит/обновляет флаг; повторный set перезаписывает остаток (refresh). true = флаг был новым.
  // Нулевой остаток валиден — has() его уже не видит, ближайший advance удалит запись.
  bool set(const utils::id flag, const utils::game_duration remaining = no_expiry) {
    const auto it = lower_bound(flag);
    if (it != entries.end() && it->flag == flag) {
      it->remaining = remaining;
      return false;
    }
    entries.insert(it, flag_entry{flag, remaining});
    return true;
  }

  // Жив ли флаг (остаток не исчерпан). Времени не требует — истёкшее эквивалентно отсутствию
  // независимо от того, успел ли пройти advance().
  bool has(const utils::id flag) const noexcept {
    const auto it = lower_bound(flag);
    return it != entries.end() && it->flag == flag && it->remaining.ticks > 0;
  }

  // Сырая запись (видит и исчерпанную до sweep — диагностика/сериализация); nullptr если нет.
  const flag_entry* find(const utils::id flag) const noexcept {
    const auto it = lower_bound(flag);
    return it != entries.end() && it->flag == flag ? &*it : nullptr;
  }

  // Снимает флаг досрочно. true = флаг был.
  bool remove(const utils::id flag) {
    const auto it = lower_bound(flag);
    if (it == entries.end() || it->flag != flag) {
      return false;
    }
    entries.erase(it);
    return true;
  }

  // Sweep-фаза: вычесть прошедшую игровую дельту (насыщенно) и удалить исчерпанные записи.
  // Возвращает число удалённых. dt == 0 (пауза) — no-op по срокам, чистит только уже нулевые.
  size_t advance(const utils::game_duration dt) {
    for (auto& e : entries) {
      e.remaining.ticks -= std::min(e.remaining.ticks, dt.ticks);
    }
    const auto it = std::remove_if(entries.begin(), entries.end(), [](const flag_entry& e) {
      return e.remaining.ticks == 0;
    });
    const size_t removed = size_t(entries.end() - it);
    entries.erase(it, entries.end());
    return removed;
  }

  size_t size() const noexcept {
    return entries.size();
  }
  bool empty() const noexcept {
    return entries.empty();
  }
  void clear() noexcept {
    entries.clear();
  }

private:
  std::vector<flag_entry>::iterator lower_bound(const utils::id flag) noexcept {
    return std::lower_bound(entries.begin(), entries.end(), flag,
                            [](const flag_entry& e, const utils::id f) { return e.flag < f; });
  }
  std::vector<flag_entry>::const_iterator lower_bound(const utils::id flag) const noexcept {
    return std::lower_bound(entries.begin(), entries.end(), flag,
                            [](const flag_entry& e, const utils::id f) { return e.flag < f; });
  }
};

} // namespace aesthetics
} // namespace devils_engine

#endif
