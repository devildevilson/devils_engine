#ifndef DEVILS_ENGINE_PAINTER_TARGET_H
#define DEVILS_ENGINE_PAINTER_TARGET_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include "vulkan_minimal.h"
#include "pipeline_create_config.h"
#include "primitives.h"

// рендер таргет с точки зрения вулкана это набор ресурсов которые подключены в инпут атачмент
// потенциально таргетом может быть любая конфигурация из картинок
// что бы я хотел тут увидеть: минимальный набор структур для описания рендер таргета
// такой что мы потенциально можем его передать в создание рендер пасса
// и это покрое львиную долю того что нужно указать в рендер пассе
// во вторых необходимые данные для того чтобы пересоздать таргет при изменении окна
// в третьих сгенерить доп данные для шейдеров

// имеет смысл делать аттачмент глубины первым в списке, а потом все остальные
// либо первым либо последним

// что я вообще пытаюсь получить? я бы хотел вещь которую я могу более менее легко подменить в зависимости от использования
// ранее у меня были проблемы с вопросом откуда это все брать
// например у меня для 3й буферизации есть 3 командных буфера, 3 фреймбуфера, и далее
// это все по идее разные вещи которые могут не пересекаться
// имеет смысл например отделить таргет от команд
// таргет можно хранить просто где то около рендер пасса
// и так можно будет легко сделать например 2 паса друг за другом

namespace devils_engine {
namespace painter {

struct attachment {
  VmaAllocation alloc;
  VkImageView view;
  VkImage image;
  uint32_t format;

  inline attachment() noexcept : alloc(VK_NULL_HANDLE), view(VK_NULL_HANDLE), image(VK_NULL_HANDLE), format(0) {}
};

// у всех изображений для фреймбуфера КРАЙНЕ ЖЕЛАТЕЛЬНО один и тот же размер (по крайней мере он должен быть больше чем размер области рендеринга)
// сюрфейс откуда получим? наверное он сюда должен придти извне
struct attachments_container : public attachments_provider, public recreate_target {
  //VkInstance instance;
  VkDevice device;
  VmaAllocator allocator;
  const class frame_acquisitor* frame_acquisitor;
  //VkPhysicalDevice physical_device;
  //VkSurfaceKHR surface;
  std::vector<attachment_config_t> attachments_config;
  
  //VkSwapchainKHR swapchain;
  //uint32_t current_image;
  std::vector<std::vector<attachment>> attachments;

  // наверное создадим отдельный аллокатор для этого типа картинок
  //attachments_container(VkInstance instaince, VkDevice device, VkPhysicalDevice physical_device, VkSurfaceKHR surface, std::vector<attachment_config_t> attachments_config);
  //VkInstance instance, 
  attachments_container(VkDevice device, VmaAllocator allocator, const class frame_acquisitor* frame_acquisitor, std::vector<attachment_config_t> attachments_config);
  ~attachments_container() noexcept;

  //uint32_t max_frames() const;
  //size_t get_views(const uint32_t frame_index, VkImageView* views, const size_t max_size) const;
  void recreate(const uint32_t width, const uint32_t height) override;
  //uint32_t acquire_next_image(VkSemaphore semaphore, uint32_t &current_image);
  void clear();
  //void destroy_swapchain();

  //void recreate_swapchain(const uint32_t width, const uint32_t height);
  void recreate_images(const uint32_t width, const uint32_t height);

  size_t attachment_handles(const size_t buffering, VkImageView* views, const size_t max_size) const override;
};

// таким образом для атачментов нам реально нужно только указать форматы 
// + придумать какой то условный способ отслеживать текущее состояние для того чтобы правильно конфиг задать
}
}

#endif