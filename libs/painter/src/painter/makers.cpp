#include "makers.h"

namespace devils_engine {
  namespace painter {
    descriptor_pool_maker::descriptor_pool_maker(vk::Device device) : device(device) {}
    descriptor_pool_maker & descriptor_pool_maker::flags(const vk::DescriptorPoolCreateFlags flags) {
      f = flags;

      return *this;
    }
    
    descriptor_pool_maker & descriptor_pool_maker::poolSize(const vk::DescriptorType type, const uint32_t count) {
      sizes.push_back({type, count});

      return *this;
    }
    
    vk::DescriptorPool descriptor_pool_maker::create(const std::string &name) {
      uint32_t maxSets = 0;
      for (size_t i = 0; i < sizes.size(); ++i) {
        maxSets += sizes[i].descriptorCount;
      }
      
      const vk::DescriptorPoolCreateInfo info(
        f,
        maxSets,
        static_cast<uint32_t>(sizes.size()),
        sizes.data()
      );

      vk::DescriptorPool p = device.createDescriptorPool(info);

      sizes.clear();
      
      // функция не заработает если у нас отсутствует функция по заданию имени
      if (!name.empty()) set_name(device, p, name);
      
      return p;
    }
    
    descriptor_set_maker::descriptor_set_maker(vk::Device device) : device(device) {}
    descriptor_set_maker & descriptor_set_maker::layout(vk::DescriptorSetLayout layout) {
      layouts.push_back(layout);

      return *this;
    }

  //   DescriptorMaker & DescriptorMaker::data(const uint32_t &binding, const vk::DescriptorType &type, const uint32_t &arrayElement, const uint32_t &setNum) {
  //     datas.back().binding = binding;
  //     datas.back().arrayElement = arrayElement;
  //     datas.back().type = type;
  //     datas.back().setNum = setNum;
  // 
  //     return *this;
  //   }
    
    std::vector<vk::DescriptorSet> descriptor_set_maker::create(const vk::DescriptorPool pool, const std::string &name) {
      const vk::DescriptorSetAllocateInfo info(
        pool,
        static_cast<uint32_t>(layouts.size()),
        layouts.data()
      );
      
      const auto descs = device.allocateDescriptorSets(info);
      
      std::vector<vk::DescriptorSet> descriptors(descs.size());
      for (size_t i = 0; i < descs.size(); ++i) {
        descriptors[i] = descs[i];
        if (!name.empty()) {
          const auto &final_name = descs.size() == 1 ? name : name + std::to_string(i);
          set_name(device, descriptors[i], final_name);
        }
      }


      layouts.clear();

      return descriptors;
    }

    descriptor_set_updater::descriptor_set_updater(vk::Device device) : device(device) {}
    descriptor_set_updater & descriptor_set_updater::currentSet(vk::DescriptorSet set) {
      current = set;
      return *this;
    }

    descriptor_set_updater & descriptor_set_updater::begin(const uint32_t binding, const uint32_t arrayElement, vk::DescriptorType type) {
      const vk::WriteDescriptorSet info(
        current,
        binding,
        arrayElement,
        0,
        type,
        nullptr,
        nullptr,
        nullptr
      );

      infos.push_back(info);
      writes.push_back({});

      switch(type) {
        case vk::DescriptorType::eUniformTexelBuffer:
        case vk::DescriptorType::eStorageTexelBuffer:
          writes.back().type = 2;
          break;
        case vk::DescriptorType::eUniformBuffer:
        case vk::DescriptorType::eStorageBuffer:
        case vk::DescriptorType::eUniformBufferDynamic:
        case vk::DescriptorType::eStorageBufferDynamic:
          writes.back().type = 1;
          break;
        case vk::DescriptorType::eSampler:
        case vk::DescriptorType::eCombinedImageSampler:
        case vk::DescriptorType::eSampledImage:
        case vk::DescriptorType::eStorageImage:
          writes.back().type = 0;
          break;
        default: throw std::runtime_error("not implemented");
      }

      return *this;
    }

    descriptor_set_updater & descriptor_set_updater::image(vk::ImageView image, const vk::ImageLayout layout, vk::Sampler sampler) {
      const vk::DescriptorImageInfo info(
        sampler,
        image,
        layout
      );

      writes.back().images.push_back(info);

      return *this;
    }

    descriptor_set_updater & descriptor_set_updater::sampler(vk::Sampler sampler) {
      const vk::DescriptorImageInfo info{
        sampler,
        nullptr,
        vk::ImageLayout::eUndefined
      };

      writes.back().images.push_back(info);

      return *this;
    }

    descriptor_set_updater & descriptor_set_updater::buffer(vk::Buffer buffer, const size_t offset, const size_t size) {
      const vk::DescriptorBufferInfo info(
        buffer,
        offset,
        size
      );

      writes.back().buffers.push_back(info);

      return *this;
    }

    descriptor_set_updater & descriptor_set_updater::texelBuffer(vk::BufferView buffer) {
      writes.back().bufferViews.push_back(buffer);

      return *this;
    }

    descriptor_set_updater & descriptor_set_updater::copy(vk::DescriptorSet srcSet, const uint32_t srcBinding, const uint32_t srcArrayElement, 
                                                          vk::DescriptorSet dstSet, const uint32_t dstBinding, const uint32_t dstArrayElement, 
                                                          const uint32_t count) {
      const vk::CopyDescriptorSet copyInfo(
        srcSet,
        srcBinding,
        srcArrayElement,
        dstSet,
        dstBinding,
        dstArrayElement,
        count
      );

      copies.push_back(copyInfo);

      return *this;
    }

    void descriptor_set_updater::update() {
      for (size_t i = 0; i < infos.size(); ++i) {
        infos[i].descriptorCount = writes[i].type == 2 ? writes[i].bufferViews.size() : writes[i].type == 1 ? writes[i].buffers.size() : writes[i].images.size();
        
        infos[i].pImageInfo = writes[i].type == 0 ? writes[i].images.data() : nullptr;
        infos[i].pBufferInfo = writes[i].type == 1 ? writes[i].buffers.data() : nullptr;
        infos[i].pTexelBufferView = writes[i].type == 2 ? writes[i].bufferViews.data() : nullptr;
      }
      
      if (infos.empty()) exit(-1);

      device.updateDescriptorSets(infos.size(), infos.data(), copies.size(), copies.data());
      
      writes.clear();
      infos.clear();
      copies.clear();
      
      current = nullptr;
    }
    
    framebuffer_maker::framebuffer_maker(vk::Device device) : device(device) {}
    framebuffer_maker & framebuffer_maker::renderpass(vk::RenderPass pass) {
      this->pass = pass;
      
      return *this;
    }
    
    framebuffer_maker & framebuffer_maker::addAttachment(vk::ImageView view) {
      views.push_back(view);
      
      return *this;
    }
    
    framebuffer_maker & framebuffer_maker::dimensions(const uint32_t width, const uint32_t height) {
      this->width = width;
      this->height = height;
      
      return *this;
    }
    
    framebuffer_maker & framebuffer_maker::layers(const uint32_t layerCount) {
      this->layerCount = layerCount;
      
      return *this;
    }
    
    vk::Framebuffer framebuffer_maker::create(const std::string &name) {
      const vk::FramebufferCreateInfo i(
        {},
        pass,
        static_cast<uint32_t>(views.size()),
        views.data(),
        width,
        height,
        layerCount
      );
      
      auto f = device.createFramebuffer(i);
      if (!name.empty()) set_name(device, f, name);
      
      return f;
    }
    
    sampler_maker::sampler_maker(vk::Device device) : device(device) {
      info = vk::SamplerCreateInfo(
        {},
        vk::Filter::eLinear,
        vk::Filter::eLinear,
        vk::SamplerMipmapMode::eLinear,
        vk::SamplerAddressMode::eMirroredRepeat,
        vk::SamplerAddressMode::eMirroredRepeat,
        vk::SamplerAddressMode::eMirroredRepeat,
        0.0f,
        VK_FALSE,
        0.0f,
        VK_FALSE,
        vk::CompareOp::eAlways,
        0.0f,
        1.0f,
        vk::BorderColor::eFloatTransparentBlack,
        VK_FALSE
      );
    }
    
    sampler_maker & sampler_maker::filter(const vk::Filter min, const vk::Filter mag) {
      info.minFilter = min;
      info.magFilter = mag;

      return *this;
    }
    
    sampler_maker & sampler_maker::mipmapMode(const vk::SamplerMipmapMode mode) {
      info.mipmapMode = mode;

      return *this;
    }
    
    sampler_maker & sampler_maker::addressMode(const vk::SamplerAddressMode u, const vk::SamplerAddressMode v, const vk::SamplerAddressMode w) {
      info.addressModeU = u;
      info.addressModeV = v;
      info.addressModeW = w;

      return *this;
    }
    
    sampler_maker & sampler_maker::anisotropy(const vk::Bool32 enable, const float max) {
      info.anisotropyEnable = enable;
      info.maxAnisotropy = max;

      return *this;
    }
    
    sampler_maker & sampler_maker::compareOp(const vk::Bool32 enable, const vk::CompareOp op) {
      info.compareEnable = enable;
      info.compareOp = op;

      return *this;
    }
    
    sampler_maker & sampler_maker::lod(const float min, const float max, const float bias) {
      info.minLod = min;
      info.maxLod = max;
      info.mipLodBias = bias;

      return *this;
    }
    
    sampler_maker & sampler_maker::borderColor(const vk::BorderColor color) {
      info.borderColor = color;

      return *this;
    }
    
    sampler_maker & sampler_maker::unnormalizedCoordinates(const vk::Bool32 enable) {
      info.unnormalizedCoordinates = enable;

      return *this;
    }
    
    vk::Sampler sampler_maker::create(const std::string &name) {
      auto s = device.createSampler(info);
      if (!name.empty()) set_name(device, s, name);

      return s;
    }
    
    descriptor_set_layout_maker::descriptor_set_layout_maker(vk::Device device) : device(device) {}
    descriptor_set_layout_maker & descriptor_set_layout_maker::binding(const uint32_t bind, const vk::DescriptorType type, const vk::ShaderStageFlags stage, const uint32_t count) {
      const vk::DescriptorSetLayoutBinding b{
        bind,
        type,
        count,
        stage,
        nullptr
      };
      
      bindings.push_back(b);

      return *this;
    }
    
    descriptor_set_layout_maker & descriptor_set_layout_maker::combined(const uint32_t bind, const vk::DescriptorType type, const vk::ShaderStageFlags stage, const std::vector<vk::Sampler> &samplers) {
      const vk::DescriptorSetLayoutBinding b{
        bind,
        type,
        static_cast<uint32_t>(samplers.size()),
        stage,
        samplers.data()
      };
      
      bindings.push_back(b);

      return *this;
    }
    
    vk::DescriptorSetLayout descriptor_set_layout_maker::create(const std::string &name) {
      const vk::DescriptorSetLayoutCreateInfo info(
        {},
        static_cast<uint32_t>(bindings.size()),
        bindings.data()
      );

      auto sl = device.createDescriptorSetLayout(info);
      if (!name.empty()) set_name(device, sl, name);
      bindings.clear();
      
      return sl;
    }
    
    pipeline_layout_maker::pipeline_layout_maker(vk::Device device) : device(device) {}
    pipeline_layout_maker & pipeline_layout_maker::addDescriptorLayout(vk::DescriptorSetLayout setLayout) {
      setLayouts.push_back(setLayout);
      return *this;
    }
    
    pipeline_layout_maker & pipeline_layout_maker::addPushConstRange(const uint32_t offset, const uint32_t size, const vk::ShaderStageFlags flag) {
      const vk::PushConstantRange range{
        flag,
        offset,
        size
      };
      
      ranges.push_back(range);

      return *this;
    }
    
    vk::PipelineLayout pipeline_layout_maker::create(const std::string &name) {
      const vk::PipelineLayoutCreateInfo info(
        {},
        static_cast<uint32_t>(setLayouts.size()),
        setLayouts.data(),
        static_cast<uint32_t>(ranges.size()),
        ranges.data()
      );
      
      auto l = device.createPipelineLayout(info);
      if (!name.empty()) set_name(device, l, name);
      
      setLayouts.clear();
      ranges.clear();
      
      return l;
    }
    
    pipeline_maker::pipeline_maker(vk::Device device) : device(device) {
      vertexInfo = vk::PipelineVertexInputStateCreateInfo(
        {},
        0, nullptr,
        0, nullptr
      );
      
      inputAssembly = vk::PipelineInputAssemblyStateCreateInfo(
        {},
        vk::PrimitiveTopology::eTriangleFan,
        VK_FALSE
      );
      
      tessellationInfo = vk::PipelineTessellationStateCreateInfo(
        {},
        0
      );
      
      viewportInfo = vk::PipelineViewportStateCreateInfo(
        {},
        0, nullptr,
        0, nullptr
      );
      
      rasterisationInfo = vk::PipelineRasterizationStateCreateInfo(
        {},
        VK_FALSE,
        VK_FALSE,
        vk::PolygonMode::eFill,
        vk::CullModeFlagBits::eNone,
        vk::FrontFace::eCounterClockwise,
        VK_FALSE,
        0.0f, 
        0.0f, 
        0.0f,
        1.0f
      );
      
      multisamplingInfo = vk::PipelineMultisampleStateCreateInfo(
        {},
        vk::SampleCountFlagBits::e1,
        VK_FALSE,
        1.0f,
        nullptr,
        VK_FALSE,
        VK_FALSE
      );
      
      depthStensilInfo = vk::PipelineDepthStencilStateCreateInfo(
        {},
        VK_FALSE,
        VK_FALSE,
        vk::CompareOp::eLess,
        VK_FALSE,
        VK_FALSE,
        {
          vk::StencilOp::eZero,
          vk::StencilOp::eKeep,
          vk::StencilOp::eZero,
          vk::CompareOp::eEqual,
          0,
          0,
          0
        },
        {
          vk::StencilOp::eZero,
          vk::StencilOp::eKeep,
          vk::StencilOp::eZero,
          vk::CompareOp::eEqual,
          0,
          0,
          0
        },
        0.0f, 
        0.0f
      );
      
      colorBlendingInfo = vk::PipelineColorBlendStateCreateInfo(
        {},
        VK_FALSE,
        vk::LogicOp::eCopy,
        0,
        nullptr,
        {0.0f, 0.0f, 0.0f, 0.0f}
      );
      
      dymStateInfo = vk::PipelineDynamicStateCreateInfo(
        {},
        0,
        nullptr
      );
      
      defaultBlending = vk::PipelineColorBlendAttachmentState(
        VK_TRUE,
        vk::BlendFactor::eSrcAlpha,
        vk::BlendFactor::eOneMinusSrcAlpha,
        vk::BlendOp::eAdd,
        vk::BlendFactor::eSrcAlpha,
        vk::BlendFactor::eOneMinusSrcAlpha,
        vk::BlendOp::eAdd,
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
      );
    }
    
    pipeline_maker & pipeline_maker::addShader(const vk::ShaderStageFlagBits flag, const vk::ShaderModule module, const char* name) {
      const vk::PipelineShaderStageCreateInfo stageInfo(
        {},
        flag,
        module,
        name == nullptr ? "main" : name,
        nullptr
      );
      
      shaders.push_back(stageInfo);
      specs.push_back({});

      return *this;
    }

    pipeline_maker & pipeline_maker::addSpecializationEntry(const uint32_t constantID, const uint32_t offset, const size_t size) {
      specs.back().entries.push_back({constantID, offset, size});

      return *this;
    }

    pipeline_maker & pipeline_maker::addData(const size_t size, const void* data) {
      specs.back().dataSize = size;
      specs.back().data = data;

      return *this;
    }

    pipeline_maker & pipeline_maker::vertexBinding(const uint32_t binding, const uint32_t stride, const vk::VertexInputRate rate) {
      vk::VertexInputBindingDescription desc{
        binding,
        stride,
        rate
      };
      
      vertexBindings.push_back(desc);

      return *this;
    }

    pipeline_maker & pipeline_maker::vertexAttribute(const uint32_t location, const uint32_t binding, const vk::Format format, const uint32_t offset) {
      vk::VertexInputAttributeDescription desc{
        location,
        binding,
        format,
        offset
      };
      
      vertexAttribs.push_back(desc);

      return *this;
    }

    pipeline_maker & pipeline_maker::viewport(const vk::Viewport &view) {
      viewports.push_back(view);

      return *this;
    }
    
    pipeline_maker & pipeline_maker::viewport() {
      viewports.push_back({});

      return *this;
    }

    pipeline_maker & pipeline_maker::scissor(const vk::Rect2D &scissor) {
      scissors.push_back(scissor);

      return *this;
    }
    
    pipeline_maker & pipeline_maker::scissor() {
      scissors.push_back({});

      return *this;
    }

    pipeline_maker & pipeline_maker::dynamicState(const vk::DynamicState state) {
      dynStates.push_back(state);

      return *this;
    }


    pipeline_maker & pipeline_maker::assembly(const vk::PrimitiveTopology topology, const vk::Bool32 primitiveRestartEnable) {
      inputAssembly.topology = topology;
      inputAssembly.primitiveRestartEnable = primitiveRestartEnable;

      return *this;
    }

    pipeline_maker & pipeline_maker::tessellation(bool enabled, const uint32_t patchControlPoints) {
      tessellationState = enabled;
      tessellationInfo.patchControlPoints = patchControlPoints;

      return *this;
    }


    pipeline_maker & pipeline_maker::depthClamp(const vk::Bool32 enable) {
      rasterisationInfo.depthBiasClamp = enable;

      return *this;
    }

    pipeline_maker & pipeline_maker::rasterizerDiscard(const vk::Bool32 enable) {
      rasterisationInfo.rasterizerDiscardEnable = enable;

      return *this;
    }

    pipeline_maker & pipeline_maker::polygonMode(const vk::PolygonMode mode) {
      rasterisationInfo.polygonMode = mode;

      return *this;
    }

    pipeline_maker & pipeline_maker::cullMode(const vk::CullModeFlags flags) {
      rasterisationInfo.cullMode = flags;

      return *this;
    }

    pipeline_maker & pipeline_maker::frontFace(const vk::FrontFace face) {
      rasterisationInfo.frontFace = face;

      return *this;
    }

    pipeline_maker & pipeline_maker::depthBias(const vk::Bool32 enable, const float constFactor, const float clamp, const float slopeFactor) {
      rasterisationInfo.depthClampEnable = enable;
      rasterisationInfo.depthBiasConstantFactor = constFactor;
      rasterisationInfo.depthBiasClamp = clamp;
      rasterisationInfo.depthBiasSlopeFactor = slopeFactor;

      return *this;
    }
    
    pipeline_maker & pipeline_maker::lineWidth(const float lineWidth) {
      rasterisationInfo.lineWidth = lineWidth;

      return *this;
    }


    pipeline_maker & pipeline_maker::rasterizationSamples(const vk::SampleCountFlagBits count) {
      multisamplingInfo.rasterizationSamples = count;

      return *this;
    }

    pipeline_maker & pipeline_maker::sampleShading(const vk::Bool32 enable, const float minSampleShading, const vk::SampleMask* masks) {
      multisamplingInfo.sampleShadingEnable = enable;
      multisamplingInfo.minSampleShading = minSampleShading;
      multisamplingInfo.pSampleMask = masks;

      return *this;
    }

    pipeline_maker & pipeline_maker::multisampleCoverage(const vk::Bool32 alphaToCoverage, const vk::Bool32 alphaToOne) {
      multisamplingInfo.alphaToCoverageEnable = alphaToCoverage;
      multisamplingInfo.alphaToOneEnable = alphaToOne;

      return *this;
    }


    pipeline_maker & pipeline_maker::depthTest(const vk::Bool32 enable) {
      depthStensilInfo.depthTestEnable = enable;

      return *this;
    }

    pipeline_maker & pipeline_maker::depthWrite(const vk::Bool32 enable) {
      depthStensilInfo.depthWriteEnable = enable;

      return *this;
    }
    
    pipeline_maker & pipeline_maker::compare(const vk::CompareOp compare) {
      depthStensilInfo.depthCompareOp = compare;

      return *this;
    }

    pipeline_maker & pipeline_maker::stencilTest(const vk::Bool32 enable, const vk::StencilOpState &front, const vk::StencilOpState &back) {
      depthStensilInfo.stencilTestEnable = enable;
      depthStensilInfo.front = front;
      depthStensilInfo.back = back;

      return *this;
    }

    pipeline_maker & pipeline_maker::depthBounds(const vk::Bool32 enable, const float minBounds, const float maxBounds) {
      depthStensilInfo.depthBoundsTestEnable = enable;
      depthStensilInfo.minDepthBounds = minBounds;
      depthStensilInfo.maxDepthBounds = maxBounds;

      return *this;
    }


    pipeline_maker & pipeline_maker::logicOp(const vk::Bool32 enable, const vk::LogicOp logic) {
      colorBlendingInfo.logicOpEnable = enable;
      colorBlendingInfo.logicOp = logic;

      return *this;
    }

    pipeline_maker & pipeline_maker::blendConstant(const float* blendConst) {
      colorBlendingInfo.blendConstants[0] = blendConst[0];
      colorBlendingInfo.blendConstants[1] = blendConst[1];
      colorBlendingInfo.blendConstants[2] = blendConst[2];
      colorBlendingInfo.blendConstants[3] = blendConst[3];

      return *this;
    }

    pipeline_maker & pipeline_maker::colorBlending(const vk::PipelineColorBlendAttachmentState &state) {
      colorBlends.push_back(state);

      return *this;
    }


    pipeline_maker & pipeline_maker::addDefaultBlending() {
      colorBlends.push_back(defaultBlending);

      return *this;
    }

    pipeline_maker & pipeline_maker::clearBlending() {
      colorBlends.clear();

      return *this;
    }

    pipeline_maker & pipeline_maker::colorBlendBegin(const vk::Bool32 enable) {
      vk::PipelineColorBlendAttachmentState state{
        enable,
        vk::BlendFactor::eSrcAlpha,
        vk::BlendFactor::eOneMinusSrcAlpha,
        vk::BlendOp::eAdd,
        vk::BlendFactor::eSrcAlpha,
        vk::BlendFactor::eOneMinusSrcAlpha,
        vk::BlendOp::eAdd,
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
      };

      colorBlends.push_back(state);

      return *this;
    }

    pipeline_maker & pipeline_maker::srcColor(const vk::BlendFactor value) {
      colorBlends.back().srcColorBlendFactor = value;

      return *this;
    }
    
    pipeline_maker & pipeline_maker::dstColor(const vk::BlendFactor value) {
      colorBlends.back().dstColorBlendFactor = value;

      return *this;
    }
    
    pipeline_maker & pipeline_maker::colorOp(const vk::BlendOp value) {
      colorBlends.back().colorBlendOp = value;

      return *this;
    }
    
    pipeline_maker & pipeline_maker::srcAlpha(const vk::BlendFactor value) {
      colorBlends.back().srcAlphaBlendFactor = value;

      return *this;
    }
    
    pipeline_maker & pipeline_maker::dstAlpha(const vk::BlendFactor value) {
      colorBlends.back().dstAlphaBlendFactor = value;

      return *this;
    }
    
    pipeline_maker & pipeline_maker::alphaOp(const vk::BlendOp value) {
      colorBlends.back().alphaBlendOp = value;

      return *this;
    }
    
    pipeline_maker & pipeline_maker::colorWriteMask(const vk::ColorComponentFlags flags) {
      colorBlends.back().colorWriteMask = flags;

      return *this;
    }
    
    vk::GraphicsPipelineCreateInfo pipeline_maker::get_info(
      vk::PipelineLayout layout,
      vk::RenderPass renderPass,
      const uint32_t subpass,
      vk::Pipeline base,
      const int32_t baseIndex
    ) {
      
      vertexInfo = vk::PipelineVertexInputStateCreateInfo(
        {},
        static_cast<uint32_t>(vertexBindings.size()),
        vertexBindings.data(),
        static_cast<uint32_t>(vertexAttribs.size()),
        vertexAttribs.data()
      );
      
      viewportInfo = vk::PipelineViewportStateCreateInfo(
        {},
        static_cast<uint32_t>(viewports.size()),
        viewports.data(),
        static_cast<uint32_t>(scissors.size()),
        scissors.data()
      );
      
      dymStateInfo = vk::PipelineDynamicStateCreateInfo(
        {},
        static_cast<uint32_t>(dynStates.size()),
        dynStates.data()
      );

      colorBlendingInfo.attachmentCount = colorBlends.size();
      colorBlendingInfo.pAttachments = colorBlends.data();

      shaders_specs.resize(shaders.size());
      for (uint32_t i = 0; i < shaders.size(); ++i) {
        shaders_specs[i].mapEntryCount = specs[i].entries.size();
        shaders_specs[i].pMapEntries = specs[i].entries.data();
        shaders_specs[i].dataSize = specs[i].dataSize;
        shaders_specs[i].pData = specs[i].data;

        shaders[i].pSpecializationInfo = specs[i].entries.empty() ? nullptr : &shaders_specs[i];
      }
      
      return vk::GraphicsPipelineCreateInfo(
        {},
        static_cast<uint32_t>(shaders.size()),
        shaders.data(),
        &vertexInfo,
        &inputAssembly,
        tessellationState ? &tessellationInfo : nullptr,
        &viewportInfo,
        &rasterisationInfo,
        &multisamplingInfo,
        &depthStensilInfo,
        &colorBlendingInfo,
        dynStates.empty() ? nullptr : &dymStateInfo,
        layout,
        renderPass,
        subpass,
        base,
        baseIndex
      );
    }
    
    vk::Pipeline pipeline_maker::create(
      const std::string &name,
      vk::PipelineCache cache,
      vk::PipelineLayout layout,
      vk::RenderPass renderPass,
      const uint32_t subpass,
      vk::Pipeline base,
      const int32_t baseIndex
    ) {
      const auto info = get_info(layout, renderPass, subpass, base, baseIndex);
      
      auto [res, p] = device.createGraphicsPipeline(cache, info);
      if (res != vk::Result::eSuccess) utils::error{}("Could not create graphics pipeline '{}'", name);
      if (!name.empty()) set_name(device, p, name);
      
      shaders_specs.clear();
      shaders.clear();
      vertexBindings.clear();
      vertexAttribs.clear();
      viewports.clear();
      scissors.clear();
      dynStates.clear();
      
      return p;
    }
    
    compute_pipeline_maker::compute_pipeline_maker(vk::Device device) : device(device) {
      shaderInfo = vk::PipelineShaderStageCreateInfo(
        {},
        vk::ShaderStageFlagBits::eAll,
        nullptr,
        nullptr,
        nullptr
      );
    }
    
    compute_pipeline_maker & compute_pipeline_maker::shader(const vk::ShaderModule module, const char* name) {
      shaderInfo = vk::PipelineShaderStageCreateInfo(
        {},
        vk::ShaderStageFlagBits::eCompute,
        module,
        name == nullptr ? "main" : name,
        nullptr
      );
      
      return *this;
    }
    
    compute_pipeline_maker & compute_pipeline_maker::addSpecializationEntry(const uint32_t constantID, const uint32_t offset, const size_t size) {
      const vk::SpecializationMapEntry entry{
        constantID,
        offset,
        size
      };
      
      entries.push_back(entry);
      
      return *this;
    }
    
    compute_pipeline_maker & compute_pipeline_maker::addData(const size_t size, void* data) {
      this->dataSize = size;
      this->data = data;
      
      return *this;
    }
    
    vk::Pipeline compute_pipeline_maker::create(
      const std::string &name,
      vk::PipelineLayout layout,
      vk::Pipeline base,
      const int32_t baseIndex
    ) {
      const vk::SpecializationInfo specInfo{
        static_cast<uint32_t>(entries.size()),
        entries.data(),
        dataSize,
        data
      };
      
      shaderInfo.pSpecializationInfo = &specInfo;
      
      const vk::ComputePipelineCreateInfo info(
        {},
        shaderInfo,
        layout,
        base,
        baseIndex
      );

      auto [res, p] = device.createComputePipeline(nullptr, info);
      if (res != vk::Result::eSuccess) utils::error{}("Could not create compute pipeline '{}'", name);
      if (!name.empty()) set_name(device, p, name);
      entries.clear();
      
      return p;
    }
    
    render_pass_maker::render_pass_maker(vk::Device device) : device(device) {}
    render_pass_maker & render_pass_maker::attachmentBegin(const vk::Format format) {
      const vk::AttachmentDescription desc{
        {},
        vk::Format::eR8G8B8A8Unorm,
        vk::SampleCountFlagBits::e1,
        vk::AttachmentLoadOp::eClear,
        vk::AttachmentStoreOp::eStore,
        vk::AttachmentLoadOp::eDontCare,
        vk::AttachmentStoreOp::eDontCare,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::ePresentSrcKHR
      };
      
      attachments.push_back(desc);
      attachments.back().format = format;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::attachmentFlags(const vk::AttachmentDescriptionFlags value) {
      attachments.back().flags = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::attachmentFormat(const vk::Format value) {
      attachments.back().format = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::attachmentSamples(const vk::SampleCountFlagBits value) {
      attachments.back().samples = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::attachmentLoadOp(const vk::AttachmentLoadOp value) {
      attachments.back().loadOp = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::attachmentStoreOp(const vk::AttachmentStoreOp value) {
      attachments.back().storeOp = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::attachmentStencilLoadOp(const vk::AttachmentLoadOp value) {
      attachments.back().stencilLoadOp = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::attachmentStencilStoreOp(const vk::AttachmentStoreOp value) {
      attachments.back().stencilStoreOp = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::attachmentInitialLayout(const vk::ImageLayout value) {
      attachments.back().initialLayout = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::attachmentFinalLayout(const vk::ImageLayout value) {
      attachments.back().finalLayout = value;
      
      return *this;
    }
    

    render_pass_maker & render_pass_maker::subpassBegin(const vk::PipelineBindPoint bind) {
      subpass_description desc{};
      descriptions.push_back(desc);
      descriptions.back().pipelineBindPoint = bind;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::subpassInputAttachment(const uint32_t attachment, const vk::ImageLayout layout) {
      descriptions.back().input.push_back({attachment, layout});
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::subpassColorAttachment(const uint32_t attachment, const vk::ImageLayout layout) {
      descriptions.back().color.push_back({attachment, layout});
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::subpassResolveAttachment(const uint32_t attachment, const vk::ImageLayout layout) {
      descriptions.back().resolve.push_back({attachment, layout});
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::subpassDepthStencilAttachment(const uint32_t attachment, const vk::ImageLayout layout) {
      descriptions.back().stensil = {attachment, layout};
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::addPreservedAttachmentIndex(const uint32_t index) {
      descriptions.back().preservedAttachments.push_back(index);
      
      return *this;
    }
    

    render_pass_maker & render_pass_maker::dependencyBegin(const uint32_t srcSubpass, const uint32_t dstSubpass) {
      const vk::SubpassDependency dep{
        srcSubpass,
        dstSubpass,
        vk::PipelineStageFlagBits::eBottomOfPipe,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::AccessFlagBits::eMemoryRead,
        vk::AccessFlagBits::eColorAttachmentWrite,
        {}
      };
      
      dependencies.push_back(dep);
      dependencies.back().srcSubpass = srcSubpass;
      dependencies.back().dstSubpass = dstSubpass;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::dependencySrcSubpass(const uint32_t value) {
      dependencies.back().srcSubpass = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::dependencyDstSubpass(const uint32_t value) {
      dependencies.back().dstSubpass = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::dependencySrcStageMask(const vk::PipelineStageFlags value) {
      dependencies.back().srcStageMask = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::dependencyDstStageMask(const vk::PipelineStageFlags value) {
      dependencies.back().dstStageMask = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::dependencySrcAccessMask(const vk::AccessFlags value) {
      dependencies.back().srcAccessMask = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::dependencyDstAccessMask(const vk::AccessFlags value) {
      dependencies.back().dstAccessMask = value;
      
      return *this;
    }
    
    render_pass_maker & render_pass_maker::dependencyDependencyFlags(const vk::DependencyFlags value) {
      dependencies.back().dependencyFlags = value;
      
      return *this;
    }
    
    vk::RenderPassCreateInfo render_pass_maker::get_info() {
      for (size_t i = 0; i < descriptions.size(); ++i) {
        const vk::SubpassDescription info(
          {},
          descriptions[i].pipelineBindPoint,
          static_cast<uint32_t>(descriptions[i].input.size()),
          descriptions[i].input.data(),
          static_cast<uint32_t>(descriptions[i].color.size()),
          descriptions[i].color.data(),
          descriptions[i].resolve.data(),
          descriptions[i].stensil.layout == vk::ImageLayout::eUndefined ? nullptr : &descriptions[i].stensil,
          static_cast<uint32_t>(descriptions[i].preservedAttachments.size()),
          descriptions[i].preservedAttachments.data()
        );
        
        descs.push_back(info);
      }
      
      return vk::RenderPassCreateInfo(
        {},
        static_cast<uint32_t>(attachments.size()),
        attachments.data(),
        static_cast<uint32_t>(descs.size()),
        descs.data(),
        static_cast<uint32_t>(dependencies.size()),
        dependencies.data()
      );
    }

    vk::RenderPass render_pass_maker::create(const std::string &name) {      
      const auto info = get_info();
      
      auto newPass = device.createRenderPass(info);
      if (!name.empty()) set_name(device, newPass, name);
      
      attachments.clear();
      descs.clear();
      descriptions.clear();
      
      return newPass;
    }
    
    device_maker::device_maker(vk::Instance inst) : printExtensionInfo(false),  inst(inst) {
      info.flags = {};
      info.queueCreateInfoCount = 0;
      info.pQueueCreateInfos = nullptr;
      info.enabledLayerCount = 0;
      info.ppEnabledLayerNames = nullptr;
      info.enabledExtensionCount = 0;
      info.ppEnabledExtensionNames = nullptr;
      info.pEnabledFeatures = nullptr;

      f = vk::PhysicalDeviceFeatures{};
    }
    
    device_maker & device_maker::beginDevice(const vk::PhysicalDevice phys) {
      this->phys = phys;

      return *this;
    }

    device_maker & device_maker::createQueues(const uint32_t maxCount, const float* priority) {
      const auto &props = phys.getQueueFamilyProperties();
      
      priorities = new float*[props.size()];
      
      for (size_t i = 0; i < props.size(); ++i) {
        const uint32_t queuesCount = std::min(maxCount, props[i].queueCount);
        
        priorities[i] = new float[queuesCount];
        
        for (uint32_t j = 0; j < queuesCount; ++j) {
          priorities[i][j] = priority == nullptr ? 1.0f : priority[j];
        }
        
        const vk::DeviceQueueCreateInfo info(
          {},
          static_cast<uint32_t>(i),
          queuesCount,
          priorities[i]
        );
        queueInfos.push_back(info);
        
        familyProperties.emplace_back();
        familyProperties.back().count = queuesCount;
        familyProperties.back().flags = props[i].queueFlags;
      }

      return *this;
    }

    device_maker & device_maker::createQueue(const uint32_t queue_family_index, const uint32_t maxCount, const float* priority) {
      const auto &props = phys.getQueueFamilyProperties();
      if (queue_family_index >= props.size()) utils::error{}("Invalid queue family index {}, max is {}", queue_family_index, props.size());
      const uint32_t queuesCount = std::min(maxCount, props[queue_family_index].queueCount);
      if (queuesCount < maxCount) utils::warn("Queue family does not provide this much {} queues, createing with {}", maxCount, queuesCount);

      if (priorities == nullptr) priorities = new float*[props.size()];
      priorities[queue_family_index] = new float[queuesCount];
      for (uint32_t j = 0; j < queuesCount; ++j) {
        priorities[queue_family_index][j] = priority == nullptr ? 1.0f : priority[j];
      }

      const vk::DeviceQueueCreateInfo info(
        {},
        static_cast<uint32_t>(queue_family_index),
        queuesCount,
        priorities[queue_family_index]
      );

      queueInfos.push_back(info);

      familyProperties.emplace_back();
      familyProperties.back().count = queuesCount;
      familyProperties.back().flags = props[queue_family_index].queueFlags;

      return *this;
    }
    
    device_maker & device_maker::features(const vk::PhysicalDeviceFeatures &f) {
      this->f = f;

      return *this;
    }
    
    device_maker & device_maker::setExtensions(const std::vector<const char*> &extensions, bool printExtensionInfo) {
      this->extensions = extensions;
      this->printExtensionInfo = printExtensionInfo;
      
      return *this;
    }

    vk::Device device_maker::create(const std::vector<const char*> &layers, const std::string &name) {
      // я бы мог проверить слои, но не проверяю

      //std::vector<const char*> lay(layers);
      //required_validation_layers(lay);
      //const auto &ext = required_device_extensions(phys, lay, extensions);

      info.queueCreateInfoCount = queueInfos.size();
      info.pQueueCreateInfos = queueInfos.data();
      info.enabledLayerCount = layers.size();
      info.ppEnabledLayerNames = layers.data();
      info.enabledExtensionCount = extensions.size();
      info.ppEnabledExtensionNames = extensions.data();
      info.pEnabledFeatures = &f;
      
      auto dev = phys.createDevice(info);
      
      for (uint32_t i = 0; i < queueInfos.size(); ++i) {
        delete [] priorities[i];
      }
      delete [] priorities;
      
      queueInfos.clear();
      extensions.clear();
      
      if (!name.empty()) set_name(dev, dev, name);
      
      return dev;
    }
  }
}