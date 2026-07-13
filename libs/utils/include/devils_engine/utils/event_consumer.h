#ifndef DEVILS_ENGINE_UTILS_EVENT_CONSIMER_H
#define DEVILS_ENGINE_UTILS_EVENT_CONSIMER_H

#include "list.h"

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
  event_consumer<T>* next(const event_consumer<T>* ref) const;
  void add(event_consumer<T>* obj);
  void unsubscribe();
};

template <typename T>
class basic_consumer_impl : public event_consumer<T> {
public:
  void consume(const T& event) override {}
};

struct basic_consumer_container {
  std::unique_ptr<basic_consumer> ptr;

  template <typename T>
  basic_consumer_container() noexcept : ptr(new basic_consumer_impl<T>()) {}

  template <typename T>
  event_consumer<T>* get() const {
    return static_cast<event_consumer<T>*>(ptr.get());
  }
};

// Template implementation

template <typename T>
event_consumer<T>* event_consumer<T>::next(const event_consumer<T>* ref) const {
  return utils::ring::list_next<0>(this, ref);
}

template <typename T>
void event_consumer<T>::add(event_consumer<T>* obj) {
  utils::ring::list_radd<0>(this, obj);
}

template <typename T>
void event_consumer<T>::unsubscribe() {
  utils::ring::list_remove(this);
}
} // namespace utils
} // namespace devils_engine

#endif
