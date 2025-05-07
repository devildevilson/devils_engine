#include "system_info.h"

#include <ranges>
namespace rv = std::ranges::views;
#include "vulkan_header.h"

#include "auxiliary.h"
#include "utils/core.h"
#include "utils/fileio.h"
#include "utils/named_serializer.h"

namespace devils_engine {
namespace painter {

static_assert(static_cast<uint32_t>(vk::PhysicalDeviceType::eCpu) == static_cast<uint32_t>(system_info::physical_device::type::cpu));
static_assert(static_cast<uint32_t>(vk::PresentModeKHR::eFifoRelaxed) == static_cast<uint32_t>(system_info::physical_device::present_mode::fifo_relaxed));

std::string system_info::physical_device::to_string(const enum system_info::physical_device::type type) {
  return vk::to_string(static_cast<vk::PhysicalDeviceType>(type));
}

std::string system_info::physical_device::to_string(const enum present_mode present_mode) {
  return vk::to_string(static_cast<vk::PresentModeKHR>(present_mode));
}

system_info::physical_device::physical_device() : handle(VK_NULL_HANDLE), memory(0), id(UINT32_MAX), vendor_id(UINT32_MAX), type(physical_device::type::count), queue_family_index_surface_support(UINT32_MAX) {}

system_info::system_info() : instance_owner(true), instance(VK_NULL_HANDLE) { init(); }
system_info::system_info(VkInstance i) : instance_owner(false), instance(i) { init(); }

system_info::~system_info() noexcept {
  if (instance_owner) vk::Instance(instance).destroy();
  instance = VK_NULL_HANDLE;
}

void system_info::init() {
  if (instance_owner) {
    load_dispatcher1();

    vk::ApplicationInfo app_info(DEVILS_ENGINE_PROJECT_NAME, 0, "devils_engine", 0, VK_API_VERSION_1_3);

    if (enable_validation_layers && !check_validation_layer_support(default_validation_layers)) {
      utils::error("Could not find validation layers for Vulkan");
    }

    const auto req_extensions = get_required_extensions();

    vk::InstanceCreateInfo i({}, &app_info, default_validation_layers, req_extensions);

    auto instancepp = vk::createInstance(i);
    instance = instancepp;

    load_dispatcher2(instance);
  }

  vk::Instance instancepp = instance;
  const auto phys_devices = instancepp.enumeratePhysicalDevices();

  devices.resize(phys_devices.size());

  for (size_t i = 0; i < phys_devices.size(); ++i) {
    auto dev = phys_devices[i];
    devices[i].handle = dev;

    const auto feats = dev.getFeatures2();
    const auto props = dev.getProperties2();
    const auto fams = dev.getQueueFamilyProperties2();
    const auto mem_props = dev.getMemoryProperties2();

    devices[i].name = std::string(props.properties.deviceName.data());
    devices[i].id = props.properties.deviceID;
    devices[i].vendor_id = props.properties.vendorID;
    devices[i].type = static_cast<enum physical_device::type>(props.properties.deviceType);
    for (const auto &fam_prop : fams) {
      devices[i].queue_families.push_back(physical_device::queue_properties_t{
        static_cast<VkFlags>(fam_prop.queueFamilyProperties.queueFlags),
        fam_prop.queueFamilyProperties.queueCount,
        fam_prop.queueFamilyProperties.timestampValidBits
      });
    }

    for (size_t j = 0; j < mem_props.memoryProperties.memoryHeapCount; ++j) {
      const auto flags = mem_props.memoryProperties.memoryHeaps[j].flags;
      const auto mem_size = mem_props.memoryProperties.memoryHeaps[j].size;

      if (flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
        devices[i].memory = std::max(devices[i].memory, mem_size);
      }
    }
  }
}

void system_info::check_devices_surface_capability(const VkSurfaceKHR s) {
  for (auto &info : devices) {
    auto dev = vk::PhysicalDevice(info.handle);
    const auto modes = dev.getSurfacePresentModesKHR(s);
    info.present_modes.resize(modes.size());
    static_assert(sizeof(info.present_modes[0]) == sizeof(modes[0]));
    memcpy(info.present_modes.data(), modes.data(), modes.size() * sizeof(info.present_modes[0]));

    for (uint32_t i = 0; i < info.queue_families.size(); ++i) {
      const bool supp = physical_device_presentation_support(instance, dev, i);
      if (supp) { info.queue_family_index_surface_support = i; break; }
    }
  }
}

VkPhysicalDevice system_info::choose_physical_device() const {
  // как найти подходящее устройство? нам 100% нужно найти устройство с возможностью вывода на экран
  // дальше желательно чтобы оно было дискретной карточкой и желательно чтобы там было побольше памяти доступно

  VkPhysicalDevice dev = VK_NULL_HANDLE;
  size_t max_mem = 0;

  for (const auto &info : devices) {
    if (info.queue_family_index_surface_support == UINT32_MAX) continue;
    if (info.type != physical_device::type::discrete_gpu) continue;

    if (max_mem < info.memory) {
      dev = info.handle;
      max_mem = info.memory;
    }
  }

  if (dev == VK_NULL_HANDLE) {
    for (const auto &info : devices) {
      if (info.queue_family_index_surface_support == UINT32_MAX) continue;

      if (max_mem < info.memory) {
        dev = info.handle;
        max_mem = info.memory;
      }
    }
  }

  return dev;
}

static uint32_t find_queue(const std::vector<system_info::physical_device::queue_properties_t> &families, const vk::QueueFlags &flags) {
  for (uint32_t i = 0; i < families.size(); ++i) {
    if ((vk::QueueFlags(families[i].flags) & flags) == flags) return i;
  }

  return UINT32_MAX;
}

static std::pair<system_info::physical_device::present_mode, system_info::physical_device::present_mode> find_present_modes(
  const std::vector<system_info::physical_device::present_mode> &modes
) {
  auto main = system_info::physical_device::present_mode::count;
  for (const auto &mode : modes) {
    if (mode == system_info::physical_device::present_mode::mailbox) main = mode;
  }

  auto secondary = system_info::physical_device::present_mode::immediate;
  for (const auto &mode : modes) {
    if (mode == system_info::physical_device::present_mode::fifo) secondary = mode;
  }

  if (main == system_info::physical_device::present_mode::mailbox) return std::make_pair(main, secondary);
  return std::make_pair(secondary, system_info::physical_device::present_mode::immediate);
}

void system_info::dump_cache_to_disk(VkPhysicalDevice dev, cached_system_data* cached_data) {
  cached_system_data data;

  for (const auto &info : devices) {
    if (dev != info.handle) continue;

    const auto [m1, m2] = find_present_modes(info.present_modes);

    data.device_name = info.name;
    data.device_type = physical_device::to_string(info.type);
    data.device_id = info.id;
    data.vendor_id = info.vendor_id;
    data.graphics_queue = find_queue(info.queue_families, vk::QueueFlagBits::eGraphics);
    data.compute_queue = find_queue(info.queue_families, vk::QueueFlagBits::eCompute);
    data.present_queue = info.queue_family_index_surface_support;
    data.desirable_present_mode = system_info::physical_device::to_string(m1);
    data.fallback_present_mode = system_info::physical_device::to_string(m2);
    data.memory_capacity = info.memory;

    std::transform(data.device_type.begin(), data.device_type.end(), data.device_type.begin(), [] (const char c) { return std::tolower(c); });
    std::transform(data.desirable_present_mode.begin(), data.desirable_present_mode.end(), data.desirable_present_mode.begin(), [] (const char c) { return std::tolower(c); });
    std::transform(data.fallback_present_mode.begin(), data.fallback_present_mode.end(), data.fallback_present_mode.begin(), [] (const char c) { return std::tolower(c); });
  }

  if (data.device_name.empty()) utils::error("Trying to dump data for invalid physical device");

  const auto project_path = utils::project_folder();
  const auto directory_path = project_path + "cache/";
  if (!file_io::exists(project_path + "cache/")) file_io::create_directory(directory_path);

  const auto file_path = directory_path + "main_device.json";
  const auto json = utils::to_json<glz::opts{ .prettify = true, .indentation_width = 2 }>(data);
  file_io::write(json.value(), file_path);

  if (cached_data != nullptr) *cached_data = data;
}
}
}