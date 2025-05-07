#ifndef DEVILS_ENGINE_PAINTER_COMMON_STAGES_H
#define DEVILS_ENGINE_PAINTER_COMMON_STAGES_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include "primitives.h"

namespace devils_engine {
namespace painter {

class frame_acquisitor;

class memory_barrier : public sibling_stage {
public:
  memory_barrier(const uint32_t srcAccessMask, const uint32_t dstAccessMask, const uint32_t srcStageMask, const uint32_t dstStageMask) noexcept;

  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  uint32_t srcAccessMask; 
  uint32_t dstAccessMask; 
  uint32_t srcStageMask; 
  uint32_t dstStageMask;
};

class compute_sync : public memory_barrier {
public:
  compute_sync() noexcept;
};

class compute_to_graphics_sync : public memory_barrier {
public:
  compute_to_graphics_sync() noexcept;
};

class buffer_memory_barrier : public memory_barrier {
public:
  buffer_memory_barrier(const buffer_provider* provider, const uint32_t srcAccessMask, const uint32_t dstAccessMask, const uint32_t srcStageMask, const uint32_t dstStageMask) noexcept;
  void process(VkCommandBuffer buffer) override;
protected:
  const buffer_provider* provider;
};

// так и нам потребуется парочка специальных типов
class storage_buffer_sync : public buffer_memory_barrier {
public:
  storage_buffer_sync(const buffer_provider* provider) noexcept;
};

class storage_buffer_to_graphics : public buffer_memory_barrier {
public:
  storage_buffer_to_graphics(const buffer_provider* provider) noexcept;
};

class indirect_buffer_to_graphics : public buffer_memory_barrier {
public:
  indirect_buffer_to_graphics(const buffer_provider* provider) noexcept;
};

class change_image_layout : public sibling_stage {
public:
  change_image_layout(VkImage img, const uint32_t old_layout, const uint32_t new_layout);
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  VkImage img;
  uint32_t old_layout;
  uint32_t new_layout;
};

class change_frame_image_layout : public change_image_layout {
public:
  change_frame_image_layout(const frame_acquisitor* frm, const uint32_t old_layout, const uint32_t new_layout);
  void process(VkCommandBuffer buffer) override;
protected:
  const frame_acquisitor* frm;
};

class set_event : public sibling_stage {
public:
  set_event(VkEvent event, const uint32_t stage_flags) noexcept;
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  VkEvent event;
  uint32_t stage_flags;
};

class reset_event : public sibling_stage {
public:
  reset_event(VkEvent event, const uint32_t stage_flags) noexcept;
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  VkEvent event;
  uint32_t stage_flags;
};

class pipeline_view : public sibling_stage {
public:
  pipeline_view(const pipeline_provider* provider) noexcept;

  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  const pipeline_provider* provider;
};

class bind_descriptor_sets : public sibling_stage {
public:
  bind_descriptor_sets(const pipeline_layout_provider* provider, const uint32_t first_set, std::vector<VkDescriptorSet> sets) noexcept;
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  const pipeline_layout_provider* provider;
  uint32_t first_set;
  std::vector<VkDescriptorSet> sets;
};

// задаем тут динамический оффсет в том плане что не нужно отдельный дескриптор сет задавать
// так мы можем обновлять буфер не боясь что мы чего то там перезапишем во время выполнения
class bind_dynamic_descriptor_set : public sibling_stage {
public:
  bind_dynamic_descriptor_set(const pipeline_layout_provider* provider, const uint32_t first_set, VkDescriptorSet set, const uint32_t offset) noexcept;
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  const pipeline_layout_provider* provider;
  uint32_t first_set;
  uint32_t offset;
  VkDescriptorSet set;
};

// буферы можем тоже прибиндить сразу все, не нужно их перебиндить
// в будущем потребуется указать еще и офсет, наверное нужно будет сделать массив буфер провайдеров
class bind_vertex_buffers : public sibling_stage {
public:
  bind_vertex_buffers(const uint32_t first_buffer, std::vector<VkBuffer> buffers, std::vector<size_t> offsets) noexcept;
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  uint32_t first_buffer;
  std::vector<VkBuffer> buffers;
  std::vector<size_t> offsets;
};

class bind_index_buffer : public sibling_stage {
public:
  bind_index_buffer(VkBuffer index, const size_t offset) noexcept;
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  VkBuffer index;
  size_t offset;
};

class draw : public sibling_stage {
public:
  draw(const vertex_draw_provider* provider) noexcept;
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  const vertex_draw_provider* provider;
};

class indexed_draw : public sibling_stage {
public:
  indexed_draw(const indexed_draw_provider* provider) noexcept;
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  const indexed_draw_provider* provider;
};

class draw_indirect : public sibling_stage {
public:
  draw_indirect(VkBuffer indirect, const size_t offset) noexcept;
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  VkBuffer indirect;
  size_t offset;
};

class indexed_draw_indirect : public sibling_stage {
public:
  indexed_draw_indirect(VkBuffer indirect, const size_t offset) noexcept;
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  VkBuffer indirect;
  size_t offset;
};

}
}

#endif