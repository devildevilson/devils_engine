#ifndef DEVILS_ENGINE_SIMUL_APP_RUNTIME_H
#define DEVILS_ENGINE_SIMUL_APP_RUNTIME_H

#include <cstddef>
#include <memory>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace devils_engine {
namespace simul {

template <typename Traits>
class app_runtime {
public:
  using traits_type = Traits;
  using broker_type = typename Traits::broker_type;
  using main_type = typename Traits::main_type;
  using render_type = typename Traits::render_type;
  using assets_type = typename Traits::assets_type;
  using sound_type = typename Traits::sound_type;
  using bootstrap_type = typename Traits::bootstrap_type;
  using boot_config_type = typename Traits::boot_config_type;
  using settings_type = typename Traits::settings_type;

  app_runtime() = default;
  app_runtime(const app_runtime&) = delete;
  app_runtime& operator=(const app_runtime&) = delete;

  ~app_runtime() noexcept { shutdown_workers(); }

  int run() {
    bootstrap_ = Traits::make_bootstrap();
    Traits::init_bootstrap(*bootstrap_);

    broker_ = Traits::make_broker(*bootstrap_);
    main_ = Traits::make_main(*bootstrap_);

    Traits::set_broker(*main_, *broker_);
    main_->init();

    sound_ = Traits::make_sound(*bootstrap_);
    render_ = Traits::make_render(*bootstrap_);
    assets_ = Traits::make_assets(*bootstrap_);

    if (sound_) {
      sound_->init();
      Traits::set_broker(*sound_, *broker_);
    }
    if (render_) {
      render_->init();
      Traits::set_broker(*render_, *broker_);
    }
    if (assets_) {
      assets_->init();
      Traits::set_broker(*assets_, *broker_);
    }

    Traits::bind_systems(*main_, *bootstrap_, sound_.get(), render_.get(), assets_.get());
    start_workers();
    Traits::after_workers_started(*main_);
    main_->run(Traits::main_wait_mcs(*main_));
    shutdown_workers();
    return Traits::exit_code(*main_);
  }

  bool save_settings() {
    if (bootstrap_ == nullptr) return false;
    return Traits::save_settings(*bootstrap_);
  }

  bool reload_settings() {
    if (bootstrap_ == nullptr) return false;
    const bool loaded = Traits::reload_settings(*bootstrap_);
    if (loaded && main_) Traits::settings_reloaded(*main_, *bootstrap_);
    return loaded;
  }

  broker_type* broker() noexcept { return broker_.get(); }
  bootstrap_type* bootstrap() noexcept { return bootstrap_.get(); }
  boot_config_type* boot_config() noexcept { return bootstrap_ != nullptr ? &Traits::boot_config(*bootstrap_) : nullptr; }
  settings_type* settings() noexcept { return bootstrap_ != nullptr ? &Traits::settings(*bootstrap_) : nullptr; }
  main_type* main_system() noexcept { return main_.get(); }
  render_type* render_system() noexcept { return render_.get(); }
  assets_type* assets_system() noexcept { return assets_.get(); }
  sound_type* sound_system() noexcept { return sound_.get(); }

private:
  void start_workers() {
    if (sound_) {
      const size_t wait = Traits::sound_wait_mcs(*bootstrap_, *sound_);
      sound_thread_ = std::jthread([sys = sound_.get(), wait](std::stop_token st) { sys->run(st, wait); });
    }
    if (render_) {
      const size_t wait = Traits::render_wait_mcs(*bootstrap_, *render_);
      render_thread_ = std::jthread([sys = render_.get(), wait](std::stop_token st) { sys->run(st, wait); });
    }
    if (assets_) {
      const size_t wait = Traits::assets_wait_mcs(*bootstrap_, *assets_);
      assets_thread_ = std::jthread([sys = assets_.get(), wait](std::stop_token st) { sys->run(st, wait); });
    }
  }

  void shutdown_workers() noexcept {
    if (sound_) sound_->stop();
    if (render_) render_->stop();
    if (assets_) assets_->stop();

    // Render joins before window/input destruction in the main system. std::jthread reset requests
    // stop then joins.
    render_thread_ = std::jthread{};
    assets_thread_ = std::jthread{};
    sound_thread_ = std::jthread{};
  }

  std::unique_ptr<broker_type> broker_;
  std::unique_ptr<bootstrap_type> bootstrap_;
  std::unique_ptr<main_type> main_;
  std::unique_ptr<render_type> render_;
  std::unique_ptr<assets_type> assets_;
  std::unique_ptr<sound_type> sound_;

  std::jthread render_thread_;
  std::jthread assets_thread_;
  std::jthread sound_thread_;
};

}
}

#endif
