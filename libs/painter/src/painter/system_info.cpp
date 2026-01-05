#include "system_info.h"

#include <gtl/phmap.hpp>
#include <ranges>
namespace rv = std::ranges::views;
#include "vulkan_header.h"

#include "auxiliary.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/named_serializer.h"

namespace devils_engine {
namespace painter {

static_assert(static_cast<uint32_t>(vk::PhysicalDeviceType::eCpu) == static_cast<uint32_t>(physical_device_type::cpu));
static_assert(static_cast<uint32_t>(vk::PhysicalDeviceType::eDiscreteGpu) == static_cast<uint32_t>(physical_device_type::discrete_gpu));
static_assert(static_cast<uint32_t>(vk::PhysicalDeviceType::eIntegratedGpu) == static_cast<uint32_t>(physical_device_type::integrated_gpu));
static_assert(static_cast<uint32_t>(vk::PhysicalDeviceType::eOther) == static_cast<uint32_t>(physical_device_type::other));
static_assert(static_cast<uint32_t>(vk::PhysicalDeviceType::eVirtualGpu) == static_cast<uint32_t>(physical_device_type::virtual_gpu));

static_assert(static_cast<uint32_t>(vk::PresentModeKHR::eFifoRelaxed) == static_cast<uint32_t>(physical_device_present_mode::fifo_relaxed));
static_assert(static_cast<uint32_t>(vk::PresentModeKHR::eImmediate) == static_cast<uint32_t>(physical_device_present_mode::immediate));
static_assert(static_cast<uint32_t>(vk::PresentModeKHR::eMailbox) == static_cast<uint32_t>(physical_device_present_mode::mailbox));
static_assert(static_cast<uint32_t>(vk::PresentModeKHR::eFifo) == static_cast<uint32_t>(physical_device_present_mode::fifo));
static_assert(static_cast<uint32_t>(vk::PresentModeKHR::eSharedDemandRefresh) == static_cast<uint32_t>(physical_device_present_mode::shared_demand_refresh));
static_assert(static_cast<uint32_t>(vk::PresentModeKHR::eSharedContinuousRefresh) == static_cast<uint32_t>(physical_device_present_mode::shared_continuous_refresh));

namespace device_features {

const std::string_view names[] = {
#define X(name, vulkan_name) #name ,
  DEVILS_ENGINE_PAINTER_FEATURES_10_LIST
#undef X
};

const gtl::flat_hash_map<std::string_view, values> map = {
#define X(name, vulkan_name) std::make_pair(names[values::name], values::name),
  DEVILS_ENGINE_PAINTER_FEATURES_10_LIST
#undef X
};

std::string_view to_string(const enum values val) noexcept {
  if (val >= values::count) return std::string_view();
  return names[val];
}

values from_string(const std::string_view& str) noexcept {
  const auto itr = map.find(str);
  if (itr == map.end()) return values::count;
  return itr->second;
}

}

namespace physical_device_type {
std::string_view to_string(const enum values val) noexcept {
  switch (val) {
#define X(name) case values::name: return #name ;
    DEVILS_ENGINE_PHYSICAL_DEVICE_TYPE_LIST
#undef X
    default: break;
  }

  return std::string_view();
}

values from_string(const std::string_view& str) noexcept {
#define X(name) if (str == #name) return values::name;
  DEVILS_ENGINE_PHYSICAL_DEVICE_TYPE_LIST
#undef X
  return values::count;
}
}

namespace physical_device_present_mode {
std::string_view to_string(const enum values val) noexcept {
  switch (val) {
#define X(name, value) case values::name: return #name ;
    DEVILS_ENGINE_PHYSICAL_DEVICE_PRESENT_MODE_LIST
#undef X
    default: break;
  }

  return std::string_view();
}

values from_string(const std::string_view& str) noexcept {
#define X(name, value) if (str == #name) return values::name;
  DEVILS_ENGINE_PHYSICAL_DEVICE_PRESENT_MODE_LIST
#undef X
  return values::count;
}
}

system_info::physical_device::physical_device() : handle(VK_NULL_HANDLE), memory(0), id(UINT32_MAX), vendor_id(UINT32_MAX), type(physical_device_type::values::count), queue_family_index_surface_support(UINT32_MAX) {}

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
      utils::error{}("Could not find validation layers for Vulkan");
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

    // VULKAN 1.0
    const auto feats = dev.getFeatures();
    const auto props = dev.getProperties();
    const auto fams = dev.getQueueFamilyProperties();
    const auto mem_props = dev.getMemoryProperties();

    devices[i].name = std::string(props.deviceName.data());
    devices[i].id = props.deviceID;
    devices[i].vendor_id = props.vendorID;
    devices[i].type = static_cast<enum physical_device_type::values>(props.deviceType);
    for (const auto &fam_prop : fams) {
      devices[i].queue_families.push_back(physical_device::queue_properties_t{
        static_cast<VkFlags>(fam_prop.queueFlags),
        fam_prop.queueCount,
        fam_prop.timestampValidBits
      });
    }

    for (size_t j = 0; j < mem_props.memoryHeapCount; ++j) {
      const auto flags = mem_props.memoryHeaps[j].flags;
      const auto mem_size = mem_props.memoryHeaps[j].size;

      if (flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
        devices[i].memory = std::max(devices[i].memory, mem_size);
      }
    }

#define X(name, vulkan_name) devices[i].features.set(device_features:: name , (bool) feats. vulkan_name );
    DEVILS_ENGINE_PAINTER_FEATURES_10_LIST
#undef X
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

static uint32_t find_queue(const std::vector<system_info::physical_device::queue_properties_t>& families, const vk::QueueFlags& flags) {
  for (uint32_t i = 0; i < families.size(); ++i) {
    if ((vk::QueueFlags(families[i].flags) & flags) == flags) return i;
  }

  return UINT32_MAX;
}

// по возможности поищем уникальные queue для каждого типа взаимодействия
static uint32_t find_queue(const std::vector<system_info::physical_device::queue_properties_t>& families, const vk::QueueFlags& flags, const std::initializer_list<uint32_t>& ignore_queue) {
  for (int32_t count = int32_t(ignore_queue.size()); count >= 0; --count) {
    for (uint32_t i = 0; i < families.size(); ++i) {
      if ((vk::QueueFlags(families[i].flags) & flags) != flags) continue;
      const auto end = ignore_queue.begin() + count;
      if (std::find(ignore_queue.begin(), end, i) != end) continue;
      return i;
    }
  }

  return UINT32_MAX;
}

static std::pair<physical_device_present_mode::values, physical_device_present_mode::values> find_present_modes(
  const std::vector<physical_device_present_mode::values>& modes
) {
  auto main = physical_device_present_mode::values::count;
  for (const auto& mode : modes) {
    if (mode == physical_device_present_mode::values::mailbox) main = mode;
  }

  auto secondary = physical_device_present_mode::values::immediate;
  for (const auto& mode : modes) {
    if (mode == physical_device_present_mode::values::fifo) secondary = mode;
  }

  if (main == physical_device_present_mode::values::mailbox) return std::make_pair(main, secondary);
  return std::make_pair(secondary, physical_device_present_mode::values::immediate);
}

physical_device_data system_info::choose_physical_device() const {
  // как найти подходящее устройство? нам 100% нужно найти устройство с возможностью вывода на экран
  // дальше желательно чтобы оно было дискретной карточкой и желательно чтобы там было побольше памяти доступно

  physical_device_data p_data;
  p_data.handle = VK_NULL_HANDLE;
  size_t max_mem = 0;
  uint32_t dev_index = UINT32_MAX;

  uint32_t index = 0;
  for (index = 0; index < devices.size(); ++index) {
    const auto& info = devices[index];

    if (info.queue_family_index_surface_support == UINT32_MAX) continue;
    if (info.type != physical_device_type::values::discrete_gpu) continue;

    if (max_mem < info.memory) {
      p_data.handle = info.handle;
      max_mem = info.memory;
      dev_index = index;
    }
  }

  if (p_data.handle != VK_NULL_HANDLE) {
    const auto& info = devices[dev_index];

    const auto [m1, m2] = find_present_modes(info.present_modes);
    p_data.desirable_present_mode = m1;
    p_data.fallback_present_mode = m2;
    p_data.features = info.features;

    p_data.graphics_queue = find_queue(info.queue_families, vk::QueueFlagBits::eGraphics, {});
    p_data.compute_queue = find_queue(info.queue_families, vk::QueueFlagBits::eCompute, { p_data.graphics_queue });
    p_data.transfer_queue = find_queue(info.queue_families, vk::QueueFlagBits::eTransfer, { p_data.graphics_queue, p_data.compute_queue });
    assert(p_data.graphics_queue != UINT32_MAX);
    assert(p_data.compute_queue != UINT32_MAX);
    assert(p_data.transfer_queue != UINT32_MAX);
    p_data.present_queue = info.queue_family_index_surface_support;

    return p_data;
  }

  for (index = 0; index < devices.size(); ++index) {
    const auto& info = devices[index];

    if (info.queue_family_index_surface_support == UINT32_MAX) continue;
    if (info.type != physical_device_type::values::integrated_gpu) continue;

    if (max_mem < info.memory) {
      p_data.handle = info.handle;
      max_mem = info.memory;
      dev_index = index;
    }
  }

  if (p_data.handle != VK_NULL_HANDLE) {
    const auto& info = devices[dev_index];

    const auto [m1, m2] = find_present_modes(info.present_modes);
    p_data.desirable_present_mode = m1;
    p_data.fallback_present_mode = m2;
    p_data.features = info.features;

    p_data.graphics_queue = find_queue(info.queue_families, vk::QueueFlagBits::eGraphics, {});
    p_data.compute_queue = find_queue(info.queue_families, vk::QueueFlagBits::eCompute, { p_data.graphics_queue });
    p_data.transfer_queue = find_queue(info.queue_families, vk::QueueFlagBits::eTransfer, { p_data.graphics_queue, p_data.compute_queue });
    assert(p_data.graphics_queue != UINT32_MAX);
    assert(p_data.compute_queue != UINT32_MAX);
    assert(p_data.transfer_queue != UINT32_MAX);
    p_data.present_queue = info.queue_family_index_surface_support;

    return p_data;
  }

  for (index = 0; index < devices.size(); ++index) {
    const auto& info = devices[index];

    if (info.queue_family_index_surface_support == UINT32_MAX) continue;

    if (max_mem < info.memory) {
      p_data.handle = info.handle;
      max_mem = info.memory;
      dev_index = index;
    }
  }

  const auto& info = devices[dev_index];

  const auto [m1, m2] = find_present_modes(info.present_modes);
  p_data.desirable_present_mode = m1;
  p_data.fallback_present_mode = m2;
  p_data.features = info.features;

  p_data.graphics_queue = find_queue(info.queue_families, vk::QueueFlagBits::eGraphics, {});
  p_data.compute_queue = find_queue(info.queue_families, vk::QueueFlagBits::eCompute, { p_data.graphics_queue });
  p_data.transfer_queue = find_queue(info.queue_families, vk::QueueFlagBits::eTransfer, { p_data.graphics_queue, p_data.compute_queue });
  assert(p_data.graphics_queue != UINT32_MAX);
  assert(p_data.compute_queue != UINT32_MAX);
  assert(p_data.transfer_queue != UINT32_MAX);
  p_data.present_queue = info.queue_family_index_surface_support;

  return p_data;
}

bool system_info::try_load_cached_data(VkInstance instance, physical_device_data* phys_data, cached_system_data* cached_data) {
  const auto directory_path = utils::cache_folder();
  if (!file_io::exists(directory_path)) return false;
  const auto file_path = directory_path + "main_device.json";
  if (!file_io::exists(file_path)) return false;

  cached_system_data cur_data;
  {
    const auto content = file_io::read(file_path);
    const auto err = utils::from_json(cur_data, content);
    if (err) {
      utils::warn("Could not load cached device data from file '{}'", file_path);
      return false;
    }
  }

  VkPhysicalDevice dev = VK_NULL_HANDLE;
  const auto devs = vk::Instance(instance).enumeratePhysicalDevices();
  for (const auto& h : devs) {
    const auto p = h.getProperties();
    if (p.deviceID == cur_data.device_id) dev = h;
  }

  if (dev == VK_NULL_HANDLE) {
    utils::warn("Could not find exact device cached in file '{}'", file_path);
    return false;
  }

  if (cached_data != nullptr) *cached_data = cur_data;

  physical_device_data d;
  d.handle = dev;
  for (const auto& feat : cur_data.features) {
    const auto id = device_features::from_string(feat);
    if (id >= device_features::count) {
      utils::warn("Could not parse device feature '{}'. Skip", feat);
      continue;
    }

    d.features.set(id, true);
  }

  d.desirable_present_mode = physical_device_present_mode::from_string(cur_data.desirable_present_mode);
  d.fallback_present_mode = physical_device_present_mode::from_string(cur_data.fallback_present_mode);
  d.graphics_queue = cur_data.graphics_queue;
  d.compute_queue = cur_data.compute_queue;
  d.transfer_queue = cur_data.transfer_queue;
  d.present_queue = cur_data.present_queue;

  if (phys_data != nullptr) *phys_data = d;

  return true;
}

void system_info::print_choosed_device(VkPhysicalDevice device) noexcept {
  const auto p = vk::PhysicalDevice(device).getProperties();
  const auto device_type = physical_device_type::to_string(static_cast<physical_device_type::values>(p.deviceType));
  utils::info("Using '{}' device '{}', driver version: {}", device_type, p.deviceName.data(), p.driverVersion);
}

void system_info::dump_cache_to_disk(VkPhysicalDevice dev, cached_system_data* cached_data) {
  cached_system_data data;

  for (const auto &info : devices) {
    if (dev != info.handle) continue;

    const auto [m1, m2] = find_present_modes(info.present_modes);

    data.device_name = info.name;
    data.device_type = physical_device_type::to_string(info.type);
    data.device_id = info.id;
    data.vendor_id = info.vendor_id;
    data.graphics_queue = find_queue(info.queue_families, vk::QueueFlagBits::eGraphics, {});
    data.compute_queue = find_queue(info.queue_families, vk::QueueFlagBits::eCompute, { data.graphics_queue });
    data.transfer_queue = find_queue(info.queue_families, vk::QueueFlagBits::eTransfer, { data.graphics_queue, data.compute_queue });
    assert(data.graphics_queue != UINT32_MAX);
    assert(data.compute_queue != UINT32_MAX);
    assert(data.transfer_queue != UINT32_MAX);
    data.present_queue = info.queue_family_index_surface_support;
    data.desirable_present_mode = physical_device_present_mode::to_string(m1);
    data.fallback_present_mode = physical_device_present_mode::to_string(m2);
    data.memory_capacity = info.memory;
    for (uint32_t i = 0; i < device_features::count; ++i) {
      const auto& feat = device_features::to_string(static_cast<device_features::values>(i));
      data.features.emplace_back(std::string(feat));
    }

    std::transform(data.device_type.begin(), data.device_type.end(), data.device_type.begin(), [] (const char c) { return std::tolower(c); });
    std::transform(data.desirable_present_mode.begin(), data.desirable_present_mode.end(), data.desirable_present_mode.begin(), [] (const char c) { return std::tolower(c); });
    std::transform(data.fallback_present_mode.begin(), data.fallback_present_mode.end(), data.fallback_present_mode.begin(), [] (const char c) { return std::tolower(c); });
  }

  if (data.device_name.empty()) utils::error{}("Trying to dump data for invalid physical device");

  const auto directory_path = utils::cache_folder();
  if (!file_io::exists(directory_path)) file_io::create_directory(directory_path);

  const auto file_path = directory_path + "main_device.json";
  const auto json = utils::to_json<glz::opts{ .prettify = true, .indentation_width = 2 }>(data);
  const bool res = file_io::write(json.value(), file_path);
  if (!res) utils::warn("Could not write file '{}'", file_path);

  if (cached_data != nullptr) *cached_data = data;
}
}
}