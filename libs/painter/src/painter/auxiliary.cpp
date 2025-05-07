#include "auxiliary.h"

#include "vulkan_header.h"

#include "input/core.h"

#include "system_info.h"

#include "gtl/phmap.hpp"

#include <ranges>
namespace rv = std::ranges::views;

namespace devils_engine {
namespace painter {

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
  VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
  VkDebugUtilsMessageTypeFlagsEXT messageType,
  const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
  void* pUserData
) {
  utils::println("validation layer:", pCallbackData->pMessage);
  return VK_FALSE;
}

bool check_validation_layer_support(const std::vector<const char*>& layers) {
  const auto layers_pred = [&](const auto &a) {
    const auto pred = [&](const auto &b) { return strcmp(a.layerName, b) == 0; };
    return std::ranges::count_if(layers, pred) != 0;
  };

  const auto layers_props = vk::enumerateInstanceLayerProperties();
  return std::ranges::count_if(layers_props, layers_pred) == layers.size();
}

std::vector<const char*> get_required_extensions() {
  uint32_t glfw_extension_count = 0;
  const char** glfw_extensions;
  glfw_extensions = input::get_required_instance_extensions(&glfw_extension_count);

  std::vector<const char*> extensions(glfw_extensions, glfw_extensions + glfw_extension_count);

  if (enable_validation_layers) {
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  return extensions;
}

void load_dispatcher1() { 
  if (!input::vulkan_supported()) utils::error("Vulkan is not supported by this system");
  VULKAN_HPP_DEFAULT_DISPATCHER.init();  
  input::init_vulkan_loader(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr);
}

void load_dispatcher2(VkInstance inst) { VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(inst)); }
void load_dispatcher3(VkDevice device) { VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device)); }

VkDebugUtilsMessengerEXT create_debug_messenger(VkInstance inst) {
  const auto severity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                        vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                        vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo;

  const auto type = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                    vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance |
                    vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | 
                    vk::DebugUtilsMessageTypeFlagBitsEXT::eDeviceAddressBinding;

  vk::DebugUtilsMessengerCreateInfoEXT info({}, severity, type, &debug_callback, nullptr);

  vk::Instance instance(inst);
  VkDebugUtilsMessengerEXT handle = instance.createDebugUtilsMessengerEXT(info);
  return handle;
}

void destroy_debug_messenger(VkInstance inst, VkDebugUtilsMessengerEXT handle) {
  vk::Instance instance(inst);
  instance.destroyDebugUtilsMessengerEXT(handle);
}

#define GLFW_TRUE 1
bool physical_device_presentation_support(VkInstance inst, VkPhysicalDevice dev, const uint32_t queue_family_index) {
  return input::get_physical_device_presentation_support(inst, dev, queue_family_index) == GLFW_TRUE;
}

VkSurfaceKHR create_surface(VkInstance instance, GLFWwindow* window) {
  VkSurfaceKHR surf = VK_NULL_HANDLE;
  const auto res = input::create_window_surface(instance, window, nullptr, &surf);
  if (res != VK_SUCCESS) utils::error("Could not create surface from window '{}', result: {}", input::window_title(window), vk::to_string(vk::Result(res)));
  return surf;
}

void destroy_surface(VkInstance instance, VkSurfaceKHR surface) {
  vk::Instance(instance).destroySurfaceKHR(surface);
}

VkPhysicalDevice find_device_process(VkInstance i, cached_system_data* cached_data) {
  painter::system_info inf(i);
  auto w = input::create_window(640, 480, "test", nullptr, nullptr);
  auto surf = painter::create_surface(inf.instance, w);
  inf.check_devices_surface_capability(surf);
  const auto dev = inf.choose_physical_device();

  inf.dump_cache_to_disk(dev, cached_data);

  painter::destroy_surface(inf.instance, surf);
  input::destroy(w);

  // инстанса уже тут не будет, мы можем чуть переделать структуру system_info
  // передав туда инстанс, вот теперь девайс должен быть валидным указателем
  return dev;
}

bool do_command(VkDevice device, VkCommandPool pool, VkQueue queue, VkFence fence, std::function<void(VkCommandBuffer)> action) {
  vk::Device d(device);
  vk::CommandBufferAllocateInfo ai(pool, vk::CommandBufferLevel::ePrimary, 1);
  const auto buffer = std::move(d.allocateCommandBuffersUnique(ai)[0]);

  vk::CommandBufferBeginInfo cbbi(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
  buffer->begin(cbbi);
  action(buffer.get());
  buffer->end();

  const vk::SubmitInfo si(nullptr, nullptr, buffer.get(), nullptr);
  vk::Queue(queue).submit(si, fence);

  const auto res = d.waitForFences(vk::Fence(fence), VK_TRUE, SIZE_MAX);
  d.resetFences(vk::Fence(fence));

  return res == vk::Result::eSuccess;
}

void copy_buffer(VkDevice device, VkCommandPool pool, VkQueue queue, VkFence fence, VkBuffer src, VkBuffer dst, size_t srcoffset, size_t dstoffset, size_t size) {
  do_command(device, pool, queue, fence, [&] (VkCommandBuffer buffer) {
    auto b = vk::CommandBuffer(buffer);
    //const vk::CommandBufferBeginInfo binfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    //b.begin(binfo);
    b.copyBuffer(src, dst, vk::BufferCopy(srcoffset, dstoffset, size));
    //b.end();
  });
}

std::vector<const char*> get_all_instance_extension() {
  gtl::flat_hash_set<const char*> names;
  const auto layers = vk::enumerateInstanceLayerProperties();
  for (const auto &layer : layers) {
    const auto exts = vk::enumerateInstanceExtensionProperties(std::string(layer.layerName.data()));
    for (const auto &ext : exts) {
      names.insert(ext.extensionName.data());
    }
  }

  const auto exts = vk::enumerateInstanceExtensionProperties();
  for (const auto &ext : exts) {
    names.insert(ext.extensionName.data());
  }

  return std::vector<const char*>(names.begin(), names.end());
}

std::vector<const char *> get_all_device_extension(VkPhysicalDevice device) {
  vk::PhysicalDevice pd(device);
  gtl::flat_hash_set<const char*> names;
  const auto layers = pd.enumerateDeviceLayerProperties();
  for (const auto &layer : layers) {
    const auto exts = pd.enumerateDeviceExtensionProperties(std::string(layer.layerName.data()));
    for (const auto &ext : exts) {
      names.insert(ext.extensionName.data());
    }
  }

  const auto exts = pd.enumerateDeviceExtensionProperties();
  for (const auto &ext : exts) {
    names.insert(ext.extensionName.data());
  }

  return std::vector<const char*>(names.begin(), names.end());
}

std::vector<const char*> check_instance_extension(std::vector<const char*> input) {
  const auto exts = get_all_instance_extension();
  std::vector<const char*> out;
  for (const auto name : input) {
    if (std::find(exts.begin(), exts.end(), name) != exts.end()) out.push_back(name);
  }
  return out;
}

std::vector<const char *> check_device_extension(VkPhysicalDevice device, std::vector<const char *> input) {
  const auto exts = get_all_device_extension(device);
  std::vector<const char*> out;
  for (const auto name : input) {
    if (std::find(exts.begin(), exts.end(), name) != exts.end()) out.push_back(name);
  }
  return out;
}

std::string format_to_string(const uint32_t format) { return vk::to_string(vk::Format(format)); }

struct VULKAN_FORMAT_INFO {
    uint32_t size;
    uint32_t channel_count;
    VkFormatCompatibilityClass format_class;
};
// Disable auto-formatting for this large table
// clang-format off
// Set up data structure with size(bytes) and number of channels for each Vulkan format
// For compressed and multi-plane formats, size is bytes per compressed or shared block
const gtl::flat_hash_map<uint32_t, VULKAN_FORMAT_INFO> vk_format_table = {
    {VK_FORMAT_UNDEFINED,                   {0, 0, VK_FORMAT_COMPATIBILITY_CLASS_NONE_BIT }},
    {VK_FORMAT_R4G4_UNORM_PACK8,            {1, 2, VK_FORMAT_COMPATIBILITY_CLASS_8_BIT}},
    {VK_FORMAT_R4G4B4A4_UNORM_PACK16,       {2, 4, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_B4G4R4A4_UNORM_PACK16,       {2, 4, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R5G6B5_UNORM_PACK16,         {2, 3, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_B5G6R5_UNORM_PACK16,         {2, 3, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R5G5B5A1_UNORM_PACK16,       {2, 4, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_B5G5R5A1_UNORM_PACK16,       {2, 4, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_A1R5G5B5_UNORM_PACK16,       {2, 4, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R8_UNORM,                    {1, 1, VK_FORMAT_COMPATIBILITY_CLASS_8_BIT}},
    {VK_FORMAT_R8_SNORM,                    {1, 1, VK_FORMAT_COMPATIBILITY_CLASS_8_BIT}},
    {VK_FORMAT_R8_USCALED,                  {1, 1, VK_FORMAT_COMPATIBILITY_CLASS_8_BIT}},
    {VK_FORMAT_R8_SSCALED,                  {1, 1, VK_FORMAT_COMPATIBILITY_CLASS_8_BIT}},
    {VK_FORMAT_R8_UINT,                     {1, 1, VK_FORMAT_COMPATIBILITY_CLASS_8_BIT}},
    {VK_FORMAT_R8_SINT,                     {1, 1, VK_FORMAT_COMPATIBILITY_CLASS_8_BIT}},
    {VK_FORMAT_R8_SRGB,                     {1, 1, VK_FORMAT_COMPATIBILITY_CLASS_8_BIT}},
    {VK_FORMAT_R8G8_UNORM,                  {2, 2, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R8G8_SNORM,                  {2, 2, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R8G8_USCALED,                {2, 2, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R8G8_SSCALED,                {2, 2, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R8G8_UINT,                   {2, 2, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R8G8_SINT,                   {2, 2, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R8G8_SRGB,                   {2, 2, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R8G8B8_UNORM,                {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_R8G8B8_SNORM,                {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_R8G8B8_USCALED,              {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_R8G8B8_SSCALED,              {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_R8G8B8_UINT,                 {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_R8G8B8_SINT,                 {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_R8G8B8_SRGB,                 {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_B8G8R8_UNORM,                {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_B8G8R8_SNORM,                {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_B8G8R8_USCALED,              {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_B8G8R8_SSCALED,              {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_B8G8R8_UINT,                 {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_B8G8R8_SINT,                 {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_B8G8R8_SRGB,                 {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_24_BIT}},
    {VK_FORMAT_R8G8B8A8_UNORM,              {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R8G8B8A8_SNORM,              {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R8G8B8A8_USCALED,            {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R8G8B8A8_SSCALED,            {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R8G8B8A8_UINT,               {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R8G8B8A8_SINT,               {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R8G8B8A8_SRGB,               {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_B8G8R8A8_UNORM,              {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_B8G8R8A8_SNORM,              {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_B8G8R8A8_USCALED,            {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_B8G8R8A8_SSCALED,            {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_B8G8R8A8_UINT,               {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_B8G8R8A8_SINT,               {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_B8G8R8A8_SRGB,               {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A8B8G8R8_UNORM_PACK32,       {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A8B8G8R8_SNORM_PACK32,       {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A8B8G8R8_USCALED_PACK32,     {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A8B8G8R8_SSCALED_PACK32,     {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A8B8G8R8_UINT_PACK32,        {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A8B8G8R8_SINT_PACK32,        {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A8B8G8R8_SRGB_PACK32,        {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A2R10G10B10_UNORM_PACK32,    {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A2R10G10B10_SNORM_PACK32,    {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A2R10G10B10_USCALED_PACK32,  {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A2R10G10B10_SSCALED_PACK32,  {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A2R10G10B10_UINT_PACK32,     {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A2R10G10B10_SINT_PACK32,     {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32,    {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A2B10G10R10_SNORM_PACK32,    {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A2B10G10R10_USCALED_PACK32,  {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A2B10G10R10_SSCALED_PACK32,  {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A2B10G10R10_UINT_PACK32,     {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_A2B10G10R10_SINT_PACK32,     {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R16_UNORM,                   {2, 1, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R16_SNORM,                   {2, 1, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R16_USCALED,                 {2, 1, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R16_SSCALED,                 {2, 1, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R16_UINT,                    {2, 1, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R16_SINT,                    {2, 1, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R16_SFLOAT,                  {2, 1, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R16G16_UNORM,                {4, 2, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R16G16_SNORM,                {4, 2, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R16G16_USCALED,              {4, 2, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R16G16_SSCALED,              {4, 2, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R16G16_UINT,                 {4, 2, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R16G16_SINT,                 {4, 2, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R16G16_SFLOAT,               {4, 2, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R16G16B16_UNORM,             {6, 3, VK_FORMAT_COMPATIBILITY_CLASS_48_BIT}},
    {VK_FORMAT_R16G16B16_SNORM,             {6, 3, VK_FORMAT_COMPATIBILITY_CLASS_48_BIT}},
    {VK_FORMAT_R16G16B16_USCALED,           {6, 3, VK_FORMAT_COMPATIBILITY_CLASS_48_BIT}},
    {VK_FORMAT_R16G16B16_SSCALED,           {6, 3, VK_FORMAT_COMPATIBILITY_CLASS_48_BIT}},
    {VK_FORMAT_R16G16B16_UINT,              {6, 3, VK_FORMAT_COMPATIBILITY_CLASS_48_BIT}},
    {VK_FORMAT_R16G16B16_SINT,              {6, 3, VK_FORMAT_COMPATIBILITY_CLASS_48_BIT}},
    {VK_FORMAT_R16G16B16_SFLOAT,            {6, 3, VK_FORMAT_COMPATIBILITY_CLASS_48_BIT}},
    {VK_FORMAT_R16G16B16A16_UNORM,          {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R16G16B16A16_SNORM,          {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R16G16B16A16_USCALED,        {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R16G16B16A16_SSCALED,        {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R16G16B16A16_UINT,           {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R16G16B16A16_SINT,           {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R16G16B16A16_SFLOAT,         {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R32_UINT,                    {4, 1, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R32_SINT,                    {4, 1, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R32_SFLOAT,                  {4, 1, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R32G32_UINT,                 {8, 2, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R32G32_SINT,                 {8, 2, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R32G32_SFLOAT,               {8, 2, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R32G32B32_UINT,              {12, 3, VK_FORMAT_COMPATIBILITY_CLASS_96_BIT}},
    {VK_FORMAT_R32G32B32_SINT,              {12, 3, VK_FORMAT_COMPATIBILITY_CLASS_96_BIT}},
    {VK_FORMAT_R32G32B32_SFLOAT,            {12, 3, VK_FORMAT_COMPATIBILITY_CLASS_96_BIT}},
    {VK_FORMAT_R32G32B32A32_UINT,           {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_128_BIT}},
    {VK_FORMAT_R32G32B32A32_SINT,           {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_128_BIT}},
    {VK_FORMAT_R32G32B32A32_SFLOAT,         {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_128_BIT}},
    {VK_FORMAT_R64_UINT,                    {8, 1, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R64_SINT,                    {8, 1, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R64_SFLOAT,                  {8, 1, VK_FORMAT_COMPATIBILITY_CLASS_64_BIT}},
    {VK_FORMAT_R64G64_UINT,                 {16, 2, VK_FORMAT_COMPATIBILITY_CLASS_128_BIT}},
    {VK_FORMAT_R64G64_SINT,                 {16, 2, VK_FORMAT_COMPATIBILITY_CLASS_128_BIT}},
    {VK_FORMAT_R64G64_SFLOAT,               {16, 2, VK_FORMAT_COMPATIBILITY_CLASS_128_BIT}},
    {VK_FORMAT_R64G64B64_UINT,              {24, 3, VK_FORMAT_COMPATIBILITY_CLASS_192_BIT}},
    {VK_FORMAT_R64G64B64_SINT,              {24, 3, VK_FORMAT_COMPATIBILITY_CLASS_192_BIT}},
    {VK_FORMAT_R64G64B64_SFLOAT,            {24, 3, VK_FORMAT_COMPATIBILITY_CLASS_192_BIT}},
    {VK_FORMAT_R64G64B64A64_UINT,           {32, 4, VK_FORMAT_COMPATIBILITY_CLASS_256_BIT}},
    {VK_FORMAT_R64G64B64A64_SINT,           {32, 4, VK_FORMAT_COMPATIBILITY_CLASS_256_BIT}},
    {VK_FORMAT_R64G64B64A64_SFLOAT,         {32, 4, VK_FORMAT_COMPATIBILITY_CLASS_256_BIT}},
    {VK_FORMAT_B10G11R11_UFLOAT_PACK32,     {4, 3, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,      {4, 3, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_D16_UNORM,                   {2, 1, VK_FORMAT_COMPATIBILITY_CLASS_NONE_BIT}},
    {VK_FORMAT_X8_D24_UNORM_PACK32,         {4, 1, VK_FORMAT_COMPATIBILITY_CLASS_NONE_BIT}},
    {VK_FORMAT_D32_SFLOAT,                  {4, 1, VK_FORMAT_COMPATIBILITY_CLASS_NONE_BIT}},
    {VK_FORMAT_S8_UINT,                     {1, 1, VK_FORMAT_COMPATIBILITY_CLASS_NONE_BIT}},
    {VK_FORMAT_D16_UNORM_S8_UINT,           {3, 2, VK_FORMAT_COMPATIBILITY_CLASS_NONE_BIT}},
    {VK_FORMAT_D24_UNORM_S8_UINT,           {4, 2, VK_FORMAT_COMPATIBILITY_CLASS_NONE_BIT}},
    {VK_FORMAT_D32_SFLOAT_S8_UINT,          {8, 2, VK_FORMAT_COMPATIBILITY_CLASS_NONE_BIT}},
    {VK_FORMAT_BC1_RGB_UNORM_BLOCK,         {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC1_RGB_BIT}},
    {VK_FORMAT_BC1_RGB_SRGB_BLOCK,          {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC1_RGB_BIT}},
    {VK_FORMAT_BC1_RGBA_UNORM_BLOCK,        {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC1_RGBA_BIT}},
    {VK_FORMAT_BC1_RGBA_SRGB_BLOCK,         {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC1_RGBA_BIT}},
    {VK_FORMAT_BC2_UNORM_BLOCK,             {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC2_BIT}},
    {VK_FORMAT_BC2_SRGB_BLOCK,              {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC2_BIT}},
    {VK_FORMAT_BC3_UNORM_BLOCK,             {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC3_BIT}},
    {VK_FORMAT_BC3_SRGB_BLOCK,              {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC3_BIT}},
    {VK_FORMAT_BC4_UNORM_BLOCK,             {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC4_BIT}},
    {VK_FORMAT_BC4_SNORM_BLOCK,             {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC4_BIT}},
    {VK_FORMAT_BC5_UNORM_BLOCK,             {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC5_BIT}},
    {VK_FORMAT_BC5_SNORM_BLOCK,             {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC5_BIT}},
    {VK_FORMAT_BC6H_UFLOAT_BLOCK,           {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC6H_BIT}},
    {VK_FORMAT_BC6H_SFLOAT_BLOCK,           {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC6H_BIT}},
    {VK_FORMAT_BC7_UNORM_BLOCK,             {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC7_BIT}},
    {VK_FORMAT_BC7_SRGB_BLOCK,              {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_BC7_BIT}},
    {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,     {8, 3, VK_FORMAT_COMPATIBILITY_CLASS_ETC2_RGB_BIT}},
    {VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,      {8, 3, VK_FORMAT_COMPATIBILITY_CLASS_ETC2_RGB_BIT}},
    {VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK,   {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_ETC2_RGBA_BIT}},
    {VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK,    {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_ETC2_RGBA_BIT}},
    {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,   {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ETC2_EAC_RGBA_BIT}},
    {VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,    {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ETC2_EAC_RGBA_BIT}},
    {VK_FORMAT_EAC_R11_UNORM_BLOCK,         {8, 1, VK_FORMAT_COMPATIBILITY_CLASS_EAC_R_BIT}},
    {VK_FORMAT_EAC_R11_SNORM_BLOCK,         {8, 1, VK_FORMAT_COMPATIBILITY_CLASS_EAC_R_BIT}},
    {VK_FORMAT_EAC_R11G11_UNORM_BLOCK,      {16, 2, VK_FORMAT_COMPATIBILITY_CLASS_EAC_RG_BIT}},
    {VK_FORMAT_EAC_R11G11_SNORM_BLOCK,      {16, 2, VK_FORMAT_COMPATIBILITY_CLASS_EAC_RG_BIT}},
    {VK_FORMAT_ASTC_4x4_UNORM_BLOCK,        {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_4X4_BIT}},
    {VK_FORMAT_ASTC_4x4_SRGB_BLOCK,         {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_4X4_BIT}},
    {VK_FORMAT_ASTC_5x4_UNORM_BLOCK,        {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_5X4_BIT}},
    {VK_FORMAT_ASTC_5x4_SRGB_BLOCK,         {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_5X4_BIT}},
    {VK_FORMAT_ASTC_5x5_UNORM_BLOCK,        {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_5X5_BIT}},
    {VK_FORMAT_ASTC_5x5_SRGB_BLOCK,         {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_5X5_BIT}},
    {VK_FORMAT_ASTC_6x5_UNORM_BLOCK,        {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_6X5_BIT}},
    {VK_FORMAT_ASTC_6x5_SRGB_BLOCK,         {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_6X5_BIT}},
    {VK_FORMAT_ASTC_6x6_UNORM_BLOCK,        {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_6X6_BIT}},
    {VK_FORMAT_ASTC_6x6_SRGB_BLOCK,         {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_6X6_BIT}},
    {VK_FORMAT_ASTC_8x5_UNORM_BLOCK,        {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_8X5_BIT}},
    {VK_FORMAT_ASTC_8x5_SRGB_BLOCK,         {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_8X5_BIT}},
    {VK_FORMAT_ASTC_8x6_UNORM_BLOCK,        {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_8X6_BIT}},
    {VK_FORMAT_ASTC_8x6_SRGB_BLOCK,         {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_8X6_BIT}},
    {VK_FORMAT_ASTC_8x8_UNORM_BLOCK,        {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_8X8_BIT}},
    {VK_FORMAT_ASTC_8x8_SRGB_BLOCK,         {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_8X8_BIT}},
    {VK_FORMAT_ASTC_10x5_UNORM_BLOCK,       {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_10X5_BIT}},
    {VK_FORMAT_ASTC_10x5_SRGB_BLOCK,        {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_10X5_BIT}},
    {VK_FORMAT_ASTC_10x6_UNORM_BLOCK,       {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_10X6_BIT}},
    {VK_FORMAT_ASTC_10x6_SRGB_BLOCK,        {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_10X6_BIT}},
    {VK_FORMAT_ASTC_10x8_UNORM_BLOCK,       {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_10X8_BIT}},
    {VK_FORMAT_ASTC_10x8_SRGB_BLOCK,        {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_10X8_BIT}},
    {VK_FORMAT_ASTC_10x10_UNORM_BLOCK,      {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_10X10_BIT}},
    {VK_FORMAT_ASTC_10x10_SRGB_BLOCK,       {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_10X10_BIT}},
    {VK_FORMAT_ASTC_12x10_UNORM_BLOCK,      {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_12X10_BIT}},
    {VK_FORMAT_ASTC_12x10_SRGB_BLOCK,       {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_12X10_BIT}},
    {VK_FORMAT_ASTC_12x12_UNORM_BLOCK,      {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_12X12_BIT}},
    {VK_FORMAT_ASTC_12x12_SRGB_BLOCK,       {16, 4, VK_FORMAT_COMPATIBILITY_CLASS_ASTC_12X12_BIT}},
    {VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG, {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_PVRTC1_2BPP_BIT}},
    {VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG, {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_PVRTC1_4BPP_BIT}},
    {VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG, {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_PVRTC2_2BPP_BIT}},
    {VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG, {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_PVRTC2_4BPP_BIT}},
    {VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG,  {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_PVRTC1_2BPP_BIT}},
    {VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG,  {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_PVRTC1_4BPP_BIT}},
    {VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG,  {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_PVRTC2_2BPP_BIT}},
    {VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG,  {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_PVRTC2_4BPP_BIT}},
    // KHR_sampler_YCbCr_conversion extension - single-plane variants
    // 'PACK' formats are normal, uncompressed
    {VK_FORMAT_R10X6_UNORM_PACK16,                          {2, 1, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R10X6G10X6_UNORM_2PACK16,                    {4, 2, VK_FORMAT_COMPATIBILITY_CLASS_32_BIT}},
    {VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16,          {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64BIT_R10G10B10A10}},
    {VK_FORMAT_R12X4_UNORM_PACK16,                          {2, 1, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R12X4G12X4_UNORM_2PACK16,                    {4, 2, VK_FORMAT_COMPATIBILITY_CLASS_16_BIT}},
    {VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16,          {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64BIT_R12G12B12A12}},
    // _422 formats encode 2 texels per entry with B, R components shared - treated as compressed w/ 2x1 block size
    {VK_FORMAT_G8B8G8R8_422_UNORM,                          {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32BIT_G8B8G8R8}},
    {VK_FORMAT_B8G8R8G8_422_UNORM,                          {4, 4, VK_FORMAT_COMPATIBILITY_CLASS_32BIT_B8G8R8G8}},
    {VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16,      {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64BIT_G10B10G10R10}},
    {VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16,      {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64BIT_B10G10R10G10}},
    {VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16,      {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64BIT_G12B12G12R12}},
    {VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16,      {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64BIT_B12G12R12G12}},
    {VK_FORMAT_G16B16G16R16_422_UNORM,                      {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64BIT_G16B16G16R16}},
    {VK_FORMAT_B16G16R16G16_422_UNORM,                      {8, 4, VK_FORMAT_COMPATIBILITY_CLASS_64BIT_B16G16R16G16}},
    // KHR_sampler_YCbCr_conversion extension - multi-plane variants
    // Formats that 'share' components among texels (_420 and _422), size represents total bytes for the smallest possible texel block
    // _420 share B, R components within a 2x2 texel block
    {VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,                   {6, 3, VK_FORMAT_COMPATIBILITY_CLASS_8BIT_3PLANE_420}},
    {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                    {6, 3, VK_FORMAT_COMPATIBILITY_CLASS_8BIT_2PLANE_420}},
    {VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16,  {12, 3, VK_FORMAT_COMPATIBILITY_CLASS_10BIT_3PLANE_420}},
    {VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,   {12, 3, VK_FORMAT_COMPATIBILITY_CLASS_10BIT_2PLANE_420}},
    {VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16,  {12, 3, VK_FORMAT_COMPATIBILITY_CLASS_12BIT_3PLANE_420}},
    {VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,   {12, 3, VK_FORMAT_COMPATIBILITY_CLASS_12BIT_2PLANE_420}},
    {VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM,                {12, 3, VK_FORMAT_COMPATIBILITY_CLASS_16BIT_3PLANE_420}},
    {VK_FORMAT_G16_B16R16_2PLANE_420_UNORM,                 {12, 3, VK_FORMAT_COMPATIBILITY_CLASS_16BIT_2PLANE_420}},
    // _422 share B, R components within a 2x1 texel block
    {VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM,                   {4, 3, VK_FORMAT_COMPATIBILITY_CLASS_8BIT_3PLANE_422}},
    {VK_FORMAT_G8_B8R8_2PLANE_422_UNORM,                    {4, 3, VK_FORMAT_COMPATIBILITY_CLASS_8BIT_2PLANE_422}},
    {VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16,  {8, 3, VK_FORMAT_COMPATIBILITY_CLASS_10BIT_3PLANE_422}},
    {VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16,   {8, 3, VK_FORMAT_COMPATIBILITY_CLASS_10BIT_2PLANE_422}},
    {VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16,  {8, 3, VK_FORMAT_COMPATIBILITY_CLASS_12BIT_3PLANE_422}},
    {VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16,   {8, 3, VK_FORMAT_COMPATIBILITY_CLASS_12BIT_2PLANE_422}},
    {VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM,                {8, 3, VK_FORMAT_COMPATIBILITY_CLASS_16BIT_3PLANE_422}},
    {VK_FORMAT_G16_B16R16_2PLANE_422_UNORM,                 {8, 3, VK_FORMAT_COMPATIBILITY_CLASS_16BIT_2PLANE_422}},
    // _444 do not share
    {VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM,                   {3, 3, VK_FORMAT_COMPATIBILITY_CLASS_8BIT_3PLANE_444}},
    {VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16,  {6, 3, VK_FORMAT_COMPATIBILITY_CLASS_10BIT_3PLANE_444}},
    {VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16,  {6, 3, VK_FORMAT_COMPATIBILITY_CLASS_12BIT_3PLANE_444}},
    {VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM,                {6, 3, VK_FORMAT_COMPATIBILITY_CLASS_16BIT_3PLANE_444}}
};
// Renable formatting
// clang-format on
// Return true if format is an ETC2 or EAC compressed texture format
bool format_is_compressed_etc2_eac(uint32_t format) {
    bool found = false;
    switch (format) {
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
            found = true;
            break;
        default:
            break;
    }
    return found;
}
// Return true if format is an ASTC compressed texture format
bool format_is_compressed_astc_ldr(uint32_t format) {
    bool found = false;
    switch (format) {
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            found = true;
            break;
        default:
            break;
    }
    return found;
}
// Return true if format is a BC compressed texture format
bool format_is_compressed_bc(uint32_t format) {
    bool found = false;
    switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            found = true;
            break;
        default:
            break;
    }
    return found;
}
// Return true if format is a PVRTC compressed texture format
bool format_is_compressed_pvrtc(uint32_t format) {
    bool found = false;
    switch (format) {
        case VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG:
            found = true;
            break;
        default:
            break;
    }
    return found;
}
// Single-plane "_422" formats are treated as 2x1 compressed (for copies)
bool format_is_single_plane_422(uint32_t format) {
    bool found = false;
    switch (format) {
        case VK_FORMAT_G8B8G8R8_422_UNORM_KHR:
        case VK_FORMAT_B8G8R8G8_422_UNORM_KHR:
        case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16_KHR:
        case VK_FORMAT_G16B16G16R16_422_UNORM_KHR:
        case VK_FORMAT_B16G16R16G16_422_UNORM_KHR:
            found = true;
            break;
        default:
            break;
    }
    return found;
}
// Return true if format is compressed
bool format_is_compressed(uint32_t format) {
  return (format_is_compressed_astc_ldr(format) || format_is_compressed_bc(format) || format_is_compressed_etc2_eac(format) || format_is_compressed_pvrtc(format));
}
// Return true if format is packed
bool format_is_packed(uint32_t format) {
    bool found = false;
    switch (format) {
        case VK_FORMAT_R4G4_UNORM_PACK8:
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_USCALED_PACK32:
        case VK_FORMAT_A8B8G8R8_SSCALED_PACK32:
        case VK_FORMAT_A8B8G8R8_UINT_PACK32:
        case VK_FORMAT_A8B8G8R8_SINT_PACK32:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_USCALED_PACK32:
        case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:
        case VK_FORMAT_A2R10G10B10_UINT_PACK32:
        case VK_FORMAT_A2R10G10B10_SINT_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
        case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_A2B10G10R10_SINT_PACK32:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_R10X6_UNORM_PACK16:
        case VK_FORMAT_R10X6G10X6_UNORM_2PACK16:
        case VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16:
        case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16:
        case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16:
        case VK_FORMAT_R12X4_UNORM_PACK16:
        case VK_FORMAT_R12X4G12X4_UNORM_2PACK16:
        case VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16:
        case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16:
        case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
            found = true;
            break;
        default:
            break;
    }
    return found;
}
// Return true if format is 'normal', with one texel per format element
bool format_element_is_texel(uint32_t format) {
    if (format_is_packed(format) || format_is_compressed(format) || format_is_single_plane_422(format) || format_is_multiplane(format)) {
        return false;
    } else {
        return true;
    }
}
// Return true if format is a depth or stencil format
bool format_is_depth_or_stencil(uint32_t format) {
    return (format_is_depth_and_stencil(format) || format_is_depth_only(format) || format_is_stencil_only(format));
}
// Return true if format contains depth and stencil information
bool format_is_depth_and_stencil(uint32_t format) {
    bool is_ds = false;
    switch (format) {
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            is_ds = true;
            break;
        default:
            break;
    }
    return is_ds;
}
// Return true if format is a stencil-only format
bool format_is_stencil_only(uint32_t format) { return (format == VK_FORMAT_S8_UINT); }
// Return true if format is a depth-only format
bool format_is_depth_only(uint32_t format) {
    bool is_depth = false;
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
            is_depth = true;
            break;
        default:
            break;
    }
    return is_depth;
}
// Return true if format is of type NORM
bool format_is_norm(uint32_t format) {
    bool is_norm = false;
    switch (format) {
        case VK_FORMAT_R4G4_UNORM_PACK8:
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8_SNORM:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16B16_UNORM:
        case VK_FORMAT_R16G16B16_SNORM:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_B8G8R8_UNORM:
        case VK_FORMAT_B8G8R8_SNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SNORM:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
            is_norm = true;
            break;
        default:
            break;
    }
    return is_norm;
}
// Return true if format is of type UNORM
bool format_is_unorm(uint32_t format) {
    bool is_unorm = false;
    switch (format) {
        case VK_FORMAT_R4G4_UNORM_PACK8:
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16B16_UNORM:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_B8G8R8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            is_unorm = true;
            break;
        default:
            break;
    }
    return is_unorm;
}
// Return true if format is of type SNORM
bool format_is_snorm(uint32_t format) {
    bool is_snorm = false;
    switch (format) {
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8B8_SNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16B16_SNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
        case VK_FORMAT_B8G8R8_SNORM:
        case VK_FORMAT_B8G8R8A8_SNORM:
        case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
            is_snorm = true;
            break;
        default:
            break;
    }
    return is_snorm;
}
// Return true if format is an integer format
bool format_is_int(uint32_t format) { return (format_is_sint(format) || format_is_uint(format)); }
// Return true if format is an unsigned integer format
bool format_is_uint(uint32_t format) {
    bool is_uint = false;
    switch (format) {
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_S8_UINT:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8B8_UINT:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_A8B8G8R8_UINT_PACK32:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16B16_UINT:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R64_UINT:
        case VK_FORMAT_R64G64_UINT:
        case VK_FORMAT_R64G64B64_UINT:
        case VK_FORMAT_R64G64B64A64_UINT:
        case VK_FORMAT_B8G8R8_UINT:
        case VK_FORMAT_B8G8R8A8_UINT:
        case VK_FORMAT_A2R10G10B10_UINT_PACK32:
            is_uint = true;
            break;
        default:
            break;
    }
    return is_uint;
}
// Return true if format is a signed integer format
bool format_is_sint(uint32_t format) {
    bool is_sint = false;
    switch (format) {
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8B8_SINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_A8B8G8R8_SINT_PACK32:
        case VK_FORMAT_A2B10G10R10_SINT_PACK32:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16B16_SINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R64_SINT:
        case VK_FORMAT_R64G64_SINT:
        case VK_FORMAT_R64G64B64_SINT:
        case VK_FORMAT_R64G64B64A64_SINT:
        case VK_FORMAT_B8G8R8_SINT:
        case VK_FORMAT_B8G8R8A8_SINT:
        case VK_FORMAT_A2R10G10B10_SINT_PACK32:
            is_sint = true;
            break;
        default:
            break;
    }
    return is_sint;
}
// Return true if format is a floating-point format
bool format_is_float(uint32_t format) {
    bool is_float = false;
    switch (format) {
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R16G16B16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R64_SFLOAT:
        case VK_FORMAT_R64G64_SFLOAT:
        case VK_FORMAT_R64G64B64_SFLOAT:
        case VK_FORMAT_R64G64B64A64_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
            is_float = true;
            break;
        default:
            break;
    }
    return is_float;
}
// Return true if format is in the SRGB colorspace
bool format_is_srgb(uint32_t format) {
    bool is_srgb = false;
    switch (format) {
        case VK_FORMAT_R8_SRGB:
        case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
        case VK_FORMAT_B8G8R8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
            is_srgb = true;
            break;
        default:
            break;
    }
    return is_srgb;
}
// Return true if format is a USCALED format
bool format_is_uscaled(uint32_t format) {
    bool is_uscaled = false;
    switch (format) {
        case VK_FORMAT_R8_USCALED:
        case VK_FORMAT_R8G8_USCALED:
        case VK_FORMAT_R8G8B8_USCALED:
        case VK_FORMAT_B8G8R8_USCALED:
        case VK_FORMAT_R8G8B8A8_USCALED:
        case VK_FORMAT_B8G8R8A8_USCALED:
        case VK_FORMAT_A8B8G8R8_USCALED_PACK32:
        case VK_FORMAT_A2R10G10B10_USCALED_PACK32:
        case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
        case VK_FORMAT_R16_USCALED:
        case VK_FORMAT_R16G16_USCALED:
        case VK_FORMAT_R16G16B16_USCALED:
        case VK_FORMAT_R16G16B16A16_USCALED:
            is_uscaled = true;
            break;
        default:
            break;
    }
    return is_uscaled;
}
// Return true if format is a SSCALED format
bool format_is_sscaled(uint32_t format) {
    bool is_sscaled = false;
    switch (format) {
        case VK_FORMAT_R8_SSCALED:
        case VK_FORMAT_R8G8_SSCALED:
        case VK_FORMAT_R8G8B8_SSCALED:
        case VK_FORMAT_B8G8R8_SSCALED:
        case VK_FORMAT_R8G8B8A8_SSCALED:
        case VK_FORMAT_B8G8R8A8_SSCALED:
        case VK_FORMAT_A8B8G8R8_SSCALED_PACK32:
        case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:
        case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
        case VK_FORMAT_R16_SSCALED:
        case VK_FORMAT_R16G16_SSCALED:
        case VK_FORMAT_R16G16B16_SSCALED:
        case VK_FORMAT_R16G16B16A16_SSCALED:
            is_sscaled = true;
            break;
        default:
            break;
    }
    return is_sscaled;
}
// Return texel block sizes for all formats
// Uncompressed formats return {1, 1, 1}
// Compressed formats return the compression block extents
// Multiplane formats return the 'shared' extent of their low-res channel(s)
std::tuple<uint32_t, uint32_t, uint32_t> format_texel_block_extent(uint32_t format) {
    VkExtent3D block_size = {1, 1, 1};
    switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
            block_size = {4, 4, 1};
            break;
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
            block_size = {5, 4, 1};
            break;
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
            block_size = {5, 5, 1};
            break;
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
            block_size = {6, 5, 1};
            break;
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
            block_size = {6, 6, 1};
            break;
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
            block_size = {8, 5, 1};
            break;
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
            block_size = {8, 6, 1};
            break;
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
            block_size = {8, 8, 1};
            break;
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
            block_size = {10, 5, 1};
            break;
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
            block_size = {10, 6, 1};
            break;
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
            block_size = {10, 8, 1};
            break;
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
            block_size = {10, 10, 1};
            break;
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
            block_size = {12, 10, 1};
            break;
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            block_size = {12, 12, 1};
            break;
        case VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG:
            block_size = {8, 4, 1};
            break;
        case VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG:
            block_size = {4, 4, 1};
            break;
        // (KHR_sampler_ycbcr_conversion) _422 single-plane formats are treated as 2x1 compressed (for copies)
        case VK_FORMAT_G8B8G8R8_422_UNORM:
        case VK_FORMAT_B8G8R8G8_422_UNORM:
        case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16:
        case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16:
        case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16:
        case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16:
        case VK_FORMAT_G16B16G16R16_422_UNORM:
        case VK_FORMAT_B16G16R16G16_422_UNORM:
            block_size = {2, 1, 1};
            break;
        // _422 multi-plane formats are not considered compressed, but shared components form a logical 2x1 block
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
        case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
            block_size = {2, 1, 1};
            break;
        // _420 formats are not considered compressed, but shared components form a logical 2x2 block
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
            block_size = {2, 2, 1};
            break;
        // _444 multi-plane formats do not share components, default to 1x1
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
        default:
            break;
    }
    return std::make_tuple(block_size.width, block_size.height, block_size.depth);
}
uint32_t format_depth_size(uint32_t format) {
    uint32_t depth_size = 0;
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D16_UNORM_S8_UINT:
            depth_size = 16;
            break;
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D24_UNORM_S8_UINT:
            depth_size = 24;
            break;
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            depth_size = 32;
            break;
        default:
            break;
    }
    return depth_size;
}
VkFormatNumericalType format_depth_numerical_type(uint32_t format) {
    VkFormatNumericalType numerical_type = VK_FORMAT_NUMERICAL_TYPE_NONE;
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D24_UNORM_S8_UINT:
            numerical_type = VK_FORMAT_NUMERICAL_TYPE_UNORM;
            break;
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            numerical_type = VK_FORMAT_NUMERICAL_TYPE_SFLOAT;
            break;
        default:
            break;
    }
    return numerical_type;
}
uint32_t format_stencil_size(uint32_t format) {
    uint32_t stencil_size = 0;
    switch (format) {
        case VK_FORMAT_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            stencil_size = 8;
            break;
        default:
            break;
    }
    return stencil_size;
}
VkFormatNumericalType format_stencil_numerical_type(uint32_t format) {
    VkFormatNumericalType numerical_type = VK_FORMAT_NUMERICAL_TYPE_NONE;
    switch (format) {
        case VK_FORMAT_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            numerical_type = VK_FORMAT_NUMERICAL_TYPE_UINT;
            break;
        default:
            break;
    }
    return numerical_type;
}
uint32_t format_plane_count(uint32_t format) {
    switch (format) {
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
        case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
        case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
        case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
            return 3;
            break;
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
        case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
            return 2;
            break;
        default:
            return 1;
            break;
    }
}
// Return format class of the specified format
VkFormatCompatibilityClass format_compatibility_class(uint32_t format) {
    auto item = vk_format_table.find(format);
    if (item != vk_format_table.end()) {
        return item->second.format_class;
    }
    return VK_FORMAT_COMPATIBILITY_CLASS_NONE_BIT;
}
// Return size, in bytes, of one element of the specified format
// For uncompressed this is one texel, for compressed it is one block
uint32_t format_element_size(uint32_t format, uint32_t aspectMask) {
    // Handle special buffer packing rules for specific depth/stencil formats
    if (aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
        format = VK_FORMAT_S8_UINT;
    } else if (aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
        switch (format) {
            case VK_FORMAT_D16_UNORM_S8_UINT:
                format = VK_FORMAT_D16_UNORM;
                break;
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                format = VK_FORMAT_D32_SFLOAT;
                break;
            default:
                break;
        }
    } else if (format_is_multiplane(format)) {
        format = find_multiplane_compatible_format(format, aspectMask);
    }
    auto item = vk_format_table.find(format);
    if (item != vk_format_table.end()) {
        return item->second.size;
    }
    return 0;
}
// Return the size in bytes of one texel of given foramt
// For compressed or multi-plane, this may be a fractional number
double format_texel_size(uint32_t format) {
  double texel_size = static_cast<double>(format_element_size(format));
  const auto [ width, height, depth ] = format_texel_block_extent(format);
  uint32_t texels_per_block = width * height * depth;
  if (1 < texels_per_block) {
    texel_size /= static_cast<double>(texels_per_block);
  }
  return texel_size;
}
// Return the number of channels for a given format
uint32_t format_channel_count(uint32_t format) {
    auto item = vk_format_table.find(format);
    if (item != vk_format_table.end()) {
        return item->second.channel_count;
    }
    return 0;
}
// Perform a zero-tolerant modulo operation
size_t safe_modulo(size_t dividend, size_t divisor) {
  size_t result = 0;
  if (divisor != 0) {
    result = dividend % divisor;
  }
  return result;
}
size_t safe_division(size_t dividend, size_t divisor) {
  size_t result = 0;
  if (divisor != 0) {
    result = dividend / divisor;
  }
  return result;
}
struct VULKAN_PER_PLANE_COMPATIBILITY {
    uint32_t width_divisor;
    uint32_t height_divisor;
    VkFormat compatible_format;
};
struct VULKAN_MULTIPLANE_COMPATIBILITY {
    VULKAN_PER_PLANE_COMPATIBILITY per_plane[VK_MULTIPLANE_FORMAT_MAX_PLANES];
};

// Source: Vulkan spec Table 45. Plane Format Compatibility Table
// clang-format off
const gtl::flat_hash_map<uint32_t, VULKAN_MULTIPLANE_COMPATIBILITY> vk_multiplane_compatibility_map {
    { VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,                  { { { 1, 1, VK_FORMAT_R8_UNORM },
                                                                { 2, 2, VK_FORMAT_R8_UNORM },
                                                                { 2, 2, VK_FORMAT_R8_UNORM } } } },
    { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                   { { { 1, 1, VK_FORMAT_R8_UNORM },
                                                                { 2, 2, VK_FORMAT_R8G8_UNORM },
                                                                { 1, 1, VK_FORMAT_UNDEFINED } } } },
    { VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM,                  { { { 1, 1, VK_FORMAT_R8_UNORM },
                                                                { 2, 1, VK_FORMAT_R8_UNORM },
                                                                { 2, 1, VK_FORMAT_R8_UNORM } } } },
    { VK_FORMAT_G8_B8R8_2PLANE_422_UNORM,                   { { { 1, 1, VK_FORMAT_R8_UNORM },
                                                                { 2, 1, VK_FORMAT_R8G8_UNORM },
                                                                { 1, 1, VK_FORMAT_UNDEFINED } } } },
    { VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM,                  { { { 1, 1, VK_FORMAT_R8_UNORM },
                                                                { 1, 1, VK_FORMAT_R8_UNORM },
                                                                { 1, 1, VK_FORMAT_R8_UNORM } } } },
    { VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16, { { { 1, 1, VK_FORMAT_R10X6_UNORM_PACK16 },
                                                                { 2, 2, VK_FORMAT_R10X6_UNORM_PACK16 },
                                                                { 2, 2, VK_FORMAT_R10X6_UNORM_PACK16 } } } },
    { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,  { { { 1, 1, VK_FORMAT_R10X6_UNORM_PACK16 },
                                                                { 2, 2, VK_FORMAT_R10X6G10X6_UNORM_2PACK16 },
                                                                { 1, 1, VK_FORMAT_UNDEFINED } } } },
    { VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16, { { { 1, 1, VK_FORMAT_R10X6_UNORM_PACK16 },
                                                                { 2, 1, VK_FORMAT_R10X6_UNORM_PACK16 },
                                                                { 2, 1, VK_FORMAT_R10X6_UNORM_PACK16 } } } },
    { VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16,  { { { 1, 1, VK_FORMAT_R10X6_UNORM_PACK16 },
                                                                { 2, 1, VK_FORMAT_R10X6G10X6_UNORM_2PACK16 },
                                                                { 1, 1, VK_FORMAT_UNDEFINED } } } },
    { VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16, { { { 1, 1, VK_FORMAT_R10X6_UNORM_PACK16 },
                                                                { 1, 1, VK_FORMAT_R10X6_UNORM_PACK16 },
                                                                { 1, 1, VK_FORMAT_R10X6_UNORM_PACK16 } } } },
    { VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16, { { { 1, 1, VK_FORMAT_R12X4_UNORM_PACK16 },
                                                                { 2, 2, VK_FORMAT_R12X4_UNORM_PACK16 },
                                                                { 2, 2, VK_FORMAT_R12X4_UNORM_PACK16 } } } },
    { VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16,  { { { 1, 1, VK_FORMAT_R12X4_UNORM_PACK16 },
                                                                { 2, 2, VK_FORMAT_R12X4G12X4_UNORM_2PACK16 },
                                                                { 1, 1, VK_FORMAT_UNDEFINED } } } },
    { VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16, { { { 1, 1, VK_FORMAT_R12X4_UNORM_PACK16 },
                                                                { 2, 1, VK_FORMAT_R12X4_UNORM_PACK16 },
                                                                { 2, 1, VK_FORMAT_R12X4_UNORM_PACK16 } } } },
    { VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16,  { { { 1, 1, VK_FORMAT_R12X4_UNORM_PACK16 },
                                                                { 2, 1, VK_FORMAT_R12X4G12X4_UNORM_2PACK16 },
                                                                { 1, 1, VK_FORMAT_UNDEFINED } } } },
    { VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16, { { { 1, 1, VK_FORMAT_R12X4_UNORM_PACK16 },
                                                                { 1, 1, VK_FORMAT_R12X4_UNORM_PACK16 },
                                                                { 1, 1, VK_FORMAT_R12X4_UNORM_PACK16 } } } },
    { VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM,               { { { 1, 1, VK_FORMAT_R16_UNORM },
                                                                { 2, 2, VK_FORMAT_R16_UNORM },
                                                                { 2, 2, VK_FORMAT_R16_UNORM } } } },
    { VK_FORMAT_G16_B16R16_2PLANE_420_UNORM,                { { { 1, 1, VK_FORMAT_R16_UNORM },
                                                                { 2, 2, VK_FORMAT_R16G16_UNORM },
                                                                { 1, 1, VK_FORMAT_UNDEFINED } } } },
    { VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM,               { { { 1, 1, VK_FORMAT_R16_UNORM },
                                                                { 2, 1, VK_FORMAT_R16_UNORM },
                                                                { 2, 1, VK_FORMAT_R16_UNORM } } } },
    { VK_FORMAT_G16_B16R16_2PLANE_422_UNORM,                { { { 1, 1, VK_FORMAT_R16_UNORM },
                                                                { 2, 1, VK_FORMAT_R16G16_UNORM },
                                                                { 1, 1, VK_FORMAT_UNDEFINED } } } },
    { VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM,               { { { 1, 1, VK_FORMAT_R16_UNORM },
                                                                { 1, 1, VK_FORMAT_R16_UNORM },
                                                                { 1, 1, VK_FORMAT_R16_UNORM } } } }
};
// clang-format on
uint32_t get_plane_index(uint32_t aspect) {
    // Returns an out of bounds index on error
    switch (aspect) {
        case VK_IMAGE_ASPECT_PLANE_0_BIT:
            return 0;
            break;
        case VK_IMAGE_ASPECT_PLANE_1_BIT:
            return 1;
            break;
        case VK_IMAGE_ASPECT_PLANE_2_BIT:
            return 2;
            break;
        default:
            // If more than one plane bit is set, return error condition
            return VK_MULTIPLANE_FORMAT_MAX_PLANES;
            break;
    }
}
uint32_t find_multiplane_compatible_format(uint32_t mp_fmt, uint32_t plane_aspect) {
    uint32_t plane_idx = get_plane_index(plane_aspect);
    auto it = vk_multiplane_compatibility_map.find(mp_fmt);
    if ((it == vk_multiplane_compatibility_map.end()) || (plane_idx >= VK_MULTIPLANE_FORMAT_MAX_PLANES)) {
        return VK_FORMAT_UNDEFINED;
    }
    return it->second.per_plane[plane_idx].compatible_format;
}
std::tuple<uint32_t, uint32_t> find_multiplane_extent_divisors(uint32_t mp_fmt, uint32_t plane_aspect) {
    VkExtent2D divisors = {1, 1};
    uint32_t plane_idx = get_plane_index(plane_aspect);
    auto it = vk_multiplane_compatibility_map.find(mp_fmt);
    if ((it == vk_multiplane_compatibility_map.end()) || (plane_idx >= VK_MULTIPLANE_FORMAT_MAX_PLANES)) {
        return std::make_tuple(divisors.width, divisors.height);
    }
    divisors.width = it->second.per_plane[plane_idx].width_divisor;
    divisors.height = it->second.per_plane[plane_idx].height_divisor;
    return std::make_tuple(divisors.width, divisors.height);
}
bool format_sizes_are_equal(uint32_t srcFormat, uint32_t dstFormat, uint32_t region_count, const VkImageCopy *regions) {
    size_t srcSize = 0, dstSize = 0;
    if (format_is_multiplane(srcFormat) || format_is_multiplane(dstFormat)) {
        for (uint32_t i = 0; i < region_count; i++) {
            if (format_is_multiplane(srcFormat)) {
                uint32_t planeFormat = find_multiplane_compatible_format(srcFormat, regions[i].srcSubresource.aspectMask);
                srcSize = format_element_size(planeFormat);
            } else {
                srcSize = format_element_size(srcFormat);
            }
            if (format_is_multiplane(dstFormat)) {
                uint32_t planeFormat = find_multiplane_compatible_format(dstFormat, regions[i].dstSubresource.aspectMask);
                dstSize = format_element_size(planeFormat);
            } else {
                dstSize = format_element_size(dstFormat);
            }
            if (dstSize != srcSize) return false;
        }
        return true;
    } else {
        srcSize = format_element_size(srcFormat);
        dstSize = format_element_size(dstFormat);
        return (dstSize == srcSize);
    }
}
bool format_requires_ycbcr_conversion(uint32_t format) {
  return format >= VK_FORMAT_G8B8G8R8_422_UNORM && format <= VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM;
}

bool format_is_undef(uint32_t format) { return (format == VK_FORMAT_UNDEFINED); }
bool format_has_depth(uint32_t format) { return (format_is_depth_only(format) || format_is_depth_and_stencil(format)); }
bool format_has_stencil(uint32_t format) { return (format_is_stencil_only(format) || format_is_depth_and_stencil(format)); }
bool format_is_multiplane(uint32_t format) { return ((format_plane_count(format)) > 1u); }
bool format_is_color(uint32_t format) { return !(format_is_undef(format) || format_is_depth_or_stencil(format) || format_is_multiplane(format)); }

}
}