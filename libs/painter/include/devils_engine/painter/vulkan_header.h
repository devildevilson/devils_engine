// тут укажем все define так чтобы они не потерялись

#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.hpp>

#ifndef DEVILS_ENGINE_PAINTER_VULKAN_HEADER_H
#define DEVILS_ENGINE_PAINTER_VULKAN_HEADER_H

// так а тут может быть имплементация кое каких вещей
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <utils/reflect>
#include <bitset>
#include <tuple>
#include "utils/type_traits.h"
#include "utils/core.h"

namespace devils_engine {
namespace painter {

template <typename T>
void set_name(vk::Device device, T handle, const std::string &name) {
  vk::DebugUtilsObjectNameInfoEXT i(T::objectType, uint64_t(typename T::CType(handle)), name.c_str());
  device.setDebugUtilsObjectNameEXT(i);
}

//template <typename T>
//consteval void count_features(size_t &counter, const T &obj) {
//  reflect::for_each([&](auto I) {
//    using value_type = decltype(reflect::get<I>(obj));
//    using mem_type = std::remove_cvref_t<value_type>;
//    if constexpr (!std::is_same_v<mem_type, VkBool32>) return;
//    counter += 1;
//  }, obj);
//}
//
//template <typename T>
//consteval bool find_feature(size_t &counter, const std::string_view &name, const T &obj) {
//  bool found = false;
//
//  reflect::for_each([&](auto I) {
//    using value_type = decltype(reflect::get<I>(obj));
//    using mem_type = std::remove_cvref_t<value_type>;
//    if constexpr (!std::is_same_v<mem_type, VkBool32>) return;
//
//    const std::string_view member_name = reflect::member_name<I>(obj);
//    if (member_name == name) { found = true; return; }
//
//    counter += 1;
//  }, obj);
//
//  return found;
//}
//
//template <typename T>
//consteval void insert_feature_to_map(phmap::flat_hash_map<std::string_view, size_t> &memo, size_t &counter, const T &obj) {
//  reflect::for_each([&](auto I) {
//    using value_type = decltype(reflect::get<I>(obj));
//    using mem_type = std::remove_cvref_t<value_type>;
//    if constexpr (!std::is_same_v<mem_type, VkBool32>) return;
//
//    const std::string_view member_name = reflect::member_name<I>(obj);
//    memo.insert(std::make_pair(member_name, counter));
//
//    counter += 1;
//  }, obj);
//}
//
//consteval size_t count_vulkan_device_features() {
//  size_t counter = 0;
//  const auto obj10 = VkPhysicalDeviceFeatures{};
//  const auto obj11 = VkPhysicalDeviceVulkan11Features{};
//  const auto obj12 = VkPhysicalDeviceVulkan12Features{};
//  const auto obj13 = VkPhysicalDeviceVulkan13Features{};
//
//  //reflect::for_each([&](auto I) {
//  //  using value_type = decltype(reflect::get<I>(obj10));
//  //  using mem_type = std::remove_cvref_t<value_type>;
//  //  //const auto member_name = reflect::member_name<I>(obj10);
//  //  //const auto type_name = utils::type_name<mem_type>();
//  //  //utils::println(type_name, member_name);
//  //  //std::cout << type_name << " " << member_name << "\n";
//  //  if constexpr (!std::is_same_v<mem_type, VkBool32>) return;
//  //  counter += 1;
//  //}, obj10);
//  count_features(counter, obj10);
//  count_features(counter, obj11);
//  count_features(counter, obj12);
//  count_features(counter, obj13);
//
//  return counter;
//}
//
//consteval size_t get_feature_index(const std::string_view &name) {
//  bool found = false;
//  size_t counter = 0;
//  const auto obj10 = VkPhysicalDeviceFeatures{};
//  const auto obj11 = VkPhysicalDeviceVulkan11Features{};
//  const auto obj12 = VkPhysicalDeviceVulkan12Features{};
//  const auto obj13 = VkPhysicalDeviceVulkan13Features{};
//
//  if (find_feature(counter, name, obj10)) return counter;
//  if (find_feature(counter, name, obj11)) return counter;
//  if (find_feature(counter, name, obj12)) return counter;
//  if (find_feature(counter, name, obj13)) return counter;
//
//  return SIZE_MAX;
//}
//
//constexpr size_t device_features_count = count_vulkan_device_features();
//using vulkan_features_bitset = std::bitset<device_features_count>;
using vulkan_features_bitset = std::bitset<256>;

// так теперь надо заполнить фичи в битовое поле
vulkan_features_bitset make_device_features_bitset(VkPhysicalDevice dev);


std::vector<vk::ExtensionProperties> required_device_extensions(vk::PhysicalDevice device, const std::vector<const char*> &layers, const std::vector<const char*> &extensions);
std::vector<vk::LayerProperties> required_validation_layers(const std::vector<const char*> &layers);
vk::PresentModeKHR choose_swapchain_present_mode(const std::vector<vk::PresentModeKHR> &modes);
vk::SurfaceFormatKHR choose_swapchain_surface_format(const std::vector<vk::SurfaceFormatKHR> &formats);
vk::Extent2D choose_swapchain_extent(const uint32_t width, const uint32_t height, const vk::SurfaceCapabilitiesKHR& capabilities);
bool check_swapchain_present_mode(const std::vector<vk::PresentModeKHR> &modes, const vk::PresentModeKHR mode);
vk::Format find_supported_format(vk::PhysicalDevice phys, const std::vector<vk::Format> &candidates, const vk::ImageTiling tiling, const vk::FormatFeatureFlags features);

vk::ImageCreateInfo texture2D(
  const vk::Extent2D &size, 
  const vk::ImageUsageFlags &usage, 
  const vk::Format &format = vk::Format::eR8G8B8A8Unorm, 
  const uint32_t &arrayLayers = 1,
  const uint32_t &mipLevels = 1,
  const vk::SampleCountFlagBits &samples = vk::SampleCountFlagBits::e1,
  const vk::ImageCreateFlags &flags = {}
);
    
vk::ImageCreateInfo texture2D_staging(
  const vk::Extent2D &size,
  const vk::ImageUsageFlags &usage = vk::ImageUsageFlagBits::eTransferSrc,
  const vk::Format &format = vk::Format::eR8G8B8A8Unorm,
  const vk::ImageCreateFlags &flags = {}
);

vk::ImageViewCreateInfo view_info(
  vk::Image img, 
  vk::Format format, 
  vk::ImageViewType type = vk::ImageViewType::e2D, 
  const vk::ImageSubresourceRange &r = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1), 
  const vk::ComponentMapping &cm = { vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity }
);
    
vk::ImageViewCreateInfo make_view_info(
  vk::Image            image,
  vk::Format           format    = vk::Format::eR8G8B8A8Unorm,
  vk::ImageViewType    viewType  = vk::ImageViewType::e2D,
  vk::ImageSubresourceRange subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
  vk::ComponentMapping components            = {},
  vk::ImageViewCreateFlags flags = {}
);
    
vk::BufferCreateInfo buffer_info(const vk::DeviceSize &size, const vk::BufferUsageFlags &usage, const vk::BufferCreateFlags &flags = {});
std::tuple<vk::BufferCreateInfo, vma::AllocationCreateInfo> dedicated_buffer(const size_t size, const vk::BufferUsageFlags usage, const vma::MemoryUsage memusage, const vk::BufferCreateFlags &flags = {});

vk::ImageUsageFlags main_attachment_usage_from_format(vk::Format format);

std::tuple<vk::Image, vma::Allocation> create_image(
  vma::Allocator allocator, 
  const vk::ImageCreateInfo &info,
  const vma::MemoryUsage &mem_usage,
  void** pData = nullptr,
  const std::string &name = ""
);

std::tuple<vk::AccessFlags, vk::AccessFlags, vk::PipelineStageFlags, vk::PipelineStageFlags> make_barrier_data(const vk::ImageLayout &old, const vk::ImageLayout &New);

std::tuple<vk::ImageMemoryBarrier, vk::PipelineStageFlags, vk::PipelineStageFlags> make_image_memory_barrier(
  vk::Image image, const vk::ImageLayout &old_layout, const vk::ImageLayout &new_layout, const vk::ImageSubresourceRange &range
);

void change_image_layout(
  vk::Device device, 
  vk::Image image, 
  vk::CommandPool transfer_pool, 
  vk::Queue transfer_queue, 
  vk::Fence fence, 
  const vk::ImageLayout &old_layout, 
  const vk::ImageLayout &new_layout, 
  const vk::ImageSubresourceRange &range
);

vma::VulkanFunctions make_functions();

vk::ComponentMapping to_rgba(vk::Format format);

}
}

#endif