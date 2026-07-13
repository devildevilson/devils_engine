#ifndef DEVILS_ENGINE_SIMUL_SYSTEMS_H
#define DEVILS_ENGINE_SIMUL_SYSTEMS_H

// Generic main/render/sound/assets system interfaces parameterized by a broker.

#include <cstddef>

#include <devils_engine/simul/interface.h>

namespace devils_engine {
namespace simul {

template <typename Broker>
class brokered_advancer : public advancer {
public:
  using broker_type = Broker;

  brokered_advancer() noexcept = default;
  explicit brokered_advancer(const size_t frame_time) noexcept : advancer(frame_time) {}

  virtual void set_broker(Broker* b) {
    broker_ = b;
  }
  Broker* broker() noexcept {
    return broker_;
  }
  const Broker* broker() const noexcept {
    return broker_;
  }

protected:
  Broker* broker_ = nullptr;
};

template <typename Broker>
class main_system : public brokered_advancer<Broker> {
public:
  using brokered_advancer<Broker>::brokered_advancer;
};

template <typename Broker>
class render_system : public brokered_advancer<Broker> {
public:
  using brokered_advancer<Broker>::brokered_advancer;
};

template <typename Broker>
class assets_system : public brokered_advancer<Broker> {
public:
  using brokered_advancer<Broker>::brokered_advancer;
};

template <typename Broker>
class sound_system : public brokered_advancer<Broker> {
public:
  using brokered_advancer<Broker>::brokered_advancer;
};

} // namespace simul
} // namespace devils_engine

#endif
