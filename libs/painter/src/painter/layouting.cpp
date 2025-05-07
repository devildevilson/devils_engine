#include "layouting.h"

#include "vulkan_header.h"
#include "makers.h"

namespace devils_engine {
namespace painter {

layouting::layouting(VkDevice device, const create_info &info, const descriptor_set_layouts_config_t* ds_configs, const pipeline_layouts_t* pl_configs) : 
  device(device) 
{
  {
    sampler_maker sm(device);
    immutable_linear = sm.addressMode(vk::SamplerAddressMode::eRepeat, vk::SamplerAddressMode::eRepeat)
                         .anisotropy(VK_TRUE, 16)
                         .borderColor(vk::BorderColor::eFloatTransparentBlack)
                         .compareOp(VK_FALSE, vk::CompareOp::eNever)
                         .filter(vk::Filter::eLinear, vk::Filter::eLinear)
                         .lod(0.0f, 0.0f)
                         .mipmapMode(vk::SamplerMipmapMode::eLinear)
                         .create("linear_sampler");

    immutable_nearest = sm.addressMode(vk::SamplerAddressMode::eRepeat, vk::SamplerAddressMode::eRepeat)
                          .anisotropy(VK_TRUE, 16)
                          .borderColor(vk::BorderColor::eFloatTransparentBlack)
                          //.compareOp(VK_FALSE, vk::CompareOp::eNever)
                          .filter(vk::Filter::eNearest, vk::Filter::eNearest)
                          //.lod(0.0f, 0.0f)
                          .mipmapMode(vk::SamplerMipmapMode::eNearest)
                          .create("linear_sampler");
  }

  {
    descriptor_pool_maker dpm(device);
    pool = dpm.poolSize(vk::DescriptorType::eStorageBuffer, info.readonly_storage_buffers_count * 2 + info.storage_buffers_count * 2)
              .poolSize(vk::DescriptorType::eUniformBuffer, 10)
              .poolSize(vk::DescriptorType::eStorageImage, info.storage_images_count * 2)
              .poolSize(vk::DescriptorType::eCombinedImageSampler, info.combined_image_samplers_count * 2)
              .poolSize(vk::DescriptorType::eInputAttachment, info.input_attachments_count * 2)
              .create("default_descriptor_pool");
  }

  {
    for (const auto &[name, conf] : *ds_configs) {
      descriptor_set_layout_maker dslm(device);
      for (size_t i = 0; i < conf.size(); ++i) {
        const auto type = vk::DescriptorType(conf[i].type);
        const auto stages = vk::ShaderStageFlags(conf[i].shader_stages);

        // как каунт забрать? как то вот так?
        uint32_t count = conf[i].count;
        if (count == 0) {
          if (type == vk::DescriptorType::eCombinedImageSampler) count = info.combined_image_samplers_count;
          else if (type == vk::DescriptorType::eStorageBuffer) count = info.readonly_storage_buffers_count;
          else if (type == vk::DescriptorType::eInputAttachment) count = info.input_attachments_count;
          else if (type == vk::DescriptorType::eStorageImage) count = info.storage_images_count;
        }
        dslm.binding(i, type, stages, count);
      }
      auto l = dslm.create(name);
      if (set_layouts.find(name) != set_layouts.end()) utils::error("Descriptor set layout with name '{}' is already exists", name);
      set_layouts[name] = l;
    }
  }
  
  /*{
    std::vector<vk::Sampler> combined(info.combined_image_samplers_count, immutable_linear);
    descriptor_set_layout_maker dslm(device);
    set_layout = dslm.binding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAll, 1)
                     .binding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eAll, info.readonly_storage_buffers_count)
                     .combined(2, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment, combined)
                     .binding(3, vk::DescriptorType::eInputAttachment, vk::ShaderStageFlagBits::eFragment, info.input_attachments_count)
                     .binding(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eAll, info.storage_buffers_count)
                     .binding(5, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eAll, info.storage_images_count)
                     .create("default_layout");
  }*/

  {
    for (const auto &[name, conf] : *pl_configs) {
      pipeline_layout_maker plm(device);
      for (const auto &l_name : conf.set_layouts) {
        const auto itr = set_layouts.find(l_name);
        if (itr == set_layouts.end()) utils::error("Could not find descriptor set layout with name '{}' for pipeline layout '{}'", l_name, name);
        plm.addDescriptorLayout(itr->second);
      }
      if (conf.push_constant_size != 0) plm.addPushConstRange(0, conf.push_constant_size);
      auto l = plm.create(name);
      if (pipe_layouts.find(name) != pipe_layouts.end()) utils::error("Pipeline layout with name '{}' is already exists", name);
      pipe_layouts[name] = l;
    }
  }

  /*{
    pipeline_layout_maker plm(device);
    pipeline_layout = plm.addDescriptorLayout(set_layout).addPushConstRange(0, 128).create("default_pipeline_layout");
  }*/

  /*{
    descriptor_set_maker dsm(device);
    set = dsm.layout(set_layout).create(pool, "main_descriptor_set")[0];
  }*/
}

layouting::~layouting() noexcept {
  vk::Device d(device);

  for (const auto &[name, l] : pipe_layouts) {
    d.destroy(l);
  }

  for (const auto &[name, l] : set_layouts) {
    d.destroy(l);
  }

  //d.destroy(pipeline_layout);
  //d.destroy(set_layout);
  d.destroy(pool);
  d.destroy(immutable_nearest);
  d.destroy(immutable_linear);
}

VkDescriptorSet layouting::create_descriptor_set(const std::string_view &layout_name, const std::string &set_name) const {
  auto layout = find_descriptor_set_layout(layout_name);
  if (layout == VK_NULL_HANDLE) utils::error("Could not create descriptor set: descriptor set layout '{}' not found", layout_name);
  descriptor_set_maker dsm(device);
  auto set = dsm.layout(layout).create(pool, set_name)[0];
  return set;
}

VkDescriptorSetLayout layouting::find_descriptor_set_layout(const std::string_view &layout_name) const {
  const auto itr = set_layouts.find(layout_name);
  if (itr == set_layouts.end()) return VK_NULL_HANDLE;
  return itr->second;
}

VkPipelineLayout layouting::find_pipeline_layout(const std::string_view &layout_name) const {
  const auto itr = pipe_layouts.find(layout_name);
  if (itr == pipe_layouts.end()) return VK_NULL_HANDLE;
  return itr->second;
}

}
}