#ifndef DEVILS_ENGINE_PAINTER_MAKERS_H
#define DEVILS_ENGINE_PAINTER_MAKERS_H

#include "vulkan_header.h"

#define DEFAULT_COLOR_WRITE_MASK vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA

namespace devils_engine {
  namespace painter {
    class descriptor_pool_maker {
    public:
      descriptor_pool_maker(vk::Device device);

      descriptor_pool_maker & flags(const vk::DescriptorPoolCreateFlags flags);
      descriptor_pool_maker & poolSize(const vk::DescriptorType type, const uint32_t count);

      vk::DescriptorPool create(const std::string &name = "");
    protected:
      vk::DescriptorPoolCreateFlags f;
      
      vk::Device device;
      std::vector<vk::DescriptorPoolSize> sizes;
    };

    class descriptor_set_maker {
    public:
      descriptor_set_maker(vk::Device device);

      descriptor_set_maker & layout(vk::DescriptorSetLayout layout);
      //descriptor_set_maker & data(const uint32_t &binding, const VkDescriptorType &type, const uint32_t &arrayElement = 0, const uint32_t &setNum = UINT32_MAX);
      // удалится сам при удалении VkDescriptorPool
      std::vector<vk::DescriptorSet> create(const vk::DescriptorPool pool, const std::string &name = "");
    protected:
      struct DescriptorData {
        uint32_t binding;
        uint32_t arrayElement;
        vk::DescriptorType type;
      };

      vk::Device device;
      std::vector<vk::DescriptorSetLayout> layouts;
  //     std::vector<VkDescriptorType> types;
  //     std::vector<DescriptorData> datas;
    };

    class descriptor_set_updater {
    public:
      descriptor_set_updater(vk::Device device);

      descriptor_set_updater & currentSet(vk::DescriptorSet set);
      descriptor_set_updater & begin(const uint32_t binding, const uint32_t arrayElement, vk::DescriptorType type);

      descriptor_set_updater & image(vk::ImageView image, const vk::ImageLayout layout, vk::Sampler sampler = vk::Sampler(nullptr));
      
      descriptor_set_updater & sampler(vk::Sampler sampler);

      descriptor_set_updater & buffer(vk::Buffer buffer, const size_t offset = 0, const size_t size = VK_WHOLE_SIZE);
  //     DescriptorUpdater & buffer(Buffer buffer, const VkDeviceSize &offset, const VkDeviceSize &size);

      descriptor_set_updater & texelBuffer(vk::BufferView buffer);

      descriptor_set_updater & copy(vk::DescriptorSet srcSet, const uint32_t srcBinding, const uint32_t srcArrayElement, 
                                    vk::DescriptorSet dstSet, const uint32_t dstBinding, const uint32_t dstArrayElement, 
                                    const uint32_t count);

      void update();
    protected:
      struct Write {
        uint8_t type; // 0 == image, 1 == buffer, 2 == buffer view
        std::vector<vk::DescriptorBufferInfo> buffers;
        std::vector<vk::DescriptorImageInfo> images;
        std::vector<vk::BufferView> bufferViews;
      };
      
      std::vector<Write> writes;
      std::vector<vk::WriteDescriptorSet> infos;
      std::vector<vk::CopyDescriptorSet> copies;

      vk::DescriptorSet current;
      vk::Device device;
    };
    
    class framebuffer_maker {
    public:
      framebuffer_maker(vk::Device device);
      
      framebuffer_maker & renderpass(vk::RenderPass pass);
      framebuffer_maker & addAttachment(vk::ImageView view);
      framebuffer_maker & dimensions(const uint32_t width, const uint32_t height);
      framebuffer_maker & layers(const uint32_t layerCount);
      
      vk::Framebuffer create(const std::string &name);
    protected:
      vk::RenderPass pass;
      std::vector<vk::ImageView> views;
      uint32_t width;
      uint32_t height;
      uint32_t layerCount;
      
      vk::Device device;
    };

    class sampler_maker {
      //friend class Device;
    public:
      sampler_maker(vk::Device device);

      sampler_maker & filter(const vk::Filter min, const vk::Filter mag);
      sampler_maker & mipmapMode(const vk::SamplerMipmapMode mode);
      sampler_maker & addressMode(const vk::SamplerAddressMode u, const vk::SamplerAddressMode v, const vk::SamplerAddressMode w = vk::SamplerAddressMode::eRepeat);
      sampler_maker & anisotropy(const vk::Bool32 enable, const float max = 1.0f);
      sampler_maker & compareOp(const vk::Bool32 enable, const vk::CompareOp op);
      sampler_maker & lod(const float min, const float max, const float bias = 0.0f);
      sampler_maker & borderColor(const vk::BorderColor color);
      sampler_maker & unnormalizedCoordinates(const vk::Bool32 enable);

      vk::Sampler create(const std::string &name);
      //VkSampler createNative();
    protected:
      vk::Device device;
      vk::SamplerCreateInfo info;
    };

    class descriptor_set_layout_maker {
    public:
      descriptor_set_layout_maker(vk::Device device);

      descriptor_set_layout_maker & binding(const uint32_t bind, const vk::DescriptorType type, const vk::ShaderStageFlags stage, const uint32_t count = 1);
      descriptor_set_layout_maker & combined(const uint32_t bind, const vk::DescriptorType type, const vk::ShaderStageFlags stage, const std::vector<vk::Sampler> &samplers);

      vk::DescriptorSetLayout create(const std::string &name);
    protected:
      vk::Device device;
      std::vector<vk::DescriptorSetLayoutBinding> bindings;
    };

    class pipeline_layout_maker {
    public:
      pipeline_layout_maker(vk::Device device);

      pipeline_layout_maker & addDescriptorLayout(vk::DescriptorSetLayout setLayout);
      pipeline_layout_maker & addPushConstRange(const uint32_t offset, const uint32_t size, const vk::ShaderStageFlags flag = vk::ShaderStageFlagBits::eVertex);

      vk::PipelineLayout create(const std::string &name);
    protected:
      vk::Device device;
      std::vector<vk::DescriptorSetLayout> setLayouts;
      std::vector<vk::PushConstantRange> ranges;
    };

    class pipeline_maker {
    public:
      pipeline_maker(vk::Device device);

      pipeline_maker & addShader(const vk::ShaderStageFlagBits flag, const vk::ShaderModule module, const char* name = nullptr);
      pipeline_maker & addSpecializationEntry(const uint32_t constantID, const uint32_t offset, const size_t size);
      pipeline_maker & addData(const size_t size, const void* data);

      pipeline_maker & vertexBinding(const uint32_t binding, const uint32_t stride, const vk::VertexInputRate rate = vk::VertexInputRate::eVertex);
      pipeline_maker & vertexAttribute(const uint32_t location, const uint32_t binding, const vk::Format format, const uint32_t offset);
      pipeline_maker & viewport(const vk::Viewport &view);
      pipeline_maker & viewport();
      pipeline_maker & scissor(const vk::Rect2D &scissor);
      pipeline_maker & scissor();

      pipeline_maker & dynamicState(const vk::DynamicState state);

      pipeline_maker & assembly(const vk::PrimitiveTopology topology, const vk::Bool32 primitiveRestartEnable = VK_FALSE);
      pipeline_maker & tessellation(bool enabled, const uint32_t patchControlPoints = 0);

      pipeline_maker & depthClamp(const vk::Bool32 enable);
      pipeline_maker & rasterizerDiscard(const vk::Bool32 enable);
      pipeline_maker & polygonMode(const vk::PolygonMode mode);
      pipeline_maker & cullMode(const vk::CullModeFlags flags);
      pipeline_maker & frontFace(const vk::FrontFace face);
      pipeline_maker & depthBias(const vk::Bool32 enable, const float constFactor = 0.0f, const float clamp = 0.0f, const float slopeFactor = 0.0f);
      pipeline_maker & lineWidth(const float lineWidth);

      pipeline_maker & rasterizationSamples(const vk::SampleCountFlagBits count);
      pipeline_maker & sampleShading(const vk::Bool32 enable, const float minSampleShading = 0.0f, const vk::SampleMask* masks = nullptr);
      pipeline_maker & multisampleCoverage(const vk::Bool32 alphaToCoverage, const vk::Bool32 alphaToOne);

      pipeline_maker & depthTest(const vk::Bool32 enable);
      pipeline_maker & depthWrite(const vk::Bool32 enable);
      pipeline_maker & compare(const vk::CompareOp compare);
      pipeline_maker & stencilTest(const vk::Bool32 enable, const vk::StencilOpState &front = {}, const vk::StencilOpState &back = {});
      pipeline_maker & depthBounds(const vk::Bool32 enable, const float minBounds = 0.0f, const float maxBounds = 0.0f);

      pipeline_maker & logicOp(const vk::Bool32 enable, const vk::LogicOp logic = vk::LogicOp::eCopy);
      pipeline_maker & blendConstant(const float* blendConst);
      pipeline_maker & colorBlending(const vk::PipelineColorBlendAttachmentState &state);

      pipeline_maker & addDefaultBlending();
      pipeline_maker & clearBlending();

      pipeline_maker & colorBlendBegin(const vk::Bool32 enable = VK_TRUE);
      pipeline_maker & srcColor(const vk::BlendFactor value);
      pipeline_maker & dstColor(const vk::BlendFactor value);
      pipeline_maker & colorOp(const vk::BlendOp value);
      pipeline_maker & srcAlpha(const vk::BlendFactor value);
      pipeline_maker & dstAlpha(const vk::BlendFactor value);
      pipeline_maker & alphaOp(const vk::BlendOp value);
      pipeline_maker & colorWriteMask(const vk::ColorComponentFlags flags = DEFAULT_COLOR_WRITE_MASK);
  
      vk::GraphicsPipelineCreateInfo get_info(
        vk::PipelineLayout layout,
        vk::RenderPass renderPass,
        const uint32_t subpass = 0,
        vk::Pipeline base = vk::Pipeline(nullptr),
        const int32_t baseIndex = -1
      );
      
      vk::Pipeline create(
        const std::string &name,
        vk::PipelineCache cache,
        vk::PipelineLayout layout,
        vk::RenderPass renderPass,
        const uint32_t subpass = 0,
        vk::Pipeline base = vk::Pipeline(nullptr),
        const int32_t baseIndex = -1
      );
    protected:
      struct ShaderSpecialization {
        std::vector<vk::SpecializationMapEntry> entries;
        size_t dataSize;
        const void* data;
      };

      bool tessellationState = false;
      vk::Device device = nullptr;

      vk::PipelineVertexInputStateCreateInfo vertexInfo;
      vk::PipelineInputAssemblyStateCreateInfo inputAssembly;
      vk::PipelineTessellationStateCreateInfo tessellationInfo;
      vk::PipelineViewportStateCreateInfo viewportInfo;
      vk::PipelineRasterizationStateCreateInfo rasterisationInfo;
      vk::PipelineMultisampleStateCreateInfo multisamplingInfo;
      vk::PipelineDepthStencilStateCreateInfo depthStensilInfo;
      vk::PipelineColorBlendStateCreateInfo colorBlendingInfo;
      vk::PipelineDynamicStateCreateInfo dymStateInfo;
      vk::PipelineColorBlendAttachmentState defaultBlending;

      std::vector<vk::SpecializationInfo> shaders_specs;
      std::vector<vk::PipelineShaderStageCreateInfo> shaders;
      std::vector<ShaderSpecialization> specs;

      std::vector<vk::VertexInputBindingDescription> vertexBindings;
      std::vector<vk::VertexInputAttributeDescription> vertexAttribs;
      std::vector<vk::Viewport> viewports;
      std::vector<vk::Rect2D> scissors;
      std::vector<vk::PipelineColorBlendAttachmentState> colorBlends;
      std::vector<vk::DynamicState> dynStates;
    };
    
    class compute_pipeline_maker {
    public:
      compute_pipeline_maker(vk::Device device);
      
      //compute_pipeline_maker & shader(const VkShaderStageFlagBits &flag, const std::string &path, const char* name = nullptr);
      compute_pipeline_maker & shader(const vk::ShaderModule module, const char* name = nullptr);
      compute_pipeline_maker & addSpecializationEntry(const uint32_t constantID, const uint32_t offset, const size_t size);
      compute_pipeline_maker & addData(const size_t size, void* data);
      
      vk::Pipeline create(const std::string &name,
                      vk::PipelineLayout layout,
                      vk::Pipeline base = vk::Pipeline(nullptr),
                      const int32_t baseIndex = -1);
    protected:
      vk::Device device = nullptr;
      
      size_t dataSize = 0;
      void* data = nullptr;
      vk::PipelineShaderStageCreateInfo shaderInfo;
      std::vector<vk::SpecializationMapEntry> entries;
    };

    class render_pass_maker {
    public:
      render_pass_maker(vk::Device device);

      render_pass_maker & attachmentBegin(const vk::Format format);
      render_pass_maker & attachmentFlags(const vk::AttachmentDescriptionFlags value);
      render_pass_maker & attachmentFormat(const vk::Format value);
      render_pass_maker & attachmentSamples(const vk::SampleCountFlagBits value);
      render_pass_maker & attachmentLoadOp(const vk::AttachmentLoadOp value);
      render_pass_maker & attachmentStoreOp(const vk::AttachmentStoreOp value);
      render_pass_maker & attachmentStencilLoadOp(const vk::AttachmentLoadOp value);
      render_pass_maker & attachmentStencilStoreOp(const vk::AttachmentStoreOp value);
      render_pass_maker & attachmentInitialLayout(const vk::ImageLayout value);
      render_pass_maker & attachmentFinalLayout(const vk::ImageLayout value);

      render_pass_maker & subpassBegin(const vk::PipelineBindPoint bind = vk::PipelineBindPoint::eGraphics);
      render_pass_maker & subpassInputAttachment(const uint32_t attachment, const vk::ImageLayout layout);
      render_pass_maker & subpassColorAttachment(const uint32_t attachment, const vk::ImageLayout layout);
      render_pass_maker & subpassResolveAttachment(const uint32_t attachment, const vk::ImageLayout layout);
      render_pass_maker & subpassDepthStencilAttachment(const uint32_t attachment, const vk::ImageLayout layout);
      render_pass_maker & addPreservedAttachmentIndex(const uint32_t index);

      render_pass_maker & dependencyBegin(const uint32_t srcSubpass, const uint32_t dstSubpass);
      render_pass_maker & dependencySrcSubpass(const uint32_t value);
      render_pass_maker & dependencyDstSubpass(const uint32_t value);
      render_pass_maker & dependencySrcStageMask(const vk::PipelineStageFlags value);
      render_pass_maker & dependencyDstStageMask(const vk::PipelineStageFlags value);
      render_pass_maker & dependencySrcAccessMask(const vk::AccessFlags value);
      render_pass_maker & dependencyDstAccessMask(const vk::AccessFlags value);
      render_pass_maker & dependencyDependencyFlags(const vk::DependencyFlags value);

      vk::RenderPassCreateInfo get_info();
      vk::RenderPass create(const std::string &name);
    protected:
      struct subpass_description {
        vk::SubpassDescriptionFlags flags;
        vk::PipelineBindPoint pipelineBindPoint;
        
        std::vector<vk::AttachmentReference> input; // уникальный
        std::vector<vk::AttachmentReference> color;
        std::vector<vk::AttachmentReference> resolve;
        vk::AttachmentReference stensil = {0, {}};
        std::vector<uint32_t> preservedAttachments;
      };
      
      vk::Device device;
      std::vector<vk::AttachmentDescription> attachments;
      std::vector<vk::SubpassDependency> dependencies;
      std::vector<subpass_description> descriptions;
      std::vector<vk::SubpassDescription> descs;
    };

    // layers такие же как и у инстанса
    class device_maker {
    public:
      device_maker(vk::Instance inst);

      device_maker & beginDevice(const vk::PhysicalDevice phys);
      device_maker & createQueues(const uint32_t maxCount = 4, const float* priority = nullptr);
      device_maker & createQueue(const uint32_t queue_family_index, const uint32_t maxCount = 4, const float* priority = nullptr);
      device_maker & features(const vk::PhysicalDeviceFeatures &f);
      device_maker & setExtensions(const std::vector<const char*> &extensions, bool printExtensionInfo = false);

      vk::Device create(const std::vector<const char*> &layers, const std::string &name);
    protected:
      struct queue_info {
        uint32_t count;
        vk::QueueFlags flags;
      };
      
      bool printExtensionInfo;
      
      vk::Instance inst;
      vk::PhysicalDevice phys;

      float** priorities = nullptr;

      std::vector<const char*> extensions;
      std::vector<vk::DeviceQueueCreateInfo> queueInfos;
      std::vector<queue_info> familyProperties;

      vk::PhysicalDeviceFeatures f;
      vk::DeviceCreateInfo info;
    };
  }
}

#endif