#ifndef DEVILS_ENGINE_PAINTER_ARBITRARY_IMAGE_CONTAINER_H
#define DEVILS_ENGINE_PAINTER_ARBITRARY_IMAGE_CONTAINER_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include "vulkan_minimal.h"
#include "primitives.h"

// откуда то надо получить сэмплеры
// у сэмплеров поди тоже будет свой конфиг

// вообще я тут подумал: еще вариант для пула картинок это:
// 1) выделить много памяти например на 1024 32*32*4  картинок
// 2) принимать картинки размером (w,h%32==0) 
// 3) поделить память на уровни вложенности по квадратам в два раза меньшего размера как kTree
// 4) искать в этом дереве свободный участок по уровню
// 5) биндить картинку в этот участок
// 6) ПРОФИТ
// почти тоже самое что и иерархия картинок.......
// хотя даже не почти, лучше!
// любые данные + точно определенное количество памяти
// (но при этом лучше не работать с прямоугольными картинками и другим булшитом)
// есть довольно большой шанс по итогу столкнуться с дефрагментацией
// с другой стороны алгоритм кДеревьев помогает довольно сильно избежать таких ситуаций
// например при добавлении картинки можно иерархически проверить где больше всего занято
// и если маленькая картинка то туда в первую очередь добавить ресурс

namespace devils_engine {
namespace painter {

extern const uint32_t rgb24_format;
extern const uint32_t rgba32_format;

constexpr size_t default_arbitrary_image_container_slots = 4096;

class arbitrary_image_container : public image_container {
public:
  struct image_t {
    VmaAllocation allocation;
    VkImage handle;
    VkImageView view;
    VkSampler sampler;
    void* memory;
    uint32_t width, height, format;
    std::string name;

    inline image_t() noexcept : allocation(VK_NULL_HANDLE), handle(VK_NULL_HANDLE), view(VK_NULL_HANDLE), sampler(VK_NULL_HANDLE), memory(nullptr), width(0), height(0), format(0) {}
  };

  // VmaAllocator can be null
  arbitrary_image_container(std::string name, VkInstance instance, VkPhysicalDevice physics_device, VkDevice device, VmaAllocator allocator, const uint32_t initial_size = default_arbitrary_image_container_slots) noexcept;
  ~arbitrary_image_container() noexcept;

  void resize(const size_t new_size);

  bool is_exists(const uint32_t index) const override;
  uint32_t create(std::string name, const extent_t extent, const uint32_t format, VkSampler sampler) override;
  uint32_t create_any(std::string name, const extent_t extent, const uint32_t format, VkSampler sampler) override;
  void destroy(const uint32_t index) override;

  VkImage storage(const uint32_t index) const override;
  VkImageView view(const uint32_t index) const override;
  VkSampler sampler(const uint32_t index) const override;
  image_container::extent_t extent(const uint32_t index) const override;
  uint32_t format(const uint32_t index) const override;
  std::string_view name(const uint32_t index) const override;

  size_t capacity() const;
  size_t size() const;

  void clear();

  // наверное имеет смысл убрать под отдельный виртуальный класс
  void update_descriptor_set(VkDescriptorSet set, const uint32_t binding, const uint32_t first_element) const override;
  void change_layout(VkCommandBuffer buffer, const uint32_t index, const uint32_t old_layout, const uint32_t new_layout) const override;
  void change_layout_all(VkCommandBuffer buffer, const uint32_t old_layout, const uint32_t new_layout) const override;
  void copy_data(VkCommandBuffer buffer, VkImage image, const uint32_t index) const override;
  void blit_data(VkCommandBuffer buffer, const std::tuple<VkImage,uint32_t,uint32_t> &src_image, const uint32_t index, const uint32_t filter = 0) const override;
protected:
  VkDevice device;
  VmaAllocator allocator;

  bool is_owning_allocator;
  size_t _size;
  std::vector<image_t> images;
  image_t null_image;
};

class host_image_container final : public arbitrary_image_container {
public:
  host_image_container(std::string name, VkDevice device, VmaAllocator allocator);
  uint32_t create(std::string name, const extent_t extent, const uint32_t format, VkSampler sampler) override;
  void* mapped_memory(const uint32_t index) const;
};

}
}

#endif