#ifndef DEVILS_ENGINE_AESTHETICS_SIMPLE_SYSTEMS_H
#define DEVILS_ENGINE_AESTHETICS_SIMPLE_SYSTEMS_H

#include <cstddef>
#include <cstdint>
#include "world.h"
#include "thread/atomic_pool.h"

namespace devils_engine {
namespace aesthetics {

template<typename... Comp_T>
class template_system : public basic_system {
public:
  using query_t = world::query<Comp_T...>;

  template_system(class world* world) noexcept;
  ~template_system() noexcept;

  void update(const size_t& time) override;
  virtual void process(const query_t::query_tuple_t &data) = 0;
protected:
  class world* world;
  query_t* query;
};

template<typename... Comp_T>
class template_system_mt : public template_system<Comp_T...> {
public:
  using query_t = world::query<Comp_T...>;

  template_system_mt(thread::atomic_pool* pool, class world* world) noexcept;
  ~template_system_mt() noexcept;

  void update(const size_t& time) override;
protected:
  thread::atomic_pool* pool;
  class world* world;
  query_t* query;
};

template<typename... Comp_T>
template_system<Comp_T...>::template_system(class world* world) noexcept : world(world), query(world->create_query<Comp_T>()) {}
template<typename... Comp_T>
template_system<Comp_T...>::~template_system() noexcept { world->remove_query(query); }
template<typename... Comp_T>
void template_system<Comp_T...>::update(const size_t& time) {
  for (const auto &t : query->array()) { process(t); }
}

template<typename... Comp_T>
template_system_mt<Comp_T...>::template_system_mt(thread::atomic_pool* pool, class world* world) noexcept : pool(pool), world(world), query(world->create_query<Comp_T>()) {}
template<typename... Comp_T>
template_system_mt<Comp_T...>::~template_system_mt() noexcept { world->remove_query(query); }
template<typename... Comp_T>
void template_system_mt<Comp_T...>::update(const size_t& time) {
  pool->distribute1(query->array().size(), [this] (const size_t start, const size_t count, query_t* query) {
    for (size_t i = start; i < start + count; ++i) {
      this->process(query->array()[i]);
    }
  }, query);

  // иногда синхронизация нужна, иногда нет
  pool->compute();
  pool->wait();
}


}
}

#endif