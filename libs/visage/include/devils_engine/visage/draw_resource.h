#ifndef DEVILS_ENGINE_VISAGE_DRAW_RESOURCE_H
#define DEVILS_ENGINE_VISAGE_DRAW_RESOURCE_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include "devils_engine/painter/vulkan_minimal.h"
#include "painter/buffer_resources.h"
#include "devils_engine/utils/stack_allocator.h"

struct nk_context;
struct nk_buffer;

// еще до всей этой шняги нужно сделать пайплайн

namespace devils_engine {
namespace visage {
struct interface_piece_t {
  uint32_t count;
  struct { uint32_t x,y,w,h; } rect;
  uint32_t texture_id;
  uint32_t userdata_id;

  inline interface_piece_t() noexcept : count(0), rect{0,0,0,0}, texture_id(0), userdata_id(0) {}
};

struct interface_provider {
  std::vector<interface_piece_t> cmds;
  VkBuffer index;
  VkBuffer vertex;
  VkDescriptorSet set;
  uint32_t set_slot;

  inline interface_provider() noexcept : index(VK_NULL_HANDLE), vertex(VK_NULL_HANDLE), set(VK_NULL_HANDLE), set_slot(0) {}
};

// где то тут мы должны запустить конверт
// где копировать? после препайр нужно что то где то скопировать или просто прямо там скопировать?
// желательно прикрутить копирование прямо в общий пайп
class draw_resource : public interface_provider, public painter::arbitrary_data {
public:
  draw_resource(VkDevice device, VmaAllocator allocator, VkDescriptorSet set, nk_context* ctx, nk_buffer* cmds);
  ~draw_resource() noexcept;

  void prepare(const uint32_t width, const uint32_t height); // window width and window height

  template <typename T, typename... Args>
  T* create(Args&&... args) { return frame_allocator.create<T>(std::forward<Args>(args)...); }
private:
  VkDevice device;
  VmaAllocator allocator;
  VkDescriptorSet set;
  nk_context* ctx;
  nk_buffer* cmds;

  painter::host_buffer index_host;
  painter::host_buffer vertex_host;
  painter::host_buffer storage_host;
  painter::host_buffer uniform_host;

  // в эти буферы как раз надо будет скопировать где то в рендеринге
  painter::index_buffer index_gpu;
  painter::vertex_buffer vertex_gpu;
  painter::storage_buffer storage_gpu;
  painter::uniform_buffer uniform_gpu;

  utils::stack_allocator frame_allocator;
};
}
}

#endif