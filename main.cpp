#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/assets_base.h"
#include "devils_engine/painter/makers.h"
#include "devils_engine/painter/auxiliary.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/input/core.h"

#include <iostream>

using namespace devils_engine;

const char* app_name = "fast_test";
const char* engine_name = "fast_test";
const uint32_t app_version = VK_MAKE_VERSION(0,0,1);
const uint32_t engine_version = VK_MAKE_VERSION(0, 0, 1);
const uint32_t api_version = VK_API_VERSION_1_0;

struct vulkan_init {
  VkInstance instance;
  VkDebugUtilsMessengerEXT mess;
  painter::physical_device_data p_data;
  VkDevice device;

  vulkan_init() {
    painter::load_dispatcher1();

    // создадим инстанс и девайс
    vk::ApplicationInfo ai{};
    ai.pApplicationName = app_name;
    ai.applicationVersion = app_version;
    ai.pEngineName = engine_name;
    ai.engineVersion = engine_version;
    ai.apiVersion = api_version;

    std::vector<const char*> exts = { "VK_EXT_debug_utils" };

    vk::InstanceCreateInfo ici{};
    ici.pApplicationInfo = &ai;
    ici.enabledLayerCount = painter::default_validation_layers.size();
    ici.ppEnabledLayerNames = painter::default_validation_layers.data();
    uint32_t count = 0;
    const auto ptrs = input::get_required_instance_extensions(&count);
    for (uint32_t i = 0; i < count; ++i) { exts.push_back(ptrs[i]); }
    ici.enabledExtensionCount = exts.size();
    ici.ppEnabledExtensionNames = exts.data();

    instance = vk::createInstance(ici);
    painter::load_dispatcher2(instance);
    mess = painter::create_debug_messenger(instance);

    if (!painter::system_info::try_load_cached_data(instance, &p_data, nullptr)) {
      auto w = input::create_window(640, 480, "fast_test");
      VkSurfaceKHR surface = VK_NULL_HANDLE;
      input::create_window_surface(instance, w, nullptr, &surface);

      painter::system_info si(instance);
      si.check_devices_surface_capability(surface);
      p_data = si.choose_physical_device();
      si.dump_cache_to_disk(p_data.handle, nullptr);

      vk::Instance(instance).destroy(surface);
      input::destroy(w);
    }

    painter::system_info::print_choosed_device(p_data.handle);

    painter::device_maker dm(instance);
    dm.beginDevice(p_data.handle);
    dm.createQueues(1);
    dm.features(vk::PhysicalDevice(p_data.handle).getFeatures());
    dm.setExtensions(painter::default_device_extensions);
    device = dm.create({}, "main_device");

    painter::load_dispatcher3(device);
  }

  ~vulkan_init() {
    vk::Device(device).destroy();
    painter::destroy_debug_messenger(instance, mess);
    vk::Instance(instance).destroy();
  }
};

static void error_callback(int err, const char* msg) noexcept {
  utils::info("Error: {}", msg);
}

int main() {
  const auto cur_folder = utils::project_folder();
  utils::println(cur_folder);

  input::init input_init(&error_callback);
  vulkan_init vk;
  vk::Device dev(vk.device);

  // получим queue, надо бы сделать еще проверку на несколько queue в одной семье
  auto graphics_queue = dev.getQueue(vk.p_data.graphics_queue, 0);
  auto compute_queue = dev.getQueue(vk.p_data.compute_queue, 0);
  auto transfer_queue = dev.getQueue(vk.p_data.transfer_queue, 0);

  painter::graphics_base base(vk.instance, vk.device, vk.p_data.handle, VK_NULL_HANDLE);

  // создать аллокатор, дескриптор пул, комманд пул, свапчеин, фенсы
  base.create_allocator();
  base.create_command_pool(vk.p_data.graphics_queue, graphics_queue);
  base.create_descriptor_pool();
  // свопчеин зависит от пачки ресурсов с диска
  //base.recreate_swapchain(640, 480);
  base.get_or_create_pipeline_cache(utils::cache_folder() + "pipeline_cache");

  painter::assets_base assets;
  assets.device = dev;
  assets.physical_device = vk.p_data.handle;

  // создать аллокатор, комманд пул

  utils::println("Exit");
}