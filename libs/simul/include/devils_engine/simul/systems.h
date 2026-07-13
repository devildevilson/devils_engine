#ifndef DEVILS_ENGINE_SIMUL_SYSTEMS_H
#define DEVILS_ENGINE_SIMUL_SYSTEMS_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <devils_engine/simul/interface.h>
#include <devils_engine/utils/type_traits.h>

// Defines the common runtime contract for broker-connected simulations. Besides the named
// main/render/assets/sound bases kept for semantic clarity, this header provides the erased worker
// collection and typed registry used by app_runtime. Projects may add any brokered_advancer-derived
// worker without teaching app_runtime about another fixed slot. Type ids are process-local lookup
// keys only; persistent identities still belong in the project's broker/resource contracts.

namespace devils_engine {
namespace simul {

class runtime_system_registry {
public:
  bool add(const size_t type, const std::string_view name, interface* system) {
    if (system == nullptr || find_type(type) != nullptr) {
      return false;
    }
    systems_.push_back({type, name, system});
    return true;
  }

  template <typename System>
  bool add(System* system) {
    return add(utils::type_id<System>(), utils::type_name<System>(), system);
  }

  interface* find(const size_t type, const std::string_view name) noexcept {
    for (const auto& entry : systems_) {
      if (entry.type == type && entry.name == name) {
        return entry.system;
      }
    }
    return nullptr;
  }

  const interface* find(const size_t type, const std::string_view name) const noexcept {
    for (const auto& entry : systems_) {
      if (entry.type == type && entry.name == name) {
        return entry.system;
      }
    }
    return nullptr;
  }

  template <typename System>
  System* find() noexcept {
    return static_cast<System*>(find(utils::type_id<System>(), utils::type_name<System>()));
  }

  template <typename System>
  const System* find() const noexcept {
    return static_cast<const System*>(find(utils::type_id<System>(), utils::type_name<System>()));
  }

  size_t size() const noexcept {
    return systems_.size();
  }

  void clear() noexcept {
    systems_.clear();
  }

private:
  interface* find_type(const size_t type) noexcept {
    for (const auto& entry : systems_) {
      if (entry.type == type) {
        return entry.system;
      }
    }
    return nullptr;
  }

  struct entry {
    size_t type;
    std::string_view name;
    interface* system;
  };

  std::vector<entry> systems_;
};

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

  void set_system_registry(runtime_system_registry* systems) noexcept {
    systems_ = systems;
  }

  template <typename System>
  System* runtime_system() noexcept {
    return systems_ != nullptr ? systems_->find<System>() : nullptr;
  }

  template <typename System>
  const System* runtime_system() const noexcept {
    return systems_ != nullptr ? systems_->find<System>() : nullptr;
  }

  virtual void workers_started() {}
  virtual void runtime_settings_reloaded() {}
  virtual int exit_code() const noexcept {
    return 0;
  }

private:
  runtime_system_registry* systems_ = nullptr;
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

template <typename Broker>
class worker_systems {
public:
  struct entry {
    size_t type;
    std::string_view name;
    std::unique_ptr<brokered_advancer<Broker>> system;
    size_t start_wait_mcs;
    int32_t shutdown_order;
  };

  worker_systems() = default;
  worker_systems(worker_systems&&) noexcept = default;
  worker_systems& operator=(worker_systems&&) noexcept = default;
  worker_systems(const worker_systems&) = delete;
  worker_systems& operator=(const worker_systems&) = delete;

  template <typename System>
  System* add(
    std::unique_ptr<System> system,
    const size_t start_wait_mcs = 0,
    const int32_t shutdown_order = 0) {
    static_assert(std::is_base_of_v<brokered_advancer<Broker>, System>);
    if (system == nullptr) {
      return nullptr;
    }

    auto* ptr = system.get();
    entries_.push_back(entry{
      utils::type_id<System>(),
      utils::type_name<System>(),
      std::move(system),
      start_wait_mcs,
      shutdown_order,
    });
    return ptr;
  }

  std::vector<entry>& entries() noexcept {
    return entries_;
  }

  const std::vector<entry>& entries() const noexcept {
    return entries_;
  }

  size_t size() const noexcept {
    return entries_.size();
  }

private:
  std::vector<entry> entries_;
};

} // namespace simul
} // namespace devils_engine

#endif
