#include "interface.h"

#include <thread>
#include "utils/time-utils.hpp"

namespace devils_engine {
namespace simul {
const size_t default_30fps = double(utils::app_clock::resolution()) / 30.0;

advancer::advancer() noexcept : _frame_time(default_30fps), new_frame_time(default_30fps), _counter(0), _stop(false) {}
void advancer::run(const size_t wait_mcs) {
  // чуть обождем, мы можем запустить звуки чуть позже чем основную симуляцию
  // тогда ограничение в виде sleep_until сработает чуть лучше
  std::this_thread::sleep_for(std::chrono::microseconds(wait_mcs));

  {
    std::unique_lock l(mutex);
    init();
    _start = clock_t::now();
  }

  bool stop = false;
  while (!stop) {
    clock_t::time_point next_tp;
    {
      std::unique_lock l(mutex);
      if (_frame_time != new_frame_time) {
        _frame_time = new_frame_time;
        _start = clock_t::now();
        _counter = 0;
      }

      _counter += 1;
      next_tp = _start + std::chrono::microseconds(_counter * _frame_time); 
      update(_frame_time);
      stop = stop_predicate() || _stop;
    }

    // update должен быть ПОД мьютексом? скорее да

    // нужно добавить проверку того что мы лагаем
    // это наверное до секунды разница с next_tp ?
    std::this_thread::sleep_until(next_tp);
  }
}

void advancer::set_frame_time(const size_t frame_time) {
  std::unique_lock l(mutex);
  new_frame_time = frame_time;
}

size_t advancer::frame_time() const { std::unique_lock l(mutex); return _frame_time; }
size_t advancer::counter() const { std::unique_lock l(mutex); return _counter; }
advancer::clock_t::time_point advancer::start() const { std::unique_lock l(mutex); return _start; }
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
}
}