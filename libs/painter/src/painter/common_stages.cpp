#include "common_stages.h"

#include "vulkan_header.h"

#undef MemoryBarrier

namespace devils_engine {
namespace painter {
memory_barrier::memory_barrier(const uint32_t srcAccessMask, const uint32_t dstAccessMask, const uint32_t srcStageMask, const uint32_t dstStageMask) noexcept :
  srcAccessMask(srcAccessMask), dstAccessMask(dstAccessMask), srcStageMask(srcStageMask), dstStageMask(dstStageMask)
{}

void memory_barrier::begin() {}
void memory_barrier::process(VkCommandBuffer buffer) {
  vk::CommandBuffer b(buffer);

  vk::MemoryBarrier bar(static_cast<vk::AccessFlags>(srcAccessMask), static_cast<vk::AccessFlags>(dstAccessMask));
  b.pipelineBarrier(static_cast<vk::PipelineStageFlags>(srcStageMask), static_cast<vk::PipelineStageFlags>(dstStageMask), vk::DependencyFlagBits::eByRegion, bar, nullptr, nullptr);
}

void memory_barrier::clear() {}

compute_sync::compute_sync() noexcept : memory_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) {}
compute_to_graphics_sync::compute_to_graphics_sync() noexcept : memory_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT) {}

buffer_memory_barrier::buffer_memory_barrier(const buffer_provider* provider, const uint32_t srcAccessMask, const uint32_t dstAccessMask, const uint32_t srcStageMask, const uint32_t dstStageMask) noexcept :
  memory_barrier(srcAccessMask, dstAccessMask, srcStageMask, dstStageMask), provider(provider)
{}

void buffer_memory_barrier::process(VkCommandBuffer buffer) {
  if (provider->buffer == VK_NULL_HANDLE) return;

  vk::CommandBuffer b(buffer);
  vk::BufferMemoryBarrier bar(static_cast<vk::AccessFlags>(srcAccessMask), static_cast<vk::AccessFlags>(dstAccessMask), VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, provider->buffer, provider->offset, provider->size);
  b.pipelineBarrier(static_cast<vk::PipelineStageFlags>(srcStageMask), static_cast<vk::PipelineStageFlags>(dstStageMask), vk::DependencyFlagBits::eByRegion, nullptr, bar, nullptr);
}

storage_buffer_sync::storage_buffer_sync(const buffer_provider* provider) noexcept : buffer_memory_barrier(provider, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) {}
storage_buffer_to_graphics::storage_buffer_to_graphics(const buffer_provider* provider) noexcept : buffer_memory_barrier(provider, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) {}
indirect_buffer_to_graphics::indirect_buffer_to_graphics(const buffer_provider* provider) noexcept : buffer_memory_barrier(provider, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT) {}

change_image_layout::change_image_layout(VkImage img, const uint32_t old_layout, const uint32_t new_layout) :
  img(img), old_layout(old_layout), new_layout(new_layout)
{}

void change_image_layout::begin() {}
void change_image_layout::process(VkCommandBuffer buffer) {
  vk::ImageSubresourceRange isr(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
  const auto &[bar, ss, ds] = make_image_memory_barrier(img, vk::ImageLayout(old_layout), vk::ImageLayout(new_layout), isr);
  vk::CommandBuffer(buffer).pipelineBarrier(ss, ds, vk::DependencyFlagBits::eByRegion, nullptr, nullptr, bar);
}
void change_image_layout::clear() {}

change_frame_image_layout::change_frame_image_layout(const frame_acquisitor* frm, const uint32_t old_layout, const uint32_t new_layout) : 
  change_image_layout(VK_NULL_HANDLE, old_layout, new_layout), frm(frm)
{}
void change_frame_image_layout::process(VkCommandBuffer buffer) {
  vk::ImageSubresourceRange isr(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
  const auto &[bar, ss, ds] = make_image_memory_barrier(frm->frame_storage(frm->current_image_index), vk::ImageLayout(old_layout), vk::ImageLayout(new_layout), isr);
  vk::CommandBuffer(buffer).pipelineBarrier(ss, ds, vk::DependencyFlagBits::eByRegion, nullptr, nullptr, bar);
}

set_event::set_event(VkEvent event, const uint32_t stage_flags) noexcept : event(event), stage_flags(stage_flags) {}
void set_event::begin() {}
void set_event::process(VkCommandBuffer buffer) {
  vk::CommandBuffer(buffer).setEvent(event, vk::PipelineStageFlags(stage_flags));
}
void set_event::clear() {}

reset_event::reset_event(VkEvent event, const uint32_t stage_flags) noexcept : event(event), stage_flags(stage_flags) {}
void reset_event::begin() {}
void reset_event::process(VkCommandBuffer buffer) {
  vk::CommandBuffer(buffer).resetEvent(event, vk::PipelineStageFlags(stage_flags));
}
void reset_event::clear() {}

pipeline_view::pipeline_view(const pipeline_provider* provider) noexcept : provider(provider) {}
void pipeline_view::begin() {}
void pipeline_view::process(VkCommandBuffer buffer) { 
  vk::CommandBuffer b(buffer); 
  const auto point = static_cast<vk::PipelineBindPoint>(provider->pipeline_bind_point);
  b.bindPipeline(point, provider->pipeline);
}
void pipeline_view::clear() {}

bind_descriptor_sets::bind_descriptor_sets(const pipeline_layout_provider* provider, const uint32_t first_set, std::vector<VkDescriptorSet> sets) noexcept : provider(provider), first_set(first_set), sets(std::move(sets)) {}
void bind_descriptor_sets::begin() {}
void bind_descriptor_sets::process(VkCommandBuffer buffer) {
  vk::CommandBuffer b(buffer); 
  const auto point = static_cast<vk::PipelineBindPoint>(provider->pipeline_bind_point);
  b.bindDescriptorSets(point, provider->pipeline_layout, first_set, sets.size(), (vk::DescriptorSet*)sets.data(), 0, nullptr);
}
void bind_descriptor_sets::clear() {}

bind_dynamic_descriptor_set::bind_dynamic_descriptor_set(const pipeline_layout_provider* provider, const uint32_t first_set, VkDescriptorSet set, const uint32_t offset) noexcept :
  provider(provider), first_set(first_set), offset(offset), set(set)
{}

void bind_dynamic_descriptor_set::begin() {}
void bind_dynamic_descriptor_set::process(VkCommandBuffer buffer) {
  vk::CommandBuffer b(buffer);
  const auto point = static_cast<vk::PipelineBindPoint>(provider->pipeline_bind_point);
  vk::DescriptorSet d(set);
  b.bindDescriptorSets(point, provider->pipeline_layout, first_set, d, offset);
}
void bind_dynamic_descriptor_set::clear() {}

// буферы можно прибиндить все с самого начала
// предпочтительно
bind_vertex_buffers::bind_vertex_buffers(const uint32_t first_buffer, std::vector<VkBuffer> buffers, std::vector<size_t> offsets) noexcept : 
  first_buffer(first_buffer), buffers(std::move(buffers)), offsets(std::move(offsets)) 
{
  if (this->offsets.size() < this->buffers.size()) utils::error("Offsets count {} must be at least not less than count of buffers {}",  offsets.size(), buffers.size());
}

void bind_vertex_buffers::begin() {}
void bind_vertex_buffers::process(VkCommandBuffer buffer) {
  vk::CommandBuffer b(buffer);
  b.bindVertexBuffers(first_buffer, buffers.size(), (vk::Buffer*)buffers.data(), offsets.data());
}
void bind_vertex_buffers::clear() {}

bind_index_buffer::bind_index_buffer(VkBuffer index, const size_t offset) noexcept : index(index), offset(offset) {}
void bind_index_buffer::begin() {}
void bind_index_buffer::process(VkCommandBuffer buffer) {
  vk::CommandBuffer b(buffer);
  b.bindIndexBuffer(index, offset, vk::IndexType::eUint32);
}
void bind_index_buffer::clear() {}

draw::draw(const vertex_draw_provider* provider) noexcept : provider(provider) {}
void draw::begin() {}
void draw::process(VkCommandBuffer buffer) {
  vk::CommandBuffer b(buffer);
  b.draw(provider->vertex_count, provider->instance_count, provider->first_vertex, provider->first_instance);
}
void draw::clear() {}

indexed_draw::indexed_draw(const indexed_draw_provider* provider) noexcept : provider(provider) {}
void indexed_draw::begin() {}
void indexed_draw::process(VkCommandBuffer buffer) {
  vk::CommandBuffer b(buffer);
  b.drawIndexed(provider->index_count, provider->instance_count, provider->first_index, provider->vertex_offset, provider->first_instance);
}
void indexed_draw::clear() {}

draw_indirect::draw_indirect(VkBuffer indirect, const size_t offset) noexcept : indirect(indirect), offset(offset) {}
void draw_indirect::begin() {}
void draw_indirect::process(VkCommandBuffer buffer) {
  vk::CommandBuffer b(buffer);
  b.drawIndirect(indirect, offset, 1, sizeof(vk::DrawIndirectCommand));
}
void draw_indirect::clear() {}

indexed_draw_indirect::indexed_draw_indirect(VkBuffer indirect, const size_t offset) noexcept : indirect(indirect), offset(offset) {}
void indexed_draw_indirect::begin() {}
void indexed_draw_indirect::process(VkCommandBuffer buffer) {
  vk::CommandBuffer b(buffer);
  b.drawIndexedIndirect(indirect, offset, 1, sizeof(vk::DrawIndexedIndirectCommand));
}
void indexed_draw_indirect::clear() {}

}
}