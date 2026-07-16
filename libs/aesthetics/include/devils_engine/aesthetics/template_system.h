#ifndef DEVILS_ENGINE_AESTHETICS_TEMPLATE_SYSTEM_H
#define DEVILS_ENGINE_AESTHETICS_TEMPLATE_SYSTEM_H

#include <cstddef>
#include <functional> // std::ref
#include <memory>     // std::unique_ptr
#include <type_traits>
#include <utility> // std::move

#include "devils_engine/thread/atomic_pool.h"
#include "world.h"

// template_system / template_system_mt — map-примитив декларативного пайплайна: система над запросом
// `world::query_t<Comp_T...>`, которая на КАЖДУЮ подходящую сущность вызывает виртуальный
// process(tuple). Подкласс реализует process и владеет своей политикой; сам примитив держит только
// query (авто-поддерживается событиями create/remove компонентов) и обход.
//   - template_system — однопоточный обход query.
//   - template_system_mt — тот же обход, распределённый по atomic_pool (distribute1/compute/wait):
//     вызывающий поток тоже берёт свой чанк. process ДОЛЖЕН быть потокобезопасен по записи — обычно
//     он пишет ТОЛЬКО «свою» сущность (её слот в message_registry/компонент этой же сущности), тогда
//     потоки трогают непересекающуюся память и локов не нужно.
//
// Коммуникация между системами — через `message_registry` (шина каналов по типу): продюсер в process
// пишет `board.channel<Msg>().store(id, msg)` по слоту обрабатываемой сущности, консюмер в отдельной
// системе читает `board.find<Msg>()`. Реестр держит и передаёт вызывающий (пайплайн), примитив о нём
// не знает — так map остаётся общим. Порядок систем и гейтинг фаз — забота simul, не примитива.
// Наследует basic_system, поэтому может и driver'иться явно (update(tick)), и композиться с событийной
// машинерией world при необходимости.

namespace devils_engine {
namespace aesthetics {

template <typename... Comp_T>
class template_system : public basic_system {
public:
  using query_t = world::query_t<Comp_T...>;
  using query_tuple_t = typename query_t::query_tuple_t;

  template_system(class world* world) noexcept : m_world(world), query(world->query<Comp_T...>()) {}

  void update(const size_t time) override {
    for (const auto& t : query) {
      process(t, time);
    }
  }

  // Обработать одну сущность: t = (entityid_t, Comp_T*...), time = текущее время/тик (прокинут из
  // update). Реализует подкласс.
  virtual void process(const query_tuple_t& t, size_t time) = 0;

  // Число сущностей в запросе (авто-поддерживается событиями компонентов).
  size_t size() const noexcept {
    return query.size();
  }

protected:
  class world* m_world;
  query_t query;
};

template <typename... Comp_T>
class template_system_mt : public template_system<Comp_T...> {
public:
  using query_t = typename template_system<Comp_T...>::query_t;

  template_system_mt(thread::atomic_pool* pool, class world* world) noexcept
    : template_system<Comp_T...>(world), pool(pool) {}

  void update(const size_t time) override {
    // Каждый чанк [start, start+count) обходит свои сущности; query передаётся аргументом, time —
    // захватом по значению (лямбда копируется на чанк). process пишет только «свою» сущность ⇒
    // параллельная запись без гонок.
    pool->distribute1(
      this->query.size(),
      [this](const size_t start, const size_t count, const query_t& q, const size_t time) {
        for (size_t i = start; i < start + count; ++i) {
          this->process(q[i], time);
        }
      },
      std::ref(this->query), time);
    pool->compute(); // вызывающий поток берёт свой чанк, а не простаивает
    pool->wait();
  }

protected:
  thread::atomic_pool* pool;
};

// ── Лямбда-системы: map-система ИЗ ЛЯМБДЫ, без подкласса ──────────────────────────────────────────
// «Проект задаёт обработку компонентов в удобной форме» (ROADMAP п.11, цель — минимум бойлерплейта):
// вместо struct-наследника с process() — make_map_system[_mt]<Comp...>(…, fn), где fn(query_tuple_t, time)
// исполняется на каждую сущность. Лямбда шаблонит систему по типу (прямой вызов, без per-entity virtual),
// а базовый тип template_system<Comp...>/basic_system делает её полиморфно listable (для списка фаз).
// Per-frame входы (dt, соседи, шина) лямбда берёт захватом (обычно [this] проекта) — примитив о них не знает.
namespace detail {
template <typename Fn, typename... Comp_T>
class lambda_system final : public template_system<Comp_T...> {
public:
  using tuple_t = typename template_system<Comp_T...>::query_tuple_t;
  lambda_system(class world* world, Fn fn) noexcept : template_system<Comp_T...>(world), fn_(std::move(fn)) {}
  void update(const size_t time) override {
    for (const auto& t : this->query) {
      fn_(t, time); // прямой вызов лямбды (инлайнится), без per-entity виртуали
    }
  }
  void process(const tuple_t& t, const size_t time) override {
    fn_(t, time); // требуется абстрактной базой; update() её не использует
  }

private:
  Fn fn_;
};

template <typename Fn, typename... Comp_T>
class lambda_system_mt final : public template_system_mt<Comp_T...> {
public:
  using tuple_t = typename template_system<Comp_T...>::query_tuple_t;
  using query_t = typename template_system_mt<Comp_T...>::query_t;
  lambda_system_mt(thread::atomic_pool* pool, class world* world, Fn fn) noexcept
    : template_system_mt<Comp_T...>(pool, world), fn_(std::move(fn)) {}
  void update(const size_t time) override {
    this->pool->distribute1(
      this->query.size(),
      [this](const size_t start, const size_t count, const query_t& q, const size_t time) {
        for (size_t i = start; i < start + count; ++i) {
          fn_(q[i], time);
        }
      },
      std::ref(this->query), time);
    this->pool->compute();
    this->pool->wait();
  }
  void process(const tuple_t& t, const size_t time) override {
    fn_(t, time);
  }

private:
  Fn fn_;
};
} // namespace detail

// Собрать map-систему из лямбды fn(query_tuple_t, time). Комп-типы задаются явно, тип лямбды выводится.
// Возвращает полиморфную базу (template_system<Comp...> : basic_system) — годна для списка фаз.
template <typename... Comp_T, typename Fn>
std::unique_ptr<template_system<Comp_T...>> make_map_system(class world* world, Fn fn) {
  return std::make_unique<detail::lambda_system<std::decay_t<Fn>, Comp_T...>>(world, std::move(fn));
}
template <typename... Comp_T, typename Fn>
std::unique_ptr<template_system<Comp_T...>> make_map_system_mt(thread::atomic_pool* pool, class world* world, Fn fn) {
  return std::make_unique<detail::lambda_system_mt<std::decay_t<Fn>, Comp_T...>>(pool, world, std::move(fn));
}

} // namespace aesthetics
} // namespace devils_engine

#endif
