#ifndef DEVILS_ENGINE_UTILS_LOAD_STAGE_H
#define DEVILS_ENGINE_UTILS_LOAD_STAGE_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace devils_engine {
namespace utils {
class load_stage {
public:
  std::string name;

  inline load_stage(std::string name) noexcept : name(std::move(name)) {}
  virtual ~load_stage() noexcept = default;
  virtual void process() const = 0;
};
}
}

#endif