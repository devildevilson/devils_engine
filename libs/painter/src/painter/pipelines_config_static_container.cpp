#include "pipeline_create_config.h"
#include "pipelines_config_static_container.h"

#include "vulkan_header.h"

#include <gtl/phmap.hpp>

namespace devils_engine {
namespace painter {

using gp_b_t = graphics_pipeline_create_config::binding_t;
using gp_b_a_t = graphics_pipeline_create_config::binding_t::attribute_t;

struct vec4_size { float x, y, z, w; };

const gtl::flat_hash_map<std::string, graphics_pipeline_create_config> default_graphics_pipeline_configs = {
  {
    std::make_pair("default",
    graphics_pipeline_create_config{
      "default",
      { { "vertex", "shaders/simple1", "main", {} }, { "fragment", "shaders/simple2", "main", {} } },
      {},

      { gp_b_t{ 0, sizeof(vec4_size), "vertex", { gp_b_a_t{ 0, 0, uint32_t(vk::Format::eR32G32B32A32Sfloat), 0 } } } },

      uint32_t(vk::PrimitiveTopology::eTriangleList), false,

      false, false, 
      uint32_t(vk::PolygonMode::eFill), uint32_t(vk::CullModeFlagBits::eNone), uint32_t(vk::FrontFace::eCounterClockwise),
      { false, 0.0f, 0.0f, 0.0f },
      1.0f,

      uint32_t(vk::SampleCountFlagBits::e1),
      { false, 1.0f, {} },
      { false, false },

      false, false, uint32_t(vk::CompareOp::eLess),
      { false, { 0, 0, 0, 0, 0, 0, 0 } },
      { false, 0.0f, 1.0f },

      { { false, 0 }, {0} }
    }),
  }
};

const gtl::flat_hash_map<std::string, compute_pipeline_create_config> default_compute_pipeline_configs = {
  std::make_pair("default",
    compute_pipeline_create_config{
      "default",
      { "compute", "somethingsomething", "main", {} }
    }  
  )
};

const auto default_color_component_flag = uint32_t(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
const gtl::flat_hash_map<std::string, render_pass_data_t> default_render_pass_configs = {
  std::make_pair(
    "default",
    render_pass_data_t{
      "default",
      {
        attachment_description_t{
          "undefined",
          "present",
          "clear",
          "keep",
          "dont_care",
          "dont_care",
        }
      },
      {
        subpass_data_t{
        "main",
        {subpass_data_t::attachment{
          subpass_attachment_type::intended,
          {
            false, uint32_t(vk::BlendFactor::eSrcColor), uint32_t(vk::BlendFactor::eDstColor), uint32_t(vk::BlendOp::eAdd),
                   uint32_t(vk::BlendFactor::eSrcAlpha), uint32_t(vk::BlendFactor::eDstAlpha), uint32_t(vk::BlendOp::eAdd),
            default_color_component_flag
          }
        }}},
        subpass_data_t{
        "external",
        {subpass_data_t::attachment{
          subpass_attachment_type::intended,
          {
            false, uint32_t(vk::BlendFactor::eSrcColor), uint32_t(vk::BlendFactor::eDstColor), uint32_t(vk::BlendOp::eAdd),
                   uint32_t(vk::BlendFactor::eSrcAlpha), uint32_t(vk::BlendFactor::eDstAlpha), uint32_t(vk::BlendOp::eAdd),
            default_color_component_flag
          }
        }}}
      }
    }
  ),
};

// капец видимо придется блитить в этот ебучий свопчеин конечное изображение
// не работает к сожалению (для мутабл формат нужны расширения, а потом этот формат используется в миллиарде мест)
// uint32_t(vk::Format::eR8G8B8A8Unorm)
const gtl::flat_hash_map<std::string, std::vector<attachment_config_t>> default_attachments_configs = {
  std::make_pair(
    "default", std::vector{ attachment_config_t{ "swapchain", uint32_t(vk::Format::eB8G8R8A8Unorm) } }
  )
};

const gtl::flat_hash_map<std::string, sampler_config_t> default_samplers_configs = {
  std::make_pair(
    "default",
    sampler_config_t{
      "default",
      "nearest","nearest",
      uint32_t(vk::SamplerMipmapMode::eNearest),
      uint32_t(vk::SamplerAddressMode::eRepeat), uint32_t(vk::SamplerAddressMode::eRepeat), uint32_t(vk::SamplerAddressMode::eRepeat),
      { true, 16.0f },
      { false, 0 },
      { 0.0f, 0.0f, 0.0f },
      uint32_t(vk::BorderColor::eFloatOpaqueBlack),
      false
    }
  )
};

const descriptor_set_layouts_config_t default_descriptor_set_layouts_config = {
  std::make_pair(
    "uniform_data",
    std::vector{
      descriptor_set_layout_binding_t{
        uint32_t(vk::DescriptorType::eUniformBuffer),
        uint32_t(vk::ShaderStageFlagBits::eAll),
        1
      },
      descriptor_set_layout_binding_t{
        uint32_t(vk::DescriptorType::eCombinedImageSampler),
        uint32_t(vk::ShaderStageFlagBits::eAll),
        0 // implementation defined
      },
    }
  ),
  std::make_pair(
    "interface_data",
    std::vector{
      descriptor_set_layout_binding_t{
        uint32_t(vk::DescriptorType::eUniformBuffer),
        uint32_t(vk::ShaderStageFlagBits::eVertex),
        1
      },
      descriptor_set_layout_binding_t{
        uint32_t(vk::DescriptorType::eStorageBuffer),
        uint32_t(vk::ShaderStageFlagBits::eVertex),
        1
      },
    }
  ),
  std::make_pair(
    "game_data",
    std::vector{
      descriptor_set_layout_binding_t{
        uint32_t(vk::DescriptorType::eStorageBuffer),
        uint32_t(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eCompute),
        0 // как лучше сделать? по биндингу на каждый, или все буферы в одном?
      }
    }
  )
};

const pipeline_layouts_t default_pipeline_layouts_config = {
  std::make_pair(
    "basic",
    pipeline_layout_config_t{
      std::vector<std::string>{ "uniform_data" },
      0
    }
  ),
  std::make_pair(
    "interface",
    pipeline_layout_config_t{
      std::vector<std::string>{ "uniform_data", "interface_data" },
      0
    }
  ),
};

template <typename T>
std::vector<std::string> get_names(const T &map) {
  std::vector<std::string> names;
  for (const auto &[k,v] : map) {
    names.push_back(k);
  }
  return names;
}

template <typename T>
auto find_data(const T &map, const std::string &name) {
  const auto itr = map.find(name);
  using ret_type = decltype(&itr->second);
  if (itr == map.end()) return ret_type(nullptr);
  return &itr->second;
}

std::vector<std::string> available_default_graphics_pipeline_configs() {
  return get_names(default_graphics_pipeline_configs);
}

std::vector<std::string> available_default_compute_pipeline_configs() {
  return get_names(default_compute_pipeline_configs);
}

std::vector<std::string> available_default_render_pass_configs() {
  return get_names(default_render_pass_configs);
}

std::vector<std::string> available_default_attachments_configs() {
  return get_names(default_attachments_configs);
}

std::vector<std::string> available_default_samplers_configs() {
  return get_names(default_samplers_configs);
}

const graphics_pipeline_create_config* get_default_graphics_pipeline_config(const std::string &name) {
  return find_data(default_graphics_pipeline_configs, name);
}

const compute_pipeline_create_config* get_default_compute_pipeline_config(const std::string &name) {
  return find_data(default_compute_pipeline_configs, name);
}

const render_pass_data_t* get_default_render_pass_config(const std::string &name) {
  return find_data(default_render_pass_configs, name);
}

const std::vector<attachment_config_t> * get_default_attachments_config(const std::string &name) {
  return find_data(default_attachments_configs, name);
}

const sampler_config_t* get_default_sampler_config(const std::string &name) {
  return find_data(default_samplers_configs, name);
}

const void* get_default_descriptor_set_layout_configs() {
  return &default_descriptor_set_layouts_config;
}

const void* get_default_pipeline_layout_configs() {
  return &default_pipeline_layouts_config;
}

}
}