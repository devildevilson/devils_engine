#include <thread>

#include <immintrin.h>

#include "devils_engine/utils/time-utils.hpp"
#include "interface.h"

namespace devils_engine {
namespace simul {
static void cpu_relax() {
  _mm_pause();
}

template <typename T>
void spin_sleep_for(const T& dur) {
  const size_t amount = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
  const auto tp = std::chrono::high_resolution_clock::now();
  size_t ns = 0;
  while (ns < amount) {
    const auto new_dur = std::chrono::high_resolution_clock::now() - tp;
    ns = std::chrono::duration_cast<std::chrono::nanoseconds>(new_dur).count();

    cpu_relax();
  }
}

constexpr size_t default_30fps = double(utils::global_time_resolution) / 30.0;

advancer::advancer() noexcept : _frame_time(default_30fps), new_frame_time(default_30fps), _counter(0), _stop(false) {}
advancer::advancer(const size_t frame_time) noexcept : _frame_time(frame_time), new_frame_time(frame_time), _counter(0), _stop(false) {}

std::mutex& advancer::acquire_sync_object() noexcept {
  return mutex;
}

void advancer::run(const size_t wait_mcs) {
  run(std::stop_token{}, wait_mcs); // пустой токен никогда не stop_requested → прежнее поведение
}

void advancer::run(std::stop_token st, const size_t wait_mcs) {
  // spin better
  spin_sleep_for(std::chrono::microseconds(wait_mcs));

  {
    std::unique_lock l(mutex);
    //init();
    _start = clock_t::now();
  }

  bool stop = false;
  while (!stop) {
    // Virtual callbacks must not run under advancer::mutex. Besides making the whole system tick
    // needlessly unavailable to observers, doing so self-deadlocks as soon as a callback uses one
    // of advancer's synchronized accessors (frame_time(), counter(), stop(), ...).
    if (stop_predicate()) {
      break;
    }

    clock_t::time_point next_tp;
    size_t current_frame_time = 0;
    {
      std::unique_lock l(mutex);
      stop = _stop || st.stop_requested();
      if (stop) {
        break;
      }

      if (_frame_time != new_frame_time) {
        _frame_time = new_frame_time;
        _start = clock_t::now();
        _counter = 0;
      }

      _counter += 1;
      next_tp = _start + std::chrono::microseconds(_counter * _frame_time);
      current_frame_time = _frame_time;
    }

    update(current_frame_time);

    // stop_token: кооперативная остановка при разрушении std::jthread (без явного stop()).
    // Проверяем derived predicate также без mutex: predicate вправе пользоваться synchronized
    // accessor-ами самого advancer.
    stop = stop_predicate();
    {
      std::unique_lock l(mutex);
      stop = stop || _stop || st.stop_requested();
    }

    // нужно добавить проверку того что мы лагаем
    // это наверное до секунды разница с next_tp ?
    if (!stop) {
      std::this_thread::sleep_until(next_tp);
    }
  }
}

void advancer::set_frame_time(const size_t frame_time) {
  std::unique_lock l(mutex);
  new_frame_time = frame_time;
}

size_t advancer::frame_time() const {
  std::unique_lock l(mutex);
  return _frame_time;
}
size_t advancer::counter() const {
  std::unique_lock l(mutex);
  return _counter;
}
advancer::clock_t::time_point advancer::start() const {
  std::unique_lock l(mutex);
  return _start;
}
double advancer::compute_fps() const {
  std::unique_lock l(mutex);
  auto tp = clock_t::now();
  const auto diff = utils::count_mcs(_start, tp);
  const auto computed_frame_time = double(diff) / double(_counter);
  return double(utils::app_clock::resolution()) / computed_frame_time;
}

void advancer::reset_counter() {
  std::unique_lock l(mutex);
  _start = clock_t::now();
  _counter = 0;
}

void advancer::stop() {
  std::unique_lock l(mutex);
  _stop = true;
}
} // namespace simul
} // namespace devils_engine
