#ifndef DEVILS_ENGINE_PAINTER_DEVICE_FEATURES_H
#define DEVILS_ENGINE_PAINTER_DEVICE_FEATURES_H

#include <cstddef>
#include <cstdint>
#include <bitset>

#define DEVILS_ENGINE_PAINTER_FEATURES_10_LIST \
  X(indirect_first_instance, drawIndirectFirstInstance) \
  X(multi_draw_indirect, multiDrawIndirect) \
  X(sampler_anisotropy, samplerAnisotropy) \
  X(compression_astc, textureCompressionASTC_LDR) \
  X(compression_bc, textureCompressionBC) \
  X(compression_etc2, textureCompressionETC2) \

#define DEVILS_ENGINE_PHYSICAL_DEVICE_TYPE_LIST \
  X(other) \
  X(integrated_gpu) \
  X(discrete_gpu) \
  X(virtual_gpu) \
  X(cpu) \

#define DEVILS_ENGINE_PHYSICAL_DEVICE_PRESENT_MODE_LIST \
  X(immediate, 0) \
  X(mailbox, 1) \
  X(fifo, 2) \
  X(fifo_relaxed, 3) \
  X(shared_demand_refresh, 1000111000) \
  X(shared_continuous_refresh, 1000111001) \

namespace devils_engine {
namespace painter {

namespace device_features {
enum values : uint32_t {
#define X(name, vulkan_name) name,
  DEVILS_ENGINE_PAINTER_FEATURES_10_LIST
#undef X
  count
};

std::string_view to_string(const enum values val) noexcept;
values from_string(const std::string_view& str) noexcept;

}

using device_features_t = std::bitset<device_features::count>;

namespace physical_device_type {
enum values : uint32_t {
#define X(name) name,
  DEVILS_ENGINE_PHYSICAL_DEVICE_TYPE_LIST
#undef X
  count
};

std::string_view to_string(const enum values val) noexcept;
values from_string(const std::string_view& str) noexcept;
}

namespace physical_device_present_mode {
enum values : uint32_t {
#define X(name, value) name = value,
  DEVILS_ENGINE_PHYSICAL_DEVICE_PRESENT_MODE_LIST
#undef X
  count
};

std::string_view to_string(const enum values val) noexcept;
values from_string(const std::string_view& str) noexcept;
}

}
}

#endif