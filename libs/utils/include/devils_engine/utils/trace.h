#pragma once

#include <source_location>

namespace devils_engine::utils {
class tracer {
public:
  std::source_location location;

  tracer(std::source_location loc = std::source_location::current()) noexcept;
  ~tracer() noexcept;

  tracer(const tracer& copy) noexcept = delete;
  tracer(tracer&& move) noexcept = delete;
  tracer& operator=(const tracer& copy) noexcept = delete;
  tracer& operator=(tracer&& move) noexcept = delete;
};

} // namespace devils_engine::utils
