#ifndef GN09_NEXT_RUNTIME_H
#define GN09_NEXT_RUNTIME_H
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/originator/generator_resource.h"
#include "devils_engine/originator/primitive_field.h"
#include "streaming.h"
namespace gn09_next {
namespace o = devils_engine::originator;
inline constexpr int chunk_cells = 16;
o::instance_key key(int64_t x, int64_t z);
o::instance_key key(int64_t x, int64_t y, int64_t z);
class runtime {
public:
  explicit runtime(const std::string& module = "gn09_next", bool natural = false, bool caves_only = false);
  o::generator_chain& chain() {
    return *chain_;
  }
  const o::parameters& constants() const {
    return loaded_.setup.constants;
  }
  std::array<double, 3> start(uint64_t seed, int64_t x, int64_t z);
  o::result_ptr chunk(uint64_t seed, const o::chunk_key& at);
  devils_engine::gn03::generator_factory factory(uint64_t seed);
  void report() const;
  std::string_view shape_source(std::string_view name) const;
  const std::string& generator_name() const {
    return generator_;
  }

private:
  std::string generator_;
  devils_engine::demiurg::module_system modules_;
  devils_engine::demiurg::resource_system resources_;
  o::loaded_chain loaded_;
  o::shape_vocabulary vocabulary_;
  std::unique_ptr<o::generator_chain> chain_;
  std::array<std::atomic<size_t>, 4> biome_chunks_{};
};
} // namespace gn09_next
#endif
