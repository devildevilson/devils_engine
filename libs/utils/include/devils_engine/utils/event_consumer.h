#ifndef DEVILS_ENGINE_UTILS_EVENT_CONSIMER_H
#define DEVILS_ENGINE_UTILS_EVENT_CONSIMER_H

#include "utils/list.h"

namespace devils_engine {
namespace utils {
class basic_consumer {
public:
  virtual ~basic_consumer() noexcept = default;
};

template <typename T>
class event_consumer : public basic_consumer, public utils::ring::list<event_consumer<T>, 0> {
public:
  virtual ~event_consumer() noexcept = default;
  virtual void consume(const T& event) = 0;
  inline event_consumer<T>* next(const event_consumer<T>* ref) const { return utils::ring::list_next<0>(this, ref); }
  inline void add(event_consumer<T>* obj) { utils::ring::list_radd<0>(this, obj); }
  inline void unsubscribe() { utils::ring::list_remove(this); }
};

template <typename T>
class basic_consumer_impl : public event_consumer<T> {
public:
  inline void consume(const T& event) override {}
};

struct basic_consumer_container {
  std::unique_ptr<basic_consumer> ptr;

  template <typename T>
  basic_consumer_container() noexcept : ptr(new basic_consumer_impl<T>()) {}

  template <typename T>
  event_consumer<T>* get() const { return static_cast<event_consumer<T>*>(ptr.get()); }
};
}
}

#endif