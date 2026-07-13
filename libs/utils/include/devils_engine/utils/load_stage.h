#ifndef DEVILS_ENGINE_UTILS_LOAD_STAGE_H
#define DEVILS_ENGINE_UTILS_LOAD_STAGE_H

// Named polymorphic unit of synchronous loader work.

#include <cstddef>
#include <cstdint>
#include <string>

namespace devils_engine {
namespace utils {
class load_stage {
public:
  std::string name;

  load_stage(std::string name) noexcept;
  virtual ~load_stage() noexcept = default;
  virtual void process() const = 0;
};
} // namespace utils
} // namespace devils_engine

#endif
