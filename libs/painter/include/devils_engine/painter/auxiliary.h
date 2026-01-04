#ifndef DEVILS_ENGINE_PAINTER_AUXILIARY_H
#define DEVILS_ENGINE_PAINTER_AUXILIARY_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string_view>
#include <functional>
#include <tuple>
#include <string>
//#include "common.h"
#include "vulkan_minimal.h"

typedef struct GLFWwindow GLFWwindow;

namespace devils_engine {
namespace painter {

struct cached_system_data;

const std::vector<const char*> default_validation_layers = { "VK_LAYER_KHRONOS_validation" };
const std::vector<const char*> default_device_extensions = { "VK_KHR_swapchain" };

bool check_validation_layer_support(const std::vector<const char *> &layers);
std::vector<const char *> get_required_extensions();

void load_dispatcher1();
void load_dispatcher2(VkInstance inst);
void load_dispatcher3(VkDevice device);

VkDebugUtilsMessengerEXT create_debug_messenger(VkInstance inst);
void destroy_debug_messenger(VkInstance inst, VkDebugUtilsMessengerEXT handle);

VkPhysicalDevice choose_physical_device(VkInstance inst);
bool physical_device_presentation_support(VkInstance inst, VkPhysicalDevice dev, const uint32_t queue_family_index);
VkSurfaceKHR create_surface(VkInstance instance, GLFWwindow* window);
void destroy_surface(VkInstance instance, VkSurfaceKHR surface);

VkPhysicalDevice find_device_process(VkInstance instance, cached_system_data* cached_data = nullptr);

bool do_command(VkDevice device, VkCommandPool pool, VkQueue queue, VkFence fence, std::function<void(VkCommandBuffer)> action);
bool do_command(VkDevice device, VkQueue queue, VkFence fence, VkCommandBuffer buf, std::function<void(VkCommandBuffer)> action);
void copy_buffer(VkDevice device, VkCommandPool pool, VkQueue queue, VkFence fence, VkBuffer src, VkBuffer dst, size_t srcoffset = 0, size_t dstoffset = 0, size_t size = VK_WHOLE_SIZE);
//void copy_image(VkDevice device, VkCommandPool pool, VkQueue queue, VkFence fence, VkImage dst, VkImage src, );

std::vector<const char*> get_all_instance_extension();
std::vector<const char*> get_all_device_extension(VkPhysicalDevice device);
std::vector<const char*> check_instance_extension(std::vector<const char*> input);
std::vector<const char*> check_device_extension(VkPhysicalDevice device, std::vector<const char*> input);

VkDevice allocator_device(VmaAllocator allocator);
VkInstance allocator_instance(VmaAllocator allocator);
size_t allocator_memory_map_aligment(VmaAllocator allocator);
size_t allocator_storage_aligment(VmaAllocator allocator);
size_t allocator_uniform_aligment(VmaAllocator allocator);

std::string format_to_string(const uint32_t format);

#define VK_MULTIPLANE_FORMAT_MAX_PLANES 3

enum VkFormatCompatibilityClass {
  VK_FORMAT_COMPATIBILITY_CLASS_NONE_BIT = 0,
  VK_FORMAT_COMPATIBILITY_CLASS_8_BIT = 1,
  VK_FORMAT_COMPATIBILITY_CLASS_16_BIT = 2,
  VK_FORMAT_COMPATIBILITY_CLASS_24_BIT = 3,
  VK_FORMAT_COMPATIBILITY_CLASS_32_BIT = 4,
  VK_FORMAT_COMPATIBILITY_CLASS_48_BIT = 5,
  VK_FORMAT_COMPATIBILITY_CLASS_64_BIT = 6,
  VK_FORMAT_COMPATIBILITY_CLASS_96_BIT = 7,
  VK_FORMAT_COMPATIBILITY_CLASS_128_BIT = 8,
  VK_FORMAT_COMPATIBILITY_CLASS_192_BIT = 9,
  VK_FORMAT_COMPATIBILITY_CLASS_256_BIT = 10,
  VK_FORMAT_COMPATIBILITY_CLASS_BC1_RGB_BIT = 11,
  VK_FORMAT_COMPATIBILITY_CLASS_BC1_RGBA_BIT = 12,
  VK_FORMAT_COMPATIBILITY_CLASS_BC2_BIT = 13,
  VK_FORMAT_COMPATIBILITY_CLASS_BC3_BIT = 14,
  VK_FORMAT_COMPATIBILITY_CLASS_BC4_BIT = 15,
  VK_FORMAT_COMPATIBILITY_CLASS_BC5_BIT = 16,
  VK_FORMAT_COMPATIBILITY_CLASS_BC6H_BIT = 17,
  VK_FORMAT_COMPATIBILITY_CLASS_BC7_BIT = 18,
  VK_FORMAT_COMPATIBILITY_CLASS_ETC2_RGB_BIT = 19,
  VK_FORMAT_COMPATIBILITY_CLASS_ETC2_RGBA_BIT = 20,
  VK_FORMAT_COMPATIBILITY_CLASS_ETC2_EAC_RGBA_BIT = 21,
  VK_FORMAT_COMPATIBILITY_CLASS_EAC_R_BIT = 22,
  VK_FORMAT_COMPATIBILITY_CLASS_EAC_RG_BIT = 23,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_4X4_BIT = 24,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_5X4_BIT = 25,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_5X5_BIT = 26,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_6X5_BIT = 27,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_6X6_BIT = 28,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_8X5_BIT = 29,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_8X6_BIT = 20,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_8X8_BIT = 31,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_10X5_BIT = 32,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_10X6_BIT = 33,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_10X8_BIT = 34,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_10X10_BIT = 35,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_12X10_BIT = 36,
  VK_FORMAT_COMPATIBILITY_CLASS_ASTC_12X12_BIT = 37,
  VK_FORMAT_COMPATIBILITY_CLASS_D16_BIT = 38,
  VK_FORMAT_COMPATIBILITY_CLASS_D24_BIT = 39,
  VK_FORMAT_COMPATIBILITY_CLASS_D32_BIT = 30,
  VK_FORMAT_COMPATIBILITY_CLASS_S8_BIT = 41,
  VK_FORMAT_COMPATIBILITY_CLASS_D16S8_BIT = 42,
  VK_FORMAT_COMPATIBILITY_CLASS_D24S8_BIT = 43,
  VK_FORMAT_COMPATIBILITY_CLASS_D32S8_BIT = 44,
  VK_FORMAT_COMPATIBILITY_CLASS_PVRTC1_2BPP_BIT = 45,
  VK_FORMAT_COMPATIBILITY_CLASS_PVRTC1_4BPP_BIT = 46,
  VK_FORMAT_COMPATIBILITY_CLASS_PVRTC2_2BPP_BIT = 47,
  VK_FORMAT_COMPATIBILITY_CLASS_PVRTC2_4BPP_BIT = 48,
  /* KHR_sampler_YCbCr_conversion */
  VK_FORMAT_COMPATIBILITY_CLASS_32BIT_G8B8G8R8 = 49,
  VK_FORMAT_COMPATIBILITY_CLASS_32BIT_B8G8R8G8 = 50,
  VK_FORMAT_COMPATIBILITY_CLASS_64BIT_R10G10B10A10 = 51,
  VK_FORMAT_COMPATIBILITY_CLASS_64BIT_G10B10G10R10 = 52,
  VK_FORMAT_COMPATIBILITY_CLASS_64BIT_B10G10R10G10 = 53,
  VK_FORMAT_COMPATIBILITY_CLASS_64BIT_R12G12B12A12 = 54,
  VK_FORMAT_COMPATIBILITY_CLASS_64BIT_G12B12G12R12 = 55,
  VK_FORMAT_COMPATIBILITY_CLASS_64BIT_B12G12R12G12 = 56,
  VK_FORMAT_COMPATIBILITY_CLASS_64BIT_G16B16G16R16 = 57,
  VK_FORMAT_COMPATIBILITY_CLASS_64BIT_B16G16R16G16 = 58,
  VK_FORMAT_COMPATIBILITY_CLASS_8BIT_3PLANE_420 = 59,
  VK_FORMAT_COMPATIBILITY_CLASS_8BIT_2PLANE_420 = 60,
  VK_FORMAT_COMPATIBILITY_CLASS_8BIT_3PLANE_422 = 61,
  VK_FORMAT_COMPATIBILITY_CLASS_8BIT_2PLANE_422 = 62,
  VK_FORMAT_COMPATIBILITY_CLASS_8BIT_3PLANE_444 = 63,
  VK_FORMAT_COMPATIBILITY_CLASS_10BIT_3PLANE_420 = 64,
  VK_FORMAT_COMPATIBILITY_CLASS_10BIT_2PLANE_420 = 65,
  VK_FORMAT_COMPATIBILITY_CLASS_10BIT_3PLANE_422 = 66,
  VK_FORMAT_COMPATIBILITY_CLASS_10BIT_2PLANE_422 = 67,
  VK_FORMAT_COMPATIBILITY_CLASS_10BIT_3PLANE_444 = 68,
  VK_FORMAT_COMPATIBILITY_CLASS_12BIT_3PLANE_420 = 69,
  VK_FORMAT_COMPATIBILITY_CLASS_12BIT_2PLANE_420 = 70,
  VK_FORMAT_COMPATIBILITY_CLASS_12BIT_3PLANE_422 = 71,
  VK_FORMAT_COMPATIBILITY_CLASS_12BIT_2PLANE_422 = 72,
  VK_FORMAT_COMPATIBILITY_CLASS_12BIT_3PLANE_444 = 73,
  VK_FORMAT_COMPATIBILITY_CLASS_16BIT_3PLANE_420 = 74,
  VK_FORMAT_COMPATIBILITY_CLASS_16BIT_2PLANE_420 = 75,
  VK_FORMAT_COMPATIBILITY_CLASS_16BIT_3PLANE_422 = 76,
  VK_FORMAT_COMPATIBILITY_CLASS_16BIT_2PLANE_422 = 77,
  VK_FORMAT_COMPATIBILITY_CLASS_16BIT_3PLANE_444 = 78,
  VK_FORMAT_COMPATIBILITY_CLASS_MAX_ENUM = 79
};

enum VkFormatNumericalType {
  VK_FORMAT_NUMERICAL_TYPE_NONE,
  VK_FORMAT_NUMERICAL_TYPE_UINT,
  VK_FORMAT_NUMERICAL_TYPE_SINT,
  VK_FORMAT_NUMERICAL_TYPE_UNORM,
  VK_FORMAT_NUMERICAL_TYPE_SNORM,
  VK_FORMAT_NUMERICAL_TYPE_USCALED,
  VK_FORMAT_NUMERICAL_TYPE_SSCALED,
  VK_FORMAT_NUMERICAL_TYPE_UFLOAT,
  VK_FORMAT_NUMERICAL_TYPE_SFLOAT,
  VK_FORMAT_NUMERICAL_TYPE_SRGB
};

bool format_is_depth_or_stencil(uint32_t format);
bool format_is_depth_and_stencil(uint32_t format);
bool format_is_depth_only(uint32_t format);
bool format_is_stencil_only(uint32_t format);
bool format_is_compressed_etc2_eac(uint32_t format);
bool format_is_compressed_astc_ldr(uint32_t format);
bool format_is_compressed_bc(uint32_t format);
bool format_is_compressed_pvrtc(uint32_t format);
bool format_is_single_plane_422(uint32_t format);
bool format_is_norm(uint32_t format);
bool format_is_unorm(uint32_t format);
bool format_is_snorm(uint32_t format);
bool format_is_int(uint32_t format);
bool format_is_sint(uint32_t format);
bool format_is_uint(uint32_t format);
bool format_is_float(uint32_t format);
bool format_is_srgb(uint32_t format);
bool format_is_uscaled(uint32_t format);
bool format_is_sscaled(uint32_t format);
bool format_is_compressed(uint32_t format);
bool format_is_packed(uint32_t format);
bool format_element_is_texel(uint32_t format);
bool format_is_undef(uint32_t format);
bool format_has_depth(uint32_t format);
bool format_has_stencil(uint32_t format);
bool format_is_multiplane(uint32_t format);
bool format_is_color(uint32_t format);

//bool format_sizes_are_equal(uint32_t srcFormat, uint32_t dstFormat, uint32_t region_count, const VkImageCopy *regions);
bool format_requires_ycbcr_conversion(uint32_t format);
uint32_t format_depth_size(uint32_t format);
VkFormatNumericalType format_depth_numerical_type(uint32_t format);
uint32_t format_stencil_size(uint32_t format);
VkFormatNumericalType format_stencil_numerical_type(uint32_t format);
uint32_t format_plane_count(uint32_t format);
uint32_t format_channel_count(uint32_t format);
std::tuple<uint32_t, uint32_t, uint32_t> format_texel_block_extent(uint32_t format);
uint32_t format_element_size(uint32_t format, uint32_t aspectMask = 1);
double format_texel_size(uint32_t format);
VkFormatCompatibilityClass format_compatibility_class(uint32_t format);
size_t safe_modulo(size_t dividend, size_t divisor);
size_t safe_division(size_t dividend, size_t divisor);
uint32_t get_plane_index(uint32_t aspect);
uint32_t find_multiplane_compatible_format(uint32_t fmt, uint32_t plane_aspect);
std::tuple<uint32_t, uint32_t> find_multiplane_extent_divisors(uint32_t mp_fmt, uint32_t plane_aspect);

}
}

#endif