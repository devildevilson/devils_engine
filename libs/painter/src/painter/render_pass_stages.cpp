#include "render_pass_stages.h"

#include "vulkan_header.h"
#include "makers.h"
#include "attachments_container.h"
#include "auxiliary.h"

namespace devils_engine {
namespace painter {
static vk::ImageLayout read_only_optimal(const uint32_t format) {
  return format_is_depth_or_stencil(format) ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eShaderReadOnlyOptimal;
}

static vk::ImageLayout layout_from_type_and_format(const subpass_attachment_type type, const uint32_t format, const vk::ImageLayout prev) {
  if (type == subpass_attachment_type::intended && format_is_depth_or_stencil(format)) {
    return vk::ImageLayout::eDepthStencilAttachmentOptimal;
  } else if (type == subpass_attachment_type::intended && format_is_color(format)) {
    return vk::ImageLayout::eColorAttachmentOptimal;
  } else if (type == subpass_attachment_type::input) {
    return read_only_optimal(format);
  } else if (type == subpass_attachment_type::preserve) {
    return prev;
  } else if (type == subpass_attachment_type::sampled) {
    return read_only_optimal(format);
  } else if (type == subpass_attachment_type::storage) {
    return vk::ImageLayout::eGeneral;
  }

  return prev;
}

static uint32_t combine_access_masks(const subpass_attachment_type type, const uint32_t format, const uint32_t prev_mask) {
  uint32_t mask = prev_mask;
  if (type == subpass_attachment_type::intended && format_is_depth_or_stencil(format)) {
    mask = mask | static_cast<uint32_t>(vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
  } else if (type == subpass_attachment_type::intended && format_is_color(format)) {
    mask = mask | static_cast<uint32_t>(vk::AccessFlagBits2::eColorAttachmentWrite);
  } else if (type == subpass_attachment_type::input) {
    if (format_is_depth_or_stencil(format)) {
      mask = mask | static_cast<uint32_t>(vk::AccessFlagBits2::eDepthStencilAttachmentRead);
    } else {
      mask = mask | static_cast<uint32_t>(vk::AccessFlagBits2::eColorAttachmentRead);
    }
  } else if (type == subpass_attachment_type::preserve) {
    mask = mask | 0; // не трогаем?
  } else if (type == subpass_attachment_type::sampled) {
    mask = mask | static_cast<uint32_t>(vk::AccessFlagBits2::eShaderSampledRead);
  } else if (type == subpass_attachment_type::storage) {
    mask = mask | static_cast<uint32_t>(vk::AccessFlagBits2::eShaderStorageRead) | static_cast<uint32_t>(vk::AccessFlagBits2::eShaderStorageWrite);
  }
  
  return mask;
}

static vk::ClearValue make_clear_value(const subpass_attachment_type type, const uint32_t format) {
  return format_is_depth_or_stencil(format) ? vk::ClearValue(vk::ClearDepthStencilValue()) : vk::ClearValue(vk::ClearColorValue(0.0f, 0.0f, 0.0f, 0.0f));
}

//render_pass_main::render_pass_main(VkDevice device, attachments_container* container, const render_pass_data_t &data) :
//  name(data.name), device(device), container(container), framebuffers{VK_NULL_HANDLE}, childs(nullptr)
//{
//  render_pass_maker rpm(device);
//
//  for (const auto &att : container->attachments[0]) {
//    rpm.attachmentBegin(static_cast<vk::Format>(att.format));
//    rpm.attachmentInitialLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
//    rpm.attachmentFinalLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
//    rpm.attachmentLoadOp(vk::AttachmentLoadOp::eLoad); // если это первый рендер пасс, то нам бы его чистить
//    rpm.attachmentStoreOp(vk::AttachmentStoreOp::eStore);
//  }
//
//  std::vector<std::tuple<uint32_t, uint32_t>> access_masks(data.subpasses.size(), std::make_tuple(0,0));
//  std::vector<std::tuple<uint32_t, uint32_t>> stage_masks(data.subpasses.size(), std::make_tuple(0,0));
//
//  for (size_t s = 0; s < data.subpasses.size()-1; ++s) {
//    auto &subpass_data = data.subpasses[s];
//    auto [src_access, dst_access] = access_masks[s];
//    auto [src_stage, dst_stage] = stage_masks[s];
//
//    if (s == 0) {
//      src_access = static_cast<uint32_t>(vk::AccessFlagBits::eNone);
//      src_stage = static_cast<uint32_t>(vk::PipelineStageFlagBits2::eBottomOfPipe);
//    } else {
//      auto [prev_src_access, prev_dst_access] = access_masks[s-1];
//      auto [prev_src_stage, prev_dst_stage] = stage_masks[s-1];
//      src_access = prev_dst_access;
//      src_stage = prev_dst_stage;
//    }
//
//    for (size_t i = 0; i < subpass_data.attachments.size(); ++i) {
//      const auto type = subpass_data.attachments[i].type;
//      const auto format = container->attachments[0][i].format;
//
//      rpm.subpassBegin();
//      if (type == subpass_attachment_type::intended && format_is_depth_or_stencil(format)) {
//        rpm.subpassDepthStencilAttachment(i, vk::ImageLayout::eDepthStencilAttachmentOptimal);
//      } else if (type == subpass_attachment_type::intended && format_is_color(format)) {
//        rpm.subpassColorAttachment(i, vk::ImageLayout::eColorAttachmentOptimal);
//      } else if (type == subpass_attachment_type::input) {
//        rpm.subpassInputAttachment(i, read_only_optimal(format));
//      } else if (type == subpass_attachment_type::preserve) {
//        rpm.addPreservedAttachmentIndex(i);
//      } else if (type == subpass_attachment_type::sampled) {
//        rpm.subpassInputAttachment(i, read_only_optimal(format));
//      } else if (type == subpass_attachment_type::storage) {
//        rpm.subpassInputAttachment(i, vk::ImageLayout::eGeneral);
//      }
//
//      dst_access = combine_access_masks(type, format, dst_access);
//    }
//
//    dst_stage = static_cast<uint32_t>(vk::PipelineStageFlagBits2::eAllGraphics);
//
//    access_masks[s] = std::make_tuple(src_access, dst_access);
//    stage_masks[s] = std::make_tuple(src_stage, dst_stage);
//  }
//
//  {
//    auto [prev_src_access, prev_dst_access] = access_masks[access_masks.size()-2];
//    auto [prev_src_stage, prev_dst_stage] = stage_masks[stage_masks.size()-2];
//    const uint32_t src_access = prev_dst_access;
//    const uint32_t src_stage = prev_dst_stage;
//    uint32_t dst_access = 0;
//    uint32_t dst_stage = static_cast<uint32_t>(vk::PipelineStageFlagBits2::eAllGraphics);
//
//    const auto &arr = data.subpasses.back().attachments;
//    for (size_t i = 0; i < arr.size(); ++i) {
//      const auto type = arr[i].type;
//      const auto format = container->attachments[0][i].format;
//      dst_access = combine_access_masks(type, format, dst_access);
//    }
//
//    access_masks.back() = std::make_tuple(src_access, dst_access);
//    stage_masks.back() = std::make_tuple(src_stage, dst_stage);
//  }
//
//  for (size_t s = 0; s < data.subpasses.size(); ++s) {
//    //auto &arr = data.subpasses[s].attachments;
//    auto [src_access, dst_access] = access_masks[s];
//    auto [src_stage, dst_stage] = stage_masks[s];
//
//    const auto prev_subpass = s == 0 ? VK_SUBPASS_EXTERNAL : s-1;
//    const auto next_subpass = s == data.subpasses.size()-1 ? VK_SUBPASS_EXTERNAL : s-1;
//    rpm.dependencyBegin(prev_subpass, next_subpass);
//    rpm.dependencyDependencyFlags(vk::DependencyFlagBits::eByRegion);
//    rpm.dependencySrcAccessMask(static_cast<vk::AccessFlags>(src_access));
//    rpm.dependencySrcStageMask(static_cast<vk::PipelineStageFlags>(src_stage));
//    rpm.dependencyDstAccessMask(static_cast<vk::AccessFlags>(dst_access));
//    rpm.dependencyDstStageMask(static_cast<vk::PipelineStageFlags>(dst_stage));
//  }
//
//  render_pass = rpm.create("render_pass_main");
//
//  memset(framebuffers, 0, sizeof(VkFramebuffer) * 4);
//
//  recreate(container->width, container->height);
//}

render_pass_main::render_pass_main(VkDevice device, const framebuffer_provider* provider) :
  device(device), provider(provider)
{}

render_pass_main::~render_pass_main() noexcept {}

void render_pass_main::begin() {
  for (auto p = childs; p != nullptr; p = p->next()) { p->begin(); }
}

void render_pass_main::process(VkCommandBuffer buffer) {
  const vk::Rect2D area(vk::Offset2D(0, 0), vk::Extent2D(provider->attachments_provider->width, provider->attachments_provider->height));
  const size_t attacments_count = provider->attachments_provider->attachments_count;
  vk::ClearValue v[8];
  for (size_t j = 0; j < attacments_count; ++j) {
    const auto format = provider->attachments_provider->attachments[j].format;
    v[j] = make_clear_value({}, format);
  }

  const auto buf = provider->current_framebuffer();
  const auto rp = provider->render_pass_provider->render_pass;
  const vk::RenderPassBeginInfo rpbi(rp, buf, area, attacments_count, v);

  const vk::Viewport a(area.offset.x, area.offset.y, area.extent.width, area.extent.height, 0.0f, 1.0f);

  vk::CommandBuffer b(buffer);
  b.beginRenderPass(rpbi, vk::SubpassContents::eInline);
  b.setViewport(0, a);
  b.setScissor(0, area);

  for (auto p = childs; p != nullptr; p = p->next()) { p->process(buffer); }

  b.endRenderPass();
}

void render_pass_main::clear() {
  for (auto p = childs; p != nullptr; p = p->next()) { p->clear(); }
}

void next_subpass::begin() {}
void next_subpass::process(VkCommandBuffer buffer) {
  vk::CommandBuffer b(buffer);
  b.nextSubpass(vk::SubpassContents::eInline);
}

void next_subpass::clear() {}

}
}
