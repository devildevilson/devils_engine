#ifndef DEVILS_ENGINE_PAINTER_SYSTEM_INFO_H
#define DEVILS_ENGINE_PAINTER_SYSTEM_INFO_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "common.h"
#include "vulkan_minimal.h"

// было бы еще неплохо посмотреть какие фичи есть у устройства
// для этого нужно подтягивать структуры с фичами, но беда в том что это не энумы а говно какое то
// так что желательно пройтись по ним и например найти полный размер всех фич
// для этого можно пробежаться по всем структурам в рефлекте
// и даже найти по имени переменной фичу + также расставить значения в битовом поле

// часть данных из этой структуры запишем в кеш - по нему быстренько снова создадим устройство

namespace devils_engine {
namespace painter {
struct system_info {
  struct physical_device {
    enum class type {
      other,
      integrated_gpu,
      discrete_gpu,
      virtual_gpu,
      cpu,
      count
    };
    static std::string to_string(const enum type type);

    enum class present_mode {
      immediate,
      mailbox,
      fifo,
      fifo_relaxed,
      shared_demand_refresh,
      shared_continuous_refresh,
      count
    };
    static std::string to_string(const enum present_mode present_mode);

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
    enum type type;
    uint32_t queue_family_index_surface_support;
    std::vector<queue_properties_t> queue_families;
    std::vector<present_mode> present_modes;

    physical_device();
  };

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

  // запишем в массив
  void check_devices_surface_capability(const VkSurfaceKHR s);

  VkPhysicalDevice choose_physical_device() const;

  void dump_cache_to_disk(VkPhysicalDevice dev, cached_system_data* cached_data = nullptr);
};

}
}

#endif