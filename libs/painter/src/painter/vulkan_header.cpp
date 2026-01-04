#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include "vulkan_header.h"

#include "auxiliary.h"

#include <gtl/phmap.hpp>

#include "devils_engine/utils/reflect"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace devils_engine {
namespace painter {
template <typename T>
void get_features(size_t &counter, vulkan_features_bitset &bitset, const T &obj) {
  reflect::for_each([&](auto I) {
    using value_type = decltype(reflect::get<I>(obj));
    using mem_type = std::remove_cvref_t<value_type>;
    if constexpr (!std::is_same_v<mem_type, VkBool32>) return;

    const bool val = reflect::get<I>(obj);
    bitset.set(counter, val);
    
    counter += 1;
  }, obj);
}

vulkan_features_bitset make_device_features_bitset(VkPhysicalDevice dev) {
  vulkan_features_bitset ret;
  vk::PhysicalDevice d(dev);

  const auto f = d.getFeatures2();
  const auto &fs10 = VkPhysicalDeviceFeatures(f.features);

  size_t counter = 0;
  get_features(counter, ret, fs10);

  for (auto ptr = reinterpret_cast<const VkBaseInStructure*>(f.pNext); ptr != nullptr; ptr = ptr->pNext) {
    if (ptr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
      auto fs11 = *reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(ptr);
      get_features(counter, ret, fs11);
      continue;
    }

    if (ptr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
      auto fs12 = *reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(ptr);
      get_features(counter, ret, fs12);
      continue;
    }

    if (ptr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
      auto fs13 = *reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(ptr);
      get_features(counter, ret, fs13);
      continue;
    }
  }

  return ret;
}

std::vector<vk::ExtensionProperties> required_device_extensions(vk::PhysicalDevice device, const std::vector<const char*> &layers, const std::vector<const char*> &extensions) {
  std::vector<vk::ExtensionProperties> finalExtensions;

  const auto ext = device.enumerateDeviceExtensionProperties(nullptr);

  gtl::flat_hash_set<std::string> intersection(extensions.begin(), extensions.end());

  for (const auto &extension : ext) {
    if (intersection.find(extension.extensionName) != intersection.end()) finalExtensions.push_back(extension);
  }

  for (auto layer : layers) {
    const auto ext = device.enumerateDeviceExtensionProperties(std::string(layer));

    for (const auto &extension : ext) {
      if (intersection.find(extension.extensionName) != intersection.end()) finalExtensions.push_back(extension);
    }
  }

  return finalExtensions;
}

std::vector<vk::LayerProperties> required_validation_layers(const std::vector<const char*> &layers) {
  const auto &availableLayers = vk::enumerateInstanceLayerProperties();

  gtl::flat_hash_set<std::string> intersection(layers.begin(), layers.end());
  std::vector<vk::LayerProperties> finalLayers;

  for (const auto &layer : availableLayers) {
    auto itr = intersection.find(std::string(layer.layerName.data()));
    if (itr != intersection.end()) finalLayers.push_back(layer);
  }

  return finalLayers;
}

vk::PresentModeKHR choose_swapchain_present_mode(const std::vector<vk::PresentModeKHR> &modes) {
  if (check_swapchain_present_mode(modes, vk::PresentModeKHR::eMailbox)) return vk::PresentModeKHR::eMailbox;
  return vk::PresentModeKHR::eFifo;
}

vk::SurfaceFormatKHR choose_swapchain_surface_format(const std::vector<vk::SurfaceFormatKHR> &formats) {
  if (formats.size() == 1 && formats[0].format == vk::Format::eUndefined) {
    return {vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
  }

  for (const auto &format : formats) {
    if (format.format == vk::Format::eB8G8R8A8Srgb && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
      return format;
    }
  }

  // если вдруг внезапно первые два условия провалились, то можно ранжировать доступные форматы
  // но, в принципе в большинстве случаев, подойдет и первый попавшийся
  return formats[0];
}

vk::Extent2D choose_swapchain_extent(const uint32_t width, const uint32_t height, const vk::SurfaceCapabilitiesKHR& capabilities) {
  vk::Extent2D ext = { width, height };
  ext.width = std::max(capabilities.minImageExtent.width, std::min(capabilities.maxImageExtent.width, ext.width));
  ext.height = std::max(capabilities.minImageExtent.height, std::min(capabilities.maxImageExtent.height, ext.height));

  return ext;
}

bool check_swapchain_present_mode(const std::vector<vk::PresentModeKHR> &modes, const vk::PresentModeKHR mode) {
  for (const auto &available_mode : modes) {
    if (mode == available_mode) return true;
  }

  return false;
}

vk::Format find_supported_format(vk::PhysicalDevice phys, const std::vector<vk::Format> &candidates, const vk::ImageTiling tiling, const vk::FormatFeatureFlags features) {
  for (const auto &format : candidates) {
    vk::FormatProperties props;
    phys.getFormatProperties(format, &props);

    if (tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & features) == features) return format;
    else if (tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & features) == features) return format;
  }

  return vk::Format::eUndefined;
}

vk::ImageCreateInfo texture2D(
  const vk::Extent2D &size, 
  const vk::ImageUsageFlags &usage, 
  const vk::Format &format, 
  const uint32_t &arrayLayers,
  const uint32_t &mipLevels,
  const vk::SampleCountFlagBits &samples,
  const vk::ImageCreateFlags &flags
) {
  return vk::ImageCreateInfo(
    flags, 
    vk::ImageType::e2D, 
    format, 
    {size.width, size.height, 1}, 
    mipLevels, arrayLayers,
    samples, 
    vk::ImageTiling::eOptimal,
    usage,
    vk::SharingMode::eExclusive,
    nullptr,
    vk::ImageLayout::eUndefined
  );
}
    
vk::ImageCreateInfo texture2D_staging(
  const vk::Extent2D &size,
  const vk::ImageUsageFlags &usage,
  const vk::Format &format,
  const vk::ImageCreateFlags &flags
) {
  return vk::ImageCreateInfo(
    flags, 
    vk::ImageType::e2D, 
    format, 
    {size.width, size.height, 1}, 
    1, 1,
    vk::SampleCountFlagBits::e1, 
    vk::ImageTiling::eLinear,
    usage,
    vk::SharingMode::eExclusive,
    nullptr,
    vk::ImageLayout::ePreinitialized
  );
}

vk::ImageViewCreateInfo view_info(
  vk::Image img, vk::Format format, vk::ImageViewType type, const vk::ImageSubresourceRange &r, const vk::ComponentMapping &cm
) {
  return vk::ImageViewCreateInfo({}, img, type, format, cm, r);
}
    
vk::ImageViewCreateInfo make_view_info(
  vk::Image            image,
  vk::Format           format,
  vk::ImageViewType    viewType,
  vk::ImageSubresourceRange subresourceRange,
  vk::ComponentMapping components,
  vk::ImageViewCreateFlags flags
) {
  return vk::ImageViewCreateInfo(flags, image, viewType, format, components, subresourceRange);
}
    
vk::BufferCreateInfo buffer_info(const vk::DeviceSize &size, const vk::BufferUsageFlags &usage, const vk::BufferCreateFlags &flags) {
  return vk::BufferCreateInfo(flags, size, usage, vk::SharingMode::eExclusive, nullptr);
}

std::tuple<vk::BufferCreateInfo, vma::AllocationCreateInfo> dedicated_buffer(const size_t size, const vk::BufferUsageFlags usage, const vma::MemoryUsage memusage, const vk::BufferCreateFlags &flags) {
  const auto memflags = 
    memusage == vma::MemoryUsage::eCpuOnly ||
    memusage == vma::MemoryUsage::eCpuToGpu ||
    memusage == vma::MemoryUsage::eCpuCopy 
  ? vma::AllocationCreateFlagBits::eMapped : vma::AllocationCreateFlags{};
  return std::make_tuple(
    vk::BufferCreateInfo(flags, size, usage, vk::SharingMode::eExclusive, nullptr),
    vma::AllocationCreateInfo(memflags, memusage)
  );
}

vk::ImageUsageFlags main_attachment_usage_from_format(vk::Format format) {
  switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD32Sfloat:
    case vk::Format::eS8Uint:
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
      return vk::ImageUsageFlagBits::eDepthStencilAttachment;
    default: break;
  }

  return vk::ImageUsageFlagBits::eColorAttachment;
}

std::tuple<vk::Image, vma::Allocation> create_image(
  vma::Allocator allocator, 
  const vk::ImageCreateInfo &info,
  const vma::MemoryUsage &mem_usage,
  void** pData,
  const std::string &name
) {
  const bool need_memory_map = mem_usage == vma::MemoryUsage::eCpuOnly || mem_usage == vma::MemoryUsage::eCpuCopy || mem_usage == vma::MemoryUsage::eCpuToGpu;
  const auto fl = need_memory_map ? vma::AllocationCreateFlagBits::eMapped : vma::AllocationCreateFlags();
  const vma::AllocationCreateInfo alloc_info(fl, mem_usage);
  std::pair<vk::Image, vma::Allocation> p;
  if (pData == nullptr) {
    p = allocator.createImage(info, alloc_info);
  } else {
    vma::AllocationInfo i;
    p = allocator.createImage(info, alloc_info, &i);
    *pData = i.pMappedData;
  }
  auto dev = allocator_device(allocator);
  set_name(dev, p.first, name);
  return std::make_tuple(p.first, p.second);
}

std::tuple<vk::AccessFlags, vk::AccessFlags, vk::PipelineStageFlags, vk::PipelineStageFlags> 
  make_barrier_data(const vk::ImageLayout &old, const vk::ImageLayout &New) 
{
  vk::AccessFlags srcFlags, dstFlags; 
  vk::PipelineStageFlags srcStage, dstStage;
      
  switch (old) {
    case vk::ImageLayout::eUndefined:
      srcFlags = vk::AccessFlags(0);
      srcStage = vk::PipelineStageFlagBits::eTopOfPipe;

      break;
    case vk::ImageLayout::ePreinitialized:
      srcFlags = vk::AccessFlagBits::eHostWrite;
      srcStage = vk::PipelineStageFlagBits::eHost;

      break;
    case vk::ImageLayout::eGeneral:
      srcFlags = vk::AccessFlagBits::eShaderWrite;
      srcStage = vk::PipelineStageFlagBits::eComputeShader;

      break;
    case vk::ImageLayout::eColorAttachmentOptimal:
      srcFlags = vk::AccessFlagBits::eColorAttachmentWrite;
      srcStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;

      break;
    case vk::ImageLayout::eDepthStencilAttachmentOptimal:
      srcFlags = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
      srcStage = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;

      break;
    case vk::ImageLayout::eTransferSrcOptimal:
      srcFlags = vk::AccessFlagBits::eTransferRead;
      srcStage = vk::PipelineStageFlagBits::eTransfer;

      break;
    case vk::ImageLayout::eTransferDstOptimal:
      srcFlags = vk::AccessFlagBits::eTransferWrite;
      srcStage = vk::PipelineStageFlagBits::eTransfer;

      break;
    case vk::ImageLayout::eShaderReadOnlyOptimal:
      srcFlags = vk::AccessFlagBits::eShaderRead;
      srcStage = vk::PipelineStageFlagBits::eFragmentShader;

      break;
    case vk::ImageLayout::ePresentSrcKHR:
      srcFlags = vk::AccessFlagBits::eMemoryRead;
      srcStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;

      break;
    default: utils::error{}("The layout '{}' is not supported yet", vk::to_string(old)); break;
  }

  switch (New) {
    case vk::ImageLayout::eColorAttachmentOptimal:
      dstFlags = vk::AccessFlagBits::eColorAttachmentWrite;
      dstStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;

      break;
    case vk::ImageLayout::eDepthStencilAttachmentOptimal:
      dstFlags = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
      dstStage = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;

      break;
    case vk::ImageLayout::eTransferSrcOptimal:
      dstFlags = vk::AccessFlagBits::eTransferRead;
      dstStage = vk::PipelineStageFlagBits::eTransfer;

      break;
    case vk::ImageLayout::eTransferDstOptimal:
      dstFlags = vk::AccessFlagBits::eTransferWrite;
      dstStage = vk::PipelineStageFlagBits::eTransfer;

      break;
    case vk::ImageLayout::eShaderReadOnlyOptimal:
      if (srcFlags == vk::AccessFlags(0)) {
        srcFlags = vk::AccessFlagBits::eHostWrite | vk::AccessFlagBits::eTransferWrite;
        srcStage = vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eHost;
      }

      dstFlags = vk::AccessFlagBits::eShaderRead;
      dstStage = vk::PipelineStageFlagBits::eFragmentShader;

      break;
    case vk::ImageLayout::ePresentSrcKHR:
      dstFlags = vk::AccessFlagBits::eMemoryRead;
      dstStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;

      break;
    case vk::ImageLayout::eGeneral:
      dstFlags = vk::AccessFlagBits::eShaderWrite;
      dstStage = vk::PipelineStageFlagBits::eComputeShader;

      break;
    default: utils::error{}("The layout '{}' is not supported yet", vk::to_string(New)); break;
  }
      
  return std::make_tuple(srcFlags, dstFlags, srcStage, dstStage);
}

std::tuple<vk::ImageMemoryBarrier, vk::PipelineStageFlags, vk::PipelineStageFlags> make_image_memory_barrier(
  vk::Image image, const vk::ImageLayout &old_layout, const vk::ImageLayout &new_layout, const vk::ImageSubresourceRange &range
) {
  vk::ImageMemoryBarrier b({}, {}, old_layout, new_layout, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, range);
  const auto [srcFlags, dstFlags, srcStage, dstStage] = make_barrier_data(old_layout, new_layout);
  b.srcAccessMask = srcFlags;
  b.dstAccessMask = dstFlags;
      
  return std::make_tuple(b, srcStage, dstStage);
}

void change_image_layout(
  vk::Device device, 
  vk::Image image, 
  vk::CommandPool transfer_pool, 
  vk::Queue transfer_queue, 
  vk::Fence fence, 
  const vk::ImageLayout &old_layout, 
  const vk::ImageLayout &new_layout, 
  const vk::ImageSubresourceRange &range
) {
  do_command(device, transfer_pool, transfer_queue, fence, [&] (VkCommandBuffer t) {
    auto task = vk::CommandBuffer(t);
    const auto [b_info, srcStage, dstStage] = make_image_memory_barrier(image, old_layout, new_layout, range);
    //const vk::CommandBufferBeginInfo binfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    //task.begin(binfo);
    task.pipelineBarrier(srcStage, dstStage, vk::DependencyFlagBits::eByRegion, nullptr, nullptr, b_info);
    //task.end();
  });
}

vma::VulkanFunctions make_functions() {
  vma::VulkanFunctions f(
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateMemory,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkMapMemory,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkUnmapMemory,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkFlushMappedMemoryRanges,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkInvalidateMappedMemoryRanges,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkBindBufferMemory,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkBindImageMemory,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferMemoryRequirements,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetImageMemoryRequirements,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateBuffer,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyBuffer,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateImage,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImage,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBuffer,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferMemoryRequirements2KHR,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetImageMemoryRequirements2KHR,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkBindBufferMemory2KHR,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkBindImageMemory2KHR,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties2KHR,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceBufferMemoryRequirements,
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceImageMemoryRequirements
  );

  return f;
}

vk::ComponentMapping to_rgba(vk::Format format) {
  auto r = vk::ComponentSwizzle::eIdentity;
  auto g = vk::ComponentSwizzle::eIdentity;
  auto b = vk::ComponentSwizzle::eIdentity;
  auto a = vk::ComponentSwizzle::eIdentity;

  switch (format) {
    case vk::Format::eB5G6R5UnormPack16:
    case vk::Format::eB5G5R5A1UnormPack16:
    case vk::Format::eB4G4R4A4UnormPack16: 
    case vk::Format::eB8G8R8Unorm:
    case vk::Format::eB8G8R8Snorm:
    case vk::Format::eB8G8R8Uscaled:
    case vk::Format::eB8G8R8Sscaled:
    case vk::Format::eB8G8R8Uint:
    case vk::Format::eB8G8R8Sint:
    case vk::Format::eB8G8R8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Snorm:
    case vk::Format::eB8G8R8A8Uscaled:
    case vk::Format::eB8G8R8A8Sscaled:
    case vk::Format::eB8G8R8A8Uint:
    case vk::Format::eB8G8R8A8Sint:
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eB10G11R11UfloatPack32:
      r = vk::ComponentSwizzle::eB; 
      b = vk::ComponentSwizzle::eR; 
      break;

    case vk::Format::eA8B8G8R8UnormPack32:
    case vk::Format::eA8B8G8R8SnormPack32:
    case vk::Format::eA8B8G8R8UscaledPack32:
    case vk::Format::eA8B8G8R8SscaledPack32:
    case vk::Format::eA8B8G8R8UintPack32:
    case vk::Format::eA8B8G8R8SintPack32:
    case vk::Format::eA8B8G8R8SrgbPack32:
    case vk::Format::eA2B10G10R10UnormPack32:
    case vk::Format::eA2B10G10R10SnormPack32:
    case vk::Format::eA2B10G10R10UscaledPack32:
    case vk::Format::eA2B10G10R10SscaledPack32:
    case vk::Format::eA2B10G10R10UintPack32:
    case vk::Format::eA2B10G10R10SintPack32:
    case vk::Format::eE5B9G9R9UfloatPack32:
    case vk::Format::eA1B5G5R5UnormPack16KHR:
    case vk::Format::eA8UnormKHR:
      r = vk::ComponentSwizzle::eA;
      g = vk::ComponentSwizzle::eB;
      b = vk::ComponentSwizzle::eG;
      a = vk::ComponentSwizzle::eR;
      break;

    case vk::Format::eA2R10G10B10UnormPack32:
    case vk::Format::eA2R10G10B10SnormPack32:
    case vk::Format::eA2R10G10B10UscaledPack32:
    case vk::Format::eA2R10G10B10SscaledPack32:
    case vk::Format::eA2R10G10B10UintPack32:
    case vk::Format::eA2R10G10B10SintPack32:
    case vk::Format::eA1R5G5B5UnormPack16:
      r = vk::ComponentSwizzle::eG;
      g = vk::ComponentSwizzle::eB;
      b = vk::ComponentSwizzle::eA;
      a = vk::ComponentSwizzle::eR;
      break;
    
    case vk::Format::eR4G4UnormPack8:
    case vk::Format::eR4G4B4A4UnormPack16:
    case vk::Format::eR5G6B5UnormPack16:
    case vk::Format::eR5G5B5A1UnormPack16:
    case vk::Format::eR8Unorm:
    case vk::Format::eR8Snorm:
    case vk::Format::eR8Uscaled:
    case vk::Format::eR8Sscaled:
    case vk::Format::eR8Uint:
    case vk::Format::eR8Sint:
    case vk::Format::eR8Srgb:
    case vk::Format::eR8G8Unorm:
    case vk::Format::eR8G8Snorm:
    case vk::Format::eR8G8Uscaled:
    case vk::Format::eR8G8Sscaled:
    case vk::Format::eR8G8Uint:
    case vk::Format::eR8G8Sint:
    case vk::Format::eR8G8Srgb:
    case vk::Format::eR8G8B8Unorm:
    case vk::Format::eR8G8B8Snorm:
    case vk::Format::eR8G8B8Uscaled:
    case vk::Format::eR8G8B8Sscaled:
    case vk::Format::eR8G8B8Uint:
    case vk::Format::eR8G8B8Sint:
    case vk::Format::eR8G8B8Srgb:
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Snorm:
    case vk::Format::eR8G8B8A8Uscaled:
    case vk::Format::eR8G8B8A8Sscaled:
    case vk::Format::eR8G8B8A8Uint:
    case vk::Format::eR8G8B8A8Sint:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eR16Unorm           :
    case vk::Format::eR16Snorm           :
    case vk::Format::eR16Uscaled         :
    case vk::Format::eR16Sscaled         :
    case vk::Format::eR16Uint            :
    case vk::Format::eR16Sint            :
    case vk::Format::eR16Sfloat          :
    case vk::Format::eR16G16Unorm        :
    case vk::Format::eR16G16Snorm        :
    case vk::Format::eR16G16Uscaled      :
    case vk::Format::eR16G16Sscaled      :
    case vk::Format::eR16G16Uint         :
    case vk::Format::eR16G16Sint         :
    case vk::Format::eR16G16Sfloat       :
    case vk::Format::eR16G16B16Unorm     :
    case vk::Format::eR16G16B16Snorm     :
    case vk::Format::eR16G16B16Uscaled   :
    case vk::Format::eR16G16B16Sscaled   :
    case vk::Format::eR16G16B16Uint      :
    case vk::Format::eR16G16B16Sint      :
    case vk::Format::eR16G16B16Sfloat    :
    case vk::Format::eR16G16B16A16Unorm  :
    case vk::Format::eR16G16B16A16Snorm  :
    case vk::Format::eR16G16B16A16Uscaled:
    case vk::Format::eR16G16B16A16Sscaled:
    case vk::Format::eR16G16B16A16Uint   :
    case vk::Format::eR16G16B16A16Sint   :
    case vk::Format::eR16G16B16A16Sfloat :
    case vk::Format::eR32Uint            :
    case vk::Format::eR32Sint            :
    case vk::Format::eR32Sfloat          :
    case vk::Format::eR32G32Uint         :
    case vk::Format::eR32G32Sint         :
    case vk::Format::eR32G32Sfloat       :
    case vk::Format::eR32G32B32Uint      :
    case vk::Format::eR32G32B32Sint      :
    case vk::Format::eR32G32B32Sfloat    :
    case vk::Format::eR32G32B32A32Uint   :
    case vk::Format::eR32G32B32A32Sint   :
    case vk::Format::eR32G32B32A32Sfloat :
    case vk::Format::eR64Uint            :
    case vk::Format::eR64Sint            :
    case vk::Format::eR64Sfloat          :
    case vk::Format::eR64G64Uint         :
    case vk::Format::eR64G64Sint         :
    case vk::Format::eR64G64Sfloat       :
    case vk::Format::eR64G64B64Uint      :
    case vk::Format::eR64G64B64Sint      :
    case vk::Format::eR64G64B64Sfloat    :
    case vk::Format::eR64G64B64A64Uint   :
    case vk::Format::eR64G64B64A64Sint   :
    case vk::Format::eR64G64B64A64Sfloat :
    case vk::Format::eBc1RgbUnormBlock      :
    case vk::Format::eBc1RgbSrgbBlock       :
    case vk::Format::eBc1RgbaUnormBlock     :
    case vk::Format::eBc1RgbaSrgbBlock      :
    case vk::Format::eBc2UnormBlock         :
    case vk::Format::eBc2SrgbBlock          :
    case vk::Format::eBc3UnormBlock         :
    case vk::Format::eBc3SrgbBlock          :
    case vk::Format::eBc4UnormBlock         :
    case vk::Format::eBc4SnormBlock         :
    case vk::Format::eBc5UnormBlock         :
    case vk::Format::eBc5SnormBlock         :
    case vk::Format::eBc6HUfloatBlock       :
    case vk::Format::eBc6HSfloatBlock       :
    case vk::Format::eBc7UnormBlock         :
    case vk::Format::eBc7SrgbBlock          :
    case vk::Format::eEtc2R8G8B8UnormBlock  :
    case vk::Format::eEtc2R8G8B8SrgbBlock   :
    case vk::Format::eEtc2R8G8B8A1UnormBlock:
    case vk::Format::eEtc2R8G8B8A1SrgbBlock :
    case vk::Format::eEtc2R8G8B8A8UnormBlock:
    case vk::Format::eEtc2R8G8B8A8SrgbBlock :
    case vk::Format::eEacR11UnormBlock      :
    case vk::Format::eEacR11SnormBlock      :
    case vk::Format::eEacR11G11UnormBlock   :
    case vk::Format::eEacR11G11SnormBlock   :
    case vk::Format::eAstc4x4UnormBlock     :
    case vk::Format::eAstc4x4SrgbBlock      :
    case vk::Format::eAstc5x4UnormBlock     :
    case vk::Format::eAstc5x4SrgbBlock      :
    case vk::Format::eAstc5x5UnormBlock     :
    case vk::Format::eAstc5x5SrgbBlock      :
    case vk::Format::eAstc6x5UnormBlock     :
    case vk::Format::eAstc6x5SrgbBlock      :
    case vk::Format::eAstc6x6UnormBlock     :
    case vk::Format::eAstc6x6SrgbBlock      :
    case vk::Format::eAstc8x5UnormBlock     :
    case vk::Format::eAstc8x5SrgbBlock      :
    case vk::Format::eAstc8x6UnormBlock     :
    case vk::Format::eAstc8x6SrgbBlock      :
    case vk::Format::eAstc8x8UnormBlock     :
    case vk::Format::eAstc8x8SrgbBlock      :
    case vk::Format::eAstc10x5UnormBlock    :
    case vk::Format::eAstc10x5SrgbBlock     :
    case vk::Format::eAstc10x6UnormBlock    :
    case vk::Format::eAstc10x6SrgbBlock     :
    case vk::Format::eAstc10x8UnormBlock    :
    case vk::Format::eAstc10x8SrgbBlock     :
    case vk::Format::eAstc10x10UnormBlock   :
    case vk::Format::eAstc10x10SrgbBlock    :
    case vk::Format::eAstc12x10UnormBlock   :
    case vk::Format::eAstc12x10SrgbBlock    :
    case vk::Format::eAstc12x12UnormBlock   :
    case vk::Format::eAstc12x12SrgbBlock    :
    case vk::Format::eAstc4x4SfloatBlock     :
    case vk::Format::eAstc5x4SfloatBlock     :
    case vk::Format::eAstc5x5SfloatBlock     :
    case vk::Format::eAstc6x5SfloatBlock     :
    case vk::Format::eAstc6x6SfloatBlock     :
    case vk::Format::eAstc8x5SfloatBlock     :
    case vk::Format::eAstc8x6SfloatBlock     :
    case vk::Format::eAstc8x8SfloatBlock     :
    case vk::Format::eAstc10x5SfloatBlock    :
    case vk::Format::eAstc10x6SfloatBlock    :
    case vk::Format::eAstc10x8SfloatBlock    :
    case vk::Format::eAstc10x10SfloatBlock   :
    case vk::Format::eAstc12x10SfloatBlock   :
    case vk::Format::eAstc12x12SfloatBlock   :
    case vk::Format::ePvrtc12BppUnormBlockIMG:
    case vk::Format::ePvrtc14BppUnormBlockIMG:
    case vk::Format::ePvrtc22BppUnormBlockIMG:
    case vk::Format::ePvrtc24BppUnormBlockIMG:
    case vk::Format::ePvrtc12BppSrgbBlockIMG :
    case vk::Format::ePvrtc14BppSrgbBlockIMG :
    case vk::Format::ePvrtc22BppSrgbBlockIMG :
    case vk::Format::ePvrtc24BppSrgbBlockIMG :
    case vk::Format::eR16G16Sfixed5NV        :
    default: break;
  }

  return vk::ComponentMapping{r,g,b,a};
}

VkDevice allocator_device(VmaAllocator allocator) { return (*allocator).m_hDevice; }
VkInstance allocator_instance(VmaAllocator allocator) { return (*allocator).m_hInstance; }
size_t allocator_memory_map_aligment(VmaAllocator allocator) {
  return (*allocator).m_PhysicalDeviceProperties.limits.minMemoryMapAlignment;
}

size_t allocator_storage_aligment(VmaAllocator allocator) {
  return (*allocator).m_PhysicalDeviceProperties.limits.minStorageBufferOffsetAlignment;
}

size_t allocator_uniform_aligment(VmaAllocator allocator) {
  return (*allocator).m_PhysicalDeviceProperties.limits.minUniformBufferOffsetAlignment;
}

vk::AttachmentStoreOp converts(const store_op::values e) {
  switch (e) {
    case store_op::none: return vk::AttachmentStoreOp::eNone;
    case store_op::store: return vk::AttachmentStoreOp::eStore;
    case store_op::clear: return vk::AttachmentStoreOp::eDontCare;
    case store_op::dont_care: return vk::AttachmentStoreOp::eDontCare;
    default: utils::error{}("Invalid resource::store_op?");
  }

  return vk::AttachmentStoreOp::eStore;
}

vk::AttachmentLoadOp convertl(const store_op::values e) {
  switch (e) {
    case store_op::none: return vk::AttachmentLoadOp::eNone;
    case store_op::store: return vk::AttachmentLoadOp::eLoad;
    case store_op::clear: return vk::AttachmentLoadOp::eClear;
    case store_op::dont_care: return vk::AttachmentLoadOp::eDontCare;
    default: utils::error{}("Invalid resource::store_op?");
  }

  return vk::AttachmentLoadOp::eLoad;
}

vk::ImageLayout convertil(const usage::values e) {
  switch (e) {
    case usage::color_attachment: return vk::ImageLayout::eColorAttachmentOptimal;
    case usage::depth_attachment: return vk::ImageLayout::eDepthStencilAttachmentOptimal;
    case usage::input_attachment: return vk::ImageLayout::eShaderReadOnlyOptimal;
    case usage::resolve_attachment: return vk::ImageLayout::eColorAttachmentOptimal;
    case usage::ignore_attachment: return vk::ImageLayout::eUndefined;
    case usage::sampled: return vk::ImageLayout::eShaderReadOnlyOptimal;
    case usage::depth_read: return vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    case usage::texel_read: return vk::ImageLayout::eGeneral;
    case usage::texel_write: return vk::ImageLayout::eGeneral;
    case usage::transfer_dst: return vk::ImageLayout::eTransferDstOptimal;
    case usage::transfer_src: return vk::ImageLayout::eTransferSrcOptimal;
    case usage::indirect_read: return vk::ImageLayout::eGeneral;
    case usage::present: return vk::ImageLayout::ePresentSrcKHR;
    case usage::host_read: return vk::ImageLayout::eGeneral;
    case usage::host_write: return vk::ImageLayout::eGeneral;
    case usage::general: return vk::ImageLayout::eGeneral;
    case usage::uniform: return vk::ImageLayout::eShaderReadOnlyOptimal;
    case usage::vertex: return vk::ImageLayout::eGeneral;
    case usage::index: return vk::ImageLayout::eGeneral;
    case usage::storage_read: return vk::ImageLayout::eGeneral;
    case usage::storage_write: return vk::ImageLayout::eGeneral;
    case usage::accel_struct_build_read: return vk::ImageLayout::eGeneral;
    case usage::accel_struct_build_write: return vk::ImageLayout::eGeneral;
    case usage::accel_struct_read: return vk::ImageLayout::eGeneral;
    case usage::accel_struct_write: return vk::ImageLayout::eGeneral;
    default: break;
  }

  utils::error{}("Bad resource usage {}", static_cast<size_t>(e));
  return vk::ImageLayout::eUndefined;
}

vk::AccessFlags convertam(const usage::values e) {
  switch (e) {
    case usage::color_attachment: return vk::AccessFlagBits::eColorAttachmentWrite;
    case usage::depth_attachment: return vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    case usage::input_attachment: return vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentRead;
    case usage::ignore_attachment: return vk::AccessFlags(0);
    case usage::sampled: return vk::AccessFlagBits::eShaderRead;
    case usage::texel_read: return vk::AccessFlagBits::eShaderRead;
    case usage::texel_write: return vk::AccessFlagBits::eShaderWrite;
    case usage::depth_read: return vk::AccessFlagBits::eShaderRead;
    case usage::transfer_dst: return vk::AccessFlagBits::eTransferWrite;
    case usage::transfer_src: return vk::AccessFlagBits::eTransferRead;
    case usage::indirect_read: return vk::AccessFlagBits::eIndirectCommandRead;
    case usage::present: return vk::AccessFlags(0);
    case usage::host_read: return vk::AccessFlagBits::eHostRead;
    case usage::host_write: return vk::AccessFlagBits::eHostWrite;
    case usage::general: return vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
    case usage::uniform: return vk::AccessFlagBits::eUniformRead;
    case usage::vertex: return vk::AccessFlagBits::eVertexAttributeRead;
    case usage::index: return vk::AccessFlagBits::eIndexRead;
    case usage::storage_read: return vk::AccessFlagBits::eShaderRead;
    case usage::storage_write: return vk::AccessFlagBits::eShaderWrite;
    case usage::accel_struct_build_read: return vk::AccessFlagBits::eAccelerationStructureReadKHR;
    case usage::accel_struct_build_write: return vk::AccessFlagBits::eAccelerationStructureWriteKHR;
    case usage::accel_struct_read: return vk::AccessFlagBits::eAccelerationStructureReadKHR;
    case usage::accel_struct_write: return vk::AccessFlagBits::eAccelerationStructureWriteKHR;
    default: break;
  }

  utils::error{}("Bad resource usage {}", static_cast<size_t>(e));
  return vk::AccessFlags(0);
}

vk::PipelineStageFlags convertps(const usage::values e) {
  const auto any_shader = vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eTessellationControlShader | vk::PipelineStageFlagBits::eTessellationEvaluationShader | vk::PipelineStageFlagBits::eGeometryShader | vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader;
  const auto basic_shader = vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader;

  switch (e) {
    case usage::color_attachment: return vk::PipelineStageFlagBits::eColorAttachmentOutput;
    case usage::depth_attachment: return vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
    case usage::input_attachment: return vk::PipelineStageFlagBits::eFragmentShader;
    case usage::ignore_attachment: return vk::PipelineStageFlagBits(0);
    case usage::sampled: return basic_shader;
    case usage::texel_read: return any_shader;
    case usage::texel_write: return any_shader;
    case usage::depth_read: return basic_shader;
    case usage::transfer_dst: return vk::PipelineStageFlagBits::eTransfer;
    case usage::transfer_src: return vk::PipelineStageFlagBits::eTransfer;
    case usage::indirect_read: return vk::PipelineStageFlagBits::eDrawIndirect;
    case usage::present: return vk::PipelineStageFlagBits::eBottomOfPipe;
    case usage::host_read: return vk::PipelineStageFlagBits::eHost;
    case usage::host_write: return vk::PipelineStageFlagBits::eHost;
    case usage::general: return vk::PipelineStageFlagBits::eAllCommands;
    case usage::uniform: return any_shader;
    case usage::vertex: return vk::PipelineStageFlagBits::eVertexInput;
    case usage::index: return vk::PipelineStageFlagBits::eVertexInput;
    case usage::storage_read: return any_shader;
    case usage::storage_write: return any_shader;
    case usage::accel_struct_build_read: return vk::PipelineStageFlagBits::eAccelerationStructureBuildKHR;
    case usage::accel_struct_build_write: return vk::PipelineStageFlagBits::eAccelerationStructureBuildKHR;
    case usage::accel_struct_read: return vk::PipelineStageFlagBits::eRayTracingShaderKHR;
    case usage::accel_struct_write: return vk::PipelineStageFlagBits::eRayTracingShaderKHR;
    default: break;
  }

  utils::error{}("Bad resource usage {}", static_cast<size_t>(e));
  return vk::PipelineStageFlags(0);
}

vk::DescriptorType convertdt(const usage::values e) {
  switch (e) {
    case usage::color_attachment: return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
    case usage::depth_attachment: return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
    case usage::input_attachment: return vk::DescriptorType::eInputAttachment;
    case usage::ignore_attachment: return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
    case usage::sampled: return vk::DescriptorType::eSampledImage;
    case usage::texel_read: return vk::DescriptorType::eStorageImage;
    case usage::texel_write: return vk::DescriptorType::eStorageImage;
    case usage::depth_read: return vk::DescriptorType::eSampledImage;
    case usage::transfer_dst: return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
    case usage::transfer_src: return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
    case usage::indirect_read: return vk::DescriptorType::eStorageBuffer;
    case usage::present: return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
    case usage::host_read: return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
    case usage::host_write: return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
    case usage::general: return vk::DescriptorType::eStorageBuffer;
    case usage::uniform: return vk::DescriptorType::eUniformBuffer;
    case usage::vertex: return vk::DescriptorType::eStorageBuffer;
    case usage::index: return vk::DescriptorType::eStorageBuffer;
    case usage::storage_read: return vk::DescriptorType::eStorageBuffer;
    case usage::storage_write: return vk::DescriptorType::eStorageBuffer;
    case usage::accel_struct_build_read: return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
    case usage::accel_struct_build_write: return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
    case usage::accel_struct_read: return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
    case usage::accel_struct_write: return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
    default: break;
  }

  utils::error{}("Bad resource usage {}", static_cast<size_t>(e));
  return vk::DescriptorType(VK_DESCRIPTOR_TYPE_MAX_ENUM);
}

vk::ImageUsageFlags convertiuf(const usage::values e) {
  switch (e) {
    case usage::color_attachment: return vk::ImageUsageFlagBits::eColorAttachment;
    case usage::depth_attachment: return vk::ImageUsageFlagBits::eDepthStencilAttachment;
    case usage::input_attachment: return vk::ImageUsageFlagBits::eInputAttachment;
    case usage::ignore_attachment: return vk::ImageUsageFlagBits{0};
    case usage::sampled: return vk::ImageUsageFlagBits::eSampled;
    case usage::texel_read: return vk::ImageUsageFlagBits::eStorage;
    case usage::texel_write: return vk::ImageUsageFlagBits::eStorage;
    case usage::depth_read: return vk::ImageUsageFlagBits::eSampled;
    case usage::transfer_dst: return vk::ImageUsageFlagBits::eTransferDst;
    case usage::transfer_src: return vk::ImageUsageFlagBits::eTransferSrc;
    case usage::indirect_read: return vk::ImageUsageFlagBits::eStorage;
    case usage::present: return vk::ImageUsageFlagBits{0};
    case usage::host_read: return vk::ImageUsageFlagBits{ 0 };
    case usage::host_write: return vk::ImageUsageFlagBits{ 0 };
    case usage::general: return vk::ImageUsageFlagBits::eStorage;
    case usage::uniform: return vk::ImageUsageFlagBits::eStorage;
    case usage::vertex: return vk::ImageUsageFlagBits::eStorage;
    case usage::index: return vk::ImageUsageFlagBits::eStorage;
    case usage::storage_read: return vk::ImageUsageFlagBits::eStorage;
    case usage::storage_write: return vk::ImageUsageFlagBits::eStorage;
    case usage::accel_struct_build_read: return vk::ImageUsageFlagBits{ 0 };
    case usage::accel_struct_build_write: return vk::ImageUsageFlagBits{ 0 };
    case usage::accel_struct_read: return vk::ImageUsageFlagBits{ 0 };
    case usage::accel_struct_write: return vk::ImageUsageFlagBits{ 0 };
    default: break;
  }

  utils::error{}("Bad resource usage {}", static_cast<size_t>(e));
  return vk::ImageUsageFlagBits{ 0 };
}

vk::BufferUsageFlags convertbuf(const usage::values e) {
  switch (e) {
    case usage::color_attachment: return vk::BufferUsageFlagBits{ 0 };
    case usage::depth_attachment: return vk::BufferUsageFlagBits{ 0 };
    case usage::input_attachment: return vk::BufferUsageFlagBits{ 0 };
    case usage::ignore_attachment: return vk::BufferUsageFlagBits{ 0 };
    case usage::sampled: return vk::BufferUsageFlagBits::eUniformTexelBuffer;
    case usage::texel_read: return vk::BufferUsageFlagBits::eStorageTexelBuffer;
    case usage::texel_write: return vk::BufferUsageFlagBits::eStorageTexelBuffer;
    case usage::depth_read: return vk::BufferUsageFlagBits::eUniformTexelBuffer;
    case usage::transfer_dst: return vk::BufferUsageFlagBits::eTransferDst;
    case usage::transfer_src: return vk::BufferUsageFlagBits::eTransferSrc;
    case usage::indirect_read: return vk::BufferUsageFlagBits::eIndirectBuffer;
    case usage::present: return vk::BufferUsageFlagBits{ 0 };
    case usage::host_read: return vk::BufferUsageFlagBits{ 0 };
    case usage::host_write: return vk::BufferUsageFlagBits{ 0 };
    case usage::general: return vk::BufferUsageFlagBits::eStorageBuffer;
    case usage::uniform: return vk::BufferUsageFlagBits::eUniformBuffer;
    case usage::vertex: return vk::BufferUsageFlagBits::eVertexBuffer;
    case usage::index: return vk::BufferUsageFlagBits::eIndexBuffer;
    case usage::storage_read: return vk::BufferUsageFlagBits::eStorageBuffer;
    case usage::storage_write: return vk::BufferUsageFlagBits::eStorageBuffer;
    case usage::accel_struct_build_read: return vk::BufferUsageFlagBits{ 0 };
    case usage::accel_struct_build_write: return vk::BufferUsageFlagBits{ 0 };
    case usage::accel_struct_read: return vk::BufferUsageFlagBits{ 0 };
    case usage::accel_struct_write: return vk::BufferUsageFlagBits{ 0 };
    default: break;
  }

  utils::error{}("Bad resource usage {}", static_cast<size_t>(e));
  return vk::BufferUsageFlagBits{ 0 };
}


}
}