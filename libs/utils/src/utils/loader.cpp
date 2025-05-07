#include "loader.h"

#include "utils/core.h"

namespace devils_engine {
namespace utils {
loader2::loader2(std::string name) noexcept : _stage(nullptr), name(std::move(name)), _counter(0) {}

void loader2::process() {
  while (true) {
    {
      lock l(_mutex);
      if (_counter >= _stages.size()) break;

      _stage = _stages[_counter].get();
      _counter += 1;
    }

    _stage->process();
  }
}

size_t loader2::counter() const noexcept { lock l(_mutex); return _counter; }
size_t loader2::size() const noexcept { lock l(_mutex); return _stages.size(); }
bool loader2::finished() const noexcept { lock l(_mutex); return _counter >= _stages.size(); }
std::string_view loader2::stage_name() const noexcept { lock l(_mutex); return _stage != nullptr ? std::string_view(_stage->name) : std::string_view(); }

template <typename Clock>
size_t mcs_epoch(typename Clock::time_point tp) {
  const auto cast = std::chrono::time_point_cast<std::chrono::microseconds>(tp);
  const auto ep = cast.time_since_epoch();
  return ep.count();
}

template <typename Clock>
static Clock::time_point tp_from_mcs(const size_t mcs) {
  std::chrono::microseconds dur(mcs);
  std::chrono::time_point<Clock> dt(dur);
  return Clock::time_point(dt);
}

loader::loader(std::string name) noexcept : load_stage(std::move(name)), _counter(0), _tp(mcs_epoch<clock>(clock::now())), waiters_count(0), waiters{nullptr} {}

void loader::add_waiter(thread::semaphore_interface* inter) {
  if (waiters_count >= max_waiters) utils::error("Too many waiters for loader '{}'", name);
  const size_t index = waiters_count;
  waiters_count += 1;
  waiters[index] = inter;
}

void loader::process() const {
  for (size_t i = 0; i < waiters_count; ++i) {
    waiters[i]->wait_for(std::chrono::seconds(2 * 60 * 60), 1000000); // 1ms
  }

  _tp.store(mcs_epoch<clock>(clock::now()), std::memory_order_relaxed); 

  for (size_t i = 0; i < _stages.size(); ++i) {
    _counter.store(i, std::memory_order_relaxed); // здесь мы можем использовать самый простой мемори ордер
    _stages[i]->process();
  }

  _counter.fetch_add(1, std::memory_order_relaxed);
}

size_t loader::counter() const noexcept { return _counter.load(std::memory_order_relaxed); }
size_t loader::size() const noexcept { return _stages.size(); }
bool loader::finished() const noexcept { return counter() >= size(); }
std::string_view loader::stage_name() const noexcept { return finished() ? std::string_view() : std::string_view(_stages[counter()]->name); }
loader::tp loader::start_time() const noexcept {
  return tp_from_mcs<clock>(_tp.load(std::memory_order_relaxed));
}

void loader::reset() { _counter.store(0, std::memory_order_relaxed); }
thread::semaphore_state::values loader::state() const { return static_cast<thread::semaphore_state::values>(finished()); }
bool loader::wait_until(tp t, const size_t tolerance_in_ns) const {
  int64_t ns = -1;
  while (ns < 0 && !finished()) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(tolerance_in_ns));
    const auto new_dur = std::chrono::high_resolution_clock::now() - t;
    ns = std::chrono::duration_cast<std::chrono::nanoseconds>(new_dur).count();
  }

  return finished();
}

}
}