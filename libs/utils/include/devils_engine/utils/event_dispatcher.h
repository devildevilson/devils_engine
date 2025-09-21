#ifndef DEVILS_ENGINE_UTILS_EVENT_DISPATCHER_H
#define DEVILS_ENGINE_UTILS_EVENT_DISPATCHER_H

#include <cstddef>
#include <cstdint>
//#include <vector>
#include <gtl/phmap.hpp>
#include "utils/event_consumer.h"
#include "utils/type_traits.h"

namespace devils_engine {
namespace utils {

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
}
}

#endif