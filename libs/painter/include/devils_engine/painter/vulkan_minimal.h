#ifndef DEVILS_ENGINE_PAINTER_VULKAN_MINIMAL_H
#define DEVILS_ENGINE_PAINTER_VULKAN_MINIMAL_H

#include <cstddef>
#include <cstdint>

#ifndef VULKAN_CORE_H_

#define VK_NULL_HANDLE nullptr

// from vulkan_core.h
#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
// lets assume that this code would not be compiled in x86
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef struct object##_T *object;

#define VK_MAKE_API_VERSION(variant, major, minor, patch) ((((uint32_t)(variant)) << 29U) | (((uint32_t)(major)) << 22U) | (((uint32_t)(minor)) << 12U) | ((uint32_t)(patch)))

#define VK_API_VERSION_1_0 VK_MAKE_API_VERSION(0, 1, 0, 0) // Patch version should always be set to 0
#define VK_API_VERSION_1_1 VK_MAKE_API_VERSION(0, 1, 1, 0)
#define VK_API_VERSION_1_2 VK_MAKE_API_VERSION(0, 1, 2, 0)
#define VK_API_VERSION_1_3 VK_MAKE_API_VERSION(0, 1, 3, 0)

typedef uint32_t VkBool32;
typedef uint64_t VkDeviceAddress;
typedef uint64_t VkDeviceSize;
typedef uint32_t VkFlags;
typedef uint32_t VkSampleMask;
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImage)
VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSemaphore)
VK_DEFINE_HANDLE(VkCommandBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFence)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDeviceMemory)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkEvent)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkQueryPool)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkBufferView)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImageView)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkShaderModule)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipelineCache)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipelineLayout)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipeline)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkRenderPass)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorSetLayout)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSampler)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorSet)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorPool)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFramebuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkCommandPool)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSurfaceKHR)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSwapchainKHR)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDisplayKHR)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDisplayModeKHR)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDebugReportCallbackEXT)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDebugUtilsMessengerEXT)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkShaderEXT)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkVideoSessionKHR)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkVideoSessionParametersKHR)
VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaPool)
VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaDefragmentationContext)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VmaVirtualAllocation)
VK_DEFINE_HANDLE(VmaVirtualBlock)
#define VK_ATTACHMENT_UNUSED              (~0U)
#define VK_FALSE                          0U
#define VK_LOD_CLAMP_NONE                 1000.0F
#define VK_QUEUE_FAMILY_IGNORED           (~0U)
#define VK_REMAINING_ARRAY_LAYERS         (~0U)
#define VK_REMAINING_MIP_LEVELS           (~0U)
#define VK_SUBPASS_EXTERNAL               (~0U)
#define VK_TRUE                           1U
#define VK_WHOLE_SIZE                     (~0ULL)
#define VK_MAX_MEMORY_TYPES               32U
#define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE  256U
#define VK_UUID_SIZE                      16U
#define VK_MAX_EXTENSION_NAME_SIZE        256U
#define VK_MAX_DESCRIPTION_SIZE           256U
#define VK_MAX_MEMORY_HEAPS               16U

// VkResult ?

#endif

// это имеет смысл сделать в рантайме
#ifdef _NDEBUG
constexpr bool enable_validation_layers = false;
#else
constexpr bool enable_validation_layers = true;
#endif

#endif