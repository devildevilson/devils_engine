#ifndef DEVILS_ENGINE_UTILS_EVENT_DISPATCHER_H
#define DEVILS_ENGINE_UTILS_EVENT_DISPATCHER_H

#include <cstddef>
#include <cstdint>
#include <concepts>
#include <vector>
#include <type_traits>
#include <memory>
#include <gtl/phmap.hpp>
#include "event_consumer.h"
#include "type_traits.h"
#include "core.h"

namespace devils_engine {
namespace utils {

constexpr size_t EVENT_DISPATCHER2 = 2352;

class event_dispatcher {
public:
  template<typename Event_T, typename T>
    requires(std::is_base_of_v<T, event_consumer<Event_T>>)
  void subscribe(T* ptr);

  template<typename Event_T, typename T>
    requires(std::is_base_of_v<T, event_consumer<Event_T>>)
  void unsubscribe(T* ptr);

  template<typename Event_T>
  void emit(const Event_T& event);
protected:
  gtl::node_hash_map<size_t, basic_consumer_container> consumers;
};

template <size_t ID>
class event_dispatcher2 {
public:
  template<typename Event_T>
  void reserve(const size_t n);

  template<typename Event_T>
  void submit(Event_T&& event);

  template<typename Event_T, typename... Args>
  void submit(Event_T&& event, Args&&... args);

  template<typename Event_T>
  void submit(std::vector<Event_T> event);

  template<typename Event_T>
  std::span<const Event_T> read() const;

  template<typename Event_T>
  std::vector<Event_T> consume();

  template<typename Event_T, typename F>
    requires(std::is_invocable_r_v<bool, F, const Event_T&, const Event_T&>)
  void sort(const F &fn);

  template<typename Event_T>
  void clear();

  void clear_all() { for (auto& arr : memory) { arr->clear(); } }
protected:
  class bucket_base {
  public:
    virtual ~bucket_base() = default;
    virtual void clear() = 0;
  };

  template<typename T>
  class bucket : public bucket_base {
  public:
    std::vector<T> data;

    void clear() override;
  };

  std::vector<std::unique_ptr<bucket_base>> memory;

  template<typename Event_T>
  bucket<Event_T>* get_bucket();

  template<typename Event_T>
  const bucket<Event_T>* get_bucket() const;
};

template <size_t ID>
class event_dispatcher2_mt {
public:
  template<typename Event_T>
  void reserve(const size_t n);

  template<typename Event_T>
  void submit(Event_T&& event);

  template<typename Event_T, typename... Args>
  void submit(Event_T&& event1, Event_T&& event2, Args&&... args);

  template<typename Event_T>
  void submit(std::vector<Event_T> events);

  template<typename Event_T>
  std::vector<Event_T> consume();

  template<typename Event_T>
  void consume(std::vector<Event_T>& arr);

  template<typename Event_T, typename F>
    requires(std::is_invocable_r_v<bool, F, const Event_T&, const Event_T&>)
  void sort(const F& fn);
protected:
  class bucket_base {
  public:
    std::mutex m;
    virtual ~bucket_base() = default;
  };

  template<typename T>
  class bucket : public bucket_base {
  public:
    std::vector<T> data;
  };

  std::vector<std::unique_ptr<bucket_base>> memory;

  template<typename Event_T>
  bucket<Event_T>* get_bucket();

  template<typename Event_T>
  void rawsubmit(Event_T&& event);

  template<typename Event_T, typename... Args>
  void rawsubmit(Event_T&& event1, Event_T&& event2, Args&&... args);
};


template<typename Event_T, typename T>
  requires(std::is_base_of_v<T, event_consumer<Event_T>>)
void event_dispatcher::subscribe(T* ptr) {
  constexpr size_t type_id = utils::type_id<Event_T>();

  auto itr = consumers.find(type_id);
  if (itr == consumers.end()) {
    itr = consumers.emplace(std::make_pair(type_id, basic_consumer_container<Event_T>())).first;
  }

  auto basic_ptr = static_cast<event_consumer<Event_T>*>(ptr);
  itr->second.get<Event_T>()->add(basic_ptr);
}

template<typename Event_T, typename T>
  requires(std::is_base_of_v<T, event_consumer<Event_T>>)
void event_dispatcher::unsubscribe(T* ptr) {
  constexpr size_t type_id = utils::type_id<Event_T>();

  auto itr = consumers.find(type_id);
  if (itr == consumers.end()) return;

  
  auto basic_ptr = static_cast<event_consumer<Event_T>*>(ptr);
  basic_ptr->unsubscribe();
}

template<typename Event_T>
void event_dispatcher::emit(const Event_T& event) {
  constexpr size_t type_id = utils::type_id<Event_T>();

  auto itr = consumers.find(type_id);
  if (itr == consumers.end()) return;

  auto empty_el = itr->second.get<Event_T>();
  for (auto p = empty_el->next(empty_el); p != nullptr; p = p->next(empty_el)) {
    p->consume(event);
  }
}

template <size_t ID>
template<typename Event_T>
void event_dispatcher2<ID>::reserve(const size_t n) {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  const size_t type_id = utils::sequential_type_id<ID, final_evt_t>();
  if (type_id >= memory.size()) memory.resize(type_id + 1);
  if (memory[type_id].get() == nullptr) memory[type_id] = std::make_unique<bucket<final_evt_t>>();

  auto ptr = get_bucket<final_evt_t>();
  ptr->data.reserve(n);
}

template <size_t ID>
template<typename Event_T>
void event_dispatcher2<ID>::submit(Event_T&& event) {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  const size_t type_id = utils::sequential_type_id<ID, final_evt_t>();
  if (type_id >= memory.size()) { reserve<final_evt_t>(100); }

  auto ptr = get_bucket<final_evt_t>();
  ptr->data.emplace_back(std::move(event));
}

template <size_t ID>
template<typename Event_T, typename... Args>
void event_dispatcher2<ID>::submit(Event_T&& event, Args&&... args) {
  submit(std::forward<Event_T>(event));
  submit(std::forward<Args>(args));
}

template <size_t ID>
template<typename Event_T>
void event_dispatcher2<ID>::submit(std::vector<Event_T> event) {
  for (auto& evt : event) { submit(std::move(evt)); }
}

template <size_t ID>
template<typename Event_T>
std::span<const Event_T> event_dispatcher2<ID>::read() const {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  auto ptr = get_bucket<final_evt_t>();
  if (ptr == nullptr) return std::span<const final_evt_t>{};
  return std::span(ptr->data);
}

template <size_t ID>
template<typename Event_T>
std::vector<Event_T> event_dispatcher2<ID>::consume() {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  auto ptr = get_bucket<final_evt_t>();
  if (ptr == nullptr) return std::vector<Event_T>{};
  std::vector<Event_T> out;
  std::swap(out, ptr->data);
  return out;
}

template <size_t ID>
template<typename Event_T, typename F>
  requires(std::is_invocable_r_v<bool, F, const Event_T&, const Event_T&>)
void event_dispatcher2<ID>::sort(const F& fn) {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  auto ptr = get_bucket<final_evt_t>();
  if (ptr == nullptr) return;
  std::sort(ptr->data.begin(), ptr->data.end(), fn);
}

template <size_t ID>
template<typename Event_T>
void event_dispatcher2<ID>::clear() {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  auto ptr = get_bucket<final_evt_t>();
  if (ptr == nullptr) return;
  ptr->data.clear();
}

template <size_t ID>
template<typename Event_T>
void event_dispatcher2<ID>::bucket<Event_T>::clear() { data.clear(); }

template <size_t ID>
template<typename Event_T>
event_dispatcher2<ID>::bucket<Event_T>* event_dispatcher2<ID>::get_bucket() {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  const size_t type_id = utils::sequential_type_id<ID, final_evt_t>();
  if (type_id >= memory.size()) return nullptr;
  return static_cast<bucket<Event_T>*>(memory[type_id].get());
}

template <size_t ID>
template<typename Event_T>
const event_dispatcher2<ID>::bucket<Event_T>* event_dispatcher2<ID>::get_bucket() const {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  const size_t type_id = utils::sequential_type_id<ID, final_evt_t>();
  if (type_id >= memory.size()) return nullptr;
  return static_cast<const bucket<Event_T>*>(memory[type_id].get());
}



template <size_t ID>
template<typename Event_T>
void event_dispatcher2_mt<ID>::reserve(const size_t n) {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  const size_t type_id = utils::sequential_type_id<ID, final_evt_t>();
  if (type_id > memory.size()) utils::error{}("Missed several types before registration of '{}'???", utils::type_name<final_evt_t>());
  if (type_id == memory.size()) memory.emplace_back(new bucket<final_evt_t>());
  auto ptr = get_bucket<final_evt_t>();
  ptr->data.reserve(n);
}

template <size_t ID>
template<typename Event_T>
void event_dispatcher2_mt<ID>::submit(Event_T&& event) {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  const size_t type_id = utils::sequential_type_id<ID, final_evt_t>();
  if (type_id >= memory.size()) utils::error{}("Register a type '{}' before use it in 'event_dispatcher2_mt'", utils::type_name<final_evt_t>());
  auto ptr = get_bucket<final_evt_t>();
  const std::lock_guard l(ptr->m);
  rawsubmit(std::forward<Event_T>(event));
}

template <size_t ID>
template<typename Event_T, typename... Args>
void event_dispatcher2_mt<ID>::submit(Event_T&& event1, Event_T&& event2, Args&&... args) {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  const size_t type_id = utils::sequential_type_id<ID, final_evt_t>();
  if (type_id >= memory.size()) utils::error{}("Register a type '{}' before use it in 'event_dispatcher2_mt'", utils::type_name<final_evt_t>());
  auto ptr = get_bucket<final_evt_t>();
  const std::lock_guard l(ptr->m);
  rawsubmit(std::forward<Event_T>(event1), std::forward<Event_T>(event2), std::forward<Args>(args)...);
}

template <size_t ID>
template<typename Event_T>
void event_dispatcher2_mt<ID>::submit(std::vector<Event_T> events) {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  const size_t type_id = utils::sequential_type_id<ID, final_evt_t>();
  if (type_id >= memory.size()) utils::error{}("Register a type '{}' before use it in 'event_dispatcher2_mt'", utils::type_name<final_evt_t>());
  auto ptr = get_bucket<final_evt_t>();
  const std::lock_guard l(ptr->m);
  for (auto &evt : events) {
    rawsubmit(std::move(evt));
  }
}

template <size_t ID>
template<typename Event_T>
std::vector<Event_T> event_dispatcher2_mt<ID>::consume() {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  const size_t type_id = utils::sequential_type_id<ID, final_evt_t>();
  if (type_id >= memory.size()) utils::error{}("Register a type '{}' before use it in 'event_dispatcher2_mt'", utils::type_name<final_evt_t>());
  auto ptr = get_bucket<final_evt_t>();
  std::vector<Event_T> out;
  const std::lock_guard l(ptr->m);
  std::swap(out, ptr->data);
  return out;
}

template <size_t ID>
template<typename Event_T>
void event_dispatcher2_mt<ID>::consume(std::vector<Event_T>& arr) {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  const size_t type_id = utils::sequential_type_id<ID, final_evt_t>();
  if (type_id >= memory.size()) utils::error{}("Register a type '{}' before use it in 'event_dispatcher2_mt'", utils::type_name<final_evt_t>());

  auto ptr = get_bucket<final_evt_t>();
  const std::lock_guard l(ptr->m);
  std::swap(arr, ptr->data);
}

template <size_t ID>
template<typename Event_T, typename F>
  requires(std::is_invocable_r_v<bool, F, const Event_T&, const Event_T&>)
void event_dispatcher2_mt<ID>::sort(const F& fn) {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  const size_t type_id = utils::sequential_type_id<ID, final_evt_t>();
  if (type_id >= memory.size()) utils::error{}("Register a type '{}' before use it in 'event_dispatcher2_mt'", utils::type_name<final_evt_t>());
  auto ptr = get_bucket<final_evt_t>();
  const std::lock_guard l(ptr->m);
  std::sort(ptr->data.begin(), ptr->data.end(), fn);
}

template <size_t ID>
template<typename Event_T>
event_dispatcher2_mt<ID>::bucket<Event_T>* event_dispatcher2_mt<ID>::get_bucket() {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  const size_t type_id = utils::sequential_type_id<ID, final_evt_t>();
  if (type_id >= memory.size()) utils::error{}("Register a type '{}' before use it in 'event_dispatcher2_mt'", utils::type_name<final_evt_t>());
  return static_cast<bucket<Event_T>*>(memory[type_id].get());
}

template <size_t ID>
template<typename Event_T>
void event_dispatcher2_mt<ID>::rawsubmit(Event_T&& event) {
  using final_evt_t = std::remove_cvref_t<Event_T>;
  auto ptr = get_bucket<final_evt_t>();
  ptr->data.emplace_back(std::move(event));
}

template <size_t ID>
template<typename Event_T, typename... Args>
void event_dispatcher2_mt<ID>::rawsubmit(Event_T&& event1, Event_T&& event2, Args&&... args) {
  rawsubmit(std::forward<Event_T>(event1));
  rawsubmit(std::forward<Event_T>(event2), std::forward<Args>(args)...);
}

}
}

#endif