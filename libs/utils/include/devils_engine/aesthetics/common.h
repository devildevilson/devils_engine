#ifndef DEVILS_ENGINE_AESTHETICS_COMMON_H
#define DEVILS_ENGINE_AESTHETICS_COMMON_H

#include <cstddef>
#include <cstdint>

namespace devils_engine {
namespace aesthetics {
using entityid_t = size_t;

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

template <typename T>
struct basic_reciever {
  virtual ~basic_reciever() noexcept = default;
  virtual void receive(const T& event) = 0;
};

// лист?
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
  const size_t offset = offsetof(class_container<T>, class_container<T>::obj);
  return reinterpret_cast<class_container<T>*>(reinterpret_cast<char*>(ptr)-offset);
}

}
}

#endif