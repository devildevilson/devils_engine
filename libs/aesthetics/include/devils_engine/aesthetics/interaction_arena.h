#ifndef DEVILS_ENGINE_AESTHETICS_INTERACTION_ARENA_H
#define DEVILS_ENGINE_AESTHETICS_INTERACTION_ARENA_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "common.h"
#include "elect_buffer.h"

// interaction_arena — переиспользуемый REDUCE-слой арбитража взаимодействий (ROADMAP п.16). Держит по
// одному elect_buffer на ТИП взаимодействия (ключ = id взаимодействия, обычно fn_id эффекта) + общий
// self-claim (сущность заявила себя инициатором в этот тик). record-фаза: claim(interaction, target,
// claimant); commit-фаза: won(interaction, target, claimant) = победитель elect И цель сама не инициатор
// (правило «intent бьёт grab» — снимает каскад и симметрию без приоритетов и резолв-прохода).
//
// Типы взаимодействий РЕГИСТРИРУЮТСЯ до параллельной фазы (ensure), т.к. рост карты в MT небезопасен;
// claim из воркеров только читает набор буферов (elect по существующему ключу = atomic-min, many→one) и
// пишет self_claim по индексу инициатора (disjoint). reset(capacity) — на главном потоке до claim.
// Буферов взаимодействий мало ⇒ линейный поиск по ключу дешевле хеш-карты (и без зависимости на gtl).

namespace devils_engine {
namespace aesthetics {

class interaction_arena {
public:
  // Зарегистрировать тип взаимодействия (создать его elect-буфер). Однопоточно, на настройке.
  void ensure(const uint64_t interaction) {
    if (find(interaction) == nullptr) {
      elects_.emplace_back(interaction, elect_buffer{});
      if (capacity_ != 0) {
        elects_.back().second.reset(capacity_);
      }
    }
  }

  // Засайзить все буферы + self-claim под ёмкость индексов; сброс выборов/претензий. До параллельной фазы.
  void reset(const size_t capacity) {
    capacity_ = capacity;
    self_claim_.assign(capacity, 0);
    for (auto& [key, e] : elects_) {
      e.reset(capacity);
    }
  }

  // Заявить претензию claimant на target в рамках interaction + пометить claimant инициатором.
  // Потокобезопасно: elect = atomic-min (many→one), self_claim = запись своего слота (disjoint).
  // interaction обязан быть заранее ensure() — в MT карту не растим.
  void claim(const uint64_t interaction, const entityid_t target, const entityid_t claimant) {
    elect_buffer* e = find(interaction);
    if (e == nullptr) {
      utils::error{}("interaction_arena: interaction {} not registered — ensure() it before the parallel claim phase", interaction);
    }
    e->claim(target, claimant);
    const size_t cidx = get_entityid_index(claimant);
    if (cidx < self_claim_.size()) {
      self_claim_[cidx] = 1;
    }
  }

  // Победил ли claimant право взаимодействовать с target: наименьший претендент elect И target сам не
  // инициатор в этот тик («intent бьёт grab»). Чтение ПОСЛЕ барьера.
  bool won(const uint64_t interaction, const entityid_t target, const entityid_t claimant) const {
    const elect_buffer* e = find(interaction);
    if (e == nullptr || !e->won(target, claimant)) {
      return false;
    }
    return !is_initiator(target);
  }

  // Заявила ли сущность себя инициатором взаимодействия в этот тик.
  bool is_initiator(const entityid_t entity) const noexcept {
    const size_t idx = get_entityid_index(entity);
    return idx < self_claim_.size() && self_claim_[idx] != 0;
  }

private:
  elect_buffer* find(const uint64_t interaction) noexcept {
    for (auto& [key, e] : elects_) {
      if (key == interaction) {
        return &e;
      }
    }
    return nullptr;
  }
  const elect_buffer* find(const uint64_t interaction) const noexcept {
    for (const auto& [key, e] : elects_) {
      if (key == interaction) {
        return &e;
      }
    }
    return nullptr;
  }

  std::vector<std::pair<uint64_t, elect_buffer>> elects_; // elect на тип взаимодействия (линейный поиск)
  std::vector<uint8_t> self_claim_;                       // индекс сущности → инициатор в этот тик (0/1)
  size_t capacity_ = 0;
};

} // namespace aesthetics
} // namespace devils_engine

#endif
