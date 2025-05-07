#ifndef DEVILS_ENGINE_PAINTER_IMAGE_POOL_H
#define DEVILS_ENGINE_PAINTER_IMAGE_POOL_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include "vulkan_minimal.h"
#include "primitives.h"

// нужно еще где то добавить получение данных для изменения лэйаута
// что с сэмлером? как раз для текстурок имеет смысл пользоваться внешними семплерами

namespace devils_engine {
namespace painter {

class image_pool final : public image_container {
public:
  struct image_t {
    VkImageView view;
    VkSampler sampler;
    // здесь укажем формат вью, с флагами ниже он может быть ПРАКТИЧЕСКИ ЛЮБЫМ в том числе свизлинг между rgba32 и bgra32
    uint32_t layer, format;
    std::string name;

    inline image_t() noexcept : view(VK_NULL_HANDLE), sampler(VK_NULL_HANDLE), layer(0), format(0) {}
  };
  
  // тут укажем формат ХРАНИЛИЩА + к нему укажем в картинке что VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT и наверное VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT
  image_pool(std::string name, VkDevice device, VmaAllocator allocator, const uint32_t format, const extent_t extent, const uint32_t count);
  ~image_pool() noexcept;

  bool is_exists(const uint32_t index) const override;
  uint32_t create(std::string name, const extent_t extent, const uint32_t format, VkSampler sampler) override;
  uint32_t create_any(std::string name, const extent_t extent, const uint32_t format, VkSampler sampler) override;
  void destroy(const uint32_t index) override;

  //bool is_free(const uint32_t index) const;
  uint32_t aquire_slot();
  void free_slot(const uint32_t index);

  extent_t extent(const uint32_t index) const override;
  uint32_t format(const uint32_t index) const override;
  VkImage storage(const uint32_t index) const override;
  VkImageView view(const uint32_t index) const override;
  VkSampler sampler(const uint32_t index) const override;
  std::string_view name(const uint32_t index) const override;

  VkImage storage() const;
  VkImageView view() const;
  size_t capacity() const;
  size_t size() const;
  std::tuple<uint32_t, uint32_t> extent() const;
  bool has_free_slots() const;

  void clear();

  void update_descriptor_set(VkDescriptorSet set, const uint32_t binding, const uint32_t first_element) const override;
  void change_layout(VkCommandBuffer buffer, const uint32_t index, const uint32_t old_layout, const uint32_t new_layout) const override;
  void change_layout_all(VkCommandBuffer buffer, const uint32_t old_layout, const uint32_t new_layout) const override;
  // assuming equal extent
  void copy_data(VkCommandBuffer buffer, VkImage image, const uint32_t index) const override;
  void blit_data(VkCommandBuffer buffer, const std::tuple<VkImage,uint32_t,uint32_t> &src_image, const uint32_t index, const uint32_t filter = 0) const override;
private:
  VkDevice _device;
  VmaAllocator _allocator;

  VmaAllocation _allocation;
  VkImage _storage;
  VkImageView _view;
  uint32_t _width, _height;
  uint32_t _format;
  uint32_t _size;
  std::vector<image_t> _images;

  image_t _null_image;
  VmaAllocation _null_allocation;
  VkImage _null_storage;
};

}
}

#endif