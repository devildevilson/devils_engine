#include "execution_pass.h"

#include "vulkan_header.h"
#include "makers.h"
#include "auxiliary.h"

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/type_traits.h"
#include "devils_engine/utils/named_serializer.h"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/string-utils.hpp"
#include "gtl/phmap.hpp"

#include <filesystem>

namespace fs = std::filesystem;

namespace devils_engine {
namespace painter {



//void create_descriptor_layout(
//  const painter_base& ctx,
//  graphics_step_instance& inst
//) {
//  const auto& step = ctx.steps[inst.super];
//  
//  descriptor_set_layout_maker dslm(ctx.device);
//  for (uint32_t i = 0; i < step.local_resources.size(); ++i) {
//    const auto& res_data = step.local_resources[i];
//    const auto& res = ctx.resources[res_data.slot];
//    const uint32_t count = static_cast<uint32_t>(res.type);
//    dslm.binding(i, convertdt(res_data.next_usage), vk::ShaderStageFlagBits::eAll, count);
//  }
//  inst.local_layout = dslm.create(step.name + ".set_layout");
//}
//
//void create_descriptor_layout(
//  const painter_base& ctx,
//  compute_step_instance& inst
//) {
//  const auto& step = ctx.steps[inst.super];
//
//  descriptor_set_layout_maker dslm(ctx.device);
//  for (uint32_t i = 0; i < step.local_resources.size(); ++i) {
//    const auto& res_data = step.local_resources[i];
//    const auto& res = ctx.resources[res_data.slot];
//    const uint32_t count = static_cast<uint32_t>(res.type);
//    dslm.binding(i, convertdt(res_data.next_usage), vk::ShaderStageFlagBits::eAll, count);
//  }
//  inst.local_layout = dslm.create(step.name + ".set_layout");
//}

void create_pipeline_layout(
  const painter_base& ctx,
  graphics_step_instance& inst
) {
  const auto& step = ctx.steps[inst.super];

  pipeline_layout_maker plm(ctx.device);
  for (const auto descriptor_index : step.sets) {
    const auto& descriptor = ctx.descriptors[descriptor_index];
    plm.addDescriptorLayout(descriptor.setlayout);
  }

  size_t offset = 0;
  for (const auto constant_index : step.push_constants) {
    const auto& constant = ctx.constants[constant_index];
    plm.addPushConstRange(offset, constant.size, vk::ShaderStageFlagBits::eAll);
    offset += constant.size;
  }

  inst.pipeline_layout = plm.create(step.name + ".pipeline_layout");
}

void create_pipeline_layout(
  const painter_base& ctx,
  compute_step_instance& inst
) {
  const auto& step = ctx.steps[inst.super];

  pipeline_layout_maker plm(ctx.device);
  for (const auto descriptor_index : step.sets) {
    const auto& descriptor = ctx.descriptors[descriptor_index];
    plm.addDescriptorLayout(descriptor.setlayout);
  }

  size_t offset = 0;
  for (const auto constant_index : step.push_constants) {
    const auto& constant = ctx.constants[constant_index];
    plm.addPushConstRange(offset, constant.size, vk::ShaderStageFlagBits::eAll);
    offset += constant.size;
  }

  inst.pipeline_layout = plm.create(step.name + ".pipeline_layout");
}

void create_pipeline(
  const painter_base& ctx,
  graphics_step_instance& inst
) {
  const auto& step = ctx.steps[inst.super];

  pipeline_maker pm(ctx.device);

  const auto& material = ctx.materials[step.material];

  {
    vk::UniqueShaderModule usm_vertex;
    vk::UniqueShaderModule usm_fragment;

    // тут шейдеры
    const auto shaders_path = utils::project_folder() + "shaders/";
    if (!material.shaders.vertex.empty()) {
      const auto full_path = shaders_path + material.shaders.vertex;
      const auto content = file_io::read<uint8_t>(full_path);
      // а как по старинке то теперь загружать =(
      vk::ShaderModuleCreateInfo smci{};
      smci.codeSize = content.size();
      smci.pCode = reinterpret_cast<const uint32_t*>(content.data());
      vk::Device dev(ctx.device);
      usm_vertex = dev.createShaderModuleUnique(smci);
    }

    if (!material.shaders.vertex.empty()) {
      const auto full_path = shaders_path + material.shaders.fragment;
      const auto content = file_io::read<uint8_t>(full_path);

      vk::ShaderModuleCreateInfo smci{};
      smci.codeSize = content.size();
      smci.pCode = reinterpret_cast<const uint32_t*>(content.data());
      vk::Device dev(ctx.device);
      usm_fragment = dev.createShaderModuleUnique(smci);
    }

    pm.addShader(vk::ShaderStageFlagBits::eVertex, usm_vertex.get());
    pm.addShader(vk::ShaderStageFlagBits::eFragment, usm_fragment.get());
  }

  const auto& geo = ctx.geometries[step.geometry];
  if (geo.stride != 0) {
    pm.vertexBinding(0, geo.stride, vk::VertexInputRate::eVertex);
    size_t offset = 0;
    for (uint32_t i = 0; i < geo.vertex_layout.size(); ++i) {
      const auto& f = geo.vertex_layout[i];
      const auto format = static_cast<vk::Format>(f);
      const auto& fmt_data = format_element_size(f);
      pm.vertexAttribute(i, 0, format, offset);
      offset += fmt_data;
    }
  }

  const auto& draw_group = ctx.draw_groups[step.draw_group];
  if (draw_group.stride != 0) {
    pm.vertexBinding(1, draw_group.stride, vk::VertexInputRate::eInstance);
    size_t offset = 0;
    for (uint32_t i = 0; i < draw_group.instance_layout.size(); ++i) {
      const auto f = draw_group.instance_layout[i];
      const auto format = static_cast<vk::Format>(f);
      const auto& fmt_data = format_element_size(f);
      pm.vertexAttribute(i, 1, format, offset);
      offset += fmt_data;
    }
  }

  // нужно явно прописать это дело в шагах
  pm.dynamicState(vk::DynamicState::eViewport);
  pm.dynamicState(vk::DynamicState::eScissor);

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
  pm.blendConstant(nullptr);

  const auto& rt = ctx.render_targets[inst.render_target_index];
  auto blending = rt.default_blending;
  for (const auto& [ res_index, blend ] : step.blending) {
    const uint32_t id = rt.resource_index(res_index);
    const auto& res = ctx.resources[res_index];
    if (id >= rt.resources.size()) utils::error{}("Could not find resource '{}' among render target '{}' resources", res.name, rt.name);
    blending[id] = blend;
  }

  for (const auto& b : blending) {
    pm.colorBlending(std::bit_cast<vk::PipelineColorBlendAttachmentState>(b));
  }

  auto pipe = pm.create(
    step.name + ".pipeline",
    VK_NULL_HANDLE, // это из контекста
    inst.pipeline_layout,
    inst.renderpass,
    inst.subpass_index,
    inst.pipeline
  );

  if (inst.pipeline != VK_NULL_HANDLE) {
    vk::Device(ctx.device).destroy(inst.pipeline);
  }

  inst.pipeline = pipe;
}

void create_pipeline(
  const painter_base& ctx,
  compute_step_instance& inst
) {
  // компут пайплайн гораздо проще
}

//void create_descriptor_set(
//  const painter_base& ctx,
//  graphics_step_instance& inst
//) {
//  const auto& step = ctx.steps[inst.super];
//  descriptor_set_maker dsm(ctx.device);
//  inst.local_set = dsm.layout(inst.local_layout).create(ctx.descriptor_pool, step.name + ".set")[0];
//}
//
//void create_descriptor_set(
//  const painter_base& ctx,
//  compute_step_instance& inst
//) {
//  const auto& step = ctx.steps[inst.super];
//  descriptor_set_maker dsm(ctx.device);
//  inst.local_set = dsm.layout(inst.local_layout).create(ctx.descriptor_pool, step.name + ".set")[0];
//}

void create_render_pass(
  const painter_base& ctx,
  execution_pass_instance& inst
) {
  const auto& pass = ctx.passes[inst.super];
  if (pass.render_target == UINT32_MAX) return;
  const auto& rt = ctx.render_targets[pass.render_target];

  const auto& start = pass.subpasses.front();
  const auto& finish = pass.subpasses.back();

  render_pass_maker rpm(ctx.device);
  for (uint32_t i = 0; i < rt.resources.size(); ++i) {
    const auto& [ res_index, type ] = rt.resources[i];
    const auto& res = ctx.resources[res_index];
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
  for (uint32_t index = 1; index < pass.subpasses.size()-1; ++index, ++subpass_index) {
    const auto& sub = pass.subpasses[index];
    rpm.subpassBegin(vk::PipelineBindPoint::eGraphics);
    for (uint32_t i = 0; i < sub.size(); ++i) {
      const auto& info = sub[i];
      const auto& [ slot, type ] = rt.resources[i];
      const auto& res = ctx.resources[slot];
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
        if (is_depth) layout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
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
      const auto& sub = pass.subpasses[index-1];
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

  inst.renderpass = rpm.create(pass.name + ".renderpass");
}

// отдельный конверт для целого рендер графа + там сразу всякие штуки поделаем
// да но при этом ничего пока не создаем
// так будет удобнее потом убрать все ресурсы

using resource_instances_arr = std::span<const resource_instance>;
using draw_command_fn = std::function<void(const painter_base&, const resource_instances_arr&, VkCommandBuffer)>;

// после парсинга возвращаем указатель на степ?
// создадим просто несколько разных функций процесс
// предраспарсить и добавить использование ресурса в данные шага
std::function<void(VkCommandBuffer)> parse_command(const painter_base& ctx, const uint32_t draw_group_index, const std::string_view &str, const std::string_view& pass_name, const std::string_view& step_name) {
  // че с командой делать? ее может быть несколько видов
  // draw, dispatch, draw constant *name*, draw indirect *name*, dispatch constant *name*, dispatch indirect *name*,
  // draw *цифра* *цифра* *цифра* *цифра* *цифра* - просто цифры плохая идея, лучше заберем из константы
  // draw *имя трюка* - поди константа оверкилл, но очень редко может пригодиться
  // copy *name* *name*, blit *name* *name*, clear *name* *constant*
  // draw - возьмет данные из драв группы

  std::array<std::string_view, 8> arr;
  const size_t count = utils::string::split2(utils::string::trim(str), " ", arr.data(), arr.size());
  if (count > 4) utils::error{}("Could not parse command '{}' for pass '{}' step '{}': too many args", str, pass_name, step_name);

  if (
    arr[0] != "draw" &&
    arr[0] != "dispatch" &&
    arr[0] != "copy" &&
    arr[0] != "blit" &&
    arr[0] != "clear"
  ) utils::error{}("Could not parse command '{}' for pass '{}' step '{}': invalid base command '{}'", str, pass_name, step_name, arr[0]);

  const bool is_execution_command = arr[0] == "draw" || arr[0] == "dispatch";
  const bool is_copy_command = arr[0] == "copy" || arr[0] == "blit";

  uint32_t constant_index = UINT32_MAX;
  uint32_t indirect_index = UINT32_MAX;
  if (is_execution_command && arr[1] == "constant") {
    if (arr[2].empty()) utils::error{}("Could not parse command '{}' for pass '{}' step '{}': constant name expected", str, pass_name, step_name);
    constant_index = check(ctx.find_constant(arr[2]), "constant", arr[2], pass_name);
  }

  if (is_execution_command && arr[1] == "indirect") {
    if (arr[2].empty()) utils::error{}("Could not parse command '{}' for pass '{}' step '{}': indirect name expected", str, pass_name, step_name);
    indirect_index = check(ctx.find_resource(arr[2]), "resource", arr[2], pass_name);
    const auto& res = ctx.resources[indirect_index];
    //res.role - проверим чтобы это была индирект роль
  }

  uint32_t res1 = UINT32_MAX;
  uint32_t res2 = UINT32_MAX;
  if (is_copy_command) {
    // ожидаем два валидных ресурса + было бы неплохо чтобы они совпали по другим характеристикам тоже
    if (arr[2].empty()) utils::error{}("Could not parse command '{}' for pass '{}' step '{}': resource1 name expected", str, pass_name, step_name);
    if (arr[3].empty()) utils::error{}("Could not parse command '{}' for pass '{}' step '{}': resource2 name expected", str, pass_name, step_name);

    res1 = check(ctx.find_resource(arr[2]), "resource", arr[2], pass_name);
    res2 = check(ctx.find_resource(arr[3]), "resource", arr[3], pass_name);
  }

  uint32_t clear_res = UINT32_MAX;
  uint32_t clear_const = UINT32_MAX;
  uint32_t special_case = 0;
  if (arr[0] == "clear") {
    // нужно найти ресурс + константу или вместо константы можем поставить 0, 1, 9?
    // 0 и 1 - очевидно что такое, а 9 пусть будет пометкой что мы хотим везде f

    if (arr[2].empty()) utils::error{}("Could not parse command '{}' for pass '{}' step '{}': resource1 name expected", str, pass_name, step_name);
    clear_res = check(ctx.find_resource(arr[2]), "resource", arr[2], pass_name);

    if (!arr[3].empty()) {
      if (arr[3] == "0") special_case = 0;
      else if (arr[3] == "1") special_case = 1;
      else if (arr[3] == "9") special_case = 9;
      else {
        clear_const = check(ctx.find_constant(arr[3]), "constant", arr[3], pass_name);
      }
    }
  }

  // пригодятся барьеры для некоторых ресурсов

  std::function<void(VkCommandBuffer)> fn;
  if (arr[0] == "draw") {
    if (constant_index != UINT32_MAX) {
      fn = [constant_index](const painter_base &ctx, VkCommandBuffer b){
        const auto& cons = ctx.constants[constant_index];
        const void* ptr = nullptr;

        // как мы понимаем какие данные мы берем? желательно задать отдельный формат для константы
        // который draw4 и indexed5

        // забираем данные из константы, для этого контекст нужен
        // в этом случае откуда брать вершины, индексы и инстансы? из геометрии
        // в геометрии могут быть не указаны вершины, индексы и инстансы (бывает)
        // но основной источник - это геометрия
        vk::CommandBuffer task(b);
        task.draw();
      };
    } else if (indirect_index != UINT32_MAX) {
      fn = [indirect_index](VkCommandBuffer b){
        // передадим индирект буфер, как мы поймем какой это буфер? 
        // по описанию, в геометрии сразу понятно есть/нету индексов
        // в отдельном буфере укажем роль
        vk::CommandBuffer task(b);
        task.draw();
      };
    } else {
      fn = [draw_group_index](VkCommandBuffer b){
        // прочитаем текущую дравгруппу, дравгруппа это объект который мы создадим паралельно шагу рендера
        // он может быть унаследован если наследуется связка [геометрия,материал]
        // в моем случае я часто рисую без вершинных буферов... ну даже если так биндинг останется

        // заберем геометрию и выясним ее свойства
        // биндить ничего здесь не будем? да, только дадим данные для отрисовщика
        // что нам нужно понять? тип индирект отрисовки и получить текущий буфер

        vk::CommandBuffer task(b);
        task.draw();
      };
    }
  } else if (arr[0] == "dispatch") {
    if (constant_index != UINT32_MAX) {
      fn = [constant_index](VkCommandBuffer b){
        // забираем данные из константы, для этого контекст нужен
        vk::CommandBuffer task(b);
        task.dispatch();
      };
    } else if (indirect_index != UINT32_MAX) {
      fn = [indirect_index](VkCommandBuffer b){
        // передадим индирект буфер
        vk::CommandBuffer task(b);
        task.dispatch();
      };
    }
    // драв группа?
  } else if (arr[0] == "copy") {
    fn = [res1, res2](VkCommandBuffer b){
      // тут придется составить команду копирования
      // может быть картинка -> буфер, буфер -> картинка, картинка -> картинка, буфер -> буфер
    };
  } else if (arr[0] == "blit") {
    fn = [res1, res2](VkCommandBuffer b){
      // тут вообще много чего может произойти, нужна структура для блита дополнительная
    };
  } else if (arr[0] == "clear") {
    // надо еще проверять что перед нами: цветная картинка или глубина
    if (clear_const != UINT32_MAX) {
      fn = [clear_res, clear_const](VkCommandBuffer b) {
        // заберем данные из константы
        vk::CommandBuffer task(b);
        
      };
    } else if (special_case == 0) {
      fn = [clear_res](VkCommandBuffer b) {
        vk::CommandBuffer task(b);
        vk::ClearColorValue val({ 0.0f, 0.0f, 0.0f, 0.0f });

      };
    } else if (special_case == 1) {
      fn = [clear_res](VkCommandBuffer b) {
        vk::CommandBuffer task(b);
        vk::ClearColorValue val({ 1.0f, 1.0f, 1.0f, 1.0f });

      };
    } else if (special_case == 9) {
      fn = [clear_res](VkCommandBuffer b) {
        vk::CommandBuffer task(b);
        const uint32_t max = UINT32_MAX;
        vk::ClearColorValue val({ max, max, max, max });
        
      };
    }
  }

  return fn;
}

static void parse_start_block(
  const pass_mirror& pass,
  painter_base& ctx, 
  execution_pass &ctx_pass,
  std::vector<execution_pass::resource_info> &current_resource_states
) {
  const bool is_compute_pass = ctx_pass.render_target == UINT32_MAX;

  std::array<uint32_t, 32> atts = {};
  memset(atts.data(), 0, sizeof(atts));

  if (!is_compute_pass) {
    const auto& rt = ctx.render_targets[ctx_pass.render_target];
    ctx_pass.start_attachments.resize(rt.resources.size());
  }

  for (const auto& [name, data] : pass.start) {
    const auto& [usage_str, store_op_str] = data;
    const uint32_t index = check(ctx.find_resource(name), "resource", name, pass.name);
    const auto cur_usage = check(usage::from_string(usage_str), usage_str, pass.name);
    const auto cur_action = check(store_op::from_string(store_op_str), store_op_str, pass.name);

    const auto res_info = execution_pass::resource_info{ index, cur_usage, cur_action };

    if (!is_compute_pass) {
      const auto& rt = ctx.render_targets[ctx_pass.render_target];
      const uint32_t id = rt.resource_index(index);
      if (id < rt.resources.size()) {
        ctx_pass.start_attachments[id] = res_info;
        atts[id] = index;
      } else {
        ctx_pass.start.push_back(res_info);
      }
    } else {
      ctx_pass.start.push_back(res_info);
    }

    current_resource_states[index] = res_info;
  }

  if (!is_compute_pass) {
    const auto& rt = ctx.render_targets[ctx_pass.render_target];
    for (size_t i = 0; i < rt.resources.size(); ++i) {
      if (atts[i] != 0) continue;

      std::string attachment_names;
      for (const auto id : rt.resources) {
        attachment_names += "'" + ctx.resources[id].name + "' ";
      }

      std::string provided_attachment_names;
      for (size_t j = 0; j < rt.resources.size(); ++j) {
        if (atts[j] == 0) continue;
        provided_attachment_names += "'" + ctx.resources[atts[j]].name + "' ";
      }

      utils::error{}("Execution pass {} 'start' block expects explicit declaration of all attachment usage\nExpected : {}\nProvided: {}", ctx_pass.name, attachment_names, provided_attachment_names);
    }
  }
}

static void parse_finish_block(
  const pass_mirror& pass,
  painter_base& ctx,
  execution_pass& ctx_pass,
  std::vector<execution_pass::resource_info>& current_resource_states
) {
  const bool is_compute_pass = ctx_pass.render_target == UINT32_MAX;

  std::array<uint32_t, 32> atts;
  memset(atts.data(), 0, sizeof(atts));

  if (!is_compute_pass) {
    const auto& rt = ctx.render_targets[ctx_pass.render_target];
    ctx_pass.finish_attachments.resize(rt.resources.size());
  }

  for (const auto& [name, data] : pass.finish) {
    const auto& [usage_str, store_op_str] = data;
    const uint32_t index = check(ctx.find_resource(name), "resource", name, pass.name);
    const auto cur_usage = check(usage::from_string(usage_str), usage_str, pass.name);
    const auto cur_action = check(store_op::from_string(store_op_str), store_op_str, pass.name);

    const auto res_info = execution_pass::resource_info{ index, cur_usage, cur_action };

    if (!is_compute_pass) {
      const auto& rt = ctx.render_targets[ctx_pass.render_target];
      const uint32_t id = rt.resource_index(index);
      if (id < rt.resources.size()) {
        ctx_pass.finish_attachments[id] = res_info;
        atts[id] = index;
      }
    }

    //  && current_resource_states[index].slot != 0
    if (!usage::is_attachment(cur_usage)) {
      ctx_pass.finish_barriers.push_back(
        execution_pass::step::barrier{ index, current_resource_states[index].usage, cur_usage }
      );
    }

    current_resource_states[index] = res_info;
  }

  if (!is_compute_pass) {
    const auto& rt = ctx.render_targets[ctx_pass.render_target];
    for (size_t i = 0; i < rt.resources.size(); ++i) {
      if (atts[i] != 0) continue;

      std::string attachment_names;
      for (const auto id : rt.resources) {
        attachment_names += "'" + ctx.resources[id].name + "' ";
      }

      std::string provided_attachment_names;
      for (size_t j = 0; j < rt.resources.size(); ++j) {
        if (atts[j] == 0) continue;
        provided_attachment_names += "'" + ctx.resources[atts[j]].name + "' ";
      }

      utils::error{}("Execution pass {} 'finish' block expects explicit declaration of all attachment usage\nExpected : {}\nProvided: {}", ctx_pass.name, attachment_names, provided_attachment_names);
    }
  }
}

static void parse_step_block(
  const pass_step_mirror &step,
  painter_base& ctx,
  execution_pass& ctx_pass,
  execution_pass::step& cur_step,
  std::vector<execution_pass::resource_info>& current_resource_states
) {
  const bool is_compute_pass = ctx_pass.render_target == UINT32_MAX;

  const size_t current_step_index = ctx_pass.steps.empty() ? 0 : ctx_pass.steps.size() - 1;

  cur_step.name = step.name;

  if (!is_compute_pass && current_step_index == 0 && step.attachments.empty()) {
    utils::error{}("Provide attachments for the first render pass step, pass name: '{}', step name: {}", ctx_pass.name, cur_step.name);
  }

  if (is_compute_pass && (!step.attachments.empty() || !step.blending.empty())) {
    utils::warn("Attachments arent neccessary for compute pass, pass name: '{}', step name: {}", ctx_pass.name, cur_step.name); 
  }

  if (!is_compute_pass && !step.attachments.empty()) {
    const auto& rt = ctx.render_targets[ctx_pass.render_target];
    cur_step.attachments.resize(rt.resources.size(), execution_pass::resource_info{});
    cur_step.blending.resize(rt.resources.size());
  }

  for (const auto& [att, data] : step.attachments) {
    const auto& rt = ctx.render_targets[ctx_pass.render_target];

    const auto& [usage_str, store_op_str] = data;
    const uint32_t index = check(ctx.find_resource(att), "resource", att, step.name);
    const uint32_t id = rt.resource_index(index);
    if (id >= rt.resources.size()) continue;

    execution_pass::resource_info a = {};
    a.slot = index;
    a.usage = check(usage::from_string(usage_str), usage_str, step.name);
    a.action = check(store_op::from_string(store_op_str), store_op_str, step.name);
    cur_step.attachments[id] = a;

    current_resource_states[a.slot].usage = a.usage;
    current_resource_states[a.slot].action = a.action;
  }

  for (const auto& att : cur_step.attachments) {
    if (att.slot != INVALID_RESOURCE_SLOT) continue;

    const auto& rt = ctx.render_targets[ctx_pass.render_target];

    std::string attachment_names;
    for (const auto id : rt.resources) {
      attachment_names += "'" + ctx.resources[id].name + "' ";
    }

    std::string provided_attachment_names;
    for (const auto& att : cur_step.attachments) {
      if (att.slot == INVALID_RESOURCE_SLOT) continue;
      provided_attachment_names += "'" + ctx.resources[att.slot].name + "' ";
    }

    utils::error{}("Execution pass {} step {} expects explicit attachment usage declaration\nExpected : {}\nProvided: {}", ctx_pass.name, cur_step.name, attachment_names, provided_attachment_names);
  }


  for (const auto &[att, blend_data] : step.blending) {
    const auto& rt = ctx.render_targets[ctx_pass.render_target];
    const uint32_t index = check(ctx.find_resource(att), "resource", att, step.name);
    const uint32_t id = rt.resource_index(index);
    if (id >= rt.resources.size()) continue;

    auto& bd = cur_step.blending[id];
    bd.enable = blend_data.enable;
    const auto& [srcColorBlendFactor, colorBlendOp, dstColorBlendFactor] = parse_blend_exp(blend_data.color);
    const auto& [srcAlphaBlendFactor, alphaBlendOp, dstAlphaBlendFactor] = parse_blend_exp(blend_data.alpha);
    bd.srcColorBlendFactor = srcColorBlendFactor;
    bd.colorBlendOp = colorBlendOp;
    bd.dstColorBlendFactor = dstColorBlendFactor;
    bd.srcAlphaBlendFactor = srcAlphaBlendFactor;
    bd.alphaBlendOp = alphaBlendOp;
    bd.dstAlphaBlendFactor = dstAlphaBlendFactor;
    if (blend_data.mask.empty()) bd.colorWriteMask = UINT32_MAX;
    for (const auto c : blend_data.mask) {
      if (c == 'r') bd.colorWriteMask = bd.colorWriteMask | VK_COLOR_COMPONENT_R_BIT;
      if (c == 'g') bd.colorWriteMask = bd.colorWriteMask | VK_COLOR_COMPONENT_G_BIT;
      if (c == 'b') bd.colorWriteMask = bd.colorWriteMask | VK_COLOR_COMPONENT_B_BIT;
      if (c == 'a') bd.colorWriteMask = bd.colorWriteMask | VK_COLOR_COMPONENT_A_BIT;
    }
  }

  for (const auto& [name, usage_str] : step.barriers) {
    const uint32_t index = ctx.find_resource(name);
    const auto usage = check(usage::from_string(usage_str), usage_str, step.name);
    /*if (current_resource_states[index].slot != INVALID_RESOURCE_SLOT) {
      cur_step.barriers.push_back(execution_pass::step::barrier{ index, current_resource_states[index].usage, usage });
    }*/

    current_resource_states[index].slot = index;
    current_resource_states[index].usage = usage;
    current_resource_states[index].action = store_op::store;
  }

  for (const auto& [name, usage_str] : step.resources) {
    const uint32_t index = check(ctx.find_resource(name), "resource", name, step.name);
    const auto usage = check(usage::from_string(usage_str), usage_str, step.name);
    //if (current_resource_states[index].slot != INVALID_RESOURCE_SLOT) {
    //  cur_step.barriers.push_back(execution_pass::step::barrier{ index, current_resource_states[index].usage, usage });
    //}

    current_resource_states[index].slot = index;
    current_resource_states[index].usage = usage;
    current_resource_states[index].action = store_op::store;
  }

  for (const auto& name : step.sets) {
    const uint32_t index = name == "local" ? UINT32_MAX : check(ctx.find_descriptor(name), "descriptor", name, step.name);
    cur_step.sets.push_back(index);
  }

  for (const auto& name : step.push_constants) {
    const uint32_t index = check(ctx.find_constant(name), "constant", name, step.name);
    cur_step.push_constants.push_back(index);
  }

  if (!step.material.empty()) cur_step.material = check(ctx.find_material(step.material), "material", step.material, step.name);
  if (!step.geometry_instance.empty()) cur_step.geometry_instance = check(ctx.find_geometry_instance(step.geometry_instance), "geometry_instance", step.geometry_instance, step.name);

  cur_step.command = step.command;

  // парсим команду
}

// так у нас еще может быть компут шейдер, как мы это понимаем? отсутствует рендер таргет?
void convert_render_graph(const render_graph_mirror &data, painter_base &ctx) {
  std::vector<execution_pass::resource_info> current_resource_states(ctx.resources.size());
  
  // препасс чтобы предсоздать ресурсы draw group? где хранится максимум инстансов?
  // ощущение такое что должен быть вообще в префабе где то...
  // 

  ctx.render_graph.present_source = UINT32_MAX;
  if (!data.present_source.empty()) ctx.render_graph.present_source = check(ctx.find_resource(data.present_source), "resource", data.present_source);
  for (const auto &pass : data.passes) {
    ctx.render_graph.passes.emplace_back();
    auto& ctx_pass = ctx.render_graph.passes.back();
    ctx_pass.name = pass.name;
    // если рендер таргет не задан, то предполагаем что это компут пасс? имеет смысл
    ctx_pass.render_target = UINT32_MAX;
    if (!pass.render_target.empty()) ctx_pass.render_target = check(ctx.find_render_target(pass.render_target), "render_target", pass.render_target, pass.name);
    const bool is_compute_pass = ctx_pass.render_target == UINT32_MAX;

    parse_start_block(pass, ctx, ctx_pass, current_resource_states);

    for (const auto &step : pass.steps) {
      ctx_pass.steps.emplace_back();
      parse_step_block(step, ctx, ctx_pass, ctx_pass.steps.back(), current_resource_states);
    }

    parse_finish_block(pass, ctx, ctx_pass, current_resource_states);

    // чистим current_resource_states ? или провалидировать следующие ресурсы?
    // в целом ресурсы толком провалидировать не выйдет - один пасс один сабмит
    // в следующий сабмит непонятно как и что придет... точнее должно быть понятно 
    // самому вулкану, но не парсеру в текущем виде, поэтому да наверное имеет смысл почистить
    // не чистим, зачем?
    //current_resource_states.clear();
    //current_resource_states.resize(ctx.resources.size());
  }

  // так положим мы распарсили все что хотели в графе, теперь нужно создать ресурсы
  // создавать начинаем с рендер паса, потом дескриптор лайауты, потом пайплайны
}

// дальше мы как раз создадим сначала граф
// а потом наконец сами ресурсы

void create_render_graph(painter_base& ctx) {
  using ri = execution_pass::resource_info;

  std::vector<ri> current_resource_states(ctx.resources.size());
  std::vector<vk::BufferUsageFlags> current_resource_buffer_usages(ctx.resources.size());
  std::vector<vk::ImageUsageFlags> current_resource_image_usages(ctx.resources.size());

  for (auto &pass : ctx.render_graph.passes) {
    render_pass_maker rpm(ctx.device);

    uint32_t current_pass = VK_SUBPASS_EXTERNAL;

    // источник состояний, заставить прописывать? пока что это самый простой выход из положения
    // все что не попадает сюда считается undefined
    for (const auto &res : pass.start) {
      current_resource_states[res.slot] = res;
      const auto& bf = convertbuf(res.usage);
      const auto& imf = convertiuf(res.usage);
      current_resource_buffer_usages[res.slot] = current_resource_buffer_usages[res.slot] | bf;
      current_resource_image_usages[res.slot] = current_resource_image_usages[res.slot] | imf;
    }

    for (uint32_t i = 0; i < pass.start_attachments.size(); ++i) {
      const auto& start = pass.start_attachments[i];
      const auto& finish = pass.finish_attachments[i];

      if (start.slot != finish.slot) utils::error{}("Error when parsing start/finish attachments, pass: {}", pass.name);

      const auto& res = ctx.resources[start.slot];
      rpm.attachmentBegin(static_cast<vk::Format>(res.format_hint));

      rpm.attachmentInitialLayout(convertil(start.usage));
      rpm.attachmentFinalLayout(convertil(finish.usage));

      rpm.attachmentLoadOp(convertl(start.action));
      rpm.attachmentStoreOp(converts(finish.action));

      // для стенсила тож бы надо отдельно сделать эту штуку
      rpm.attachmentStencilLoadOp(convertl(start.action));
      rpm.attachmentStencilStoreOp(converts(finish.action));

      current_resource_states[start.slot] = start;
    }

    vk::AccessFlags src_mask{};
    vk::AccessFlags dst_mask{};
    vk::PipelineStageFlags src_stage{};
    vk::PipelineStageFlags dst_stage{};

    for (const auto& step : pass.steps) {
      // соберем аттачменты
      for (uint32_t i = 0; i < step.attachments.size(); ++i) {
        const auto& att = step.attachments[i];
        const auto& prev_state = current_resource_states[att.slot];

        const auto& cur_l = convertil(att.usage);

        rpm.subpassBegin();
        if (att.usage == usage::color_attachment) {
          rpm.subpassColorAttachment(i, cur_l);
        }

        if (att.usage == usage::depth_attachment) {
          rpm.subpassDepthStencilAttachment(i, cur_l);
        }

        if (att.usage == usage::input_attachment) {
          rpm.subpassInputAttachment(i, cur_l);
        }

        // resolve

        src_mask = src_mask | convertam(prev_state.usage);
        dst_mask = dst_mask | convertam(att.usage);

        src_stage = src_stage | convertps(prev_state.usage);
        dst_stage = dst_stage | convertps(att.usage);

        current_resource_states[att.slot] = att;
        const auto& bf = convertbuf(att.usage);
        const auto& imf = convertiuf(att.usage);
        current_resource_buffer_usages[att.slot] = current_resource_buffer_usages[att.slot] | bf;
        current_resource_image_usages[att.slot] = current_resource_image_usages[att.slot] | imf;
      }

      // депенденси

      if (!step.attachments.empty()) {
        rpm.dependencyBegin(current_pass, current_pass + 1);
        rpm.dependencySrcAccessMask(src_mask);
        rpm.dependencyDstAccessMask(dst_mask);
        rpm.dependencySrcStageMask(src_stage);
        rpm.dependencyDstStageMask(dst_stage);
        rpm.dependencyDependencyFlags(vk::DependencyFlagBits::eByRegion);

        src_mask = {};
        dst_mask = {};
        src_stage = {};
        dst_stage = {};

        current_pass += 1;
        if (step.subpass_index != current_pass) utils::error{}("Check subpass indices");
      }

      // соберем юсаджи
      for (const auto& bar : step.barriers) {
        current_resource_states[bar.slot] = ri{ bar.slot, bar.next_usage, store_op::store };
        const auto& bf = convertbuf(bar.next_usage);
        const auto& imf = convertiuf(bar.next_usage);
        current_resource_buffer_usages[bar.slot] = current_resource_buffer_usages[bar.slot] | bf;
        current_resource_image_usages[bar.slot] = current_resource_image_usages[bar.slot] | imf;
      }

      // а local_resources генерирует барьер? вообще я бы сказал что да
      // ну правильно, барьеры генерируют все упоминания ресурсов
      // но для local_resources мы уже добавили их в список баррьеров
      // чувствую что нужно будет зафорсить барьер из непонятного стейта в понятный в любом случае
      // ну то есть для промежуточных буферов это норм, 
      // а если нужно прочитать данные с предыдущего кадра это не норм
      // предыдущий кадр по идее будет следующим по индексу в дескрипторах
      // и мы его трогать вообще не будем
      // нет у нас есть ресурсы per_update и нужно примерно гарантировать 
      // что без специального упоминания они будут сохранятся между кадрами
      // значит когда нам это нужно? когда ресурс пер_апдейт используется в дескрипторе
      // блен неужели хранить состояния ресурсов и на этой основе барьеры расставлять...
      // 
      
    }

    for (const auto& att : pass.finish_attachments) {
      const auto& prev_state = current_resource_states[att.slot];

      src_mask = src_mask | convertam(prev_state.usage);
      dst_mask = dst_mask | convertam(att.usage);

      src_stage = src_stage | convertps(prev_state.usage);
      dst_stage = dst_stage | convertps(att.usage);

      current_resource_states[att.slot] = att;
      const auto& bf = convertbuf(att.usage);
      const auto& imf = convertiuf(att.usage);
      current_resource_buffer_usages[att.slot] = current_resource_buffer_usages[att.slot] | bf;
      current_resource_image_usages[att.slot] = current_resource_image_usages[att.slot] | imf;
    }

    rpm.dependencyBegin(current_pass, VK_SUBPASS_EXTERNAL);
    rpm.dependencySrcAccessMask(src_mask);
    rpm.dependencyDstAccessMask(dst_mask);
    rpm.dependencySrcStageMask(src_stage);
    rpm.dependencyDstStageMask(dst_stage);
    rpm.dependencyDependencyFlags(vk::DependencyFlagBits::eByRegion);

    // сделаем рендер пасс
    pass.render_pass = rpm.create(pass.name);

    for (const auto& bar : pass.finish_barriers) {
      current_resource_states[bar.slot] = ri{ bar.slot, bar.next_usage, store_op::store };
      const auto& bf = convertbuf(bar.next_usage);
      const auto& imf = convertiuf(bar.next_usage);
      current_resource_buffer_usages[bar.slot] = current_resource_buffer_usages[bar.slot] | bf;
      current_resource_image_usages[bar.slot] = current_resource_image_usages[bar.slot] | imf;
    }
  }

  for (size_t i = 0; i < ctx.resources.size(); ++i) {
    uint32_t mask = 0;
    if (role::is_image(ctx.resources[i].role)) {
      mask = static_cast<uint32_t>(current_resource_image_usages[i]);
    } else {
      mask = static_cast<uint32_t>(current_resource_buffer_usages[i]);
    }

    ctx.resources[i].usage_mask = mask;
  }

  uint32_t material_index = UINT32_MAX;
  uint32_t geometry_index = UINT32_MAX;

  // придется вынести в отдельный шаг... короче говоря ДО дескрипторов
  for (auto& pass : ctx.render_graph.passes) {
    for (auto& step : pass.steps) {
      const bool same_material = step.material == UINT32_MAX || material_index == step.material;
      const bool same_geometry = step.geometry_instance == UINT32_MAX || geometry_index == step.geometry_instance;

      if (step.material != UINT32_MAX) material_index = step.material;
      if (step.geometry_instance != UINT32_MAX) geometry_index = step.geometry_instance;

      // создадим сет лайоут
      {
        descriptor_set_layout_maker dslm(ctx.device);
        for (uint32_t i = 0; i < step.local_resources.size(); ++i) {
          const auto& local_res = step.local_resources[i];
          const auto& res = ctx.resources[local_res.slot];
          const auto dt = convertdt(local_res.next_usage);
          const uint32_t buffers_count = static_cast<uint32_t>(res.type);
          dslm.binding(i, dt, vk::ShaderStageFlagBits::eAll, buffers_count);
        }
        step.local_layout = dslm.create(pass.name + "." + step.name + ".set_layout");
      }

      // пиплин лайоут
      {
        pipeline_layout_maker plm(ctx.device);
        for (const auto& desc_index : step.sets) {
          auto l = desc_index == UINT32_MAX ? step.local_layout : ctx.descriptors[desc_index].setlayout;
          plm.addDescriptorLayout(l);
        }

        size_t offset = 0;
        for (const auto& pc_index : step.push_constants) {
          const auto& pc = ctx.constants[pc_index];
          plm.addPushConstRange(offset, pc.size, vk::ShaderStageFlagBits::eAll);
          offset += pc.size;
        }
        step.pipeline_layout = plm.create(pass.name + "." + step.name + ".pipeline_layout");
      }

      // создадим пиплин (сложно)
      {
        pipeline_maker pm(ctx.device);

        const auto& material = ctx.materials[step.material];

        vk::UniqueShaderModule usm_vertex;
        vk::UniqueShaderModule usm_fragment;
        
        // тут шейдеры
        const auto shaders_path = utils::project_folder() + "shaders/";
        if (!material.shaders.vertex.empty()) {
          const auto full_path = shaders_path + material.shaders.vertex;
          const auto content = file_io::read<uint8_t>(full_path);
          // а как по старинке то теперь загружать =(
          vk::ShaderModuleCreateInfo smci{};
          smci.codeSize = content.size();
          smci.pCode = reinterpret_cast<const uint32_t*>(content.data());
          vk::Device dev(ctx.device);
          usm_vertex = dev.createShaderModuleUnique(smci);
        }

        if (!material.shaders.vertex.empty()) {
          const auto full_path = shaders_path + material.shaders.fragment;
          const auto content = file_io::read<uint8_t>(full_path);
          
          vk::ShaderModuleCreateInfo smci{};
          smci.codeSize = content.size();
          smci.pCode = reinterpret_cast<const uint32_t*>(content.data());
          vk::Device dev(ctx.device);
          usm_fragment = dev.createShaderModuleUnique(smci);
        }

        pm.addShader(vk::ShaderStageFlagBits::eVertex, usm_vertex.get());
        pm.addShader(vk::ShaderStageFlagBits::eFragment, usm_fragment.get());

        const auto& geo_inst = ctx.geometry_instances[step.geometry_instance];
        const auto& geo = ctx.geometries[geo_inst.geometry];
        if (geo.stride != 0) {
          pm.vertexBinding(0, geo.stride, vk::VertexInputRate::eVertex);
          size_t offset = 0;
          for (uint32_t i = 0; i < geo.vertex_layout.size(); ++i) {
            const auto& f = geo.vertex_layout[i];
            const auto format = static_cast<vk::Format>(f);
            const auto& fmt_data = format_element_size(f);
            pm.vertexAttribute(i, 0, format, offset);
            offset += fmt_data;
          }
        }

        if (geo_inst.stride != 0) {
          pm.vertexBinding(1, geo_inst.stride, vk::VertexInputRate::eInstance);
          size_t offset = 0;
          for (uint32_t i = 0; i < geo_inst.instance_layout.size(); ++i) {
            const auto f = geo_inst.instance_layout[i];
            const auto format = static_cast<vk::Format>(f);
            const auto& fmt_data = format_element_size(f);
            pm.vertexAttribute(i, 1, format, offset);
            offset += fmt_data;
          }
        }

        // нужно явно прописать это дело в шагах
        pm.dynamicState(vk::DynamicState::eViewport);
        pm.dynamicState(vk::DynamicState::eScissor);

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
        pm.blendConstant(nullptr);
        for (const auto& blend : step.blending) {
          pm.colorBlending(std::bit_cast<vk::PipelineColorBlendAttachmentState>(blend));
        }

        step.pipeline = pm.create(
          pass.name + "." + step.name + ".pipeline", 
          VK_NULL_HANDLE, 
          step.pipeline_layout, 
          pass.render_pass, 
          step.subpass_index
        );
      }
      
      // нужно еще создать драв группу
      // она вообще из себя представляет несколько очень больших буферов
      // которые управляются индирект буфером, если индирект буфер 0
      // то мы этот слот не обрабатываем, что значит создать драв группу с моей точки зрения?
      // передать контексту данные об еще парочке ресурсов, какой размер? неизвестно
      // пока что оставим это дело
      if (!same_material || !same_geometry) {
        step.draw_group = ctx.draw_groups.size();
        ctx.draw_groups.emplace_back();
        auto& dg = ctx.draw_groups[step.draw_group];
        dg.geometry_instance = geometry_index;
        dg.material = material_index;
        // все равно нужно что то придумать с буферами
      }

    }
  }
}

void create_descriptor_set_layouts(painter_base& ctx) {
  for (auto& desc : ctx.descriptors) {
    descriptor_set_layout_maker dslm(ctx.device);
    for (uint32_t i = 0; i < desc.layout.size(); ++i) {
      const auto& [ slot, usage ] = desc.layout[i];
      const auto& res = ctx.resources[slot];
      const auto dt = convertdt(usage);
      const uint32_t buffers_count = static_cast<uint32_t>(res.type);
      dslm.binding(i, convertdt(usage), vk::ShaderStageFlagBits::eAll, buffers_count);
    }
    desc.setlayout = dslm.create(desc.name);
  }
}

// должны ли в этих же слотах быть текстурки из ассетов? вряд ли
void create_resources(painter_base& ctx) {
  const auto& caps = vk::PhysicalDevice(ctx.physical_device).getSurfaceCapabilitiesKHR(ctx.surface);
  const auto& ext = choose_swapchain_extent(caps.currentExtent.width, caps.currentExtent.height, caps);
  vma::Allocator al(ctx.allocator);

  for (auto &res : ctx.resources) {
    // создадим конкретные ресурсы
    // некоторые буферы попадут в один большой
    // у нас наверное будет ресурс контейнеры которые почти повторяют сами ресурсы
    // довольно много чего можно расположить в одной 2д текстурке со слоями
    // как минимум все индиректы попадут в один буфер

    // тут наверное соберем ресурсы по группам и попытаемся впихнуть все в одно место
    // хотя у нас откровенно есть такая потребность только когда мы делаем дравгруппу
    // блин драв группа создается кажется на основе подгруженных ресурсов

    const bool is_image = role::is_image(res.role);
    const bool is_buffer = role::is_buffer(res.role);
    const bool is_attachment = role::is_attachment(res.role);

    // это ресурс для хоста? 
    // размер для буфера расчитывается разными способами
    // размер картинки напрямую зависит от глобальных установок

    // от чего все таки зависит размер буфера? индирект например фиксированный
    // вершинный и индексный буфер создаются отдельно
    // инстансный буфер зависит от максимального количества инстансов + задается при создании драв группы
    // картинки походу зависят исключительно от констант
    // то есть чтобы например создать несколько сопроводительных картинок с шумом
    // нужно задать константу 512x512 и тут ее съесть
    // я бы даже сказал что сайз тоже придется хранить в строке и обращаться к таблице

    // так пока что все равно не понятно какого размера буферы инстансов и индиректов
    // впрочем они подцепляются с помощью дескрипторов в компут шейдеры например
    // а при отрисовке перебиндиваем буфер? звучит более менее как план на самом деле
    // вот что нужно понять: какие меши в какую драв группу идут

    const uint32_t buffering = static_cast<uint32_t>(res.type);

    const auto& size_value = ctx.constant_values[res.size];

    auto image_extent = ext;
    size_t buffer_size = 0;
    if (size_value.type == value_type::screensize) {
      const auto& [xs, ys, zs] = size_value.current_scale;
      buffer_size = size_t(ext.width * xs) * size_t(ext.height * ys) * size_t(res.size_hint);
      image_extent = vk::Extent2D(ext.width * xs, ext.height * ys);
    } else if (size_value.type == value_type::fixed) {
      const auto& [x, y, z] = size_value.current_value;
      const auto& [xs, ys, zs] = size_value.current_scale;
      buffer_size = size_t(x*xs) * size_t(res.size_hint);
      image_extent = vk::Extent2D(x * xs, x * xs);
    } else if (size_value.type == value_type::fixed_2d) {
      const auto& [x, y, z] = size_value.current_value;
      const auto& [xs, ys, zs] = size_value.current_scale;
      buffer_size = size_t(x * xs) * size_t(y*ys) * size_t(res.size_hint);
      image_extent = vk::Extent2D(x * xs, y * ys);
    } else if (size_value.type == value_type::fixed_3d) {
      const auto& [x, y, z] = size_value.current_value;
      const auto& [xs, ys, zs] = size_value.current_scale;
      buffer_size = size_t(x*xs) * size_t(y*ys) * size_t(z*zs) * size_t(res.size_hint);
      image_extent = vk::Extent2D(x * xs, y * ys);
    } 
    
    // наверное указать что за глобальная юниформа? а сколько их бывает то
    if (res.role == role::global_uniform) {
      // константа (данные камеры)
      buffer_size = res.size_hint; // ???
    } else if (res.role == role::frame_constants) {
      // константа (счетчик фреймов + время + ???)
      buffer_size = res.size_hint; // ???
    } else if (res.role == role::indirect) {
      // размер одного буфера фиксирован 
      buffer_size = sizeof(vk::DrawIndirectCommand) * 2;
    }

    if (is_buffer) {
      vk::BufferCreateInfo bci{};
      bci.usage = static_cast<vk::BufferUsageFlags>(res.usage_mask);
      bci.size = buffer_size * buffering;

      vma::AllocationCreateInfo aci{};
      aci.usage = vma::MemoryUsage::eGpuOnly;
      if (
        aci.usage == vma::MemoryUsage::eCpuOnly || 
        aci.usage == vma::MemoryUsage::eCpuCopy || 
        aci.usage == vma::MemoryUsage::eCpuToGpu ||
        aci.usage == vma::MemoryUsage::eGpuToCpu
      ) { aci.flags = aci.flags | vma::AllocationCreateFlagBits::eMapped; }

      auto [ buf_handle, allocation ] = al.createBuffer(bci, aci);

      // буфер вью?
    } else {
      vk::ImageCreateInfo ici{};
      ici.usage = static_cast<vk::ImageUsageFlags>(res.usage_mask);
      ici.format = static_cast<vk::Format>(res.format_hint);
      ici.imageType = vk::ImageType::e2D;
      ici.initialLayout = vk::ImageLayout::eUndefined; // general?
      ici.samples = vk::SampleCountFlagBits::e1;
      ici.arrayLayers = buffering;
      ici.mipLevels = 1;
      ici.extent = vk::Extent3D{ext.width, ext.height, 1u};
      ici.tiling = vk::ImageTiling::eOptimal;

      vma::AllocationCreateInfo aci{};
      aci.usage = vma::MemoryUsage::eGpuOnly;
      if (
        aci.usage == vma::MemoryUsage::eCpuOnly || 
        aci.usage == vma::MemoryUsage::eCpuCopy || 
        aci.usage == vma::MemoryUsage::eCpuToGpu ||
        aci.usage == vma::MemoryUsage::eGpuToCpu
      ) { aci.flags = aci.flags | vma::AllocationCreateFlagBits::eMapped; }

      auto [ img_handle, allocation ] = al.createImage(ici, aci);

      vk::ImageViewCreateInfo ivci{};
      ivci.image = img_handle;
      ivci.format = static_cast<vk::Format>(res.format_hint);
      ivci.viewType = vk::ImageViewType::e2D;
      ivci.components = vk::ComponentMapping{};
      ivci.subresourceRange = vk::ImageSubresourceRange{};
      ivci.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor; // определяется форматом? ролью?
      ivci.subresourceRange.baseMipLevel = 0;
      ivci.subresourceRange.levelCount = 1;
      ivci.subresourceRange.baseArrayLayer = 0; // конкретный индекс определяем буферизацией
      ivci.subresourceRange.levelCount = 1;

      // создаем view на контейнер - это будет основной vulkan handle для всяких штук
    }
  }
}

void create_framebuffers(painter_base& ctx) {
  for (auto& pass : ctx.render_graph.passes) {
    // тут будет отдельная структура с хранилищем всех фреймбуферов
    // нужный будем брать по frame_index
  }
}

// ...
void update_descriptor_sets() {

}

template <typename T>
void parse_every_entry(const fs::path &p, T& arr) {
  using mirror_T = T::value_type;
  T local_arr;

  for (const auto& entry : fs::directory_iterator(p)) {
    if (fs::is_regular_file(entry)) {
      const auto str = file_io::read(entry.path().generic_string());
      const auto err = utils::from_json(local_arr, str);
      if (err) {
        mirror_T val;
        const auto err = utils::from_json(val, str);
        if (err) {
          utils::warn("Could not parse '{}', got error '{}'", entry.path().generic_string(), err.custom_error_message);
          continue;
        }
        arr.emplace_back(std::move(val));
      } else {
        arr.insert(arr.end(), local_arr.begin(), local_arr.end());
        local_arr.clear();
      }
    }
  }
}

template <typename T>
bool valid_folder(const std::string_view &name) {
  if (name == "constants" && std::is_same_v<constant_mirror, T>) return true;
  if (name == "descriptors" && std::is_same_v<descriptor_mirror, T>) return true;
  if (name == "geometries" && std::is_same_v<geometry_mirror, T>) return true;
  if (name == "geometry_instances" && std::is_same_v<geometry_instance_mirror, T>) return true;
  if (name == "materials" && std::is_same_v<material_mirror, T>) return true;
  if (name == "render_targets" && std::is_same_v<render_target_mirror, T>) return true;
  if (name == "resources" && std::is_same_v<resource_mirror, T>) return true;
  return false;
}

void painter_base::resize_viewport(const uint32_t width, const uint32_t height) {}
void painter_base::recreate_pipelines() {}
void painter_base::recreate_render_graph() {
  std::vector<resource_mirror> rms;
  std::vector<constant_mirror> cms;
  std::vector<render_target_mirror> rtms;
  std::vector<descriptor_mirror> dms;
  std::vector<material_mirror> mms;
  std::vector<geometry_mirror> gms;
  std::vector<geometry_instance_mirror> gims;

  render_graph_mirror rgm;

  if (!file_io::exists("../render_config") || !file_io::is_directory("../render_config")) utils::error{}("Could not find folder '../render_config'");
  for (const auto &entry : fs::directory_iterator("../render_config")) {
    if (entry.is_directory()) {
      if (valid_folder<constant_mirror>(entry.path().filename().generic_string())) parse_every_entry(entry.path(), cms);
      if (valid_folder<descriptor_mirror>(entry.path().filename().generic_string())) parse_every_entry(entry.path(), dms);
      if (valid_folder<geometry_mirror>(entry.path().filename().generic_string())) parse_every_entry(entry.path(), gms);
      if (valid_folder<geometry_instance_mirror>(entry.path().filename().generic_string())) parse_every_entry(entry.path(), gims);
      if (valid_folder<material_mirror>(entry.path().filename().generic_string())) parse_every_entry(entry.path(), mms);
      if (valid_folder<render_target_mirror>(entry.path().filename().generic_string())) parse_every_entry(entry.path(), rtms);
      if (valid_folder<resource_mirror>(entry.path().filename().generic_string())) parse_every_entry(entry.path(), rms);
    } else {
      if (entry.path().filename().generic_string() == "render_graph") {
        const auto str = file_io::read(entry.path().generic_string());
        const auto err = utils::from_json(rgm, str);
        if (err) utils::warn("Could not parse '{}', got error '{}'", entry.path().generic_string(), err.custom_error_message);
      }
    }
  }

  // пытаемся сначала распарсить поля в новые векторы

  painter_base ctx;

  ctx.counters.resize(cms.size());
  for (size_t i = 0; i < cms.size(); ++i) {
    ctx.counters[i] = cms.convert
  }

  for (const auto &rm : rms) {
    ctx.resources.emplace_back(rm.convert(ctx));
  }

  // подменим массивы из ctx 
}

// чистим теперь при смене графа или разрушении объекта
void painter_base::clear_render_graph() {
  
}

// чистим ресурсы зачем? почистить неиспользуемое?
void painter_base::clear_resources() {
  auto dev = vk::Device(device);
  auto aloc = vma::Allocator(allocator);
  for (const auto &res : resources) {
    for (size_t i = 0; i < res.handles.size() && res.handles[i].handle != 0; ++i) {
      if (role::is_buffer(res.role)) {
        dev.destroy(res.handles[i].view);
        aloc.destroyBuffer(VkBuffer(res.handles[i].handle), res.handles[i].alloc);
      }

      if (role::is_image(res.role)) {
        dev.destroy(res.handles[i].view);
        aloc.destroyImage(VkImage(res.handles[i].handle), res.handles[i].alloc);
      }
    }
  }

  resources.clear();
}


void painter_base::clear() {
  clear_render_graph();
  clear_resources();

  // удалим все остальное
}

void painter_base::draw() {
  // обновим фрейм
  inc_counter(0);
  counters[0].value = counters[0].next_value;

  // подождем фенс

  // обновим дескрипторы

  // 

  std::vector<vk::SubmitInfo> infos;
  std::vector<resource_instance> instances;
  for (const auto& pass : render_graph.passes) {
    for (const auto& step : pass.steps) {
      // ...
    }

    // соберем сабмиты
  }

  // закинем на queue и все?
}

void painter_base::write_constant(const uint32_t slot, void* data, const size_t size) {
  // memcpy
}

void painter_base::inc_counter(const uint32_t slot) {
  if (slot >= counters.size()) return;
  counters[slot].next_value.fetch_add(1);
}

void painter_base::update_counters() {
  for (auto &c : counters) {
    c.value = c.next_value;
    
  }
}



}
}