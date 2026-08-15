#include "devils_engine/playground/frame_pacer.h"

#include <thread>

namespace devils_engine::playground {

frame_pacer::frame_pacer(const uint32_t target_fps) noexcept {
  set_target(target_fps);
}

void frame_pacer::set_target(const uint32_t target_fps) noexcept {
  target_fps_ = target_fps;
  period_ = target_fps == 0
              ? clock_t::duration::zero()
              : std::chrono::duration_cast<clock_t::duration>(std::chrono::duration<double>(1.0 / double(target_fps)));
  deadline_ = period_ == clock_t::duration::zero() ? clock_t::time_point{} : clock_t::now() + period_;
}

uint32_t frame_pacer::target() const noexcept {
  return target_fps_;
}

void frame_pacer::wait() {
  if (period_ == clock_t::duration::zero()) {
    return;
  }

  const auto now = clock_t::now();
  if (now < deadline_) {
    std::this_thread::sleep_until(deadline_);
    deadline_ += period_;
  } else {
    // Drop the missed schedule. Catch-up bursts only create heat and do not recover presentation.
    deadline_ = now + period_;
  }
}

} // namespace devils_engine::playground
