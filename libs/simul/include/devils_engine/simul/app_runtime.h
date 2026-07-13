#ifndef DEVILS_ENGINE_SIMUL_APP_RUNTIME_H
#define DEVILS_ENGINE_SIMUL_APP_RUNTIME_H

#include <algorithm>
#include <cstddef>
#include <memory>
#include <numeric>
#include <stop_token>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <devils_engine/simul/lifecycle.h>
#include <devils_engine/simul/systems.h>

// Owns the synchronous application shell: bootstrap, one broker, the main loop and an extensible
// collection of worker simulations. Traits select concrete project types and construct the worker
// collection; creation/wiring/init/thread ownership/shutdown are invariant engine policy. Workers
// are discoverable by concrete type through main_system::runtime_system(), so adding or omitting a
// subsystem does not require another app_runtime field or a project-side binding function.

namespace devils_engine {
namespace simul {

template <typename Traits>
class app_runtime {
public:
  using traits_type = Traits;
  using broker_type = typename Traits::broker_type;
  using main_type = typename Traits::main_type;
  using bootstrap_type = typename Traits::bootstrap_type;
  using boot_config_type = std::remove_reference_t<decltype(std::declval<bootstrap_type&>().engine)>;
  using settings_type = std::remove_reference_t<decltype(std::declval<bootstrap_type&>().settings)>;
  using main_base_type = main_system<broker_type>;
  using worker_systems_type = worker_systems<broker_type>;

  app_runtime() = default;
  app_runtime(const app_runtime&) = delete;
  app_runtime& operator=(const app_runtime&) = delete;

  ~app_runtime() noexcept {
    shutdown_workers();
    registry_.clear();
    workers_ = worker_systems_type{};
    main_.reset();
  }

  int run() {
    static_assert(std::is_base_of_v<main_base_type, main_type>);

    if constexpr (requires { Traits::make_bootstrap(); }) {
      bootstrap_ = Traits::make_bootstrap();
    } else {
      bootstrap_ = std::make_unique<bootstrap_type>();
    }

    Traits::init_bootstrap(*bootstrap_);
    notify_stage(runtime_stage::bootstrap_ready);

    if constexpr (requires { Traits::make_broker(*bootstrap_); }) {
      broker_ = Traits::make_broker(*bootstrap_);
    } else {
      broker_ = std::make_unique<broker_type>();
    }

    if constexpr (requires { Traits::make_main(*bootstrap_); }) {
      main_ = Traits::make_main(*bootstrap_);
    } else if constexpr (std::is_constructible_v<main_type, bootstrap_type*>) {
      main_ = std::make_unique<main_type>(bootstrap_.get());
    } else {
      main_ = std::make_unique<main_type>();
    }

    if constexpr (requires { Traits::make_workers(*bootstrap_); }) {
      workers_ = Traits::make_workers(*bootstrap_);
    }
    if constexpr (requires { Traits::configure_topology(*bootstrap_, workers_); }) {
      Traits::configure_topology(*bootstrap_, workers_);
    }

    registry_.add(main_.get());
    main_->set_system_registry(&registry_);
    main_->set_broker(broker_.get());
    for (auto& worker : workers_.entries()) {
      if (!registry_.add(worker.type, worker.name, worker.system.get())) {
        throw std::logic_error("simul::app_runtime: duplicate worker system type or type-id collision");
      }
      worker.system->set_broker(broker_.get());
    }
    notify_stage(runtime_stage::systems_created);

    main_->init();
    for (auto& worker : workers_.entries()) {
      worker.system->init();
    }

    notify_stage(runtime_stage::systems_initialized);
    start_workers();
    notify_stage(runtime_stage::workers_started);
    static_cast<main_base_type*>(main_.get())->workers_started();
    notify_stage(runtime_stage::main_loop);
    main_->run(0);
    shutdown_workers();
    notify_stage(runtime_stage::workers_stopped);
    return static_cast<const main_base_type*>(main_.get())->exit_code();
  }

  bool save_settings() {
    if (bootstrap_ == nullptr) {
      return false;
    }
    return Traits::save_settings(*bootstrap_);
  }

  bool reload_settings() {
    if (bootstrap_ == nullptr) {
      return false;
    }
    const bool loaded = Traits::reload_settings(*bootstrap_);
    if (loaded && main_) {
      static_cast<main_base_type*>(main_.get())->runtime_settings_reloaded();
    }
    return loaded;
  }

  broker_type* broker() noexcept {
    return broker_.get();
  }
  bootstrap_type* bootstrap() noexcept {
    return bootstrap_.get();
  }
  boot_config_type* boot_config() noexcept {
    return bootstrap_ != nullptr ? &bootstrap_->engine : nullptr;
  }
  settings_type* settings() noexcept {
    return bootstrap_ != nullptr ? &bootstrap_->settings : nullptr;
  }
  main_type* main() noexcept {
    return main_.get();
  }
  template <typename System>
  System* system() noexcept {
    return registry_.find<System>();
  }

private:
  void notify_stage(const runtime_stage stage) {
    if constexpr (requires { Traits::runtime_stage_changed(stage, *this); }) {
      Traits::runtime_stage_changed(stage, *this);
    }
  }

  void start_workers() {
    worker_threads_.reserve(workers_.size());
    for (auto& worker : workers_.entries()) {
      worker_threads_.emplace_back([sys = worker.system.get(), wait = worker.start_wait_mcs](std::stop_token st) {
        sys->run(st, wait);
      });
    }
  }

  void shutdown_workers() noexcept {
    for (auto& worker : workers_.entries()) {
      worker.system->stop();
    }

    std::vector<size_t> order(worker_threads_.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [this](const size_t a, const size_t b) {
      return workers_.entries()[a].shutdown_order < workers_.entries()[b].shutdown_order;
    });
    for (const size_t i : order) {
      auto& thread = worker_threads_[i];
      if (thread.joinable()) {
        thread.request_stop();
        thread.join();
      }
    }
    worker_threads_.clear();
  }

  std::unique_ptr<broker_type> broker_;
  std::unique_ptr<bootstrap_type> bootstrap_;
  std::unique_ptr<main_type> main_;
  runtime_system_registry registry_;
  worker_systems_type workers_;
  std::vector<std::jthread> worker_threads_;
};

} // namespace simul
} // namespace devils_engine

#endif
