#ifndef DEVILS_ENGINE_PAINTER_CONTAINER_H
#define DEVILS_ENGINE_PAINTER_CONTAINER_H

#include <cstdint>
#include <cstddef>
#include <bitset>
#include "vulkan_minimal.h"

namespace devils_engine {
namespace painter {

// имеет смысл переназвать этот класс в палитра
struct container {
  static const size_t max_features_count = 256;
  static const size_t multi_draw_indirect;

  struct device_limits {
    uint32_t max_vertex_input_bindings;
    uint32_t max_vertex_input_attributes;
    uint32_t max_bound_descriptor_sets;
  };

  VkInstance instance;
  VkDebugUtilsMessengerEXT debug_messenger;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue graphics_queue;
  VkQueue compute_queue;
  VkQueue presentation_queue;
  VkPipelineCache cache;

  VmaAllocator buffer_allocator;

  VkCommandPool graphics_command_pool;
  VkCommandPool transfer_command_pool;
  VkFence transfer_fence;

  device_limits limits;
  std::bitset<max_features_count> features;

  container();
  ~container() noexcept;

  void flush_cache();
};


}
}

#endif