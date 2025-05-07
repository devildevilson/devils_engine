#include "pipelines_resources.h"

#include "vulkan_header.h"
#include "makers.h"
#include "demiurg/resource_base.h"
#include "demiurg/resource_system.h"
#include "auxiliary.h"
#include "shader_crafter.h"
#include "glsl_source_file.h"

namespace devils_engine {
namespace painter {

const uint32_t shaderc_vertex_shader = 0;
const uint32_t shaderc_fragment_shader = 1;
const uint32_t shaderc_compute_shader = 2;
const uint32_t shaderc_geometry_shader = 3;
const uint32_t shaderc_tess_control_shader = 4;
const uint32_t shaderc_tess_evaluation_shader = 5;

static vk::StencilOpState cast(const graphics_pipeline_create_config::stencil_op_state_t state) {
  return vk::StencilOpState(
    static_cast<vk::StencilOp>(state.fail_op),
    static_cast<vk::StencilOp>(state.pass_op),
    static_cast<vk::StencilOp>(state.depth_fail_op),
    static_cast<vk::CompareOp>(state.compare_op),
    state.compare_mask,
    state.write_mask,
    state.reference
  );
}

simple_graphics_pipeline::simple_graphics_pipeline(VkDevice device, VkPipelineLayout pipeline_layout, VkPipelineCache cache, const demiurg::resource_system* system) : 
  device(device), cache(cache), system(system), render_pass(VK_NULL_HANDLE), subpass(0), conf(nullptr), attachments_count(0), atts(nullptr)
{
  this->pipeline_layout = pipeline_layout;
  this->pipeline_bind_point = static_cast<uint32_t>(vk::PipelineBindPoint::eGraphics);
}

simple_graphics_pipeline::~simple_graphics_pipeline() noexcept {
  vk::Device d(device);
  if (pipeline != VK_NULL_HANDLE) d.destroy(pipeline);
  pipeline = VK_NULL_HANDLE;
}

void simple_graphics_pipeline::init(VkRenderPass render_pass, const uint32_t subpass, const graphics_pipeline_create_config *conf, const size_t attachments_count, const subpass_data_t::attachment *atts) {
  this->render_pass = render_pass;
  this->subpass = subpass;
  this->conf = conf;
  this->attachments_count = attachments_count;
  this->atts = atts;

  recompile_shaders();
}

uint32_t simple_graphics_pipeline::recompile_shaders() {
  if (conf == nullptr) {
    utils::error("Graphics pipeline was not properly initialized");
    return 1;
  }

  pipeline_maker pm(device);
  
  // КАК ЗАДАТЬ БИНДИНГИ?
  // КАК ЗАДАТЬ КОНСТАНТЫ?
  
  std::vector<vk::UniqueHandle<vk::ShaderModule, VULKAN_HPP_DEFAULT_DISPATCHER_TYPE>> modules;

  
  for (const auto &sdr : conf->shaders) {
    auto res = system->get<glsl_source_file>(sdr.path);
    if (res == nullptr) utils::error("Could not find shader file '{}'", sdr.path);

    const auto &shader_text = res->memory;

    // если шейдер фейлит то не вылетаем а возвращаем ошибку и выходим

    // почему я это делаю тут вообще? я же могу просто это сделать в ресурсе
    shader_crafter cr(system);
    for (const auto& [name, def] : sdr.defines) {
      cr.add_definition(name, def);
    }

    cr.set_optimization(true);
    cr.set_shader_entry_point(sdr.func_name);
    
    auto stage = vk::ShaderStageFlagBits::eVertex;
    if (sdr.type == "vertex") {
      stage = vk::ShaderStageFlagBits::eVertex;
      cr.set_shader_type(shaderc_vertex_shader);
    } else if (sdr.type == "tesselation_control") {
      stage = vk::ShaderStageFlagBits::eTessellationControl;
      cr.set_shader_type(shaderc_tess_control_shader);
    } else if (sdr.type == "tesselation_evaluation") {
      stage = vk::ShaderStageFlagBits::eTessellationEvaluation;
      cr.set_shader_type(shaderc_tess_evaluation_shader);
    } else if (sdr.type == "geometry") {
      stage = vk::ShaderStageFlagBits::eGeometry;
      cr.set_shader_type(shaderc_geometry_shader);
    } else if (sdr.type == "fragment") {
      stage = vk::ShaderStageFlagBits::eFragment;
      cr.set_shader_type(shaderc_fragment_shader);
    } else {
      utils::error("Found unsupported shader type '{}' while creating graphics pipeline '{}'", sdr.type, conf->name);
    }

    const auto code = cr.compile(std::string(res->id), std::string(shader_text));
    if (cr.err_type() != 0) {
      utils::println(cr.err_msg());
      utils::error("Shader '{}' compile error while creating graphics pipeline '{}'", res->id, conf->name);
    }

    vk::ShaderModuleCreateInfo smci({}, code);
    modules.push_back(vk::Device(device).createShaderModuleUnique(smci));
    pm.addShader(stage, modules.back().get(), sdr.func_name.c_str());
    
    //pm.addSpecializationEntry();
    //pm.addData();
  }

  for (const auto & b : conf->bindings) {
    const auto input = b.type == "vertex" ? vk::VertexInputRate::eVertex : vk::VertexInputRate::eInstance;
    pm.vertexBinding(b.id, b.stride, input);

    for (const auto & a : b.attributes) {
      pm.vertexAttribute(a.location, a.binding, vk::Format(a.format), a.offset);
    }
  }

  pm.dynamicState(vk::DynamicState::eViewport);
  pm.dynamicState(vk::DynamicState::eScissor);

  pm.viewport();
  pm.scissor();

  pm.assembly(static_cast<vk::PrimitiveTopology>(conf->topology), conf->primitive_restart);
  pm.tessellation(false);

  pm.depthClamp(conf->depth_clamp);
  pm.rasterizerDiscard(conf->rasterizer_discard);
  pm.polygonMode(static_cast<vk::PolygonMode>(conf->polygon_mode));
  pm.cullMode(static_cast<vk::CullModeFlags>(conf->cull_mode));
  pm.frontFace(static_cast<vk::FrontFace>(conf->front_face));
  pm.depthBias(conf->depth_bias.enable, conf->depth_bias.const_factor, conf->depth_bias.clamp, conf->depth_bias.slope_factor);
  pm.lineWidth(conf->line_width);

  pm.rasterizationSamples(static_cast<vk::SampleCountFlagBits>(conf->rasterization_samples));
  pm.sampleShading(conf->sample_shading.enable, conf->sample_shading.min_sample_shading, conf->sample_shading.masks.size() != 0 ? conf->sample_shading.masks.data() : nullptr);
  pm.multisampleCoverage(conf->multisample_coverage.alpha_to_coverage, conf->multisample_coverage.alpha_to_one);

  pm.depthTest(conf->depth_test);
  pm.depthWrite(conf->depth_write);
  pm.compare(static_cast<vk::CompareOp>(conf->depth_compare));
  pm.stencilTest(conf->stencil_test.enable, cast(conf->stencil_test.front), cast(conf->stencil_test.back));
  pm.depthBounds(conf->depth_bounds.enable, conf->depth_bounds.min_bounds, conf->depth_bounds.max_bounds);

  pm.logicOp(conf->color_blending_state.logic_op.enable, static_cast<vk::LogicOp>(conf->color_blending_state.logic_op.operation));
  pm.blendConstant(conf->color_blending_state.blend_constants.data());

  for (size_t i = 0; i < attachments_count; ++i) {
    const auto& att = atts[i];
    //if (!format_is_color(att.format)) continue;
    if (att.type != subpass_attachment_type::intended) continue;
    // блен вообще нужно формат проверять......

    pm.colorBlendBegin(att.blending.enable);
    pm.srcColor(static_cast<vk::BlendFactor>(att.blending.src_color_blend_factor));
    pm.dstColor(static_cast<vk::BlendFactor>(att.blending.dst_color_blend_factor));
    pm.colorOp(static_cast<vk::BlendOp>(att.blending.color_blend_op));
    pm.srcAlpha(static_cast<vk::BlendFactor>(att.blending.src_alpha_blend_factor));
    pm.dstAlpha(static_cast<vk::BlendFactor>(att.blending.dst_alpha_blend_factor));
    pm.alphaOp(static_cast<vk::BlendOp>(att.blending.alpha_blend_op));
    pm.colorWriteMask(static_cast<vk::ColorComponentFlags>(att.blending.color_write_mask));
  }

  auto new_pip = pm.create(conf->name, cache, pipeline_layout, render_pass, subpass, pipeline, pipeline == VK_NULL_HANDLE ? -1 : 0);
  vk::Device(device).destroy(pipeline);
  pipeline = new_pip;

  return 0;
}

simple_compute_pipeline::simple_compute_pipeline(VkDevice device, VkPipelineLayout pipeline_layout, VkPipelineCache cache, const demiurg::resource_system* system) :
  device(device), cache(cache), system(system), conf(nullptr)
{
  this->pipeline_layout = pipeline_layout;
  this->pipeline_bind_point = static_cast<uint32_t>(vk::PipelineBindPoint::eCompute);
}

simple_compute_pipeline::~simple_compute_pipeline() noexcept {
  if (pipeline != VK_NULL_HANDLE) vk::Device(device).destroy(pipeline);
  pipeline = VK_NULL_HANDLE;
}

void simple_compute_pipeline::init(const compute_pipeline_create_config *conf) {
  this->conf = conf;
  recompile_shaders();
}

uint32_t simple_compute_pipeline::recompile_shaders() {
  if (conf == nullptr) {
    utils::error("Compute pipeline was not properly initialized");
    return 1;
  }

  compute_pipeline_maker cpm(device);

  auto res = system->get<glsl_source_file>(conf->shader.path);

  const auto &shader_text = res->memory;

  // если шейдер фейлит то не вылетаем а возвращаем ошибку и выходим
  shader_crafter cr(system);
  for (const auto& [name, def] : conf->shader.defines) {
    cr.add_definition(name, def);
  }

  cr.set_optimization(true);
  cr.set_shader_entry_point(conf->shader.func_name);
    
  auto stage = vk::ShaderStageFlagBits::eCompute;
  if (conf->shader.type == "compute") {
    stage = vk::ShaderStageFlagBits::eCompute;
    cr.set_shader_type(shaderc_compute_shader);
  } else {
    utils::error("Found unsupported shader type '{}' while creating compute pipeline '{}'", conf->shader.type, conf->name);
    return 1;
  }

  const auto code = cr.compile(std::string(res->id), std::string(shader_text));
  if (cr.err_type() != 0) {
    utils::println(cr.err_msg());
    utils::error("Shader '{}' compile error while creating compute pipeline '{}'", res->id, conf->name);
    return 1;
  }

  vk::ShaderModuleCreateInfo smci({}, code);
  const auto m = vk::Device(device).createShaderModuleUnique(smci);
  cpm.shader(m.get(), conf->shader.func_name.c_str());

  return 0;
}

}
}
