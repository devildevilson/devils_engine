#pragma once

#include "simulation.h"

namespace devils_engine::phy03 {

enum class replay_case { strong, weak, low, push, rescue, overlap, stack };
constexpr uint32_t replay_ticks = 510;
replay_case replay_from_name(std::string_view name);
intent recorded_input(replay_case scenario, uint32_t tick);

// Consumes render time, but reads input and advances physics only on tick boundaries.
class fixed_clock {
public:
  template <typename Step>
  void advance(const double seconds, Step&& step) {
    credit += seconds * tick_rate;
    while (credit >= 1 - 1e-9) {
      credit -= 1;
      step();
    }
  }

  float alpha() const;

private:
  double credit = 0;
};

uint64_t fingerprint(uint64_t previous, const snapshot& sample);
uint64_t run_replay(replay_case scenario, uint32_t render_rate, bool dump);
void verify();

}
