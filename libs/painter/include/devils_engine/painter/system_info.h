#ifndef DEVILS_ENGINE_PAINTER_SYSTEM_INFO_H
#define DEVILS_ENGINE_PAINTER_SYSTEM_INFO_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "vulkan_minimal.h"
#include "device_features.h"

/*
на счет разных фич устройства: к сожалению простого способа понять что мне нужно а что нет
несуществует, единственное что я могу сделать это структуру которая проверит жеско заданные
фичи, которые меня интересуют, собственно нужно просто выкинуть их в большой энум
типа мне нужны просто пачка проверок

тут нужно предусмотреть наличие на диске кешированного устройства
*/



namespace devils_engine {
namespace painter {

struct cached_system_data {
  std::string device_name;
  std::string device_type;
  uint32_t device_id;
  uint32_t vendor_id;
  uint32_t graphics_queue;
  uint32_t compute_queue;
  uint32_t transfer_queue;
  uint32_t present_queue; // было бы чудесно если бы совпадало с graphics_queue
  std::string desirable_present_mode;
  std::string fallback_present_mode;
  size_t memory_capacity;
  std::vector<std::string> features;
};

struct physical_device_data {
  VkPhysicalDevice handle;
  device_features_t features;
  uint32_t desirable_present_mode;
  uint32_t fallback_present_mode;
  uint32_t graphics_queue;
  uint32_t compute_queue;
  uint32_t transfer_queue;
  uint32_t present_queue;
};

struct system_info {
  struct physical_device {
    struct queue_properties_t {
      VkFlags flags;
      uint32_t queue_count;
      uint32_t timestamp_valid_bits;
    };

    VkPhysicalDevice handle;
    std::string name;
    size_t memory;
    uint32_t id;
    uint32_t vendor_id;
    physical_device_type::values type;
    uint32_t queue_family_index_surface_support;
    std::vector<queue_properties_t> queue_families;
    std::vector<physical_device_present_mode::values> present_modes;
    device_features_t features;

    physical_device();
  };

  static bool try_load_cached_data(VkInstance instance, physical_device_data* phys_data = nullptr, cached_system_data* cached_data = nullptr);
  static void print_choosed_device(VkPhysicalDevice device) noexcept;

  bool instance_owner;
  VkInstance instance;
  std::vector<physical_device> devices;

  // да наверн сразу все и найдем че нам
  system_info();
  system_info(VkInstance instance);
  // не забыть перед этим удалить VkSurfaceKHR, да и окно тоже
  ~system_info() noexcept;

  // запускаем в конструкторе
  void init();

  void check_devices_surface_capability(const VkSurfaceKHR s);

  physical_device_data choose_physical_device() const;

  void dump_cache_to_disk(VkPhysicalDevice dev, cached_system_data* cached_data = nullptr);
};

}
}

#endif