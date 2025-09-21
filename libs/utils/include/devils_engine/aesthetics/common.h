#ifndef DEVILS_ENGINE_AESTHETICS_COMMON_H
#define DEVILS_ENGINE_AESTHETICS_COMMON_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include "utils/list.h"

#ifndef DEVILS_ENGINE_AESTHETICS_VERSION_BITS
#define DEVILS_ENGINE_AESTHETICS_VERSION_BITS 10
#endif

#ifndef DEVILS_ENGINE_AESTHETICS_ENTITY_ID_SIZE
#define DEVILS_ENGINE_AESTHETICS_ENTITY_ID_SIZE uint32_t
#endif

namespace devils_engine {
namespace aesthetics {
constexpr size_t make_mask(const size_t bits) noexcept {
  size_t mask = 0;
  for (size_t i = 0; i < bits; ++i) { mask = mask | (size_t(0x1) << i); }
  return mask;
}

constexpr size_t entityid_version_bits = DEVILS_ENGINE_AESTHETICS_VERSION_BITS;
using entityid_t = DEVILS_ENGINE_AESTHETICS_ENTITY_ID_SIZE;
constexpr entityid_t invalid_entityid = entityid_t(make_mask(sizeof(entityid_t) * CHAR_BIT));
static_assert(invalid_entityid == UINT32_MAX);

constexpr entityid_t make_entityid(const size_t index, const uint32_t version) noexcept {
  constexpr size_t mask = make_mask(entityid_version_bits);
  return entityid_t((index << entityid_version_bits) | (size_t(version) & mask));
}

constexpr size_t get_entityid_index(const entityid_t id) noexcept {
  return size_t(id >> entityid_version_bits);
}

constexpr size_t get_entityid_version(const entityid_t id) noexcept {
  constexpr size_t mask = make_mask(entityid_version_bits);
  return size_t(id) & mask;
}

constexpr bool is_invalid_entityid(const entityid_t id) noexcept {
  return id == invalid_entityid;
}

constexpr auto test1 = make_entityid(4, 21);
static_assert(get_entityid_index(test1) == 4);
static_assert(get_entityid_version(test1) == 21);

struct global_index {
  static size_t index;
  template <typename T>
  static size_t get() {
    static size_t cur = index++;
    return cur;
  }
};

template <typename T>
struct create_component_event {
  entityid_t id;
  T* component;
};

template <typename T>
struct remove_component_event {
  entityid_t id;
  T* component;
};

struct update_event { size_t time; };

class accurate_remover {
public:
  virtual ~accurate_remover() noexcept = default;
};

// лист?
template <typename T>
class basic_reciever : public accurate_remover, public utils::ring::list<basic_reciever<T>, 0> {
public:
  virtual ~basic_reciever() noexcept = default;
  virtual void receive(const T& event) = 0;
  inline basic_reciever<T>* next(const basic_reciever<T>* ref) const { return utils::ring::list_next<0>(this, ref); }
  inline void add(basic_reciever<T>* obj) { utils::ring::list_radd<0>(this, obj); }
  inline void unsubscribe() { utils::ring::list_remove(this); }
};

template <typename T>
class basic_reciever_impl : public basic_reciever<T> {
public:
  inline void receive(const T& event) override {}
};

// это первый элемент в листе basic_reciever, нужен чтобы более аккуратно сделать добавление удаление
// с этим у нас есть гарантия что мы можем использовать basic_reciever в векторе без проблем
// по крайней мере там должен правильно отработать move 
struct basic_reciever_container {
  //char mem[container_size]; // не получится использовать статическую память =(
  //accurate_remover* ptr;
  std::unique_ptr<accurate_remover> ptr;

  template <typename T>
  basic_reciever_container() noexcept : ptr(new basic_reciever_impl<T>()) {}

  template <typename T>
  basic_reciever<T>* get() const { return static_cast<basic_reciever<T>*>(ptr.get()); }
};

class basic_system : public basic_reciever<update_event> {
public:
  virtual ~basic_system() noexcept = default;
  inline void receive(const update_event& event) override { update(event.time); }
  virtual void update(const size_t& time) = 0;
};

inline bool all_of_not_null() { return true; }

template <typename T, typename... Comp_T>
bool all_of_not_null(const T& val, const Comp_T&... values) {
  if constexpr (!std::is_pointer_v<T>) return all_of_not_null(values...);
  else return val != nullptr && all_of_not_null(values...);
}

inline bool all_of_is_null() { return true; }

template <typename T, typename... Comp_T>
bool all_of_is_null(const T& val, const Comp_T&... values) {
  if constexpr (!std::is_pointer_v<T>) return all_of_is_null(values...);
  else return val == nullptr && all_of_is_null(values...);
}

// грязный хак, но зато очень удобно - легко создавать любые классы без наследования лишних базовых классов
// надо бы так же сделать и в рендере
class basic_container {
public:
  virtual ~basic_container() noexcept = default;
};

template <typename T>
class class_container : public basic_container {
public:
  template <typename... Args>
  class_container(Args&&... args) noexcept : obj(std::forward<Args>(args)...) {}

  T* get_ptr() { return std::addressof(obj); }

  T obj;
};

template <typename T>
class_container<T>* make_container_ptr(T* ptr) {
  constexpr size_t offset = offsetof(class_container<T>, class_container<T>::obj);
  return reinterpret_cast<class_container<T>*>(reinterpret_cast<char*>(ptr)-offset);
}

}
}

#endif