#include "compute_context.h"

#include <algorithm>
#include <cstring>

#include "devils_engine/utils/core.h"

#include "graphics_base.h"
#include "makers.h"
#include "shader_crafter.h"
#include "system_info.h"
#include "vulkan_header.h"

#include <shaderc/shaderc.h>

namespace devils_engine {
namespace painter {

struct compute_context::buffer_entry {
  VkBuffer handle = nullptr;
  VmaAllocation allocation = nullptr;
  void* mapped = nullptr;
  size_t byte_size = 0;
};

struct compute_context::image_entry {
  VkImage handle = nullptr;
  VkImageView view = nullptr;
  VmaAllocation allocation = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  size_t byte_size = 0;
};

struct compute_context::program_entry {
  // Ключ кэша: сам текст плюс форма привязок. Текст хранится целиком, а не только его хеш — на хеше
  // столкновение означало бы, что вместо одной программы исполняется другая, и понять это по
  // результату невозможно.
  std::string source;
  VkDescriptorSetLayout set_layout = nullptr;
  VkPipelineLayout pipeline_layout = nullptr;
  VkPipeline pipeline = nullptr;
  VkDescriptorSet set = nullptr;
  std::vector<binding_kind> bindings;
  uint32_t push_byte_size = 0;
};

struct compute_context::binding_set_entry {
  VkDescriptorSet set = nullptr;
  program_id program = invalid_id;
};

bool compute_device_available() {
  // Инстанс поднимается и опускается: узнать, есть ли устройство, иначе нечем, а «нет устройства» —
  // законный ответ, а не ошибка. Валидация здесь выключена намеренно: проверка не должна падать на
  // машине, где слоёв не установлено.
  instance_options options;
  options.app_name = "devils_compute_probe";
  options.presentation = false;
  options.validation = false;

  try {
    const auto created = create_instance(options);
    if (created.instance == nullptr) {
      return false;
    }

    bool found = false;
    try {
      system_info si(created.instance);
      const auto data = si.choose_physical_device_headless();
      found = data.handle != nullptr;
    } catch (const std::exception&) {
      found = false;
    }

    vk::Instance(created.instance).destroy();
    return found;
  } catch (const std::exception&) {
    return false;
  }
}

compute_context::compute_context(compute_context_config config) : config_(std::move(config)) {
  instance_options options;
  options.app_name = config_.app_name;
  options.presentation = false;
  options.validation = config_.validation;
  instance_ = create_instance(options);

  system_info si(instance_.instance);
  const auto physical = si.choose_physical_device_headless();
  if (physical.handle == nullptr) {
    utils::error{}("compute context '{}': no Vulkan device is available", config_.app_name);
  }
  system_info::print_choosed_device(physical.handle);

  device_ = create_device(instance_.instance, physical, false, config_.app_name);

  const auto properties = vk::PhysicalDevice(physical.handle).getProperties();
  device_name_.assign(properties.deviceName.data());
  for (uint32_t axis = 0; axis < 3; ++axis) {
    limits_.max_group_count[axis] = properties.limits.maxComputeWorkGroupCount[axis];
    limits_.max_group_size[axis] = properties.limits.maxComputeWorkGroupSize[axis];
  }
  limits_.max_group_invocations = properties.limits.maxComputeWorkGroupInvocations;
  limits_.max_image_2d = properties.limits.maxImageDimension2D;
  limits_.max_shared_memory_bytes = properties.limits.maxComputeSharedMemorySize;
  limits_.max_storage_buffer_range = properties.limits.maxStorageBufferRange;

  base_ = std::make_unique<graphics_base>(
    instance_.instance, device_.device, physical.handle, presentation_engine_type::no_present);
  base_->create_allocator();
  base_->create_descriptor_pool();
  if (config_.pipeline_cache_path.empty()) {
    // Пустой путь означает кэш в памяти: `get_or_create_pipeline_cache` с несуществующим файлом
    // создаёт пустой кэш и не пишет его обратно, поэтому отдельной ветки для этого не нужно.
    base_->get_or_create_pipeline_cache(std::string{});
  } else {
    base_->get_or_create_pipeline_cache(config_.pipeline_cache_path);
  }
  base_->set_shader_cache_directory(config_.shader_cache_directory);

  // Пул СВОЙ и на вычислительной очереди: graphics_base создаёт его на семействе графики, а
  // отдельная вычислительная очередь ради этого и планируется.
  vk::Device dev(device_.device);
  vk::CommandPoolCreateInfo cpci{};
  cpci.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  cpci.queueFamilyIndex = device_.queues.compute.family_index();
  command_pool_ = dev.createCommandPool(cpci);
  set_name(dev, vk::CommandPool(command_pool_), config_.app_name + ".compute_command_pool");

  // Один линейный сэмплер на контекст. Он и есть та причина, по которой картинка бывает выгоднее
  // буфера: фильтр между элементами достаётся аппаратно. Он же и граница — фильтр не детерминирован.
  vk::SamplerCreateInfo sci{};
  sci.magFilter = vk::Filter::eLinear;
  sci.minFilter = vk::Filter::eLinear;
  sci.addressModeU = vk::SamplerAddressMode::eClampToEdge;
  sci.addressModeV = vk::SamplerAddressMode::eClampToEdge;
  sci.addressModeW = vk::SamplerAddressMode::eClampToEdge;
  linear_sampler_ = dev.createSampler(sci);
  set_name(dev, vk::Sampler(linear_sampler_), config_.app_name + ".linear_sampler");

  // Гранулярный fence, а не device-wide waitIdle: ждать надо ровно свою отправку.
  fence_ = dev.createFence(vk::FenceCreateInfo{});
  set_name(dev, vk::Fence(fence_), config_.app_name + ".compute_fence");
}

compute_context::~compute_context() noexcept {
  if (device_.device == nullptr) {
    return;
  }

  vk::Device dev(device_.device);
  dev.waitIdle();

  for (auto& program : programs_) {
    if (program.pipeline != nullptr) dev.destroy(vk::Pipeline(program.pipeline));
    if (program.pipeline_layout != nullptr) dev.destroy(vk::PipelineLayout(program.pipeline_layout));
    if (program.set_layout != nullptr) dev.destroy(vk::DescriptorSetLayout(program.set_layout));
  }
  programs_.clear();

  if (base_ != nullptr && base_->allocator != nullptr) {
    vma::Allocator allocator(base_->allocator);
    for (auto& entry : images_) {
      if (entry.view != nullptr) {
        dev.destroy(vk::ImageView(entry.view));
      }
      if (entry.handle != nullptr) {
        allocator.destroyImage(vk::Image(entry.handle), vma::Allocation(entry.allocation));
      }
    }
    for (auto& entry : buffers_) {
      if (entry.mapped != nullptr) {
        allocator.unmapMemory(vma::Allocation(entry.allocation));
      }
      if (entry.handle != nullptr) {
        allocator.destroyBuffer(vk::Buffer(entry.handle), vma::Allocation(entry.allocation));
      }
    }
  }
  buffers_.clear();

  images_.clear();

  if (linear_sampler_ != nullptr) dev.destroy(vk::Sampler(linear_sampler_));
  if (fence_ != nullptr) dev.destroy(vk::Fence(fence_));
  if (command_pool_ != nullptr) dev.destroy(vk::CommandPool(command_pool_));

  // ОБЪЯВЛЕННЫЙ ПУТЬ КЭША ОБЯЗАН БЫТЬ ЗАПИСАН. Читать файл и не писать его — это путь, который
  // выглядит кэшем и им не является: цена компиляции ИЗМЕРЕНА и составляет около ста миллисекунд на
  // программу, то есть без записи каждый запуск платит её заново.
  if (base_ != nullptr && !config_.pipeline_cache_path.empty()) {
    base_->dump_cache_on_disk(config_.pipeline_cache_path);
  }

  // graphics_base снимает аллокатор, пул дескрипторов и кэш пайплайнов; устройство и инстанс он не
  // трогает — их создал этот контекст, ему их и закрывать.
  base_.reset();

  dev.destroy();
  if (instance_.messenger != nullptr) {
    destroy_debug_messenger(instance_.instance, instance_.messenger);
  }
  vk::Instance(instance_.instance).destroy();
}

VkDevice compute_context::device() const noexcept {
  return device_.device;
}

const std::string& compute_context::device_name() const noexcept {
  return device_name_;
}

const compute_context::device_limits& compute_context::limits() const noexcept {
  return limits_;
}

const compute_context::buffer_entry& compute_context::buffer_at(const buffer_id id) const {
  if (id >= buffers_.size()) {
    utils::error{}("compute context '{}': buffer {} does not exist", config_.app_name, id);
  }
  return buffers_[id];
}

compute_context::buffer_id compute_context::create_buffer(const size_t byte_size, const bool host_visible) {
  if (byte_size == 0) {
    utils::error{}("compute context '{}': a buffer of zero bytes has nothing to compute over", config_.app_name);
  }
  if (byte_size > limits_.max_storage_buffer_range) {
    utils::error{}("compute context '{}': buffer of {} bytes exceeds maxStorageBufferRange {} on '{}'",
                   config_.app_name, byte_size, limits_.max_storage_buffer_range, device_name_);
  }

  vk::BufferCreateInfo bci{};
  bci.size = byte_size;
  bci.usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
              vk::BufferUsageFlagBits::eTransferDst;
  bci.sharingMode = vk::SharingMode::eExclusive;

  vma::AllocationCreateInfo aci{};
  aci.usage = host_visible ? vma::MemoryUsage::eCpuToGpu : vma::MemoryUsage::eGpuOnly;
  if (host_visible) {
    aci.flags = vma::AllocationCreateFlagBits::eMapped;
  }

  vma::Allocator allocator(base_->allocator);
  auto [handle, allocation] = allocator.createBuffer(bci, aci);

  buffer_entry entry;
  entry.handle = handle;
  entry.allocation = allocation;
  entry.byte_size = byte_size;
  if (host_visible) {
    entry.mapped = allocator.mapMemory(allocation);
  }

  set_name(vk::Device(device_.device), vk::Buffer(handle),
           config_.app_name + ".buffer" + std::to_string(buffers_.size()));
  buffers_.push_back(entry);
  return buffer_id(buffers_.size() - 1);
}

size_t compute_context::buffer_byte_size(const buffer_id id) const {
  return buffer_at(id).byte_size;
}

void compute_context::write(const buffer_id id, const void* data, const size_t byte_size) {
  const auto& entry = buffer_at(id);
  if (entry.mapped == nullptr) {
    utils::error{}("compute context '{}': buffer {} lives on the device and has no host mapping",
                   config_.app_name, id);
  }
  if (byte_size > entry.byte_size) {
    utils::error{}("compute context '{}': writing {} bytes into buffer {} of {} bytes",
                   config_.app_name, byte_size, id, entry.byte_size);
  }
  std::memcpy(entry.mapped, data, byte_size);
  vma::Allocator(base_->allocator).flushAllocation(vma::Allocation(entry.allocation), 0, byte_size);
}

void compute_context::read(const buffer_id id, void* data, const size_t byte_size) const {
  const auto& entry = buffer_at(id);
  if (entry.mapped == nullptr) {
    utils::error{}("compute context '{}': buffer {} lives on the device and has no host mapping",
                   config_.app_name, id);
  }
  if (byte_size > entry.byte_size) {
    utils::error{}("compute context '{}': reading {} bytes from buffer {} of {} bytes",
                   config_.app_name, byte_size, id, entry.byte_size);
  }
  vma::Allocator(base_->allocator).invalidateAllocation(vma::Allocation(entry.allocation), 0, byte_size);
  std::memcpy(data, entry.mapped, byte_size);
}

void compute_context::copy(const buffer_id from, const buffer_id to, const size_t byte_size) {
  const auto& source = buffer_at(from);
  const auto& target = buffer_at(to);
  if (byte_size > source.byte_size || byte_size > target.byte_size) {
    utils::error{}("compute context '{}': copying {} bytes between buffers of {} and {} bytes",
                   config_.app_name, byte_size, source.byte_size, target.byte_size);
  }

  const bool done = do_command(
    device_.device, command_pool_, device_.queues.compute.handle(), fence_,
    [&](VkCommandBuffer raw) {
      vk::CommandBuffer buf(raw);
      const vk::BufferCopy region(0, 0, byte_size);
      buf.copyBuffer(vk::Buffer(source.handle), vk::Buffer(target.handle), 1, &region);
    });
  if (!done) {
    utils::error{}("compute context '{}': buffer copy did not complete", config_.app_name);
  }
}

const compute_context::image_entry& compute_context::image_at(const image_id id) const {
  if (id >= images_.size()) {
    utils::error{}("compute context '{}': image {} does not exist", config_.app_name, id);
  }
  return images_[id];
}

compute_context::image_id compute_context::create_image(const uint32_t width,
                                                        const uint32_t height,
                                                        const image_format format) {
  if (width == 0 || height == 0) {
    utils::error{}("compute context '{}': an image of {}x{} has nothing to compute over",
                   config_.app_name, width, height);
  }

  const auto properties = vk::PhysicalDevice(base_->physical_device).getProperties();
  if (width > properties.limits.maxImageDimension2D || height > properties.limits.maxImageDimension2D) {
    utils::error{}("compute context '{}': image {}x{} exceeds maxImageDimension2D {} on '{}'",
                   config_.app_name, width, height, properties.limits.maxImageDimension2D, device_name_);
  }

  const auto vk_format = format == image_format::r32f ? vk::Format::eR32Sfloat : vk::Format::eR8G8B8A8Unorm;
  const size_t pixel_bytes = format == image_format::r32f ? 4 : 4;

  // ФОРМАТ СПРАШИВАЕТСЯ У УСТРОЙСТВА, а не берётся на веру. Обязательные форматы Vulkan гарантируют
  // и storage-образ, и линейный фильтр для этих двух, но «гарантирует спецификация» и «умеет ЭТА
  // машина» — разные утверждения, а молча отсутствующая возможность даёт неопределённое поведение.
  const auto supported = vk::PhysicalDevice(base_->physical_device).getFormatProperties(vk_format);
  const auto required = vk::FormatFeatureFlagBits::eStorageImage | vk::FormatFeatureFlagBits::eSampledImage |
                        vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
  if ((supported.optimalTilingFeatures & required) != required) {
    utils::error{}("compute context '{}': format for the requested image lacks storage, sampling or linear "
                   "filtering on '{}'",
                   config_.app_name, device_name_);
  }

  vk::ImageCreateInfo ici{};
  ici.imageType = vk::ImageType::e2D;
  ici.format = vk_format;
  ici.extent = vk::Extent3D(width, height, 1);
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = vk::SampleCountFlagBits::e1;
  ici.tiling = vk::ImageTiling::eOptimal;
  ici.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
              vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
  ici.sharingMode = vk::SharingMode::eExclusive;
  ici.initialLayout = vk::ImageLayout::eUndefined;

  vma::AllocationCreateInfo aci{};
  aci.usage = vma::MemoryUsage::eGpuOnly;

  vma::Allocator allocator(base_->allocator);
  auto [handle, allocation] = allocator.createImage(ici, aci);

  vk::Device dev(device_.device);
  vk::ImageViewCreateInfo ivci{};
  ivci.image = handle;
  ivci.viewType = vk::ImageViewType::e2D;
  ivci.format = vk_format;
  ivci.subresourceRange = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

  image_entry entry;
  entry.handle = handle;
  entry.allocation = allocation;
  entry.view = dev.createImageView(ivci);
  entry.width = width;
  entry.height = height;
  entry.format = uint32_t(vk_format);
  entry.byte_size = size_t(width) * size_t(height) * pixel_bytes;

  set_name(dev, vk::Image(handle), config_.app_name + ".image" + std::to_string(images_.size()));

  // ОДИН переход: UNDEFINED -> GENERAL, и дальше картинка в нём и живёт. Упрощение лаборатории, и оно
  // названо в заголовке: GENERAL законен и для записи, и для выборки, но дороже оптимального.
  const bool done = do_command(
    device_.device, command_pool_, device_.queues.compute.handle(), fence_,
    [&](VkCommandBuffer raw) {
      vk::CommandBuffer buf(raw);
      vk::ImageMemoryBarrier barrier{};
      barrier.oldLayout = vk::ImageLayout::eUndefined;
      barrier.newLayout = vk::ImageLayout::eGeneral;
      barrier.image = handle;
      barrier.subresourceRange = ivci.subresourceRange;
      barrier.srcAccessMask = {};
      barrier.dstAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eShaderRead;
      buf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eComputeShader,
                          {}, {}, {}, {barrier});
    });
  if (!done) {
    utils::error{}("compute context '{}': image layout transition did not complete", config_.app_name);
  }

  images_.push_back(entry);
  return image_id(images_.size() - 1);
}

uint32_t compute_context::image_width(const image_id id) const {
  return image_at(id).width;
}

uint32_t compute_context::image_height(const image_id id) const {
  return image_at(id).height;
}

void compute_context::read_image(const image_id id, void* data, const size_t byte_size) {
  const auto& entry = image_at(id);
  if (byte_size > entry.byte_size) {
    utils::error{}("compute context '{}': reading {} bytes from an image of {} bytes",
                   config_.app_name, byte_size, entry.byte_size);
  }

  const auto staging = create_buffer(entry.byte_size, true);
  const auto& target = buffer_at(staging);

  const bool done = do_command(
    device_.device, command_pool_, device_.queues.compute.handle(), fence_,
    [&](VkCommandBuffer raw) {
      vk::CommandBuffer buf(raw);
      vk::BufferImageCopy region{};
      region.imageSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
      region.imageExtent = vk::Extent3D(entry.width, entry.height, 1);
      buf.copyImageToBuffer(vk::Image(entry.handle), vk::ImageLayout::eGeneral, vk::Buffer(target.handle),
                            1, &region);
    });
  if (!done) {
    utils::error{}("compute context '{}': image readback did not complete", config_.app_name);
  }

  read(staging, data, byte_size);
}

compute_context::program_id compute_context::create_program(const std::string& name,
                                                            const std::string& source,
                                                            const uint32_t storage_count,
                                                            const uint32_t push_byte_size) {
  // Прежняя форма: все привязки — storage-буферы. Осталась потому, что так их объявляет
  // подавляющее большинство переводов, и заставлять их перечислять один и тот же род было бы шумом.
  const std::vector<binding_kind> bindings(storage_count, binding_kind::storage_buffer);
  return make_program(name, source, bindings, push_byte_size);
}

compute_context::program_id compute_context::create_program(const std::string& name,
                                                            const std::string& source,
                                                            const std::span<const binding_kind>& bindings,
                                                            const uint32_t push_byte_size) {
  return make_program(name, source, bindings, push_byte_size);
}

compute_context::program_id compute_context::make_program(const std::string& name,
                                                          const std::string& source,
                                                          const std::span<const binding_kind>& bindings,
                                                          const uint32_t push_byte_size) {
  if (bindings.empty()) {
    utils::error{}("compute context '{}': program '{}' binds nothing and has nothing to read or write",
                   config_.app_name, name);
  }

  for (size_t i = 0; i < programs_.size(); ++i) {
    const auto& known = programs_[i];
    if (known.push_byte_size != push_byte_size) continue;
    if (known.bindings.size() != bindings.size()) continue;
    if (!std::equal(known.bindings.begin(), known.bindings.end(), bindings.begin())) continue;
    if (known.source != source) continue;
    return program_id(i);
  }

  // Тем же компилятором, которым движок компилирует шейдеры материалов. Реестр не нужен: текст
  // приходит целиком, а `#include` у шейдера, написанного руками, нет.
  shader_crafter crafter(nullptr);
  crafter.set_optimization(config_.optimize_shaders);
  crafter.set_shader_entry_point("main");
  crafter.set_shader_type(shaderc_compute_shader);
  const auto spirv = crafter.compile(base_->shaders(), name, source);
  if (spirv.empty()) {
    utils::error{}("compute context '{}': program '{}' did not compile: {}",
                   config_.app_name, name, crafter.err_msg());
  }

  vk::Device dev(device_.device);

  const auto descriptor_of = [](const binding_kind kind) {
    switch (kind) {
      case binding_kind::storage_image: return vk::DescriptorType::eStorageImage;
      case binding_kind::sampled_image: return vk::DescriptorType::eCombinedImageSampler;
      default: return vk::DescriptorType::eStorageBuffer;
    }
  };

  descriptor_set_layout_maker layout_maker(dev);
  for (size_t binding = 0; binding < bindings.size(); ++binding) {
    layout_maker.binding(uint32_t(binding), descriptor_of(bindings[binding]), vk::ShaderStageFlagBits::eCompute);
  }

  program_entry program;
  program.source = source;
  program.bindings.assign(bindings.begin(), bindings.end());
  program.push_byte_size = push_byte_size;
  program.set_layout = layout_maker.create(name + ".set_layout");

  pipeline_layout_maker pipeline_layout(dev);
  pipeline_layout.addDescriptorLayout(vk::DescriptorSetLayout(program.set_layout));
  if (push_byte_size != 0) {
    pipeline_layout.addPushConstRange(0, push_byte_size, vk::ShaderStageFlagBits::eCompute);
  }
  program.pipeline_layout = pipeline_layout.create(name + ".pipeline_layout");

  vk::ShaderModuleCreateInfo smci{};
  smci.codeSize = spirv.size() * sizeof(uint32_t);
  smci.pCode = spirv.data();
  const auto module = dev.createShaderModuleUnique(smci);

  compute_pipeline_maker pipeline_maker(dev);
  pipeline_maker.shader(module.get());
  // Через КЭШ ПАЙПЛАЙНОВ. До этой правки вычислительный путь его не использовал: maker звал
  // `createComputePipeline` с nullptr, а созданный без кэша пайплайн работает точно так же — просто
  // создаётся дольше, и по поведению это неотличимо.
  program.pipeline =
    pipeline_maker.create(name, vk::PipelineCache(base_->cache), vk::PipelineLayout(program.pipeline_layout));

  descriptor_set_maker set_maker(dev);
  set_maker.layout(vk::DescriptorSetLayout(program.set_layout));
  const auto sets = set_maker.create(vk::DescriptorPool(base_->descriptor_pool), name + ".set");
  if (sets.empty()) {
    utils::error{}("compute context '{}': program '{}' got no descriptor set", config_.app_name, name);
  }
  program.set = sets.front();

  programs_.push_back(std::move(program));
  ++compiled_;
  return program_id(programs_.size() - 1);
}

size_t compute_context::compiled_programs() const noexcept {
  return compiled_;
}

void compute_context::dispatch(const program_id id,
                               const std::span<const buffer_id>& buffers,
                               const void* push,
                               const size_t push_byte_size,
                               const size_t element_count,
                               const uint32_t group_size) {
  if (id >= programs_.size()) {
    utils::error{}("compute context '{}': program {} does not exist", config_.app_name, id);
  }
  const auto& program = programs_[id];

  if (buffers.size() != program.bindings.size()) {
    utils::error{}("compute context '{}': program expects {} bindings, got {}",
                   config_.app_name, program.bindings.size(), buffers.size());
  }
  for (const auto kind : program.bindings) {
    if (kind == binding_kind::storage_buffer) continue;
    utils::error{}("compute context '{}': program binds an image, so it needs dispatch_2d with typed resources",
                   config_.app_name);
  }
  if (push_byte_size != program.push_byte_size) {
    utils::error{}("compute context '{}': program declares {} push bytes, got {}",
                   config_.app_name, program.push_byte_size, push_byte_size);
  }
  if (group_size == 0 || group_size > limits_.max_group_invocations || group_size > limits_.max_group_size[0]) {
    utils::error{}("compute context '{}': group size {} does not fit '{}' (max invocations {}, max size x {})",
                   config_.app_name, group_size, device_name_, limits_.max_group_invocations,
                   limits_.max_group_size[0]);
  }
  if (element_count == 0) {
    return;
  }

  vk::Device dev(device_.device);

  std::vector<vk::DescriptorBufferInfo> infos;
  std::vector<vk::WriteDescriptorSet> writes;
  infos.reserve(buffers.size());
  writes.reserve(buffers.size());
  for (size_t i = 0; i < buffers.size(); ++i) {
    const auto& entry = buffer_at(buffers[i]);
    infos.emplace_back(vk::Buffer(entry.handle), 0, entry.byte_size);
  }
  for (size_t i = 0; i < buffers.size(); ++i) {
    writes.emplace_back(vk::DescriptorSet(program.set), uint32_t(i), 0, 1,
                        vk::DescriptorType::eStorageBuffer, nullptr, &infos[i]);
  }
  dev.updateDescriptorSets(writes, {});

  // ЧИСЛО ГРУПП СВОРАЧИВАЕТСЯ В ОСИ, и это не оптимизация. Гарантированный Vulkan'ом минимум
  // `maxComputeWorkGroupCount[0]` это 65535, то есть при группе 64 линейный диспатч кончается на
  // 4.2 млн элементов — а буферы генератора бывают и по 16.7 млн. Одномерный диспатч прошёл бы на
  // машине автора (у Intel предел по x это 2^31-1) и упал бы у игрока.
  const uint64_t total_groups = (uint64_t(element_count) + group_size - 1) / group_size;
  uint32_t groups_x = uint32_t(std::min<uint64_t>(total_groups, limits_.max_group_count[0]));
  uint32_t groups_y = 1;
  if (total_groups > limits_.max_group_count[0]) {
    groups_x = limits_.max_group_count[0];
    const uint64_t rows = (total_groups + groups_x - 1) / groups_x;
    if (rows > limits_.max_group_count[1]) {
      utils::error{}("compute context '{}': {} elements need {} groups, which does not fit {}x{} on '{}'",
                     config_.app_name, element_count, total_groups, limits_.max_group_count[0],
                     limits_.max_group_count[1], device_name_);
    }
    groups_y = uint32_t(rows);
  }

  const bool done = do_command(
    device_.device, command_pool_, device_.queues.compute.handle(), fence_,
    [&](VkCommandBuffer raw) {
      vk::CommandBuffer buf(raw);
      buf.bindPipeline(vk::PipelineBindPoint::eCompute, vk::Pipeline(program.pipeline));
      buf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, vk::PipelineLayout(program.pipeline_layout), 0,
                             {vk::DescriptorSet(program.set)}, {});
      if (push_byte_size != 0) {
        buf.pushConstants(vk::PipelineLayout(program.pipeline_layout), vk::ShaderStageFlagBits::eCompute, 0,
                          uint32_t(push_byte_size), push);
      }
      buf.dispatch(groups_x, groups_y, 1);
    });
  if (!done) {
    utils::error{}("compute context '{}': dispatch did not complete", config_.app_name);
  }
}

void compute_context::dispatch_2d(const program_id id,
                                  const std::span<const bound_resource>& resources,
                                  const void* push,
                                  const size_t push_byte_size,
                                  const uint32_t width,
                                  const uint32_t height,
                                  const uint32_t group_x,
                                  const uint32_t group_y) {
  if (id >= programs_.size()) {
    utils::error{}("compute context '{}': program {} does not exist", config_.app_name, id);
  }
  const auto& program = programs_[id];

  if (resources.size() != program.bindings.size()) {
    utils::error{}("compute context '{}': program expects {} bindings, got {}",
                   config_.app_name, program.bindings.size(), resources.size());
  }
  if (push_byte_size != program.push_byte_size) {
    utils::error{}("compute context '{}': program declares {} push bytes, got {}",
                   config_.app_name, program.push_byte_size, push_byte_size);
  }
  if (group_x == 0 || group_y == 0 || uint64_t(group_x) * group_y > limits_.max_group_invocations) {
    utils::error{}("compute context '{}': group {}x{} does not fit '{}' (max invocations {})",
                   config_.app_name, group_x, group_y, device_name_, limits_.max_group_invocations);
  }
  if (width == 0 || height == 0) {
    return;
  }

  vk::Device dev(device_.device);

  std::vector<vk::DescriptorBufferInfo> buffer_infos(resources.size());
  std::vector<vk::DescriptorImageInfo> image_infos(resources.size());
  std::vector<vk::WriteDescriptorSet> writes;
  writes.reserve(resources.size());

  for (size_t i = 0; i < resources.size(); ++i) {
    const auto kind = program.bindings[i];
    const auto& resource = resources[i];

    // Род ресурса обязан совпасть с родом биндинга. Дескриптор типизирован, и перепутанные род
    // означал бы чтение картинки как буфера — а это не ошибка драйвера, это молча другие числа.
    if (kind == binding_kind::storage_buffer) {
      if (resource.buffer == invalid_id) {
        utils::error{}("compute context '{}': binding {} wants a buffer, got an image", config_.app_name, i);
      }
      const auto& entry = buffer_at(resource.buffer);
      buffer_infos[i] = vk::DescriptorBufferInfo(vk::Buffer(entry.handle), 0, entry.byte_size);
      writes.emplace_back(vk::DescriptorSet(program.set), uint32_t(i), 0, 1,
                          vk::DescriptorType::eStorageBuffer, nullptr, &buffer_infos[i]);
      continue;
    }

    if (resource.image == invalid_id) {
      utils::error{}("compute context '{}': binding {} wants an image, got a buffer", config_.app_name, i);
    }
    const auto& entry = image_at(resource.image);
    const bool sampled = kind == binding_kind::sampled_image;
    image_infos[i] = vk::DescriptorImageInfo(sampled ? vk::Sampler(linear_sampler_) : vk::Sampler(nullptr),
                                             vk::ImageView(entry.view), vk::ImageLayout::eGeneral);
    writes.emplace_back(vk::DescriptorSet(program.set), uint32_t(i), 0, 1,
                        sampled ? vk::DescriptorType::eCombinedImageSampler : vk::DescriptorType::eStorageImage,
                        &image_infos[i], nullptr);
  }

  dev.updateDescriptorSets(writes, {});

  const uint32_t groups_x = (width + group_x - 1) / group_x;
  const uint32_t groups_y = (height + group_y - 1) / group_y;
  if (groups_x > limits_.max_group_count[0] || groups_y > limits_.max_group_count[1]) {
    utils::error{}("compute context '{}': {}x{} needs {}x{} groups, which does not fit {}x{} on '{}'",
                   config_.app_name, width, height, groups_x, groups_y, limits_.max_group_count[0],
                   limits_.max_group_count[1], device_name_);
  }

  const bool done = do_command(
    device_.device, command_pool_, device_.queues.compute.handle(), fence_,
    [&](VkCommandBuffer raw) {
      vk::CommandBuffer buf(raw);
      buf.bindPipeline(vk::PipelineBindPoint::eCompute, vk::Pipeline(program.pipeline));
      buf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, vk::PipelineLayout(program.pipeline_layout), 0,
                             {vk::DescriptorSet(program.set)}, {});
      if (push_byte_size != 0) {
        buf.pushConstants(vk::PipelineLayout(program.pipeline_layout), vk::ShaderStageFlagBits::eCompute, 0,
                          uint32_t(push_byte_size), push);
      }
      buf.dispatch(groups_x, groups_y, 1);
    });
  if (!done) {
    utils::error{}("compute context '{}': dispatch did not complete", config_.app_name);
  }
}

const compute_context::binding_set_entry& compute_context::set_at(const binding_set_id id) const {
  if (id >= sets_.size()) {
    utils::error{}("compute context '{}': binding set {} does not exist", config_.app_name, id);
  }
  return sets_[id];
}

compute_context::binding_set_id compute_context::create_binding_set(const program_id program) {
  if (program >= programs_.size()) {
    utils::error{}("compute context '{}': program {} does not exist", config_.app_name, program);
  }

  descriptor_set_maker maker(vk::Device(device_.device));
  maker.layout(vk::DescriptorSetLayout(programs_[program].set_layout));
  const auto sets = maker.create(vk::DescriptorPool(base_->descriptor_pool),
                                 config_.app_name + ".set" + std::to_string(sets_.size()));
  if (sets.empty()) {
    utils::error{}("compute context '{}': could not allocate a binding set", config_.app_name);
  }

  binding_set_entry entry;
  entry.set = sets.front();
  entry.program = program;
  sets_.push_back(entry);
  return binding_set_id(sets_.size() - 1);
}

void compute_context::update_binding_set(const binding_set_id id,
                                         const std::span<const bound_resource>& resources) {
  const auto& entry = set_at(id);
  const auto& program = programs_[entry.program];

  if (resources.size() != program.bindings.size()) {
    utils::error{}("compute context '{}': the set's program expects {} bindings, got {}",
                   config_.app_name, program.bindings.size(), resources.size());
  }

  std::vector<vk::DescriptorBufferInfo> buffer_infos(resources.size());
  std::vector<vk::DescriptorImageInfo> image_infos(resources.size());
  std::vector<vk::WriteDescriptorSet> writes;
  writes.reserve(resources.size());

  for (size_t i = 0; i < resources.size(); ++i) {
    const auto kind = program.bindings[i];
    const auto& resource = resources[i];

    if (kind == binding_kind::storage_buffer) {
      if (resource.buffer == invalid_id) {
        utils::error{}("compute context '{}': binding {} wants a buffer, got an image", config_.app_name, i);
      }
      const auto& buffer = buffer_at(resource.buffer);
      buffer_infos[i] = vk::DescriptorBufferInfo(vk::Buffer(buffer.handle), 0, buffer.byte_size);
      writes.emplace_back(vk::DescriptorSet(entry.set), uint32_t(i), 0, 1,
                          vk::DescriptorType::eStorageBuffer, nullptr, &buffer_infos[i]);
      continue;
    }

    if (resource.image == invalid_id) {
      utils::error{}("compute context '{}': binding {} wants an image, got a buffer", config_.app_name, i);
    }
    const auto& image = image_at(resource.image);
    const bool sampled = kind == binding_kind::sampled_image;
    image_infos[i] = vk::DescriptorImageInfo(sampled ? vk::Sampler(linear_sampler_) : vk::Sampler(nullptr),
                                             vk::ImageView(image.view), vk::ImageLayout::eGeneral);
    writes.emplace_back(vk::DescriptorSet(entry.set), uint32_t(i), 0, 1,
                        sampled ? vk::DescriptorType::eCombinedImageSampler : vk::DescriptorType::eStorageImage,
                        &image_infos[i], nullptr);
  }

  vk::Device(device_.device).updateDescriptorSets(writes, {});
}

compute_context::recorder::recorder(compute_context& owner, VkCommandBuffer buffer) noexcept
  : owner_(&owner), buffer_(buffer) {}

void compute_context::recorder::copy(const buffer_id from, const buffer_id to, const size_t byte_size) {
  const auto& source = owner_->buffer_at(from);
  const auto& target = owner_->buffer_at(to);
  if (byte_size > source.byte_size || byte_size > target.byte_size) {
    utils::error{}("compute context '{}': copying {} bytes between buffers of {} and {} bytes",
                   owner_->config_.app_name, byte_size, source.byte_size, target.byte_size);
  }
  if (byte_size == 0) {
    return;
  }

  vk::CommandBuffer buf(buffer_);
  const vk::BufferCopy region(0, 0, byte_size);
  buf.copyBuffer(vk::Buffer(source.handle), vk::Buffer(target.handle), 1, &region);
}

void compute_context::recorder::barrier() {
  // Глобальный барьер: запись предыдущей операции видна чтению следующей. Точность до ресурса здесь
  // ничего не дала бы — составитель очереди уже знает, между какими проходами барьер НУЖЕН, и между
  // остальными его не ставит.
  //
  // Стадии названы ОБЕ, вычислительная и трансферная, и это не перестраховка. В одной отправке
  // соседями бывают три разные пары: загрузка -> проход (трансфер -> вычисление), проход -> проход
  // (вычисление -> вычисление) и проход -> выгрузка (вычисление -> трансфер). Барьер только по
  // вычислительной стадии закрывал бы одну из трёх, а две оставшиеся были бы гонкой, которая на
  // интегрированной памяти «работает» и разъезжается на дискретной — то есть ровно тот класс, что
  // работает у автора и ломается у игрока.
  vk::CommandBuffer buf(buffer_);
  vk::MemoryBarrier memory{};
  memory.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
  memory.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite |
                         vk::AccessFlagBits::eTransferRead;
  const auto stages = vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer;
  buf.pipelineBarrier(stages, stages, {}, {memory}, {}, {});
}

void compute_context::recorder::copy_to_image(const buffer_id from, const image_id to) {
  const auto& source = owner_->buffer_at(from);
  const auto& target = owner_->image_at(to);
  if (source.byte_size < target.byte_size) {
    utils::error{}("compute context '{}': filling an image of {} bytes from a buffer of {} bytes",
                   owner_->config_.app_name, target.byte_size, source.byte_size);
  }

  vk::CommandBuffer buf(buffer_);
  vk::BufferImageCopy region{};
  region.imageSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  region.imageExtent = vk::Extent3D(target.width, target.height, 1);
  buf.copyBufferToImage(vk::Buffer(source.handle), vk::Image(target.handle), vk::ImageLayout::eGeneral,
                        1, &region);
}

void compute_context::recorder::copy_from_image(const image_id from, const buffer_id to) {
  const auto& source = owner_->image_at(from);
  const auto& target = owner_->buffer_at(to);
  if (target.byte_size < source.byte_size) {
    utils::error{}("compute context '{}': reading an image of {} bytes into a buffer of {} bytes",
                   owner_->config_.app_name, source.byte_size, target.byte_size);
  }

  vk::CommandBuffer buf(buffer_);
  vk::BufferImageCopy region{};
  region.imageSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  region.imageExtent = vk::Extent3D(source.width, source.height, 1);
  buf.copyImageToBuffer(vk::Image(source.handle), vk::ImageLayout::eGeneral, vk::Buffer(target.handle),
                        1, &region);
}

void compute_context::recorder::dispatch(const program_id program,
                                         const binding_set_id set,
                                         const void* push,
                                         const size_t push_byte_size,
                                         const size_t element_count,
                                         const uint32_t group_size) {
  auto& owner = *owner_;
  if (program >= owner.programs_.size()) {
    utils::error{}("compute context '{}': program {} does not exist", owner.config_.app_name, program);
  }
  const auto& entry = owner.programs_[program];
  const auto& bound = owner.set_at(set);
  if (bound.program != program) {
    utils::error{}("compute context '{}': binding set {} belongs to another program",
                   owner.config_.app_name, set);
  }
  if (push_byte_size != entry.push_byte_size) {
    utils::error{}("compute context '{}': program declares {} push bytes, got {}",
                   owner.config_.app_name, entry.push_byte_size, push_byte_size);
  }
  if (group_size == 0 || group_size > owner.limits_.max_group_invocations) {
    utils::error{}("compute context '{}': group size {} does not fit '{}' (max invocations {})",
                   owner.config_.app_name, group_size, owner.device_name_, owner.limits_.max_group_invocations);
  }
  if (element_count == 0) {
    return;
  }

  const uint64_t total_groups = (uint64_t(element_count) + group_size - 1) / group_size;
  uint32_t groups_x = uint32_t(std::min<uint64_t>(total_groups, owner.limits_.max_group_count[0]));
  uint32_t groups_y = 1;
  if (total_groups > owner.limits_.max_group_count[0]) {
    groups_x = owner.limits_.max_group_count[0];
    const uint64_t rows = (total_groups + groups_x - 1) / groups_x;
    if (rows > owner.limits_.max_group_count[1]) {
      utils::error{}("compute context '{}': {} elements need {} groups, which does not fit {}x{} on '{}'",
                     owner.config_.app_name, element_count, total_groups, owner.limits_.max_group_count[0],
                     owner.limits_.max_group_count[1], owner.device_name_);
    }
    groups_y = uint32_t(rows);
  }

  vk::CommandBuffer buf(buffer_);
  buf.bindPipeline(vk::PipelineBindPoint::eCompute, vk::Pipeline(entry.pipeline));
  buf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, vk::PipelineLayout(entry.pipeline_layout), 0,
                         {vk::DescriptorSet(bound.set)}, {});
  if (push_byte_size != 0) {
    buf.pushConstants(vk::PipelineLayout(entry.pipeline_layout), vk::ShaderStageFlagBits::eCompute, 0,
                      uint32_t(push_byte_size), push);
  }
  buf.dispatch(groups_x, groups_y, 1);
}

void compute_context::submit(const std::function<void(recorder&)>& record) {
  const bool done = do_command(
    device_.device, command_pool_, device_.queues.compute.handle(), fence_,
    [&](VkCommandBuffer raw) {
      recorder rec(*this, raw);
      record(rec);
    });
  if (!done) {
    utils::error{}("compute context '{}': the submission did not complete", config_.app_name);
  }
}

} // namespace painter
} // namespace devils_engine
