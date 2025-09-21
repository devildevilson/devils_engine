#ifndef DEVILS_ENGINE_CATALOGUE_COMMON_H
#define DEVILS_ENGINE_CATALOGUE_COMMON_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace devils_engine {
namespace catalogue {
// наверное будет где то ближе к реализации 
//struct tick_buffer_header {
//  uint32_t tick;
//  uint32_t checksum;
//  uint32_t size;
//};

struct function_buffer_header {
  uint32_t tick;
  uint32_t id;     // no collision? under ~250 function names uint32 is ok
  uint32_t offset; // enough? packet more than 2^32 bytes?
};

struct buffer {
  std::vector<function_buffer_header> headers;
  std::vector<uint8_t> payload;
};

class consumer {
public:
  virtual ~consumer() noexcept = default;
  virtual void consume(const buffer&) = 0;
};
}
}

#endif