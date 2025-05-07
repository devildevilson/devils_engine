#ifndef DEVILS_ENGINE_PAINTER_COMMON_H
#define DEVILS_ENGINE_PAINTER_COMMON_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace devils_engine {
namespace painter {
struct cached_system_data {
  std::string device_name;
  std::string device_type;
  uint32_t device_id;
  uint32_t vendor_id;
  uint32_t graphics_queue;
  uint32_t compute_queue;
  uint32_t present_queue;
  std::string desirable_present_mode;
  std::string fallback_present_mode;
  size_t memory_capacity;
};

#define DEVILS_ENGINE_PAINTER_CACHED_SYSTEM_DATA_INIT {{}, {}, 0, 0, 0, 0, 0, {}, {}, 0}

#define DEVILS_ENGINE_PAINTER_IMAGE_TARGET_LIST \
  X(undefined) \
  X(general) \
  X(attachment) \
  X(read_only) \
  X(transfer_src) \
  X(transfer_dst) \
  X(present) \


namespace image_target {
enum values : uint32_t {
#define X(name) name,
  DEVILS_ENGINE_PAINTER_IMAGE_TARGET_LIST
#undef X
  count,
  invalid = UINT32_MAX
};

std::string_view to_string(const values val);
values from_string(const std::string_view &str);
}

// + возможно имеет смысл некоторые форматы
// с другой стороны зачем я так впрягаюсь сильно
// не уверен что это получится сделать адекватно для какого то продакшен конфига

}
}

#endif