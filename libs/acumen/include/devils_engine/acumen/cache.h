#ifndef DEVILS_ENGINE_ACUMEN_CACHE_H
#define DEVILS_ENGINE_ACUMEN_CACHE_H

#include <array>
#include <cstddef>
#include <cstdint>

#include <gtl/phmap.hpp>

#include "common.h"

namespace devils_engine {
namespace acumen {
// Мемоизация решений GOAP. Решение — ЧИСТАЯ функция (system, цель, значащие биты
// состояния), а A* детерминирован → одинаковый ключ ⇒ одинаковый план ВСЕГДА. Значит
// это точная мемоизация, не аппроксимация: 4096 акторов в одном значащем состоянии
// делят один просчёт.
//
// Ключ — тождество цели (дешёвый goal_id от вызывающего) + проекция значащих бит
// старт-состояния (бит вне объединения масок действий/целей на план не влияет, в ключ
// не входит → пространство ключей схлопывается, hit-rate высокий). Проекция точная
// (равенство по словам), коллизий-подмены плана нет.

// Ключ мемоизации. bits — значащие биты состояния, упакованные в плотные младшие позиции.
struct plan_key {
  uint64_t goal_id = 0;
  std::array<uint64_t, state_words> bits{};
  bool operator==(const plan_key& o) const noexcept = default;
};

struct plan_key_hash {
  // hash_combine по словам ключа + fmix64-финализатор (оба из utils/hash.h). Реализация в cache.cpp.
  size_t operator()(const plan_key& k) const noexcept;
};

// Кешированный план: до max_plan индексов действий (в system.get_actions()) в порядке
// исполнения. full_length — реальная длина (clamped к 255, для диагностики/обрезания).
struct cached_plan {
  std::array<uint16_t, max_plan> actions{};
  uint8_t length = 0;      // сколько индексов реально лежит в actions (<= max_plan)
  uint8_t full_length = 0; // полная длина плана (может быть > length, если был обрезан)
};

// solution_cache — таблица мемоизации. Чистый АКСЕЛЕРАТОР: возвращает то же, что живой
// A* → политика бюджета/вытеснения НИКОГДА не влияет на исход симуляции, только на
// скорость (поэтому детерминизм-безопасна при любом наполнении). Потокобезопасности нет
// (намеренно): держи по одной на поток исполнения и сливай в общую тёплую таблицу между
// кадрами через merge() — lock-free чтение в горячем пути, без гонок на запись.
class solution_cache {
public:
  static constexpr size_t entry_bytes = sizeof(plan_key) + sizeof(cached_plan);
  static constexpr size_t default_bytes = 1u << 20; // 1 MiB по умолчанию

  // бюджет в БАЙТАХ → лимит записей; при переполнении insert — no-op (решаем вживую,
  // без churn). Минимум 1 запись.
  explicit solution_cache(const size_t max_bytes = default_bytes) noexcept;

  const cached_plan* find(const plan_key& k) const noexcept;

  // false ⇒ бюджет исчерпан и ключа ещё нет (вставка пропущена). Перезапись существующего
  // ключа всегда ок (значение детерминированно то же, бюджет не растёт).
  bool insert(const plan_key& k, const cached_plan& p);

  // влить чужие записи (шаринг прогретой таблицы между потоками между кадрами).
  void merge(const solution_cache& other);

  void clear() noexcept;

  size_t size() const noexcept;
  size_t capacity_entries() const noexcept;
  bool full() const noexcept;
  size_t hits() const noexcept;
  size_t misses() const noexcept;

private:
  gtl::flat_hash_map<plan_key, cached_plan, plan_key_hash> entries;
  size_t max_entries;
  mutable size_t hit_count = 0;
  mutable size_t miss_count = 0;
};

} // namespace acumen
} // namespace devils_engine

#endif
