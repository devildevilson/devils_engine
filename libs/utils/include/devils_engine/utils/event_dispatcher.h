#ifndef DEVILS_ENGINE_UTILS_EVENT_DISPATCHER_H
#define DEVILS_ENGINE_UTILS_EVENT_DISPATCHER_H

#include <cstddef>
#include <cstdint>
#include <vector>
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
  gtl::node_hash_map<size_t, std::vector<basic_consumer*>> consumers;
};

template<typename Event_T, typename T>
  requires(std::is_base_of_v<T, event_consumer<Event_T>>)
void event_dispatcher::subscribe(T* ptr) {
  constexpr size_t type_id = utils::type_id<Event_T>();

  auto itr = consumers.find(type_id);
  if (itr == consumers.end()) {
    itr = consumers.emplace(std::make_pair(type_id, std::vector<basic_consumer*>{})).first;
  }

  auto basic_ptr = static_cast<basic_consumer*>(ptr);
  itr->second.push_back(basic_ptr); // тут скорее всего очень важна последовательность
}

template<typename Event_T, typename T>
  requires(std::is_base_of_v<T, event_consumer<Event_T>>)
void event_dispatcher::unsubscribe(T* ptr) {
  constexpr size_t type_id = utils::type_id<Event_T>();

  auto itr = consumers.find(type_id);
  if (itr == consumers.end()) return;

  
  auto basic_ptr = static_cast<basic_consumer*>(ptr);
  auto cons_itr = std::find(itr->second.begin(), itr->second.end(), basic_ptr);
  itr->second.erase(cons_itr);
}

template<typename Event_T>
void event_dispatcher::emit(const Event_T& event) {
  constexpr size_t type_id = utils::type_id<Event_T>();

  auto itr = consumers.find(type_id);
  if (itr == consumers.end()) return;

  for (auto &ptr : itr->second) {
    auto cons_ptr = static_cast<event_consumer<Event_T>*>(ptr);
    cons_ptr->consume(event);
  }
}
}
}

#endif