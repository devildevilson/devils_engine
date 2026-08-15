#ifndef DEVILS_ENGINE_PLAYGROUND_FRAME_PACER_H
#define DEVILS_ENGINE_PLAYGROUND_FRAME_PACER_H

#include <chrono>
#include <cstdint>

namespace devils_engine::playground {

// Producer-loop pacing independent of swapchain presentation mode. Missed deadlines are dropped
// instead of replayed, so a slow frame cannot trigger a burst of catch-up frames.
class frame_pacer {
public:
  explicit frame_pacer(uint32_t target_fps = 0) noexcept;
  void set_target(uint32_t target_fps) noexcept;
  uint32_t target() const noexcept;
  void wait();

private:
  using clock_t = std::chrono::steady_clock;
  clock_t::duration period_{};
  clock_t::time_point deadline_{};
  uint32_t target_fps_ = 0;
};

} // namespace devils_engine::playground

#endif
