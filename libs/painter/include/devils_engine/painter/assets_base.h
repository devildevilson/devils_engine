#ifndef DEVILS_ENGINE_PAINTER_ASSETS_BASE_H
#define DEVILS_ENGINE_PAINTER_ASSETS_BASE_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <atomic>
#include <span>
#include "vulkan_minimal.h"

namespace devils_engine {
namespace painter {

constexpr size_t MAX_BUFFER_SLOTS = 512;
constexpr size_t MAX_TEXTURE_SLOTS = 4098;

//struct buffer_asset_handle { uint32_t slot, generation; };
//struct texture_asset_handle { uint32_t slot, generation; };
using buffer_asset_handle = uint32_t;
using texture_asset_handle = uint32_t;
enum class asset_state { empty, reserved, ready, pending_remove };

// необходимый минимум для слота какой?
// должен быть host_visible меш для UI например
// + у нас как обычно есть несколько кадров где используются данные UI буфера
// так UI будет отдельной системой не пересекающей ассеты
struct buffer_slot {
  std::string name;
  std::string geometry_name;
  std::atomic<asset_state> state;
  uint32_t forbid_after_frame; // нам просто нужно записать раньше чем поменяем состояние

  // остается вопрос с заполнением инстанс буфера с цпу
  // оформлять его как событие? например одно событие на пачку обновлений?
  // так мы хотя бы не выстрелим себе в ногу, да и UI скорее всего только так и будет обновляться

  uint32_t geometry;

  uint32_t vertex_count;
  uint32_t first_vertex; // всегда 0?
  int32_t vertex_offset;
  uint32_t index_count;
  uint32_t first_index; // всегда 0?

  VmaAllocation vertex_alc;
  VkBuffer vertex_storage;
  VmaAllocation index_alc;
  VkBuffer index_storage;

  size_t vertex_size;
  size_t index_size;

  buffer_slot() noexcept;
  buffer_slot(const buffer_slot& copy) noexcept;
  buffer_slot& operator=(const buffer_slot& copy) noexcept;
  buffer_slot(buffer_slot&& move) noexcept;
  buffer_slot& operator=(buffer_slot&& move) noexcept;
};

// текстурки зависят от дескриптора
// смена текстурок в слоте должна проиходить плавнее в том плане что
// мы бы хотели положить текстурку возможно в тот же слот что и старая текстурка которую сейчас удаляют
// наверное для буферов норм, а для текстурок так себе
struct texture_slot {
  std::string name;
  std::atomic<asset_state> state;
  uint32_t forbid_after_frame;

  uint32_t format;
  struct { uint32_t x, y, z; } extents;

  VmaAllocation alc;
  VkImage storage;
  VkImageView view;

  texture_slot() noexcept;
  texture_slot(const texture_slot& copy) noexcept;
  texture_slot& operator=(const texture_slot& copy) noexcept;
  texture_slot(texture_slot&& move) noexcept;
  texture_slot& operator=(texture_slot&& move) noexcept;
};

struct texture_create_info { 
  //std::string name;
  struct { uint32_t x, y, z; } extents; 
  uint32_t format; 
};

struct buffer_create_info { 
  //std::string name;
  std::string geometry_name;

  uint32_t vertex_count; 
  uint32_t index_count;
};

struct graphics_base;

struct assets_base {
  VkDevice device;
  VkPhysicalDevice physical_device;
  VkQueue transfer;
  VkFence fence;
  VkCommandPool command_pool;
  //VkDescriptorPool descriptor_pool;
  VkCommandBuffer command_buffer;

  VmaAllocator allocator;

  const graphics_base* base; // нужно чтобы геометрию найти

  // первый слот это всегда пустой буфер или картинка
  // что такое пустой буфер? маленький буфер со специальной топологией 
  // ну вообще если мы хотим нарисовать empty mesh то это скорее 
  // мы хотим взять топологию такую что мы сами в вершинном буфере сгенерируем интерсующие нас данные 
  // например топология гексагонального цилиндра - нам не нужны точки, мы их и так знаем 
  std::vector<buffer_slot> buffer_slots;
  std::vector<texture_slot> texture_slots;

  assets_base(VkDevice device, VkPhysicalDevice physical_device) noexcept;
  ~assets_base() noexcept;

  void create_fence();
  void create_command_buffer(VkQueue transfer, const uint32_t queue_family_index);
  void create_allocator(VkInstance inst, const size_t preferred_heap_block = 0);
  void set_graphics_base(const graphics_base* base);

  // это дело должно работать обособлено
  // рендеру интересно только тогда когда заполнен дескриптор или создана связка (mesh, draw_group)
  buffer_asset_handle register_buffer_storage(std::string name);
  texture_asset_handle register_texture_storage(std::string name);
  void clear_buffer_storage(const buffer_asset_handle& h);
  void clear_texture_storage(const texture_asset_handle& h);
  buffer_asset_handle find_buffer_storage(const std::string_view& name) const;
  texture_asset_handle find_texture_storage(const std::string_view& name) const;

  void create_buffer_storage(const buffer_asset_handle& h, const buffer_create_info &info);
  void create_texture_storage(const texture_asset_handle& h, const texture_create_info &info);

  // как бы оформить батч копи? то есть это надо предсоздать стаджинг буферы
  // потом закинуть, вообще для батч копи нужно создать много буферов
  void populate_buffer_storage(const buffer_asset_handle& h, const std::span<const uint8_t>& vertex_data, const std::span<const uint8_t>& index_data);
  void populate_texture_storage(const texture_asset_handle& h, const std::span<const uint8_t>& data);

  void mark_ready_buffer_slot(const buffer_asset_handle& h);
  void mark_ready_texture_slot(const texture_asset_handle& h);
  void mark_remove_buffer_slot(const buffer_asset_handle& h);
  void mark_remove_texture_slot(const texture_asset_handle& h);
};

}
}

#endif