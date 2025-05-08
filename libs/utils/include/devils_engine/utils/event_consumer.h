#ifndef DEVILS_ENGINE_UTILS_EVENT_CONSIMER_H
#define DEVILS_ENGINE_UTILS_EVENT_CONSIMER_H

namespace devils_engine {
namespace utils {
class basic_consumer {
public:
  virtual ~basic_consumer() noexcept = default;
};

template <typename T>
class event_consumer : public basic_consumer {
public:
  virtual ~event_consumer() noexcept = default;
  virtual void consume(const T& event) = 0;
};
}
}

#endif