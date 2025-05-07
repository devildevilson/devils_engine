#ifndef DEVILS_ENGINE_PAINTER_HIERARCHICAL_IMAGE_CONTAINER_H
#define DEVILS_ENGINE_PAINTER_HIERARCHICAL_IMAGE_CONTAINER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <sul/dynamic_bitset.hpp>
#include "vulkan_minimal.h"
#include "image_pool.h"
#include "primitives.h"

// на какие вопросы должен отвечать этот класс?
// 1) создай пулы по нескольким размерам
// 2) найди слот по размерам
// 3) найди слот по размерам или ниже
// 4) положи все пулы в сет
// предполагается что игра может запрашивать картинку определенного качества
// и мы сможем предоставить ХОТЯ БЫ какую то картинку приложению

// короче говоря придумал алгоритм поинтереснее
// 1) вместо ебучих картинок создаем VkDeviceMemory достаточно большой чтобы например держать в себе 4 4к32бит текстурки
// 2) подразбиваем как кДерево это пространство на тонну мелких участков размером 32*32*4 например
// 3) выделяем память для картинки так чтобы она занимала ровно по участку памяти
// точнее выделять можно произвольно, но занимает в памяти именно это пространство
// 4) "выделение" происходит просто за счет того что указываем оффсет
// 5) для новых картинок ищем позицию 

namespace devils_engine {
namespace painter {

class image_pool;

class hierarchical_image_container final : public image_container {
public:
  struct image_t {
    VkImage handle;
    VkImageView view;
    VkSampler sampler;
    uint32_t width, height, format;
    size_t offset, size;
    std::string name;

    inline image_t() noexcept : handle(VK_NULL_HANDLE), view(VK_NULL_HANDLE), sampler(VK_NULL_HANDLE), width(0), height(0), format(0), offset(0), size(0) {}
  };

  hierarchical_image_container(std::string name, VkDevice device, VkPhysicalDevice physical_device, extent_t memory_extents, extent_t block_extents);
  ~hierarchical_image_container() noexcept;

  bool is_exists(const uint32_t index) const override;
  uint32_t create(std::string name, const extent_t extent, const extent_t real_extent, const uint32_t format, VkSampler sampler);
  uint32_t create(std::string name, const extent_t extent, const uint32_t format, VkSampler sampler) override;
  uint32_t create_any(std::string name, const extent_t extent, const uint32_t format, VkSampler sampler) override;
  void destroy(const uint32_t index) override;
  void adjust_extents(const uint32_t index, const extent_t extent);

  VkImage storage(const uint32_t index) const override;
  VkImageView view(const uint32_t index) const override;
  VkSampler sampler(const uint32_t index) const override;
  extent_t extent(const uint32_t index) const override;
  uint32_t format(const uint32_t index) const override;
  std::string_view name(const uint32_t index) const override;

  size_t capacity() const;
  size_t size() const; // нужно ли?

  void clear();

  // наверное имеет смысл убрать под отдельный виртуальный класс
  void update_descriptor_set(VkDescriptorSet set, const uint32_t binding, const uint32_t first_element) const override;
  void change_layout(VkCommandBuffer buffer, const uint32_t index, const uint32_t old_layout, const uint32_t new_layout) const override;
  void change_layout_all(VkCommandBuffer buffer, const uint32_t old_layout, const uint32_t new_layout) const override;
  void copy_data(VkCommandBuffer buffer, VkImage image, const uint32_t index) const override;
  void blit_data(VkCommandBuffer buffer, const std::tuple<VkImage,uint32_t,uint32_t> &src_image, const uint32_t index, const uint32_t filter = 0) const override;
private:
  VkDevice device;
  VkPhysicalDevice physical_device;
  VkDeviceMemory memory;
  extent_t memory_extents;
  extent_t block_extents;
  size_t block_size;
  size_t memory_size;

  image_t null_image;
  VkDeviceMemory null_memory;
  std::vector<image_t> images;
  sul::dynamic_bitset<size_t> block_usage;
};

class hierarchical_image_container2 {
public:
  struct size_info {
    uint32_t width, height;
    uint32_t count;
  };

  hierarchical_image_container2(VkDevice device, VmaAllocator allocator, const uint32_t format, std::vector<size_info> info);
  ~hierarchical_image_container2() noexcept;

  // надо прилично так постараться чтобы использовать аж 65к картинок, 
  // поэтому положим в первые 16 бит индекс в иерархии
  uint32_t aquire_image(const uint32_t width, const uint32_t height);
  uint32_t aquire_image_any(const uint32_t width, const uint32_t height);
  void free_image(const uint32_t slot);

  VkImage storage(const uint32_t slot) const;
  VkImageView view(const uint32_t slot) const;
  size_t capaсity(const uint32_t slot) const;
  size_t size(const uint32_t slot) const;
  std::tuple<uint32_t, uint32_t> extent(const uint32_t slot) const;

  void write_descriptor_set(VkDescriptorSet set, const uint32_t binding, const uint32_t start_element = 0) const;
private:
  VkDevice device;
  std::vector<std::unique_ptr<image_pool>> pools;
};

}
}

#endif