#include <algorithm>

#include <devils_engine/catalogue/logging.h>

#include "assets_base.h"
#include "auxiliary.h"
#include "common_steps.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/string-utils.hpp"
#include "glsl_source_file.h"
#include "graphics_base.h"
#include "makers.h"
#include "shader_crafter.h"
#include "shader_source_file.h"
#include "shaderc/shaderc.h"
#include "vulkan_header.h"

// у нас тут будет удобный способ брать текущее состояние ресурсов
// и запишем туда текущий usage

namespace devils_engine {
namespace painter {

step_interface::step_interface() noexcept : type(type::invalid), super(UINT32_MAX) {}

step_interface::step_interface(const enum type type, const uint32_t super) noexcept
  : type(type), super(super) {}

graphics_step_instance::graphics_step_instance() noexcept
  : renderpass(VK_NULL_HANDLE),
    subpass_index(0),
    render_target_index(UINT32_MAX),
    device(VK_NULL_HANDLE),
    pipeline_layout(VK_NULL_HANDLE),
    pipeline(VK_NULL_HANDLE) {}

graphics_step_instance::graphics_step_instance(
  const uint32_t super,
  VkDevice device,
  VkRenderPass renderpass,
  const uint32_t subpass_index,
  const uint32_t render_target_index) noexcept
  : step_interface(step_interface::type::step, super),
    renderpass(renderpass),
    subpass_index(subpass_index),
    render_target_index(render_target_index),
    device(device),
    pipeline_layout(VK_NULL_HANDLE),
    pipeline(VK_NULL_HANDLE) {}

compute_step_instance::compute_step_instance() noexcept
  : device(VK_NULL_HANDLE), pipeline_layout(VK_NULL_HANDLE), pipeline(VK_NULL_HANDLE) {}

compute_step_instance::compute_step_instance(const uint32_t super, VkDevice device) noexcept
  : step_interface(step_interface::type::step, super),
    device(device),
    pipeline_layout(VK_NULL_HANDLE),
    pipeline(VK_NULL_HANDLE) {}

transfer_step_instance::transfer_step_instance() noexcept = default;

transfer_step_instance::transfer_step_instance(const uint32_t super) noexcept
  : step_interface(step_interface::type::step, super) {}

execution_pass_instance::execution_pass_instance() noexcept
  : device(VK_NULL_HANDLE), renderpass(VK_NULL_HANDLE), width(0), height(0) {}

subpass_next::subpass_next() noexcept : renderpass(VK_NULL_HANDLE) {}

execution_group::frame::frame() noexcept : buffer(VK_NULL_HANDLE) {}

execution_group::execution_group() noexcept : device(VK_NULL_HANDLE), pool(VK_NULL_HANDLE) {}

render_graph_instance::semaphore::semaphore() noexcept {
  memset(handles.data(), 0, sizeof(handles));
}

// bit_cast конечно супер удобен жесть

// sanity check for bit_cast
static_assert(sizeof(buffer_memory_barrier) == sizeof(VkBufferMemoryBarrier));
static_assert(alignof(buffer_memory_barrier) == alignof(VkBufferMemoryBarrier));
static_assert(sizeof(image_memory_barrier) == sizeof(VkImageMemoryBarrier));
static_assert(alignof(image_memory_barrier) == alignof(VkImageMemoryBarrier));

static_assert(sizeof(buffer_memory_barrier) == sizeof(vk::BufferMemoryBarrier));
static_assert(alignof(buffer_memory_barrier) == alignof(vk::BufferMemoryBarrier));
static_assert(sizeof(image_memory_barrier) == sizeof(vk::ImageMemoryBarrier));
static_assert(alignof(image_memory_barrier) == alignof(vk::ImageMemoryBarrier));

static_assert(sizeof(subresource_image) == sizeof(VkImageSubresourceRange));
static_assert(alignof(subresource_image) == alignof(VkImageSubresourceRange));
static_assert(sizeof(subresource_image) == sizeof(vk::ImageSubresourceRange));

static vk::PipelineColorBlendAttachmentState blend_state(const blend_data& b) noexcept {
  vk::PipelineColorBlendAttachmentState state{};
  state.blendEnable = b.enable ? VK_TRUE : VK_FALSE;
  state.srcColorBlendFactor = static_cast<vk::BlendFactor>(b.srcColorBlendFactor);
  state.dstColorBlendFactor = static_cast<vk::BlendFactor>(b.dstColorBlendFactor);
  state.colorBlendOp = static_cast<vk::BlendOp>(b.colorBlendOp);
  state.srcAlphaBlendFactor = static_cast<vk::BlendFactor>(b.srcAlphaBlendFactor);
  state.dstAlphaBlendFactor = static_cast<vk::BlendFactor>(b.dstAlphaBlendFactor);
  state.alphaBlendOp = static_cast<vk::BlendOp>(b.alphaBlendOp);
  state.colorWriteMask = static_cast<vk::ColorComponentFlags>(b.colorWriteMask);
  return state;
}
static_assert(alignof(subresource_image) == alignof(vk::ImageSubresourceRange));

static_assert(sizeof(vk::ImageLayout) == sizeof(uint32_t));
static_assert(sizeof(vk::ImageAspectFlags) == sizeof(uint32_t));
static_assert(sizeof(vk::AccessFlags) == sizeof(uint32_t));
static_assert(sizeof(vk::PipelineStageFlags) == sizeof(uint32_t));
static_assert(sizeof(vk::ImageUsageFlags) == sizeof(uint32_t));
static_assert(sizeof(vk::BufferUsageFlags) == sizeof(uint32_t));

static_assert(offsetof(buffer_memory_barrier, pNext) == offsetof(VkBufferMemoryBarrier, pNext));
static_assert(offsetof(buffer_memory_barrier, srcAccessMask) == offsetof(VkBufferMemoryBarrier, srcAccessMask));
static_assert(offsetof(buffer_memory_barrier, dstAccessMask) == offsetof(VkBufferMemoryBarrier, dstAccessMask));
static_assert(offsetof(buffer_memory_barrier, srcQueueFamilyIndex) == offsetof(VkBufferMemoryBarrier, srcQueueFamilyIndex));
static_assert(offsetof(buffer_memory_barrier, dstQueueFamilyIndex) == offsetof(VkBufferMemoryBarrier, dstQueueFamilyIndex));
static_assert(offsetof(buffer_memory_barrier, buffer) == offsetof(VkBufferMemoryBarrier, buffer));
static_assert(offsetof(buffer_memory_barrier, offset) == offsetof(VkBufferMemoryBarrier, offset));
static_assert(offsetof(buffer_memory_barrier, size) == offsetof(VkBufferMemoryBarrier, size));

static_assert(offsetof(image_memory_barrier, pNext) == offsetof(VkImageMemoryBarrier, pNext));
static_assert(offsetof(image_memory_barrier, srcAccessMask) == offsetof(VkImageMemoryBarrier, srcAccessMask));
static_assert(offsetof(image_memory_barrier, dstAccessMask) == offsetof(VkImageMemoryBarrier, dstAccessMask));
static_assert(offsetof(image_memory_barrier, oldLayout) == offsetof(VkImageMemoryBarrier, oldLayout));
static_assert(offsetof(image_memory_barrier, newLayout) == offsetof(VkImageMemoryBarrier, newLayout));
static_assert(offsetof(image_memory_barrier, srcQueueFamilyIndex) == offsetof(VkImageMemoryBarrier, srcQueueFamilyIndex));
static_assert(offsetof(image_memory_barrier, dstQueueFamilyIndex) == offsetof(VkImageMemoryBarrier, dstQueueFamilyIndex));
static_assert(offsetof(image_memory_barrier, image) == offsetof(VkImageMemoryBarrier, image));
static_assert(offsetof(image_memory_barrier, subresourceRange) == offsetof(VkImageMemoryBarrier, subresourceRange));

static_assert(offsetof(subresource_image, aspect_mask) == offsetof(VkImageSubresourceRange, aspectMask));
static_assert(offsetof(subresource_image, base_mip_level) == offsetof(VkImageSubresourceRange, baseMipLevel));
static_assert(offsetof(subresource_image, level_count) == offsetof(VkImageSubresourceRange, levelCount));
static_assert(offsetof(subresource_image, base_array_layer) == offsetof(VkImageSubresourceRange, baseArrayLayer));
static_assert(offsetof(subresource_image, layer_count) == offsetof(VkImageSubresourceRange, layerCount));

static void make_barriers1(graphics_ctx* ctx, VkCommandBuffer buf, const std::vector<std::tuple<uint32_t, usage::values>>& barriers) {
  vk::CommandBuffer task(buf);

  vk::PipelineStageFlags src_stages{};
  vk::PipelineStageFlags dst_stages{};
  for (const auto& [index, usage] : barriers) {
    auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, index);
    if (res.usage == usage) {
      continue;
    }

    src_stages = src_stages | convertps(res.usage);
    dst_stages = dst_stages | convertps(usage);

    // + собираем структуры для img barrier и buf barrier

    if (role::is_image(res.role)) {
      vk::ImageMemoryBarrier img_bar{};
      img_bar.srcAccessMask = convertam(res.usage);
      img_bar.dstAccessMask = convertam(usage);
      img_bar.oldLayout = convertil(res.usage);
      img_bar.newLayout = convertil(usage);
      img_bar.image = res.img;
      img_bar.subresourceRange = std::bit_cast<vk::ImageSubresourceRange>(res.subimg);
      ctx->image_barriers.emplace_back(std::bit_cast<image_memory_barrier>(img_bar));
    } else {
      vk::BufferMemoryBarrier buf_bar{};
      buf_bar.srcAccessMask = convertam(res.usage);
      buf_bar.dstAccessMask = convertam(usage);
      buf_bar.buffer = res.buf;
      buf_bar.offset = res.subbuf.offset;
      buf_bar.size = res.subbuf.size;
      ctx->buffer_barriers.emplace_back(std::bit_cast<buffer_memory_barrier>(buf_bar));
    }

    //{
    //  const auto& bres = DS_ASSERT_ARRAY_GET(ctx->base->resources, index);
    //  utils::info("Make barrier for resource '{}' from '{}' to '{}'", bres.name, usage::to_string(res.usage), usage::to_string(usage));
    //}

    res.usage = usage;
  }

  if (ctx->image_barriers.empty() && ctx->buffer_barriers.empty()) {
    return;
  }

  const auto img_ptr = reinterpret_cast<const vk::ImageMemoryBarrier*>(ctx->image_barriers.data());
  const auto buf_ptr = reinterpret_cast<const vk::BufferMemoryBarrier*>(ctx->buffer_barriers.data());
  task.pipelineBarrier(
    src_stages, dst_stages, vk::DependencyFlagBits::eByRegion,
    0u, nullptr,
    uint32_t(ctx->buffer_barriers.size()), buf_ptr,
    uint32_t(ctx->image_barriers.size()), img_ptr);

  ctx->image_barriers.clear();
  ctx->buffer_barriers.clear();
}

static void make_barriers2(graphics_ctx* ctx, VkCommandBuffer buf, const std::vector<execution_pass_base::resource_info>& barriers) {
  vk::CommandBuffer task(buf);

  vk::PipelineStageFlags src_stages{};
  vk::PipelineStageFlags dst_stages{};
  for (const auto& ri : barriers) {
    auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, ri.slot);
    if (res.usage == ri.usage) {
      continue;
    }

    src_stages = src_stages | convertps(res.usage);
    dst_stages = dst_stages | convertps(ri.usage);

    // + собираем структуры для img barrier и buf barrier

    if (role::is_image(res.role)) {
      vk::ImageMemoryBarrier img_bar{};
      img_bar.srcAccessMask = convertam(res.usage);
      img_bar.dstAccessMask = convertam(ri.usage);
      img_bar.oldLayout = convertil(res.usage);
      img_bar.newLayout = convertil(ri.usage);
      img_bar.image = res.img;
      img_bar.subresourceRange = std::bit_cast<vk::ImageSubresourceRange>(res.subimg);
      ctx->image_barriers.emplace_back(std::bit_cast<image_memory_barrier>(img_bar));
    } else {
      vk::BufferMemoryBarrier buf_bar{};
      buf_bar.srcAccessMask = convertam(res.usage);
      buf_bar.dstAccessMask = convertam(ri.usage);
      buf_bar.buffer = res.buf;
      buf_bar.offset = res.subbuf.offset;
      buf_bar.size = res.subbuf.size;
      ctx->buffer_barriers.emplace_back(std::bit_cast<buffer_memory_barrier>(buf_bar));
    }

    //{
    //  const auto& bres = DS_ASSERT_ARRAY_GET(ctx->base->resources, ri.slot);
    //  utils::info("Make barrier for resource '{}' from '{}' to '{}'", bres.name, usage::to_string(res.usage), usage::to_string(ri.usage));
    //}

    res.usage = ri.usage;
  }

  if (ctx->image_barriers.empty() && ctx->buffer_barriers.empty()) {
    return;
  }

  const auto img_ptr = reinterpret_cast<const vk::ImageMemoryBarrier*>(ctx->image_barriers.data());
  const auto buf_ptr = reinterpret_cast<const vk::BufferMemoryBarrier*>(ctx->buffer_barriers.data());
  task.pipelineBarrier(
    src_stages, dst_stages, vk::DependencyFlagBits::eByRegion,
    0u, nullptr,
    uint32_t(ctx->buffer_barriers.size()), buf_ptr,
    uint32_t(ctx->image_barriers.size()), img_ptr);

  ctx->image_barriers.clear();
  ctx->buffer_barriers.clear();
}

static void change_usages(graphics_ctx* ctx, const std::vector<execution_pass_base::resource_info>& barriers) {
  for (const auto& ri : barriers) {
    auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, ri.slot);
    res.usage = ri.usage;
  }
}

graphics_step_instance::~graphics_step_instance() noexcept {
  vk::Device dev(device);
  dev.destroy(pipeline);
  dev.destroy(pipeline_layout);
}

void graphics_step_instance::recreate_pipeline(const graphics_base* ctx) {
  create_pipeline(ctx);
}

void graphics_step_instance::create_related_primitives(const graphics_base* ctx) {
  create_pipeline_layout(ctx);
  create_pipeline(ctx);
}

void graphics_step_instance::create_pipeline_layout(const graphics_base* ctx) {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->steps, super);

  pipeline_layout_maker plm(device);
  for (const auto descriptor_index : step.sets) {
    const auto& descriptor = DS_ASSERT_ARRAY_GET(ctx->descriptors, descriptor_index);
    plm.addDescriptorLayout(descriptor.setlayout);
  }

  size_t offset = 0;
  for (const auto constant_index : step.push_constants) {
    const auto& constant = DS_ASSERT_ARRAY_GET(ctx->constants, constant_index);
    plm.addPushConstRange(offset, constant.size, vk::ShaderStageFlagBits::eAll);
    offset += constant.size;
  }

  pipeline_layout = plm.create(step.name + ".pipeline_layout");
}

// Загрузка одного шейдер-модуля (Фаза 1: конфиг painter на рельсах demiurg).
// По расширению: .spv → готовые байты (shader_source_file, без компиляции); иначе GLSL
// (glsl_source_file) → shader_crafter с реальным реестром (резолв #include через demiurg).
// Если ctx->config_reg_ == nullptr — fs-fallback через file_io (fast_test / корневой main.cpp).
static vk::UniqueShaderModule load_shader_module(const graphics_base* ctx, const std::string& shader_path, const uint32_t shader_kind) {
  const auto make_module = [ctx](const void* data, const size_t bytes) {
    vk::ShaderModuleCreateInfo smci{};
    smci.codeSize = bytes;
    smci.pCode = reinterpret_cast<const uint32_t*>(data);
    return vk::Device(ctx->device).createShaderModuleUnique(smci);
  };

  const bool is_spv = shader_path.size() >= 4 && shader_path.compare(shader_path.size() - 4, 4, ".spv") == 0;

  if (ctx->config_reg_ != nullptr) {
    // demiurg-id = префикс + путь без ПОСЛЕДНЕГО расширения (parse_path режет по последней '.')
    const size_t last_dot = shader_path.rfind('.');
    const std::string stem = last_dot == std::string::npos ? shader_path : shader_path.substr(0, last_dot);
    const std::string id = ctx->shader_prefix_ + stem;

    if (is_spv) {
      auto* res = ctx->config_reg_->get<shader_source_file>(id);
      if (res == nullptr) {
        utils::error{}("Shader resource '{}' (spv) not found in engine registry", id);
      }
      return make_module(res->memory.data(), res->memory.size());
    }

    auto* res = ctx->config_reg_->get<glsl_source_file>(id);
    if (res == nullptr) {
      utils::error{}("Shader resource '{}' (glsl) not found in engine registry", id);
    }

    if (!res->prepared(shader_kind)) {
      std::string err;
      if (!res->prepare_spirv(ctx->config_reg_, shader_kind, &err)) {
        utils::error{}("Shader '{}' compilation failed\nError: {}", id, err);
      }
      utils::warn("Shader '{}' was compiled on render thread; expected assets-side prepared SPIR-V", id);
    }

    return make_module(res->spirv.data(), res->spirv.size() * sizeof(res->spirv[0]));
  }

  // fs-fallback
  const auto full_path = utils::project_folder() + "tests/shaders/" + shader_path;
  if (!file_io::exists(full_path)) {
    utils::error{}("Shader file '{}' not found", full_path);
  }
  const auto content = file_io::read(full_path);
  shader_crafter sc(nullptr);
  sc.set_optimization(true);
  sc.set_shader_entry_point("main");
  sc.set_shader_type(shader_kind);
  const auto spv = sc.compile(full_path, content);
  if (spv.empty()) {
    utils::error{}("Shader '{}' compilation failed\nError: {}", full_path, sc.err_msg());
  }
  return make_module(spv.data(), spv.size() * sizeof(spv[0]));
}

void graphics_step_instance::create_pipeline(const graphics_base* ctx) {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->steps, super);

  vk::UniqueShaderModule usm_vertex;
  vk::UniqueShaderModule usm_fragment;

  pipeline_maker pm(device);

  const auto& material = DS_ASSERT_ARRAY_GET(ctx->materials, step.material);

  {
    if (!material.shaders.vertex.empty()) {
      usm_vertex = load_shader_module(ctx, material.shaders.vertex, shaderc_vertex_shader);
    }
    if (!material.shaders.fragment.empty()) {
      usm_fragment = load_shader_module(ctx, material.shaders.fragment, shaderc_fragment_shader);
    }

    pm.addShader(vk::ShaderStageFlagBits::eVertex, usm_vertex.get());
    pm.addShader(vk::ShaderStageFlagBits::eFragment, usm_fragment.get());
  }

  uint32_t location_offset = 0;
  const auto& geo = DS_ASSERT_ARRAY_GET(ctx->geometries, step.geometry);
  if (geo.stride != 0) {
    pm.vertexBinding(0, geo.stride, vk::VertexInputRate::eVertex);
    size_t offset = 0;
    for (uint32_t i = 0; i < geo.vertex_layout.size(); ++i) {
      const auto f = geo.vertex_layout[i];
      const auto format = static_cast<vk::Format>(format::to_vulkan_format(f));
      DE_LOG(catalogue::log_domain::render, trace, "Vertex stride for '{}' location {}, offset {}, format '{}'", material.name, location_offset + i, offset, vk::to_string(format));
      const auto el_size = format_element_size(static_cast<uint32_t>(format));
      pm.vertexAttribute(location_offset + i, 0, format, offset);
      offset += el_size;
    }

    location_offset += geo.vertex_layout.size();
  }

  if (step.draw_group != invalid_resource_slot) {
    const auto& draw_group = DS_ASSERT_ARRAY_GET(ctx->draw_groups, step.draw_group);
    if (draw_group.stride != 0) {
      pm.vertexBinding(1, draw_group.stride, vk::VertexInputRate::eInstance);
      size_t offset = 0;
      for (uint32_t i = 0; i < draw_group.instance_layout.size(); ++i) {
        const auto f = draw_group.instance_layout[i];
        const auto format = static_cast<vk::Format>(format::to_vulkan_format(f));
        DE_LOG(catalogue::log_domain::render, trace, "Instance stride for '{}' location {}, offset {}, format '{}'", material.name, location_offset + i, offset, vk::to_string(format));
        const auto el_size = format_element_size(static_cast<uint32_t>(format));
        pm.vertexAttribute(location_offset + i, 1, format, offset);
        offset += el_size;
      }

      location_offset += draw_group.instance_layout.size();
    }
  }

  // нужно явно прописать это дело в шагах
  pm.dynamicState(vk::DynamicState::eViewport);
  pm.dynamicState(vk::DynamicState::eScissor);

  pm.viewport(); // empty viewport for DynamicState
  pm.scissor();  // empty scissor  for DynamicState

  pm.assembly(static_cast<vk::PrimitiveTopology>(geo.topology_type), geo.restart);
  pm.tessellation(false);

  pm.depthClamp(material.raster.depth_clamp);
  pm.rasterizerDiscard(material.raster.raster_discard);
  pm.polygonMode(static_cast<vk::PolygonMode>(material.raster.polygon));
  pm.cullMode(static_cast<vk::CullModeFlags>(material.raster.cull));
  pm.frontFace(static_cast<vk::FrontFace>(material.raster.front_face));
  pm.depthBias(material.raster.depth_bias, material.raster.bias_constant, material.raster.bias_clamp, material.raster.bias_slope);
  pm.lineWidth(material.raster.line_width);

  pm.rasterizationSamples(vk::SampleCountFlagBits::e1);
  pm.sampleShading(false);
  pm.multisampleCoverage(false, false);

  pm.depthTest(material.depth.test);
  pm.depthWrite(material.depth.write);
  pm.compare(static_cast<vk::CompareOp>(material.depth.compare));
  pm.stencilTest(material.depth.stencil_test, std::bit_cast<vk::StencilOpState>(material.depth.front), std::bit_cast<vk::StencilOpState>(material.depth.back));
  pm.depthBounds(material.depth.bounds_test, material.depth.min_bounds, material.depth.max_bounds);

  pm.logicOp(false);
  //pm.blendConstant(nullptr);

  const auto& rt = DS_ASSERT_ARRAY_GET(ctx->render_targets, render_target_index);
  auto blending = rt.default_blending; // copy
  for (const auto& [res_index, blend] : step.blending) {
    const uint32_t id = rt.resource_index(res_index);
    const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, res_index);
    if (id >= rt.resources.size()) {
      utils::error{}("Could not find resource '{}' among render target '{}' resources", res.name, rt.name);
    }
    blending[id] = blend;
  }

  for (const auto& b : blending) {
    pm.colorBlending(blend_state(b));
  }

  auto pipe = pm.create(
    step.name + ".pipeline",
    VK_NULL_HANDLE, // это из контекста
    pipeline_layout,
    renderpass,
    subpass_index,
    pipeline);

  if (pipeline != VK_NULL_HANDLE) {
    vk::Device(device).destroy(pipeline);
  }

  pipeline = pipe;
}

compute_step_instance::~compute_step_instance() noexcept {
  vk::Device dev(device);
  dev.destroy(pipeline);
  dev.destroy(pipeline_layout);
}

void compute_step_instance::recreate_pipeline(const graphics_base* ctx) {
  create_pipeline(ctx);
}

void compute_step_instance::create_related_primitives(const graphics_base* ctx) {
  create_pipeline_layout(ctx);
  create_pipeline(ctx);
}

void compute_step_instance::create_pipeline_layout(const graphics_base* ctx) {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->steps, super);

  pipeline_layout_maker plm(device);
  for (const auto descriptor_index : step.sets) {
    const auto& descriptor = DS_ASSERT_ARRAY_GET(ctx->descriptors, descriptor_index);
    plm.addDescriptorLayout(descriptor.setlayout);
  }

  size_t offset = 0;
  for (const auto constant_index : step.push_constants) {
    const auto& constant = DS_ASSERT_ARRAY_GET(ctx->constants, constant_index);
    plm.addPushConstRange(offset, constant.size, vk::ShaderStageFlagBits::eAll);
    offset += constant.size;
  }

  pipeline_layout = plm.create(step.name + ".pipeline_layout");
}

void compute_step_instance::create_pipeline(const graphics_base*) {
  // компут пайплайн гораздо проще
}

execution_pass_instance::~execution_pass_instance() noexcept {
  clear_framebuffers();
  vk::Device(device).destroy(renderpass);
}

void execution_pass_instance::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& pass = DS_ASSERT_ARRAY_GET(ctx->base->passes, super);
  make_barriers2(ctx, buf, pass.barriers[0]);

  if (pass.render_target != invalid_resource_slot) {
    const auto& rt = DS_ASSERT_ARRAY_GET(ctx->base->render_targets, pass.render_target);

    // здесь мы должны:
    // бегин рендер пасс (клир + фреймбуфер), сет динамик статес

    vk::CommandBuffer task(buf);

    const uint32_t f_index = compute_frame_index(ctx->base);

    vk::Rect2D area{};
    area.offset = vk::Offset2D{0, 0};
    area.extent = vk::Extent2D(this->width, this->height);

    std::array<vk::ClearValue, 16> cvs{};

    const auto sc = vk::SubpassContents::eInline;

    vk::RenderPassBeginInfo rpbi{};
    rpbi.renderPass = renderpass;
    rpbi.framebuffer = framebuffers[f_index];
    rpbi.renderArea = area;
    rpbi.clearValueCount = rt.resources.size();
    rpbi.pClearValues = cvs.data();
    task.beginRenderPass(rpbi, sc);

    vk::Viewport v{};
    v.x = 0;
    v.y = 0;
    v.width = width;
    v.height = height;
    v.minDepth = 0.0f;
    v.maxDepth = 1.0f;
    task.setViewport(0, v);
    task.setScissor(0, area);

    change_usages(ctx, pass.subpasses.front());
  }
}

// разделить нужно на версию С динамикой и без
void execution_pass_instance::resize_viewport(const graphics_base* ctx, const uint32_t, const uint32_t) {
  clear_framebuffers();
  create_framebuffers(ctx);
}

void execution_pass_instance::create_related_primitives(const graphics_base* ctx) {
  create_render_pass(ctx);
  create_framebuffers(ctx);
}

void execution_pass_instance::create_render_pass(const graphics_base* ctx) {
  const auto& pass = DS_ASSERT_ARRAY_GET(ctx->passes, super);
  if (pass.render_target == invalid_resource_slot) {
    return;
  }
  const auto& rt = DS_ASSERT_ARRAY_GET(ctx->render_targets, pass.render_target);

  const auto& start = pass.subpasses.front();
  const auto& finish = pass.subpasses.back();

  render_pass_maker rpm(device);
  for (uint32_t i = 0; i < rt.resources.size(); ++i) {
    const auto& [res_index, type] = rt.resources[i];
    const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, res_index);
    rpm.attachmentBegin(static_cast<vk::Format>(res.format_hint));
    rpm.attachmentSamples(vk::SampleCountFlagBits::e1);

    const auto& sinfo = start[i];
    const auto& finfo = finish[i];

    rpm.attachmentLoadOp(convertl(sinfo.action));
    rpm.attachmentStoreOp(converts(finfo.action));
    rpm.attachmentStencilLoadOp(convertl(sinfo.action));
    rpm.attachmentStencilStoreOp(converts(finfo.action));
    rpm.attachmentInitialLayout(convertil(sinfo.usage));
    rpm.attachmentFinalLayout(convertil(finfo.usage));
  }

  uint32_t subpass_index = VK_SUBPASS_EXTERNAL;
  for (uint32_t index = 1; index < pass.subpasses.size() - 1; ++index, ++subpass_index) {
    const auto& sub = pass.subpasses[index];
    rpm.subpassBegin(vk::PipelineBindPoint::eGraphics);
    for (uint32_t i = 0; i < sub.size(); ++i) {
      const auto& info = sub[i];
      const auto& [slot, type] = rt.resources[i];
      const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, slot);
      const bool is_depth = format::is_depth_vk_format(res.format_hint);

      // наверное если usage не аттачмент то вылетим с ошибкой
      if (info.usage == usage::color_attachment) {
        rpm.subpassColorAttachment(i, convertil(info.usage));
      }

      if (info.usage == usage::depth_attachment) {
        rpm.subpassDepthStencilAttachment(i, convertil(info.usage));
      }

      if (info.usage == usage::input_attachment) {
        auto layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        if (is_depth) {
          layout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
        }
        rpm.subpassInputAttachment(i, layout);
      }

      if (info.usage == usage::resolve_attachment) {
        rpm.subpassResolveAttachment(i, convertil(info.usage));
      }

      if (info.usage == usage::ignore_attachment) {
        rpm.addPreservedAttachmentIndex(i);
      }
    }

    //const uint32_t next_subpass = subpass_index+1 >= pass.subpasses.size() ? VK_SUBPASS_EXTERNAL : subpass_index;
    const uint32_t next_subpass = subpass_index + 1;
    rpm.dependencyBegin(subpass_index, next_subpass);
    rpm.dependencyDependencyFlags(vk::DependencyFlagBits::eByRegion);

    vk::PipelineStageFlags src_stage{};
    vk::PipelineStageFlags dst_stage{};

    vk::AccessFlags src_mask{};
    vk::AccessFlags dst_mask{};

    {
      const auto& sub = pass.subpasses[index - 1];
      for (uint32_t i = 0; i < sub.size(); ++i) {
        const auto& info = sub[i];

        src_stage = src_stage | convertps(info.usage);
        src_mask = src_mask | convertam(info.usage);
      }
    }

    {
      const auto& sub = pass.subpasses[index];
      for (uint32_t i = 0; i < sub.size(); ++i) {
        const auto& info = sub[i];

        dst_stage = src_stage | convertps(info.usage);
        dst_mask = src_mask | convertam(info.usage);
      }
    }

    rpm.dependencySrcStageMask(src_stage);
    rpm.dependencyDstStageMask(dst_stage);
    rpm.dependencySrcAccessMask(src_mask);
    rpm.dependencyDstAccessMask(dst_mask);
  }

  rpm.dependencyBegin(subpass_index, VK_SUBPASS_EXTERNAL);
  rpm.dependencyDependencyFlags(vk::DependencyFlagBits::eByRegion);

  vk::PipelineStageFlags src_stage{};
  vk::PipelineStageFlags dst_stage{};

  vk::AccessFlags src_mask{};
  vk::AccessFlags dst_mask{};

  {
    const auto& sub = pass.subpasses[pass.subpasses.size() - 2];
    for (uint32_t i = 0; i < sub.size(); ++i) {
      const auto& info = sub[i];

      src_stage = src_stage | convertps(info.usage);
      src_mask = src_mask | convertam(info.usage);
    }
  }

  {
    const auto& sub = pass.subpasses.back();
    for (uint32_t i = 0; i < sub.size(); ++i) {
      const auto& info = sub[i];

      dst_stage = src_stage | convertps(info.usage);
      dst_mask = src_mask | convertam(info.usage);
    }
  }

  rpm.dependencySrcStageMask(src_stage);
  rpm.dependencyDstStageMask(dst_stage);
  rpm.dependencySrcAccessMask(src_mask);
  rpm.dependencyDstAccessMask(dst_mask);

  renderpass = rpm.create(pass.name + ".renderpass");
}

void execution_pass_instance::create_framebuffers(const graphics_base* ctx) {
  const auto& pass = DS_ASSERT_ARRAY_GET(ctx->passes, super);
  if (pass.render_target == invalid_resource_slot) {
    return;
  }
  const auto& rt = DS_ASSERT_ARRAY_GET(ctx->render_targets, pass.render_target);

  // frameindex = i1 * (c2 * c3) + i2 * (c3) + i3
  strides.clear();
  strides.resize(rt.resources.size(), 1);
  uint32_t cur_stride = 1;
  for (int32_t i = rt.resources.size() - 1; i >= 0; --i) {
    const auto& [res_index, usage] = rt.resources[i];
    const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, res_index);

    strides[i] = cur_stride;
    cur_stride *= res.compute_buffering(ctx);
  }

  framebuffers.resize(cur_stride, VK_NULL_HANDLE);

  this->width = 0;
  this->height = 0;

  std::array<vk::ImageView, 8> views;
  for (uint32_t i = 0; i < framebuffers.size(); ++i) {
    views = {};

    vk::FramebufferCreateInfo fci{};
    fci.renderPass = renderpass;

    for (uint32_t j = 0; j < rt.resources.size(); ++j) {
      const auto& [res_index, usage] = rt.resources[j];
      const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, res_index);

      const uint32_t buffering = res.compute_buffering(ctx);
      const uint32_t current_index = i % buffering;
      views[j] = res.handles[current_index].view;

      const auto [size, img_ext] = res.compute_frame_size(ctx);
      const auto [img_width, img_height] = img_ext;

      if (this->width == 0) {
        this->width = img_width;
      }
      if (this->height == 0) {
        this->height = img_height;
      }

      if (this->width != img_width || this->height != img_height) {
        std::string names;
        for (uint32_t j = 0; j < rt.resources.size(); ++j) {
          const auto& [res_index, usage] = rt.resources[j];
          const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, res_index);
          names += "'" + res.name + "' ";
        }

        utils::error{}("Image sizes mismatch within render target '{}', resources ({})", rt.name, utils::string::trim(names));
      }
    }

    fci.width = this->width;
    fci.height = this->height;
    fci.layers = 1;
    fci.attachmentCount = rt.resources.size();
    fci.pAttachments = views.data();
    framebuffers[i] = vk::Device(device).createFramebuffer(fci);

    set_name(device, vk::Framebuffer(framebuffers[i]), std::format("{}.framebuffer{:02}", pass.name, i));
  }
}

void execution_pass_instance::clear_framebuffers() {
  vk::Device dev(device);
  for (auto& f : framebuffers) {
    dev.destroy(f);
    f = VK_NULL_HANDLE;
  }

  framebuffers.clear();
}

uint32_t execution_pass_instance::compute_frame_index(const graphics_base* ctx) const {
  const auto& pass = DS_ASSERT_ARRAY_GET(ctx->passes, super);
  if (pass.render_target == invalid_resource_slot) {
    return invalid_resource_slot;
  }
  const auto& rt = DS_ASSERT_ARRAY_GET(ctx->render_targets, pass.render_target);

  uint32_t frameindex = 0;
  for (uint32_t i = 0; i < rt.resources.size(); ++i) {
    const auto& [res_index, usage] = rt.resources[i];
    const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, res_index);
    const auto& counter = DS_ASSERT_ARRAY_GET(ctx->counters, res.swap);
    const uint32_t buffering = res.compute_buffering(ctx);
    frameindex += (counter.get_value() % buffering) * strides[i];
  }

  return frameindex;
}

void subpass_next::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& pass = DS_ASSERT_ARRAY_GET(ctx->base->passes, super);
  if (pass.render_target != invalid_resource_slot) {
    vk::CommandBuffer task(buf);
    task.nextSubpass(vk::SubpassContents::eInline);
  }

  const auto& barriers = DS_ASSERT_ARRAY_GET(pass.barriers, index + 1);
  make_barriers2(ctx, buf, barriers);

  if (pass.render_target != invalid_resource_slot) {
    const auto& subpasses = DS_ASSERT_ARRAY_GET(pass.subpasses, index + 1);
    change_usages(ctx, subpasses);
  }
}

void execution_pass_end_instance::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& pass = DS_ASSERT_ARRAY_GET(ctx->base->passes, super);
  if (pass.render_target != invalid_resource_slot) {
    vk::CommandBuffer task(buf);
    task.endRenderPass();
    change_usages(ctx, pass.subpasses.back());
  }

  make_barriers2(ctx, buf, pass.barriers.back());
}

execution_group::~execution_group() noexcept {
  // вернем командный буфер?
}

void execution_group::process(graphics_ctx* ctx) const {
  const uint32_t cur_index = ctx->base->current_frame_index() % frames.size();
  auto cb = frames[cur_index].buffer;
  vk::CommandBuffer task(cb);
  vk::CommandBufferBeginInfo cbbi{};
  cbbi.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  task.begin(cbbi);
  for (auto p : steps) {
    p->process(ctx, cb);
  }
  task.end();
}

void execution_group::populate_command_buffers() {
  vk::CommandBufferAllocateInfo cbai{};
  cbai.commandPool = pool;
  cbai.level = vk::CommandBufferLevel::ePrimary;
  cbai.commandBufferCount = frames.size();

  auto cbs = vk::Device(device).allocateCommandBuffers(cbai);
  for (uint32_t i = 0; i < frames.size(); ++i) {
    frames[i].buffer = cbs[i];
  }
}

void render_graph_instance::process(graphics_ctx* ctx, VkCommandBuffer) const {
  for (const auto& g : groups) {
    g.process(ctx);
  }
}

void render_graph_instance::recreate_pipeline(const graphics_base* ctx) {
  for (auto ptr : pipeline_steps) {
    ptr->recreate_pipeline(ctx);
  }
}

void render_graph_instance::resize_viewport(const graphics_base* ctx, const uint32_t width, const uint32_t height) {
  for (auto ptr : viewport_steps) {
    ptr->resize_viewport(ctx, width, height);
  }
}

void render_graph_instance::clear() {
  pipeline_steps.clear();
  viewport_steps.clear();

  for (auto& group : groups) {
    std::vector<vk::CommandBuffer> buffers;
    buffers.reserve(group.frames.size());
    for (auto& frame : group.frames) {
      if (frame.buffer == VK_NULL_HANDLE) {
        continue;
      }
      buffers.emplace_back(frame.buffer);
      frame.buffer = VK_NULL_HANDLE;
    }
    if (!buffers.empty() && group.device != VK_NULL_HANDLE && group.pool != VK_NULL_HANDLE) {
      vk::Device(group.device).freeCommandBuffers(group.pool, buffers);
    }
  }

  groups.clear();
  steps.clear();

  for (auto& s : local_semaphores) {
    for (uint32_t i = 0; i < s.handles.size(); ++i) {
      vk::Device(device).destroy(s.handles[i]);
    }
  }

  local_semaphores.clear();
}

void render_graph_instance::submit(const graphics_base* ctx, VkQueue q, VkSemaphore finish, VkFence f) const {
  std::array<vk::Semaphore, 16> finish_semaphores;
  std::array<vk::SubmitInfo, 16> infos;
  assert(groups.size() < 16);
  const uint32_t frame_index = ctx->current_frame_index();
  for (uint32_t i = 0; i < groups.size(); ++i) {
    const auto& group = groups[i];
    const auto& cur_frame = group.frames[frame_index % group.frames.size()];
    assert(cur_frame.wait_for.size() == cur_frame.wait_for_stages.size());
    assert(cur_frame.signal.size() < 15);

    std::copy_n(
      reinterpret_cast<const vk::Semaphore*>(cur_frame.signal.data()),
      cur_frame.signal.size(),
      finish_semaphores.data());
    if (i == groups.size() - 1) {
      finish_semaphores[cur_frame.signal.size()] = finish;
    }

    infos[i].waitSemaphoreCount = cur_frame.wait_for.size();
    infos[i].pWaitSemaphores = reinterpret_cast<const vk::Semaphore*>(cur_frame.wait_for.data());
    //for (const auto sem : cur_frame.wait_for) { utils::info("Current wait for semaphore {:p}", (const void*)sem); }
    infos[i].pWaitDstStageMask = reinterpret_cast<const vk::PipelineStageFlags*>(cur_frame.wait_for_stages.data());
    infos[i].signalSemaphoreCount = cur_frame.signal.size() + size_t(i == groups.size() - 1 && finish != VK_NULL_HANDLE);
    infos[i].pSignalSemaphores = finish_semaphores.data();
    infos[i].commandBufferCount = 1;
    infos[i].pCommandBuffers = reinterpret_cast<const vk::CommandBuffer*>(&cur_frame.buffer);
  }

  // mutex
  const auto res = vk::Queue(q).submit(groups.size(), infos.data(), f);
  if (res != vk::Result::eSuccess) {
    utils::error{}("Failed to submit commads to queue, result: {}", vk::to_string(res));
  }
}

uint32_t render_graph_instance::create_semaphore(std::string name, const uint32_t count) {
  const uint32_t index = find_semaphore(name);
  if (index != invalid_resource_slot) {
    utils::error{}("Semaphore with name '{}' is already exists", name);
  }

  const uint32_t ret_index = local_semaphores.size();
  local_semaphores.emplace_back();
  local_semaphores.back().name = std::move(name);
  for (uint32_t i = 0; i < count; ++i) {
    local_semaphores.back().handles[i] = vk::Device(device).createSemaphore(vk::SemaphoreCreateInfo());
  }

  return ret_index;
}

uint32_t render_graph_instance::find_semaphore(const std::string_view& name) const {
  for (uint32_t i = 0; i < local_semaphores.size(); ++i) {
    if (local_semaphores[i].name == name) {
      return i;
    }
  }

  return invalid_resource_slot;
}

static void bind_descriptor_sets(graphics_ctx* ctx, VkCommandBuffer buf, VkPipelineLayout pipeline_layout, const std::vector<uint32_t>& sets) {
  if (sets.empty()) {
    return;
  }
  vk::CommandBuffer task(buf);

  for (const auto desc : sets) {
    const auto& set = DS_ASSERT_ARRAY_GET(ctx->descriptors, desc);
    ctx->descriptors_cache.push_back(set);
  }

  auto ptr = reinterpret_cast<const vk::DescriptorSet*>(ctx->descriptors_cache.data());
  task.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0u, ctx->descriptors_cache.size(), ptr, 0u, nullptr);
  ctx->descriptors_cache.clear();
}

static void push_constants(graphics_ctx* ctx, VkCommandBuffer buf, VkPipelineLayout pipeline_layout, const std::vector<uint32_t>& push_constants) {
  vk::CommandBuffer task(buf);

  // так в итоге что с константами?
  for (const auto c : push_constants) {
    const auto& data = DS_ASSERT_ARRAY_GET(ctx->base->constants, c);
    auto ptr = ctx->base->get_constant_data(c);
    task.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eAll, 0, data.size, ptr);
  }
}

graphics_draw::graphics_draw(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept : graphics_step_instance(super, device, renderpass, subpass_index, render_target_index) {}

// просто капец
void graphics_draw::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  vk::CommandBuffer task(buf);

  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);
  const auto& draw_group = DS_ASSERT_ARRAY_GET(ctx->base->draw_groups, step.draw_group);

  make_barriers1(ctx, buf, step.barriers);

  // подключаем все что есть, прибиндим сразу все
  bind_descriptor_sets(ctx, buf, pipeline_layout, step.sets);

  push_constants(ctx, buf, pipeline_layout, step.push_constants);

  task.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  if (step.geometry != invalid_resource_slot) {
    for (const auto pair_index : draw_group.pairs) {
      const auto& pair = DS_ASSERT_ARRAY_GET(ctx->base->pairs, pair_index);
      const auto& mesh = DS_ASSERT_ARRAY_GET(ctx->assets->buffer_slots, pair.mesh);
      const auto& inst_buf = DS_ASSERT_ARRAY_GET(ctx->resources, draw_group.instances_buffer);
      const auto& indi_buf = DS_ASSERT_ARRAY_GET(ctx->resources, draw_group.indirect_buffer);

      task.bindVertexBuffers(0u, vk::Buffer(mesh.vertex_storage), size_t(0));

      const size_t offset = inst_buf.subbuf.offset + pair.instance_offset;
      task.bindVertexBuffers(1, vk::Buffer(inst_buf.buf), offset);

      // должны быть барьеры для инстансного буфера и индирект, где?
      // вообще наверное прямо тут, но они могут быть лишними
      const size_t ind_offset = indi_buf.subbuf.offset + pair.indirect_offset;
      task.drawIndirect(indi_buf.buf, ind_offset, 1, 32);
    }
  } else {
    const auto& inst_buf = DS_ASSERT_ARRAY_GET(ctx->resources, draw_group.instances_buffer);
    const auto& indi_buf = DS_ASSERT_ARRAY_GET(ctx->resources, draw_group.indirect_buffer);
    const size_t offset = inst_buf.subbuf.offset;
    task.bindVertexBuffers(0, vk::Buffer(inst_buf.buf), offset);
    const size_t ind_offset = indi_buf.subbuf.offset;
    task.drawIndirect(indi_buf.buf, ind_offset, 1, 32);
  }
}

graphics_draw_indexed::graphics_draw_indexed(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept : graphics_step_instance(super, device, renderpass, subpass_index, render_target_index) {}

void graphics_draw_indexed::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);
  const auto& draw_group = DS_ASSERT_ARRAY_GET(ctx->base->draw_groups, step.draw_group);

  if (step.geometry != invalid_resource_slot && draw_group.pairs.empty()) {
    return;
  }

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);

  // подключаем все что есть, прибиндим сразу все
  bind_descriptor_sets(ctx, buf, pipeline_layout, step.sets);

  push_constants(ctx, buf, pipeline_layout, step.push_constants);

  task.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  uint32_t vertex_bind = 0;

  if (step.geometry != invalid_resource_slot) {
    const auto& geo = DS_ASSERT_ARRAY_GET(ctx->base->geometries, step.geometry);
    for (const auto pair_index : draw_group.pairs) {
      const auto& pair = DS_ASSERT_ARRAY_GET(ctx->base->pairs, pair_index);
      const auto& mesh = DS_ASSERT_ARRAY_GET(ctx->assets->buffer_slots, pair.mesh);
      const auto& inst_buf = DS_ASSERT_ARRAY_GET(ctx->resources, draw_group.instances_buffer);
      const auto& indi_buf = DS_ASSERT_ARRAY_GET(ctx->resources, draw_group.indirect_buffer);

      if (!geo.layout_str.empty()) {
        task.bindVertexBuffers(vertex_bind, vk::Buffer(mesh.vertex_storage), size_t(0));
        vertex_bind += 1;
      }

      auto type = vk::IndexType::eUint32;
      if (geo.index_type == geometry::index_type::u16) {
        type = vk::IndexType::eUint16;
      }
      if (geo.index_type == geometry::index_type::u8) {
        type = vk::IndexType::eUint8;
      }
      task.bindIndexBuffer(mesh.index_storage, 0, type);

      const size_t offset = inst_buf.subbuf.offset + pair.instance_offset;
      task.bindVertexBuffers(vertex_bind, vk::Buffer(inst_buf.buf), offset);

      const size_t ind_offset = indi_buf.subbuf.offset + pair.indirect_offset;
      task.drawIndirect(indi_buf.buf, ind_offset, 1, 32);
    }
  } else {
    const auto& inst_buf = DS_ASSERT_ARRAY_GET(ctx->resources, draw_group.instances_buffer);
    const auto& indi_buf = DS_ASSERT_ARRAY_GET(ctx->resources, draw_group.indirect_buffer);
    const size_t offset = inst_buf.subbuf.offset;
    task.bindVertexBuffers(0, vk::Buffer(inst_buf.buf), offset);
    const size_t ind_offset = indi_buf.subbuf.offset;
    task.drawIndirect(indi_buf.buf, ind_offset, 1, 32);
  }
}

graphics_draw_constant::graphics_draw_constant(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept : graphics_step_instance(super, device, renderpass, subpass_index, render_target_index) {}

void graphics_draw_constant::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  // здесь мы берем VkDrawIndirect из константы
  const auto cmd = ctx->base->get_constant_data<VkDrawIndirectCommand>(step.cmd_params.constants[0]);

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);
  bind_descriptor_sets(ctx, buf, pipeline_layout, step.sets);
  push_constants(ctx, buf, pipeline_layout, step.push_constants);
  task.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  uint32_t vertex_bind = 0;

  // стоп геометрия будет скорее всего, а значит и меш тоже наверное будет
  // следить за тем чтобы тут был только один меш?
  // геометрия тут нужна для того чтобы задать примитив ассембли в материале
  // без геометрии как? определять ассембли в материале и в геометрии его переопределять
  // наверное тут должен быть скорее дополнительный тип объектов - компут генератед меш...
  if (step.geometry != invalid_resource_slot) {
    const auto& geo = DS_ASSERT_ARRAY_GET(ctx->base->geometries, step.geometry);
    if (!geo.vertex_layout.empty()) {
      // откуда брать буфер?
    }

    if (geo.index_type != geometry::index_type::none) {
      // откуда брать буфер?
    }
  }

  if (step.draw_group != invalid_resource_slot) {
    const auto& draw_group = DS_ASSERT_ARRAY_GET(ctx->base->draw_groups, step.draw_group);
    const auto& inst_buf = DS_ASSERT_ARRAY_GET(ctx->resources, draw_group.instances_buffer);
    const size_t offset = inst_buf.subbuf.offset;
    task.bindVertexBuffers(vertex_bind, vk::Buffer(inst_buf.buf), offset);
  }

  // команда по итогу будет вот такая, что с инстансами?
  // а что с геометрией?
  task.draw(cmd.vertexCount, cmd.instanceCount, cmd.firstVertex, cmd.firstInstance);
}

graphics_draw_indexed_constant::graphics_draw_indexed_constant(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept : graphics_step_instance(super, device, renderpass, subpass_index, render_target_index) {}

void graphics_draw_indexed_constant::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  // здесь мы берем VkDrawIndirect из константы
  const auto cmd = ctx->base->get_constant_data<VkDrawIndexedIndirectCommand>(step.cmd_params.constants[0]);

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);
  bind_descriptor_sets(ctx, buf, pipeline_layout, step.sets);
  push_constants(ctx, buf, pipeline_layout, step.push_constants);
  task.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  uint32_t vertex_bind = 0;

  // стоп геометрия будет скорее всего, а значит и меш тоже наверное будет
  // следить за тем чтобы тут был только один меш?
  // геометрия тут нужна для того чтобы задать примитив ассембли в материале
  // без геометрии как? определять ассембли в материале и в геометрии его переопределять
  // наверное тут должен быть скорее дополнительный тип объектов - компут генератед меш...
  if (step.geometry != invalid_resource_slot) {
    const auto& geo = DS_ASSERT_ARRAY_GET(ctx->base->geometries, step.geometry);
    if (!geo.vertex_layout.empty()) {
      // откуда брать буфер?
    }

    if (geo.index_type != geometry::index_type::none) {
      // откуда брать буфер?
    }
  }

  if (step.draw_group != invalid_resource_slot) {
    const auto& draw_group = DS_ASSERT_ARRAY_GET(ctx->base->draw_groups, step.draw_group);
    const auto& inst_buf = DS_ASSERT_ARRAY_GET(ctx->resources, draw_group.instances_buffer);
    const size_t offset = inst_buf.subbuf.offset;
    task.bindVertexBuffers(vertex_bind, vk::Buffer(inst_buf.buf), offset);
  }

  // команда по итогу будет вот такая, что с инстансами?
  // а что с геометрией?
  task.drawIndexed(cmd.indexCount, cmd.instanceCount, cmd.firstIndex, cmd.vertexOffset, cmd.firstInstance);
}

graphics_draw_indirect::graphics_draw_indirect(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept : graphics_step_instance(super, device, renderpass, subpass_index, render_target_index) {}

void graphics_draw_indirect::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto& [res_index, usage] = step.cmd_params.resources[0];
  const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, res_index);

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);
  bind_descriptor_sets(ctx, buf, pipeline_layout, step.sets);
  push_constants(ctx, buf, pipeline_layout, step.push_constants);
  task.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  uint32_t vertex_bind = 0;

  // стоп геометрия будет скорее всего, а значит и меш тоже наверное будет
  // следить за тем чтобы тут был только один меш?
  // геометрия тут нужна для того чтобы задать примитив ассембли в материале
  // без геометрии как? определять ассембли в материале и в геометрии его переопределять
  // наверное тут должен быть скорее дополнительный тип объектов - компут генератед меш...
  if (step.geometry != invalid_resource_slot) {
    const auto& geo = DS_ASSERT_ARRAY_GET(ctx->base->geometries, step.geometry);
    if (!geo.vertex_layout.empty()) {
      // откуда брать буфер?
    }

    if (geo.index_type != geometry::index_type::none) {
      // откуда брать буфер?
    }
  }

  if (step.draw_group != invalid_resource_slot) {
    const auto& draw_group = DS_ASSERT_ARRAY_GET(ctx->base->draw_groups, step.draw_group);
    const auto& inst_buf = DS_ASSERT_ARRAY_GET(ctx->resources, draw_group.instances_buffer);
    const size_t offset = inst_buf.subbuf.offset;
    task.bindVertexBuffers(vertex_bind, vk::Buffer(inst_buf.buf), offset);
  }

  // 1? скорее всего
  task.drawIndirect(res.buf, res.subbuf.offset, 1, res.subbuf.size);
}

graphics_draw_indexed_indirect::graphics_draw_indexed_indirect(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept : graphics_step_instance(super, device, renderpass, subpass_index, render_target_index) {}

void graphics_draw_indexed_indirect::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto& [res_index, usage] = step.cmd_params.resources[0];
  const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, res_index);

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);
  bind_descriptor_sets(ctx, buf, pipeline_layout, step.sets);
  push_constants(ctx, buf, pipeline_layout, step.push_constants);
  task.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  uint32_t vertex_bind = 0;

  // стоп геометрия будет скорее всего, а значит и меш тоже наверное будет
  // следить за тем чтобы тут был только один меш?
  // геометрия тут нужна для того чтобы задать примитив ассембли в материале
  // без геометрии как? определять ассембли в материале и в геометрии его переопределять
  // наверное тут должен быть скорее дополнительный тип объектов - компут генератед меш...
  if (step.geometry != invalid_resource_slot) {
    const auto& geo = DS_ASSERT_ARRAY_GET(ctx->base->geometries, step.geometry);
    if (!geo.vertex_layout.empty()) {
      // откуда брать буфер?
    }

    if (geo.index_type != geometry::index_type::none) {
      // откуда брать буфер?
    }
  }

  if (step.draw_group != invalid_resource_slot) {
    const auto& draw_group = DS_ASSERT_ARRAY_GET(ctx->base->draw_groups, step.draw_group);
    const auto& inst_buf = DS_ASSERT_ARRAY_GET(ctx->resources, draw_group.instances_buffer);
    const size_t offset = inst_buf.subbuf.offset;
    task.bindVertexBuffers(vertex_bind, vk::Buffer(inst_buf.buf), offset);
  }

  // 1? скорее всего
  task.drawIndexedIndirect(res.buf, res.subbuf.offset, 1, res.subbuf.size);
}

graphics_draw_ui::graphics_draw_ui(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept : graphics_step_instance(super, device, renderpass, subpass_index, render_target_index) {}

// Зеркало visage::gui_draw_command_t (render_output.h) — байтовый контракт main->render.
// Должно совпадать поле-в-поле (48 байт, всё по 4 байта). Буфер команд: [uint32 count][массив].
// texture_id УПАКОВАН (tex_id: тип+mirror+индекс); payload[6] — нагрузка per-draw, интерпретируется
// шейдером ПО ТИПУ (SDF-текст / cooldown / mix — см. render_output.h и ui.frag).
namespace {
struct ui_command_wire {
  uint32_t elem_count;
  float clip_x, clip_y, clip_w, clip_h;
  uint32_t texture_id;
  uint32_t payload[6];
};
static_assert(sizeof(ui_command_wire) == 48, "ui_command_wire должен совпадать с visage::gui_draw_command_t");

// push-константа UI: упакованный tex_id + payload[6]. Совпадает с constant ui_push и pc в ui.frag.
struct ui_push_t {
  uint32_t tex_id;
  uint32_t payload[6];
};
} // namespace

void graphics_draw_ui::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto& [vtx_i, vtx_u] = step.cmd_params.resources[0];
  const auto& [idx_i, idx_u] = step.cmd_params.resources[1];
  const auto& [cmd_i, cmd_u] = step.cmd_params.resources[2];

  vk::CommandBuffer task(buf);

  // НЕ вызываем make_barriers1: барьеры внутри render pass требуют self-dependency сабпасса
  // (VUID-07889). UI-буферы host-visible и пишутся до сабмита кадра — host-coherent записи
  // делает видимыми сам vkQueueSubmit, явный барьер не нужен. step.barriers используется только
  // для сбора usage_mask буферов (create_resources), не для барьеров здесь.
  bind_descriptor_sets(ctx, buf, pipeline_layout, step.sets);
  task.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // буферы — обычные per_update ресурсы; .handle для bind, .mapped для CPU-чтения команд
  const auto vtx = ctx->base->get_current_buffer_resource_frame(vtx_i);
  const auto idx = ctx->base->get_current_buffer_resource_frame(idx_i);
  const auto cmds = ctx->base->get_current_buffer_resource_frame(cmd_i);
  if (cmds.mapped == nullptr) {
    return; // буфер команд обязан быть host-visible
  }

  task.bindVertexBuffers(0, vk::Buffer(vtx.handle), vk::DeviceSize(vtx.sub.offset));
  task.bindIndexBuffer(idx.handle, idx.sub.offset, vk::IndexType::eUint16); // nuklear: 16-бит индексы

  const uint8_t* cmd_base = static_cast<const uint8_t*>(cmds.mapped) + cmds.sub.offset;
  const uint32_t count = *reinterpret_cast<const uint32_t*>(cmd_base);
  const auto* list = reinterpret_cast<const ui_command_wire*>(cmd_base + sizeof(uint32_t));

  uint32_t first_index = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const auto& c = list[i];

    // честный scissor per-command (clip_rect nuklear в пикселях окна). clip может выходить за
    // левый/верхний край (отрицательные) — клампим offset>=0 и пересчитываем extent (Vulkan
    // требует offset>=0, VUID-00595).
    const float x0 = c.clip_x < 0.0f ? 0.0f : c.clip_x;
    const float y0 = c.clip_y < 0.0f ? 0.0f : c.clip_y;
    const float x1 = c.clip_x + c.clip_w;
    const float y1 = c.clip_y + c.clip_h;
    vk::Rect2D scissor;
    scissor.offset = vk::Offset2D(int32_t(x0), int32_t(y0));
    scissor.extent = vk::Extent2D(uint32_t(x1 > x0 ? x1 - x0 : 0.0f), uint32_t(y1 > y0 ? y1 - y0 : 0.0f));
    task.setScissor(0, scissor);

    const ui_push_t pc{c.texture_id, {c.payload[0], c.payload[1], c.payload[2], c.payload[3], c.payload[4], c.payload[5]}};
    task.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eAll, 0, sizeof(pc), &pc);

    task.drawIndexed(c.elem_count, 1, first_index, 0, 0);
    first_index += c.elem_count;
  }
}

compute_dispatch_constant::compute_dispatch_constant(const uint32_t super, VkDevice device) noexcept : compute_step_instance(super, device) {}

void compute_dispatch_constant::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto cmd = ctx->base->get_constant_data<VkDispatchIndirectCommand>(step.cmd_params.constants[0]);

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);
  bind_descriptor_sets(ctx, buf, pipeline_layout, step.sets);
  push_constants(ctx, buf, pipeline_layout, step.push_constants);
  task.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);

  // тут геометрии не должно быть
  if (step.geometry != invalid_resource_slot) {
    utils::error{}("How it would looks like?");
  }

  if (step.draw_group != invalid_resource_slot) {
    utils::error{}("How it would looks like?");
  }

  task.dispatch(cmd.x, cmd.y, cmd.z);
}

compute_dispatch_indirect::compute_dispatch_indirect(const uint32_t super, VkDevice device) noexcept : compute_step_instance(super, device) {}

void compute_dispatch_indirect::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto& [res_index, usage] = step.cmd_params.resources[0];
  const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, res_index);

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);
  bind_descriptor_sets(ctx, buf, pipeline_layout, step.sets);
  push_constants(ctx, buf, pipeline_layout, step.push_constants);
  task.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);

  // стоп геометрия будет скорее всего, а значит и меш тоже наверное будет
  // следить за тем чтобы тут был только один меш?
  if (step.geometry != invalid_resource_slot) {
    utils::error{}("How it would looks like?");
  }

  if (step.draw_group != invalid_resource_slot) {
    utils::error{}("How it would looks like?");
  }

  task.dispatchIndirect(res.buf, res.subbuf.offset);
}

transfer_copy_buffer::transfer_copy_buffer(const uint32_t super) noexcept : transfer_step_instance(super) {}
void transfer_copy_buffer::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto& [res_index1, usage1] = step.cmd_params.resources[0];
  const auto& res1 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index1);

  const auto& [res_index2, usage2] = step.cmd_params.resources[1];
  const auto& res2 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index2);

  assert(!role::is_image(res1.role));
  assert(!role::is_image(res2.role));

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);

  vk::BufferCopy c{};
  c.size = std::min(res1.subbuf.size, res2.subbuf.size);
  c.srcOffset = res1.subbuf.offset;
  c.dstOffset = res2.subbuf.offset;
  task.copyBuffer(res1.buf, res2.buf, c);
}

static vk::ImageSubresourceLayers convert_to_isl(const vk::ImageSubresourceRange& isr, const uint32_t mip_level = UINT32_MAX) {
  vk::ImageSubresourceLayers isl{};
  isl.aspectMask = isr.aspectMask;
  isl.mipLevel = mip_level != UINT32_MAX ? mip_level : isr.baseMipLevel;
  isl.baseArrayLayer = isr.baseArrayLayer;
  isl.layerCount = isr.layerCount;
  return isl;
}

transfer_copy_image::transfer_copy_image(const uint32_t super) noexcept : transfer_step_instance(super) {}
void transfer_copy_image::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto& [res_index1, usage1] = step.cmd_params.resources[0];
  const auto& res1 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index1);

  const auto& [res_index2, usage2] = step.cmd_params.resources[1];
  const auto& res2 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index2);

  assert(role::is_image(res1.role));
  assert(role::is_image(res2.role));
  assert(res1.extent.x == res2.extent.x);
  assert(res1.extent.y == res2.extent.y);

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);

  vk::ImageCopy c{};
  c.srcSubresource = convert_to_isl(std::bit_cast<vk::ImageSubresourceRange>(res1.subimg));
  c.srcOffset = vk::Offset3D{0, 0, 0};
  c.dstSubresource = convert_to_isl(std::bit_cast<vk::ImageSubresourceRange>(res2.subimg));
  c.dstOffset = vk::Offset3D{0, 0, 0};
  // тут мы вынуждены копировать только подходящие картинки
  c.extent = vk::Extent3D{std::min(res1.extent.x, res2.extent.x), std::min(res1.extent.y, res2.extent.y), 1};
  task.copyImage(res1.img, vk::ImageLayout::eTransferSrcOptimal, res2.img, vk::ImageLayout::eTransferDstOptimal, c);
}

transfer_copy_buffer_image::transfer_copy_buffer_image(const uint32_t super) noexcept : transfer_step_instance(super) {}
void transfer_copy_buffer_image::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto& [res_index1, usage1] = step.cmd_params.resources[0];
  const auto& res1 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index1);

  const auto& [res_index2, usage2] = step.cmd_params.resources[1];
  const auto& res2 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index2);

  assert(!role::is_image(res1.role));
  assert(role::is_image(res2.role));

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);

  // если у нас буфер специально сделан по картинке,
  // то можно указать bufferRowLength и bufferImageHeight == 0
  vk::BufferImageCopy c{};
  c.bufferOffset = res1.subbuf.offset;
  c.bufferRowLength = 0;
  c.bufferImageHeight = 0;
  c.imageSubresource = convert_to_isl(std::bit_cast<vk::ImageSubresourceRange>(res2.subimg));
  c.imageOffset = vk::Offset3D{0, 0, 0};
  c.imageExtent = vk::Extent3D{res2.extent.x, res2.extent.y, 1};
  task.copyBufferToImage(res1.buf, res2.img, vk::ImageLayout::eTransferDstOptimal, c);
}

transfer_copy_image_buffer::transfer_copy_image_buffer(const uint32_t super) noexcept : transfer_step_instance(super) {}
void transfer_copy_image_buffer::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto& [res_index1, usage1] = step.cmd_params.resources[0];
  const auto& res1 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index1);

  const auto& [res_index2, usage2] = step.cmd_params.resources[1];
  const auto& res2 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index2);

  assert(role::is_image(res1.role));
  assert(!role::is_image(res2.role));

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);

  // если у нас буфер специально сделан по картинке,
  // то можно указать bufferRowLength и bufferImageHeight == 0
  vk::BufferImageCopy c{};
  c.bufferOffset = res2.subbuf.offset;
  c.bufferRowLength = 0;
  c.bufferImageHeight = 0;
  c.imageSubresource = convert_to_isl(std::bit_cast<vk::ImageSubresourceRange>(res1.subimg));
  c.imageOffset = vk::Offset3D{0, 0, 0};
  c.imageExtent = vk::Extent3D{res1.extent.x, res1.extent.y, 1};
  task.copyImageToBuffer(res1.img, vk::ImageLayout::eTransferSrcOptimal, res2.buf, c);
}

transfer_blit_linear::transfer_blit_linear(const uint32_t super) noexcept : transfer_step_instance(super) {}
void transfer_blit_linear::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto& [res_index1, usage1] = step.cmd_params.resources[0];
  const auto& res1 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index1);

  const auto& [res_index2, usage2] = step.cmd_params.resources[1];
  const auto& res2 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index2);

  assert(role::is_image(res1.role));
  assert(role::is_image(res2.role));

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);

  vk::ImageBlit b{};
  b.srcSubresource = convert_to_isl(std::bit_cast<vk::ImageSubresourceRange>(res1.subimg));
  b.srcOffsets[0] = vk::Offset3D{0, 0, 0};
  b.srcOffsets[1] = vk::Offset3D(res1.extent.x, res1.extent.y, 1);
  b.dstSubresource = convert_to_isl(std::bit_cast<vk::ImageSubresourceRange>(res2.subimg));
  b.dstOffsets[0] = vk::Offset3D{0, 0, 0};
  b.dstOffsets[1] = vk::Offset3D(res2.extent.x, res2.extent.y, 1);
  task.blitImage(res1.img, vk::ImageLayout::eTransferSrcOptimal, res2.img, vk::ImageLayout::eTransferDstOptimal, b, vk::Filter::eLinear);
}

transfer_blit_nearest::transfer_blit_nearest(const uint32_t super) noexcept : transfer_step_instance(super) {}
void transfer_blit_nearest::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto& [res_index1, usage1] = step.cmd_params.resources[0];
  const auto& res1 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index1);

  const auto& [res_index2, usage2] = step.cmd_params.resources[1];
  const auto& res2 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index2);

  assert(role::is_image(res1.role));
  assert(role::is_image(res2.role));

  vk::CommandBuffer task(buf);

  make_barriers1(ctx, buf, step.barriers);

  vk::ImageBlit b{};
  b.srcSubresource = convert_to_isl(std::bit_cast<vk::ImageSubresourceRange>(res1.subimg));
  b.srcOffsets[0] = vk::Offset3D{0, 0, 0};
  b.srcOffsets[1] = vk::Offset3D(res1.extent.x, res1.extent.y, 1);
  b.dstSubresource = convert_to_isl(std::bit_cast<vk::ImageSubresourceRange>(res2.subimg));
  b.dstOffsets[0] = vk::Offset3D{0, 0, 0};
  b.dstOffsets[1] = vk::Offset3D(res2.extent.x, res2.extent.y, 1);
  task.blitImage(res1.img, vk::ImageLayout::eTransferSrcOptimal, res2.img, vk::ImageLayout::eTransferDstOptimal, b, vk::Filter::eNearest);
}

transfer_clear_color::transfer_clear_color(const uint32_t super) noexcept : transfer_step_instance(super) {}
void transfer_clear_color::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto& [res_index, usage] = step.cmd_params.resources[0];
  const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, res_index);
  assert(role::is_image(res.role));

  // Цвет лежит в КОНСТАНТЕ (её можно обновлять извне через write_constant_data). Отличаем упаковку по
  // первому формату layout: v4 = 4×float32 (16Б, байты сразу ложатся в union) vs c4 = rgba8 (4Б unorm,
  // распаковываем каждый байт /255 в float). Тот же приём покрывает ui4/i4 (16Б memcpy в union).
  const uint32_t const_index = step.cmd_params.constants[0];
  const auto& c = DS_ASSERT_ARRAY_GET(ctx->base->constants, const_index);
  const auto* raw = static_cast<const uint8_t*>(ctx->base->get_constant_data(const_index));

  vk::ClearColorValue cv{};
  const format::values f0 = c.layout.empty() ? format::v4 : c.layout[0];
  const uint32_t vk_fmt = format::to_vulkan_format(f0);
  const uint32_t channels = std::min<uint32_t>(4u, format_channel_count(vk_fmt));
  const uint32_t elem = format_element_size(vk_fmt, format::to_vulkan_aspect(f0));
  const uint32_t scalar = channels != 0 ? elem / channels : 4u;
  if (scalar == 1) {
    for (uint32_t i = 0; i < channels; ++i) {
      cv.float32[i] = float(raw[i]) / 255.0f; // 8-бит unorm -> [0,1]
    }
  } else {
    memcpy(&cv, raw, std::min<size_t>(c.size, sizeof(cv))); // 32-бит: байты уже совпадают с union
  }

  vk::CommandBuffer task(buf);
  make_barriers1(ctx, buf, step.barriers); // перевод в transfer_dst
  const auto range = std::bit_cast<vk::ImageSubresourceRange>(res.subimg);
  task.clearColorImage(res.img, vk::ImageLayout::eTransferDstOptimal, cv, range);
}

transfer_clear_depth::transfer_clear_depth(const uint32_t super) noexcept : transfer_step_instance(super) {}
void transfer_clear_depth::process(graphics_ctx* ctx, VkCommandBuffer buf) const {
  const auto& step = DS_ASSERT_ARRAY_GET(ctx->base->steps, super);

  const auto& [res_index, usage] = step.cmd_params.resources[0];
  const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, res_index);
  assert(role::is_image(res.role));

  // Глубина/трафарет тоже из константы: первый float = depth, необязательный второй компонент = stencil.
  const uint32_t const_index = step.cmd_params.constants[0];
  const auto& c = DS_ASSERT_ARRAY_GET(ctx->base->constants, const_index);
  const auto* raw = static_cast<const uint8_t*>(ctx->base->get_constant_data(const_index));

  vk::ClearDepthStencilValue dsv{};
  dsv.depth = 1.0f;
  if (c.size >= sizeof(float)) {
    memcpy(&dsv.depth, raw, sizeof(float));
  }
  dsv.stencil = c.size >= sizeof(float) + sizeof(uint32_t)
                  ? *reinterpret_cast<const uint32_t*>(raw + sizeof(float))
                  : 0u;

  vk::CommandBuffer task(buf);
  make_barriers1(ctx, buf, step.barriers); // перевод в transfer_dst
  const auto range = std::bit_cast<vk::ImageSubresourceRange>(res.subimg);
  task.clearDepthStencilImage(res.img, vk::ImageLayout::eTransferDstOptimal, dsv, range);
}

} // namespace painter
} // namespace devils_engine
