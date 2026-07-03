#include "graphics_base.h"

#include "vulkan_header.h"
#include "makers.h"
#include "auxiliary.h"

#include "devils_engine/utils/fileio.h"
#include "devils_engine/demiurg/resource_system.h"
#include "pipeline_cache_resource.h"

#include <cmath>
#include <limits>

namespace devils_engine {
namespace painter {

// resource::usage_mask хранит Vulkan usage как uint32_t (сюда кастятся vk::Buffer/ImageUsageFlags
// в create_resources). Ловим момент, когда базовые usage-флаги перестанут влезать в 32 бита
// (напр. переход на VkImageUsageFlagBits2 — 64-битный): тогда usage_mask нужно расширять.
static_assert(sizeof(VkImageUsageFlags) == 4, "VkImageUsageFlags больше 32 бит — расширить resource::usage_mask");
static_assert(sizeof(VkBufferUsageFlags) == 4, "VkBufferUsageFlags больше 32 бит — расширить resource::usage_mask");

static size_t buffer_suballocation_alignment(const VkPhysicalDevice physical_device, const uint32_t usage_mask) {
  if (physical_device == VK_NULL_HANDLE) return 1;

  const auto props = vk::PhysicalDevice(physical_device).getProperties();
  size_t alignment = 1;

  const auto has_usage = [usage_mask](const VkBufferUsageFlags bit) noexcept {
    return (usage_mask & uint32_t(bit)) != 0;
  };

  if (has_usage(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
    alignment = std::max(alignment, size_t(props.limits.minUniformBufferOffsetAlignment));
  }
  if (has_usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
    alignment = std::max(alignment, size_t(props.limits.minStorageBufferOffsetAlignment));
  }
  if (has_usage(VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT) || has_usage(VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) {
    alignment = std::max(alignment, size_t(props.limits.minTexelBufferOffsetAlignment));
  }

  return std::max<size_t>(alignment, 1);
}

graphics_base::graphics_base(VkInstance instance, VkDevice device, VkPhysicalDevice physical_device, enum presentation_engine_type presentation_engine_type) noexcept :
  instance(instance),
  device(device),
  physical_device(physical_device),
  surface(VK_NULL_HANDLE),
  cache(VK_NULL_HANDLE),
  graphics(VK_NULL_HANDLE),
  transfer(VK_NULL_HANDLE),
  command_pool(VK_NULL_HANDLE),
  descriptor_pool(VK_NULL_HANDLE),
  swapchain(VK_NULL_HANDLE),
  allocator(VK_NULL_HANDLE),
  presentation_engine_type(presentation_engine_type),
  current_presentable_state(static_cast<uint32_t>(vk::Result::eSuccess)),
  swapchain_slot(INVALID_RESOURCE_SLOT),
  swapchain_counter_index(INVALID_RESOURCE_SLOT),
  per_frame_counter_index(INVALID_RESOURCE_SLOT),
  per_update_counter_index(INVALID_RESOURCE_SLOT),
  frames_in_flight_constant_value_index(INVALID_RESOURCE_SLOT),
  current_render_graph_index(INVALID_RESOURCE_SLOT),
  swapchain_image_semaphore(INVALID_RESOURCE_SLOT),
  finish_rendering_semaphore(INVALID_RESOURCE_SLOT),
  computed_current_frame_index(INVALID_RESOURCE_SLOT),
  swapchain_image_size(std::make_tuple(640, 480))
{}

graphics_base::~graphics_base() noexcept {
  if (device == VK_NULL_HANDLE) return;

  vk::Device dev(device);
  //dev.waitIdle();

  wait_all_fences();

  clear_render_graph();
  clear_descriptors();
  if (allocator != VK_NULL_HANDLE) {
    clear_resources();
    vma::Allocator(allocator).destroy();
    allocator = VK_NULL_HANDLE;
  }
  clear_semaphores();

  for (auto& f : fences) { dev.destroy(f); }
  if (command_pool != VK_NULL_HANDLE) dev.destroy(command_pool);
  if (descriptor_pool != VK_NULL_HANDLE) dev.destroy(descriptor_pool);
  if (swapchain != VK_NULL_HANDLE) dev.destroy(swapchain);
  if (cache != VK_NULL_HANDLE) dev.destroy(cache);
}

void graphics_base::create_allocator(const size_t preferred_heap_block) {
  vma::VulkanFunctions fns = make_functions();

  // аллокатор
  vma::AllocatorCreateInfo aci{};
  aci.instance = instance;
  aci.physicalDevice = physical_device;
  aci.device = device;
  aci.vulkanApiVersion = VK_API_VERSION_1_0;
  aci.pVulkanFunctions = &fns;
  aci.preferredLargeHeapBlockSize = preferred_heap_block;

  allocator = vma::createAllocator(aci);
}

void graphics_base::create_command_pool(const uint32_t queue_family_index, VkQueue graphics) {
  this->graphics = graphics;

  vk::CommandPoolCreateInfo cpci{};
  cpci.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  cpci.queueFamilyIndex = queue_family_index;
  command_pool = vk::Device(device).createCommandPool(cpci);
  set_name(device, vk::CommandPool(command_pool), "graphics_base.command_pool");
}

void graphics_base::create_descriptor_pool() {
  // их наверное можно посчитать
  const vk::DescriptorPoolSize sizes[] = {
    vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, 256),
    vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 256),
    vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, 256),
    vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 256),
    vk::DescriptorPoolSize(vk::DescriptorType::eInputAttachment, 256),
    vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 256),
    vk::DescriptorPoolSize(vk::DescriptorType::eSampler, 256),
    vk::DescriptorPoolSize(vk::DescriptorType::eStorageTexelBuffer, 256),
    vk::DescriptorPoolSize(vk::DescriptorType::eUniformTexelBuffer, 256),
  };

  vk::DescriptorPoolCreateInfo dpci{};
  dpci.maxSets = 256;
  dpci.poolSizeCount = sizeof(sizes) / sizeof(sizes[0]);
  dpci.pPoolSizes = sizes;

  descriptor_pool = vk::Device(device).createDescriptorPool(dpci);
  set_name(device, vk::DescriptorPool(descriptor_pool), "graphics_base.descriptor_pool");
}

void graphics_base::get_or_create_pipeline_cache(const std::string& path) {
  std::vector<uint8_t> initial_mem;
  if (file_io::exists(path)) {
    initial_mem = file_io::read<uint8_t>(path);
  }

  vk::PipelineCacheCreateInfo pcci{};
  pcci.initialDataSize = initial_mem.size();
  pcci.pInitialData = initial_mem.data();
  
  cache = vk::Device(device).createPipelineCache(pcci);
}

void graphics_base::get_or_create_pipeline_cache(const demiurg::resource_system* reg, const std::string& id) {
  vk::PipelineCacheCreateInfo pcci{};

  const auto* res = reg != nullptr ? reg->get<pipeline_cache_resource>(id) : nullptr;
  if (res != nullptr && !res->memory.empty()) {
    pcci.initialDataSize = res->memory.size();
    pcci.pInitialData = res->memory.data();
    utils::info("graphics_base: pipeline cache seeded from resource '{}' ({} bytes)", id, res->memory.size());
  } else {
    utils::info("graphics_base: pipeline cache resource '{}' not present, creating empty", id);
  }

  cache = vk::Device(device).createPipelineCache(pcci);
}

void graphics_base::dump_cache_on_disk(const std::string& path) const {
  if (cache == VK_NULL_HANDLE) return;

  const auto data = vk::Device(device).getPipelineCacheData(cache);
  const bool res = file_io::write(data, path);
  if (!res) utils::warn("Could not write pipeline cache '{}' on disk", path);
}

void graphics_base::set_surface(VkSurfaceKHR surface, const uint32_t width, const uint32_t height) {
  this->surface = surface;
  // когда мы вызываем эту команду? думаю что ее нужно просто переделать в set_surface
  this->swapchain_image_size = std::make_tuple(width, height);
}

static double mix(const double x, const double y, const double a) {
  return x * (1.0 - a) + y * a;
}

static uint16_t float2half_rn(float a) {
  uint32_t ia = std::bit_cast<uint32_t>(a);
  uint16_t ir = (ia >> 16) & 0x8000;

  if ((ia & 0x7f800000) == 0x7f800000) {
    if ((ia & 0x7fffffff) == 0x7f800000) {
      ir |= 0x7c00; /* infinity */
    } else {
      ir |= 0x7e00 | ((ia >> (24 - 11)) & 0x1ff); /* NaN, quietened */
    }
  } else if ((ia & 0x7f800000) >= 0x33000000) {
    int shift = (int)((ia >> 23) & 0xff) - 127;
    if (shift > 15) {
      ir |= 0x7c00; /* infinity */
    } else {
      ia = (ia & 0x007fffff) | 0x00800000; /* extract mantissa */
      if (shift < -14) { /* denormal */  
        ir |= ia >> (-1 - shift);
        ia = ia << (32 - (-1 - shift));
      } else { /* normal */
        ir |= ia >> (24 - 11);
        ia = ia << (32 - (24 - 11));
        ir = ir + ((14 + shift) << 10);
      }
      /* IEEE-754 round to nearest of even */
      if ((ia > 0x80000000) || ((ia == 0x80000000) && (ir & 1))) {
        ir++;
      }
    }
  }
  return ir;
}

static std::tuple<size_t, size_t> populate_dispatch3(const std::span<uint32_t>& memory, const std::span<const double>& values) {
  memory[0] = 0 < values.size() ? uint32_t(values[0]) : 0u;
  memory[1] = 1 < values.size() ? uint32_t(values[1]) : 0u;
  memory[2] = 2 < values.size() ? uint32_t(values[2]) : 0u;
  return std::make_tuple(3 * sizeof(uint32_t),3);
}

static std::tuple<size_t, size_t> populate_draw4(const std::span<uint32_t>& memory, const std::span<const double>& values) {
  memory[0] = 0 < values.size() ? uint32_t(values[0]) : 0u;
  memory[1] = 1 < values.size() ? uint32_t(values[1]) : 0u;
  memory[2] = 2 < values.size() ? uint32_t(values[2]) : 0u;
  memory[3] = 3 < values.size() ? uint32_t(values[3]) : 0u;
  return std::make_tuple(4 * sizeof(uint32_t), 4);
}

static std::tuple<size_t, size_t> populate_indexed5(const std::span<uint32_t>& memory, const std::span<const double>& values) {
  memory[0] = 0 < values.size() ? uint32_t(values[0]) : 0u;
  memory[1] = 1 < values.size() ? uint32_t(values[1]) : 0u;
  memory[2] = 2 < values.size() ? uint32_t(values[2]) : 0u;
  memory[3] = 3 < values.size() ? std::bit_cast<uint32_t>(int32_t(values[3])) : 0u;
  memory[4] = 4 < values.size() ? uint32_t(values[4]) : 0u;
  return std::make_tuple(5 * sizeof(uint32_t), 5);
}

static std::tuple<size_t, size_t> populate_d32s8(const std::span<uint32_t>& memory, const std::span<const double>& values) {
  const float depth = 0 < values.size() ? float(values[0]) : 0.0f;
  const uint32_t stencil = 1 < values.size() ? uint32_t(values[1]) : 0u;
  memory[0] = std::bit_cast<uint32_t>(depth);
  memory[1] = stencil;
  return std::make_tuple(2 * sizeof(uint32_t),2);
}

static std::tuple<size_t, size_t> populate_d24s8(const std::span<uint32_t>& memory, const std::span<const double>& values) {
  const float depth = 0 < values.size() ? float(values[0]) : 0.0f;
  const uint32_t stencil = 1 < values.size() ? uint32_t(values[1]) : 0u;
  const uint32_t depth24 = uint32_t(depth * 16777215.0f); // clamp [0,1]
  const uint32_t stencil8 = stencil & 0xFF;
  memory[0] = (depth24 << 0) | (stencil8 << 24);
  return std::make_tuple(1 * sizeof(uint32_t), 2);
}

static std::tuple<size_t, size_t> populate_d16s8(const std::span<uint32_t>& memory, const std::span<const double>& values) {
  const double depth = 0 < values.size() ? double(values[0]) : 0.0;
  const uint32_t stencil = 1 < values.size() ? uint32_t(values[1]) : 0u;
  const uint32_t depth16 = uint32_t(depth * double(UINT16_MAX)); // clamp [0,1]
  const uint32_t stencil8 = stencil & 0xFF;
  memory[0] = (depth16 << 0) | (stencil8 << 16);
  return std::make_tuple(1 * sizeof(uint32_t), 2);
}

static std::tuple<size_t, size_t> put_values(const std::span<uint32_t> &memory, const std::span<const format::values> &layout, const std::span<const double>& values) {
  size_t offset = 0;
  size_t counter = 0;
  auto cur_memory = memory;
  for (const auto fmt : layout) {
    if (counter >= values.size()) break;

    cur_memory = std::span(memory.begin() + (offset/sizeof(uint32_t)), memory.end());

    if (fmt == format::pad1) {
      offset += 1 * sizeof(uint32_t);
      //counter += 1; // ?
      continue;
    }

    if (fmt == format::pad2) {
      offset += 2 * sizeof(uint32_t);
      //counter += 2; // ?
      continue;
    }

    if (fmt == format::pad3) {
      offset += 3 * sizeof(uint32_t);
      //counter += 3; // ?
      continue;
    }

    if (fmt == format::dispatch3) {
      const auto [add_offset, add_counter] = populate_dispatch3(cur_memory, values);
      offset += add_offset;
      counter += add_counter;
      continue;
    }

    if (fmt == format::draw4) {
      const auto [add_offset, add_counter] = populate_draw4(cur_memory, values);
      offset += add_offset;
      counter += add_counter;
      continue;
    }

    if (fmt == format::indexed5) {
      const auto [add_offset, add_counter] = populate_indexed5(cur_memory, values);
      offset += add_offset;
      counter += add_counter;
      continue;
    }

    const uint32_t vk_fmt = format::to_vulkan_format(fmt);

    if (vk_fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) {
      const auto [add_offset, add_counter] = populate_d32s8(cur_memory, values);
      offset += add_offset;
      counter += add_counter;
      continue;
    }

    if (vk_fmt == VK_FORMAT_D24_UNORM_S8_UINT) {
      const auto [add_offset, add_counter] = populate_d24s8(cur_memory, values);
      offset += add_offset;
      counter += add_counter;
      continue;
    }

    if (vk_fmt == VK_FORMAT_D16_UNORM_S8_UINT) {
      const auto [add_offset, add_counter] = populate_d16s8(cur_memory, values);
      offset += add_offset;
      counter += add_counter;
      continue;
    }

    const uint32_t channels = format_channel_count(vk_fmt);
    const uint32_t elem_size = format_element_size(vk_fmt, format::to_vulkan_aspect(fmt));
    // alignof(vec4) == 16, alignof(vec3) == 16, alignof(vec2) == 8, alignof(vec1) == 4
    //const uint32_t final_elem_size = elem_size == 12 ? 16 : std::max(elem_size, 4u);
    //const uint32_t local_aligment = (elem_size / channels) < 4 ? 1 : final_elem_size;
      
    // тут укладываем байт за байтом придерживаясь алигмента
    // с1с1с1с1 - должен попасть в отдельный uint ... нет для простоты пусть минимум будет 4 байта, то есть тут 4 uint =( 
    // а с4с1с4с1 - попадут в 4 отдельных uint
    // а с4с1с2с1 - это 2 uint....

    // вообще что делать с форматами DEPTH & STENCIL?
    // как минимум я должен распаковать clear value

    constexpr auto min8 = std::numeric_limits<int8_t>::min();
    constexpr auto max8 = std::numeric_limits<int8_t>::max();
    constexpr auto min16 = std::numeric_limits<int16_t>::min();
    constexpr auto max16 = std::numeric_limits<int16_t>::max();

    if (elem_size / channels == sizeof(uint8_t)) {
      const auto el_type = format::element_type(fmt);
      switch (el_type) {
        case format_element_type::UNORM: {
          auto ptr = reinterpret_cast<uint8_t*>(&cur_memory[0]);
          for (uint32_t i = 0; i < channels && counter < values.size(); ++i, ++counter) {
            ptr[i] = uint8_t(std::round(double(UINT8_MAX) * std::clamp(values[counter], 0.0, 1.0)));
          }
          break;
        }
        case format_element_type::SNORM: {
          auto ptr = reinterpret_cast<int8_t*>(&cur_memory[0]);
          for (uint32_t i = 0; i < channels && counter < values.size(); ++i, ++counter) {
            double tmp = (std::clamp(values[counter], -1.0, 1.0) + 1.0) / 2.0;
            double val = mix(min8, max8, tmp);
            ptr[i] = int8_t(val);
          }
          break;
        }
        case format_element_type::UINT: {
          auto ptr = reinterpret_cast<uint8_t*>(&cur_memory[0]);
          for (uint32_t i = 0; i < channels && counter < values.size(); ++i, ++counter) {
            ptr[i] = uint8_t(values[counter]);
          }
          break;
        }
        case format_element_type::SINT: {
          auto ptr = reinterpret_cast<int8_t*>(&cur_memory[0]);
          for (uint32_t i = 0; i < channels && counter < values.size(); ++i, ++counter) {
            ptr[i] = int8_t(values[counter]);
          }
          break;
        }
        default: utils::error{}("Invalid format element type {} for {} byte element", static_cast<uint32_t>(el_type), elem_size / channels);
      }

      offset += 1 * sizeof(uint32_t);
      continue;
    }

    if (elem_size / channels == sizeof(uint16_t)) {
      const auto el_type = format::element_type(fmt);
      const uint32_t offset_count = (channels + 1) / 2;
      switch (el_type) {
        case format_element_type::UNORM: {
          uint32_t i = 0;
          for (uint32_t add = 0; add < offset_count; ++add) {
            for (; i < channels && counter < values.size(); ++i, ++counter) {
              auto ptr = reinterpret_cast<uint16_t*>(&cur_memory[add]);
              assert((i - 2 * add) < 2);
              ptr[(i - 2 * add)] = uint16_t(std::round(double(UINT16_MAX) * std::clamp(values[counter], 0.0, 1.0)));
            }
          }
          break;
        }
        case format_element_type::SNORM: {
          uint32_t i = 0;
          for (uint32_t add = 0; add < offset_count; ++add) {
            for (; i < channels && counter < values.size(); ++i, ++counter) {
              auto ptr = reinterpret_cast<int16_t*>(&cur_memory[add]);
              double tmp = (std::clamp(values[counter], -1.0, 1.0) + 1.0) / 2.0;
              double val = mix(min16, max16, tmp);
              assert((i - 2 * add) < 2);
              ptr[(i - 2 * add)] = int16_t(val);
            }
          }
          break;
        }
        case format_element_type::UINT: {
          uint32_t i = 0;
          for (uint32_t add = 0; add < offset_count; ++add) {
            for (; i < channels && counter < values.size(); ++i, ++counter) {
              auto ptr = reinterpret_cast<uint16_t*>(&cur_memory[add]);
              assert((i - 2 * add) < 2);
              ptr[(i - 2 * add)] = uint16_t(values[counter]);
            }
          }
          break;
        }
        case format_element_type::SINT: {
          uint32_t i = 0;
          for (uint32_t add = 0; add < offset_count; ++add) {
            for (; i < channels && counter < values.size(); ++i, ++counter) {
              auto ptr = reinterpret_cast<int16_t*>(&cur_memory[add]);
              assert((i - 2 * add) < 2);
              ptr[(i - 2 * add)] = int16_t(values[counter]);
            }
          }
          break;
        }
        case format_element_type::SFLOAT: {
          uint32_t i = 0;
          for (uint32_t add = 0; add < offset_count; ++add) {
            for (; i < channels && counter < values.size(); ++i, ++counter) {
              auto ptr = reinterpret_cast<uint16_t*>(&cur_memory[add]);
              assert((i - 2 * add) < 2);
              ptr[(i - 2 * add)] = float2half_rn(values[counter]);
            }
          }
          break;
        }
        default: utils::error{}("Invalid format element type {} for {} byte element", static_cast<uint32_t>(el_type), elem_size / channels);
      }

      offset += offset_count * sizeof(uint32_t);
      continue;
    }

    if (elem_size / channels == sizeof(uint32_t)) {
      const auto el_type = format::element_type(fmt);
      switch (el_type) {
        case format_element_type::UINT: {
          for (uint32_t i = 0, add = 0; i < channels && counter < values.size(); ++i, ++counter, ++add) {
            auto ptr = reinterpret_cast<uint32_t*>(&cur_memory[add]);
            *ptr = uint32_t(values[counter]);
          }
          break;
        }
        case format_element_type::SINT: {
          for (uint32_t i = 0, add = 0; i < channels && counter < values.size(); ++i, ++counter, ++add) {
            auto ptr = reinterpret_cast<int32_t*>(&cur_memory[add]);
            *ptr = int32_t(values[counter]);
          }
          break;
        }
        case format_element_type::SFLOAT: {
          for (uint32_t i = 0, add = 0; i < channels && counter < values.size(); ++i, ++counter, ++add) {
            auto ptr = reinterpret_cast<float*>(&cur_memory[add]);
            *ptr = float(values[counter]);
          }
          break;
        }
        default: utils::error{}("Invalid format element type {} for {} byte element", static_cast<uint32_t>(el_type), elem_size / channels);
      }

      offset += channels * sizeof(uint32_t);  
      continue;
    }

    utils::error{}("Bad format? '{}'", format::to_string(fmt));
  }

  return std::make_tuple(offset, counter);
}

// вроде бы почти все сделал, нужно добавить тип pad
// нужно распределить на отдельные функции это дело
// + нужно сделать общую функцию где я принимаю на вход std::span<double> 
// и запихиваю правильные данные в память как я это делаю здесь
void graphics_base::populate_constant_default_values() {
  for (const auto& c : constants) {
    auto spn = std::span(constants_memory[1].begin() + (c.offset/sizeof(uint32_t)), constants_memory[1].end());
    put_values(spn, c.layout, c.value);
  }
}

buffer_frame graphics_base::get_current_buffer_resource_frame(const uint32_t res_index, const uint32_t counter_offset) const {
  if (res_index >= resources.size()) return buffer_frame();
  const auto& res = resources[res_index];
  if (role::is_image(res.role)) return buffer_frame();

  const auto& counter = DS_ASSERT_ARRAY_GET(counters, res.swap);
  const uint32_t cur_index = counter.get_value();
  const uint32_t buffering = res.compute_buffering(this);
  const auto& frame = res.handles[(cur_index+counter_offset) % buffering];

  const auto& res_cont = DS_ASSERT_ARRAY_GET(resource_containers, frame.index);

  buffer_frame bf;
  bf.name = res.name;
  bf.format = res.format;
  bf.role = res.role;
  bf.stride = res.size_hint;
  bf.sub = frame.subbuffer;
  bf.handle = std::bit_cast<VkBuffer>(res_cont.handle);
  bf.mapped = res_cont.mem_ptr; // first byte of VkBuffer

  return bf;
}

image_frame graphics_base::get_current_image_resource_frame(const uint32_t res_index, const uint32_t counter_offset) const {
  if (res_index >= resources.size()) return image_frame();
  const auto& res = resources[res_index];
  if (!role::is_image(res.role)) return image_frame();

  const auto& counter = DS_ASSERT_ARRAY_GET(counters, res.swap);
  const uint32_t cur_index = counter.get_value();
  const uint32_t buffering = res.compute_buffering(this);
  const auto& frame = res.handles[(cur_index+counter_offset) % buffering];

  VkImage img = VK_NULL_HANDLE;
  void* mem_ptr = nullptr;
  if (res.role == role::present) {
    img = swapchain_images[cur_index % buffering];
  } else {
    const auto& res_cont = DS_ASSERT_ARRAY_GET(resource_containers, frame.index);
    img = std::bit_cast<VkImage>(res_cont.handle);
    mem_ptr = res_cont.mem_ptr;
  }

  image_frame ifr;
  ifr.name = res.name;
  ifr.format = res.format;
  ifr.role = res.role;
  ifr.vk_format = res.format_hint;
  ifr.sub = frame.subimage;
  ifr.view = frame.view;
  ifr.handle = img;
  ifr.mapped = mem_ptr; // first byte of VkImage
  return ifr;
}

buffer_frame graphics_base::get_current_instance_resource_frame(const uint32_t pair_index, const uint32_t counter_offset) const {
  if (pair_index >= pairs.size()) return buffer_frame();

  const auto& pair = pairs[pair_index];
  const auto& dg = DS_ASSERT_ARRAY_GET(draw_groups, pair.draw_group);
  auto fr = get_current_buffer_resource_frame(dg.instances_buffer, counter_offset);
  fr.sub.offset = fr.sub.offset + pair.instance_offset;
  fr.sub.size = size_t(pair.max_size) * size_t(fr.stride);

  return fr;
}

buffer_frame graphics_base::get_current_indirect_resource_frame(const uint32_t pair_index, const uint32_t counter_offset) const {
  if (pair_index >= pairs.size()) return buffer_frame();

  const auto& pair = pairs[pair_index];
  const auto& dg = DS_ASSERT_ARRAY_GET(draw_groups, pair.draw_group);
  auto fr = get_current_buffer_resource_frame(dg.indirect_buffer, counter_offset);
  fr.sub.offset = fr.sub.offset + pair.indirect_offset;
  fr.sub.size = fr.stride;

  return fr;
}

bool graphics_base::wait_all_fences(const size_t timeout) const {
  const auto res = vk::Device(device).waitForFences(fences.size(), reinterpret_cast<const vk::Fence*>(fences.data()), true, timeout);
  switch (res) { 
    case vk::Result::eErrorDeviceLost: 
    case vk::Result::eErrorOutOfDeviceMemory: 
    case vk::Result::eErrorOutOfHostMemory: 
    case vk::Result::eErrorUnknown: 
    case vk::Result::eErrorValidationFailedEXT: utils::warn("'vkWaitForFences' returns '{}' in 'graphics_base'", vk::to_string(res)); break;
    default: break;
  }

  return res == vk::Result::eSuccess;
}

// ?
void graphics_base::resize_viewport(const uint32_t width, const uint32_t height) {
  swapchain_image_size = std::make_tuple(width, height);
  recreate_swapchain(width, height);
  recreate_screensize_resources(width, height);
  execution_graph.resize_viewport(this, width, height);
  // все?
}

void graphics_base::recreate_pipelines() {
  execution_graph.recreate_pipeline(this);
}

// ?
void graphics_base::recreate_render_graph() {

}

void graphics_base::clear_render_graph() {
  execution_graph.clear();
}

void graphics_base::clear_resources() {
  vk::Device dev(device);
  vma::Allocator alc(allocator);

  for (auto& res : resources) {
    if (!role::is_image(res.role)) continue;

    const uint32_t buffering = res.compute_buffering(this);
    for (uint32_t i = 0; i < buffering; ++i) {
      dev.destroy(res.handles[i].view);
    }
  }

  for (auto& res_c : resource_containers) {
    if (res_c.mem_ptr != nullptr) alc.unmapMemory(res_c.alloc);

    if (res_c.is_image()) {
      alc.destroyImage(std::bit_cast<VkImage>(res_c.handle), res_c.alloc);
    } else {
      alc.destroyBuffer(std::bit_cast<VkBuffer>(res_c.handle), res_c.alloc);
    }
  }

  resources.clear();
  resource_containers.clear();
}

void graphics_base::clear_semaphores() {
  vk::Device dev(device);

  // Уничтожаем ВЕСЬ массив хэндлов, а не frames_in_flight: present-wait семафор (finish_rendering)
  // создаётся на все MAX_FRAMES_IN_FLIGHT слотов (индексация по образу свопчейна), и обрезка по
  // frames_in_flight оставляла бы хвост неосвобождённым (VUID-vkDestroyDevice). destroy(null) безопасен.
  for (auto& sem : semaphores) {
    for (auto& s : sem.handles) {
      dev.destroy(s);
      s = VK_NULL_HANDLE;
    }
  }

  semaphores.clear();
}

void graphics_base::clear_descriptors() {
  vk::Device dev(device);

  for (auto& set : descriptors) {
    dev.destroy(set.setlayout);
  }

  for (auto& s : samplers) {
    if (s.handle != VK_NULL_HANDLE) dev.destroy(vk::Sampler(s.handle));
    s.handle = VK_NULL_HANDLE;
  }

  descriptors.clear();
  samplers.clear();
}

void graphics_base::clear() {
  clear_render_graph();
  clear_resources();
  clear_descriptors();
}

void graphics_base::recreate_screensize_resources(const uint32_t, const uint32_t) {
  // какой алгоритм? в общем то все связи остаются какими есть
  // пробегаем все ресурсы у которых размер зависит, помечаем контейнеры
  // пересоздаем контейнеры и пересоздаем views

  vk::Device dev(device);
  vma::Allocator alc(allocator);

  std::vector<uint32_t> indices;
  indices.reserve(resource_containers.size());

  for (auto& res : resources) {
    if (res.role == role::present) continue;
    const auto& cv = DS_ASSERT_ARRAY_GET(constant_values, res.size);
    if (cv.type != value_type::screensize) continue;

    const bool is_image = role::is_image(res.role);

    const auto [size, exts] = res.compute_frame_size(this);
    const uint32_t buffering = res.compute_buffering(this);
    for (uint32_t i = 0; i < buffering; ++i) {
      if (is_image) dev.destroy(res.handles[i].view);
      
      const auto itr = std::find(indices.begin(), indices.end(), res.handles[i].index);
      if (itr == indices.end()) {
        indices.push_back(res.handles[i].index);
        // желательно слои прикрутить к контейнерам буферов
        // определять что картинка или нет по другим метрикам (например по мип уровням)
        auto& cont = DS_ASSERT_ARRAY_GET(resource_containers, res.handles[i].index);
        cont.size = size * buffering;
        cont.extent = { std::get<0>(exts), std::get<1>(exts) };
      }
    }
  }

  for (uint32_t i = 0; i < indices.size(); ++i) {
    auto& cont = DS_ASSERT_ARRAY_GET(resource_containers, indices[i]);
    const bool host_res = cont.host_visible();

    if (cont.is_image()) {
      alc.destroyImage(std::bit_cast<VkImage>(cont.handle), cont.alloc);
    } else {
      alc.destroyBuffer(std::bit_cast<VkBuffer>(cont.handle), cont.alloc);
    }

    cont.create_container(allocator, host_res);
  }

  for (auto& res : resources) {
    if (res.role == role::present) continue;
    const auto& cv = DS_ASSERT_ARRAY_GET(constant_values, res.size);
    if (cv.type != value_type::screensize) continue;

    const bool is_image = role::is_image(res.role);

    const uint32_t buffering = res.compute_buffering(this);
    for (uint32_t i = 0; i < buffering; ++i) {
      const auto& cont = DS_ASSERT_ARRAY_GET(resource_containers, res.handles[i].index);

      if (is_image) {
        const auto img_handle = std::bit_cast<VkImage>(cont.handle);
        const auto fmt = static_cast<vk::Format>(res.format_hint);

        vk::ImageViewCreateInfo ivci{};
        ivci.image = img_handle;
        ivci.format = fmt;
        ivci.viewType = vk::ImageViewType::e2D;
        ivci.components = vk::ComponentMapping{};
        ivci.subresourceRange = std::bit_cast<vk::ImageSubresourceRange>(res.handles[i].subimage);

        res.handles[i].view = dev.createImageView(ivci);
        set_name(device, vk::ImageView(res.handles[i].view), res.name + ".view" + std::to_string(i));
      }
    }
  }
}

void graphics_base::create_fences() {
  vk::Device dev(device);
  for (auto& f : fences) { dev.destroy(f); }
  fences.clear();
  fences.resize(frames_in_flight(), VK_NULL_HANDLE);
  for (uint32_t i = 0; i < fences.size(); ++i) {
    auto& f = fences[i];
    f = dev.createFence(vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled)); 
    set_name(device, vk::Fence(f), "frame_fence_" + std::to_string(i)); 
  }
}

void graphics_base::create_global_semaphores() {
  vk::Device dev(device);
  const uint32_t count = frames_in_flight();

  {
    uint32_t sem_index = find_semaphore("swapchain_image");
    if (sem_index == INVALID_RESOURCE_SLOT) {
      sem_index = semaphores.size();
      semaphores.emplace_back().name = "swapchain_image";
    } else {
      auto& sem = DS_ASSERT_ARRAY_GET(semaphores, sem_index);
      for (auto& s : sem.handles) {
        dev.destroy(s);
        s = VK_NULL_HANDLE;
      }
    }

    auto& sem = DS_ASSERT_ARRAY_GET(semaphores, sem_index);
    for (uint32_t i = 0; i < count; ++i) {
      sem.handles[i] = dev.createSemaphore(vk::SemaphoreCreateInfo{});
    }

    swapchain_image_semaphore = sem_index;
  }

  {
    uint32_t sem_index = find_semaphore("finish_rendering");
    if (sem_index == INVALID_RESOURCE_SLOT) {
      sem_index = semaphores.size();
      semaphores.emplace_back().name = "finish_rendering";
    }
    else {
      auto& sem = DS_ASSERT_ARRAY_GET(semaphores, sem_index);
      for (auto& s : sem.handles) {
        dev.destroy(s);
        s = VK_NULL_HANDLE;
      }
    }

    // finish_rendering — present-wait семафор: его держит vkQueuePresentKHR до показа образа,
    // поэтому он индексируется по ИНДЕКСУ ОБРАЗА свопчейна (см. submit_frame), а не по frame-in-flight.
    // Число образов на момент вызова ещё неизвестно (свопчейн создаётся позже), а движок и так
    // ограничивает образы MAX_FRAMES_IN_FLIGHT — поэтому создаём хэндлы на все слоты.
    auto& sem = DS_ASSERT_ARRAY_GET(semaphores, sem_index);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
      sem.handles[i] = dev.createSemaphore(vk::SemaphoreCreateInfo{});
    }

    finish_rendering_semaphore = sem_index;
  }
}

void graphics_base::draw() {
  
}

// ожидание фрейма лучше отдельно сделать
void graphics_base::prepare_frame() {
  update_frame();
  update_counters();
  computed_current_frame_index = current_frame_in_flight();
  wait_fence();
  image_acquire();
  //update_constant_memory(); // слишком часто
  update_descriptors(); // тут перевыделения памяти, лучше убрать отсюда
  
  //if (!can_draw()) return;

  /*auto f = fences[computed_current_frame_index];
  const size_t sec1 = size_t(1000) * size_t(1000) * size_t(1000);
  const auto res = vk::Device(device).waitForFences(vk::Fence(f), true, sec1);
  if (res != vk::Result::eSuccess) utils::error{}("waitForFences failed for current frame {}", current_frame_index());
  vk::Device(device).resetFences(vk::Fence(f));*/

  // ???
}

void graphics_base::submit_frame() {
  if (!can_draw()) return;

  //const uint32_t frames_count = frames_in_flight();
  //const uint32_t cur_frame = current_frame_index();
  auto f = fences[computed_current_frame_index];

  // present-wait семафор индексируем по ИНДЕКСУ ОБРАЗА свопчейна (из acquireNextImage), а не по
  // frame-in-flight: его держит pending-present до показа образа, и он свободен ровно тогда, когда
  // этот образ снова заacquire'ен. Иначе пере-сигналим занятый презентом семафор (VU про "in use
  // by swapchain"). acquire-семафор остаётся per-frame — это другой семафор и к образу не привязан.
  const uint32_t current_index =
    (presentation_engine_type == presentation_engine_type::main)
      ? DS_ASSERT_ARRAY_GET(counters, swapchain_counter_index).get_value()
      : 0u;

  VkSemaphore finish_rendering = VK_NULL_HANDLE;
  if (presentation_engine_type == presentation_engine_type::main) {
    const auto& sem = DS_ASSERT_ARRAY_GET(semaphores, finish_rendering_semaphore);
    finish_rendering = sem.handles[current_index];
  }

  execution_graph.submit(this, graphics, finish_rendering, f);

  if (presentation_engine_type != presentation_engine_type::main) return;

  const auto handle = vk::Semaphore(finish_rendering);
  vk::SwapchainKHR sw(swapchain);

  vk::PresentInfoKHR pi{};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &handle;
  pi.swapchainCount = 1;
  pi.pSwapchains = &sw;
  pi.pImageIndices = &current_index;
  const auto res = vk::Queue(graphics).presentKHR(pi);
  current_presentable_state = static_cast<uint32_t>(res);

  switch (res) {
    case vk::Result::eSuccess: [[fallthrough]];
    case vk::Result::eSuboptimalKHR: [[fallthrough]];
    case vk::Result::eErrorOutOfDateKHR: [[fallthrough]];
    case vk::Result::eErrorSurfaceLostKHR: break;
    default: utils::error{}("Could not acquire next swapchain image, got: {}", vk::to_string(res));
  }
}

void graphics_base::update_frame() {
  inc_counter(per_frame_counter_index);
}

void graphics_base::update_event() {
  update_constant_memory();
  inc_counter(per_update_counter_index);
}

bool graphics_base::can_draw() const {
  return presentable_state_stable() || presentable_state_suboptimal();
}

void graphics_base::inc_counter(const uint32_t slot) {
  auto& counter = DS_ASSERT_ARRAY_GET(counters, slot);
  counter.inc_next_value();
}

void graphics_base::update_counters() {
  for (auto& c : counters) {
    c.push_value();
  }
}

void graphics_base::image_acquire() {
  if (presentation_engine_type != presentation_engine_type::main) return;
  if (!can_draw()) return;

  if (surface == VK_NULL_HANDLE) {
    utils::error{}("No surface?");
  }

  const auto& sem = DS_ASSERT_ARRAY_GET(semaphores, swapchain_image_semaphore);
  const auto cur = sem.handles[computed_current_frame_index];
  //utils::info("Current image aqcuire semaphore {:p}", (const void*)cur);
  // прежний таймаут 1мс почти всегда падал в Timeout (vsync-кадр ~16мс). Берём щедрый, но конечный
  // (1с) — при норме acquire возвращается быстро, а при реальном дедлоке даст внятную ошибку, не hang.
  constexpr size_t acquire_timeout_ns = size_t(1000) * size_t(1000) * size_t(1000);
  const auto res = vk::Device(device).acquireNextImageKHR(swapchain, acquire_timeout_ns, cur);
  current_presentable_state = static_cast<uint32_t>(res.result);
  switch (res.result) {
    case vk::Result::eSuccess: [[fallthrough]];
    case vk::Result::eSuboptimalKHR: [[fallthrough]];
    case vk::Result::eErrorOutOfDateKHR: [[fallthrough]];
    case vk::Result::eErrorSurfaceLostKHR: break;
    default: utils::error{}("Could not acquire next swapchain image, got: {}", vk::to_string(res.result));
  }

  auto& counter = DS_ASSERT_ARRAY_GET(counters, swapchain_counter_index);
  counter.set_value(res.value);
  counter.push_value(); // ......
}

void graphics_base::update_constant_memory() {
  constants_memory[0] = constants_memory[1]; // копи
}

uint32_t graphics_base::compute_frame_index(const int32_t offset) const {
  return current_update_index() + offset;
}

uint32_t graphics_base::current_swapchain_image_index() const {
  const auto& c = DS_ASSERT_ARRAY_GET(counters, swapchain_counter_index);
  return c.get_value();
}

uint32_t graphics_base::current_frame_index() const {
  const auto& c = DS_ASSERT_ARRAY_GET(counters, per_frame_counter_index);
  return c.get_value();
}

uint32_t graphics_base::current_update_index() const {
  const auto& c = DS_ASSERT_ARRAY_GET(counters, per_update_counter_index);
  return c.get_value();
}

uint32_t graphics_base::frames_in_flight() const {
  if (frames_in_flight_constant_value_index == INVALID_RESOURCE_SLOT) return default_frames_in_flight;
  const auto& c_value = DS_ASSERT_ARRAY_GET(constant_values, frames_in_flight_constant_value_index);
  return c_value.reduce_value();
}

uint32_t graphics_base::swapchain_frames() const {
  return swapchain_images.size();
}

uint32_t graphics_base::current_frame_in_flight() const {
  return current_frame_index() % frames_in_flight();
}

std::tuple<uint32_t, uint32_t> graphics_base::swapchain_extent() const {
  return swapchain_image_size;
}

uint32_t graphics_base::find_constant_value(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < constant_values.size() && constant_values[i].name != name; ++i) {}
  return i >= constant_values.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::find_resource(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < resources.size() && resources[i].name != name; ++i) {}
  return i >= resources.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::find_counter(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < counters.size() && counters[i].name != name; ++i) {}
  return i >= counters.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::find_constant(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < constants.size() && constants[i].name != name; ++i) {}
  return i >= constants.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::find_render_target(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < render_targets.size() && render_targets[i].name != name; ++i) {}
  return i >= render_targets.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::find_descriptor(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < descriptors.size() && descriptors[i].name != name; ++i) {}
  return i >= descriptors.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::find_sampler(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < samplers.size() && samplers[i].name != name; ++i) {}
  return i >= samplers.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::find_material(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < materials.size() && materials[i].name != name; ++i) {}
  return i >= materials.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::find_geometry(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < geometries.size() && geometries[i].name != name; ++i) {}
  return i >= geometries.size() ? INVALID_RESOURCE_SLOT : i;
}

//uint32_t painter_base::find_geometry_instance(const std::string_view& name) const {
//  uint32_t i = 0;
//  for (; i < geometry_instances.size() && geometry_instances[i].name != name; ++i) {}
//  return i >= geometry_instances.size() ? INVALID_RESOURCE_SLOT : i;
//}

uint32_t graphics_base::find_draw_group(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < draw_groups.size() && draw_groups[i].name != name; ++i) {}
  return i >= draw_groups.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::find_execution_step(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < steps.size() && steps[i].name != name; ++i) {}
  return i >= steps.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::find_execution_pass(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < passes.size() && passes[i].name != name; ++i) {}
  return i >= passes.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::find_render_graph(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < graphs.size() && graphs[i].name != name; ++i) {}
  return i >= graphs.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::find_semaphore(const std::string_view& name) const {
  uint32_t i = 0;
  for (; i < semaphores.size() && semaphores[i].name != name; ++i) {}
  return i >= semaphores.size() ? INVALID_RESOURCE_SLOT : i;
}

uint32_t graphics_base::register_pair(const uint32_t draw_group, const uint32_t mesh, const uint32_t max_count) {
  auto& group = DS_ASSERT_ARRAY_GET(draw_groups, draw_group);

  const uint32_t check = find_pair(draw_group, mesh);
  if (check != INVALID_RESOURCE_SLOT) utils::error{}("Draw group '{}' already has pair with mesh {}", group.name, mesh);

  uint32_t index = pairs.size();
  mesh_draw_group_pair* cur_pair = nullptr;
  const auto itr = std::find_if(pairs.begin(), pairs.end(), [](const auto& pair) { return pair.draw_group == INVALID_RESOURCE_SLOT && pair.mesh == INVALID_RESOURCE_SLOT; });
  if (itr != pairs.end()) {
    cur_pair = &(*itr);
    index = std::distance(pairs.begin(), itr);
  } else {
    pairs.emplace_back();
    cur_pair = &pairs.back();
  }

  cur_pair->draw_group_name = group.name;

  cur_pair->draw_group = draw_group;
  cur_pair->mesh = mesh;
  cur_pair->max_size = max_count;

  if (group.pairs.empty()) {
    cur_pair->instance_offset = 0;
    cur_pair->indirect_offset = 0;
  } else {
    const uint32_t last_pair_index = group.pairs.back();
    const auto& last_pair = DS_ASSERT_ARRAY_GET(pairs, last_pair_index);
    cur_pair->instance_offset = last_pair.instance_offset + last_pair.max_size * group.stride;
    cur_pair->indirect_offset = last_pair.indirect_offset + last_pair.max_size * INDIRECT_BUFFER_SIZE;
  }

  const auto& budget_cv = DS_ASSERT_ARRAY_GET(constant_values, group.budget_constant);
  const size_t val = budget_cv.reduce_value();
  // что означает val? максимальное количество инстансов
  // то есть вот это по идее:
  if (val * group.stride < cur_pair->instance_offset + cur_pair->max_size * group.stride) {
    utils::error{}("Could not allocate {} instances for draw group '{}' and mesh {}, maximum budget < offset + max pair size ({} < {})", cur_pair->max_size, group.name, mesh, (val * group.stride), (cur_pair->instance_offset + cur_pair->max_size * group.stride));
  }

  group.pairs.push_back(index);
  return index;
}

void* graphics_base::get_constant_data(const uint32_t index) {
  const auto& constant = DS_ASSERT_ARRAY_GET(constants, index);
  return reinterpret_cast<void*>(constants_memory[0].data() + constant.offset);
}

const void* graphics_base::get_constant_data(const uint32_t index) const {
  const auto& constant = DS_ASSERT_ARRAY_GET(constants, index);
  return reinterpret_cast<const void*>(constants_memory[0].data() + constant.offset);
}

void graphics_base::write_constant_data(const uint32_t slot, const void* data, const size_t size) {
  const auto& constant = DS_ASSERT_ARRAY_GET(constants, slot);
  if (constant.size < size) utils::error{}("Trying to copy object more size than constant '{}'", constant.name);
  auto ptr = reinterpret_cast<void*>(constants_memory[1].data() + constant.offset);
  memcpy(ptr, data, size);
}

void graphics_base::unregister_pair(const uint32_t draw_group, const uint32_t mesh) {
  const uint32_t pair_index = find_pair(draw_group, mesh);
  if (pair_index == INVALID_RESOURCE_SLOT) return;

  auto& group = DS_ASSERT_ARRAY_GET(draw_groups, draw_group);
  const auto itr = std::find(group.pairs.begin(), group.pairs.end(), pair_index);
  group.pairs.erase(itr);

  pairs[pair_index].draw_group_name.clear();
  pairs[pair_index].draw_group = INVALID_RESOURCE_SLOT;
  pairs[pair_index].mesh = INVALID_RESOURCE_SLOT;
}

uint32_t graphics_base::find_pair(const uint32_t draw_group, const uint32_t mesh) const {
  for (uint32_t i = 0; i < pairs.size(); ++i) {
    if (pairs[i].draw_group == draw_group && pairs[i].mesh == mesh) return i;
  }

  return INVALID_RESOURCE_SLOT;
}

int32_t graphics_base::commit_parsed_resources(render_config_storage& storage) {
  // если дошли до этой точки то все связи уже в порядке (парсинг сделан снаружи, п.7)

  std::vector<std::string> draw_group_names(draw_groups.size());
  for (uint32_t i = 0; i < draw_groups.size(); ++i) { draw_group_names[i] = draw_groups[i].name; }

  // на фоне может происходить несвязная работа
  vk::Queue(graphics).waitIdle();

  // уничтожим старое (в т.ч. resource_containers — их в storage нет, это рантайм GPU)
  clear_prev_resources();

  // забираем распарсенный конфиг из storage
  std::swap(constant_values, storage.constant_values);
  std::swap(counters, storage.counters);
  std::swap(resources, storage.resources);
  std::swap(constants, storage.constants);
  std::swap(render_targets, storage.render_targets);
  std::swap(descriptors, storage.descriptors);
  std::swap(samplers, storage.samplers);
  std::swap(materials, storage.materials);
  std::swap(geometries, storage.geometries);
  std::swap(draw_groups, storage.draw_groups);
  std::swap(steps, storage.steps);
  std::swap(passes, storage.passes);
  std::swap(graphs, storage.graphs);

  swapchain_counter_index = storage.swapchain_counter_index;
  per_frame_counter_index = storage.per_frame_counter_index;
  per_update_counter_index = storage.per_update_counter_index;
  swapchain_slot = storage.swapchain_slot;

  utils::info(
    "graphics_base: parsed resources={}, graphs={}, swapchain_slot={}",
    resources.size(),
    graphs.size(),
    swapchain_slot
  );

  size_t offset = 0;
  for (auto& c : constants) {
    c.offset = offset;
    offset += c.size;
  }

  constants_memory[0].clear();
  constants_memory[1].clear();

  constants_memory[0].resize(offset/sizeof(uint32_t), 0);
  constants_memory[1].resize(offset/sizeof(uint32_t), 0);

  // Фаза 3: если заданы resident-графы — считаем объединённый used-set и создаём ТОЛЬКО
  // их ресурсы/дескрипторы. Иначе graph_filtered_ остаётся false ⇒ создаём всё
  // (fs-путь fast_test/main.cpp).
  graph_filtered_ = false;
  if (!resident_graphs_.empty()) {
    std::vector<uint32_t> graph_indices;
    graph_indices.reserve(resident_graphs_.size());
    for (const auto& graph_name : resident_graphs_) {
      const uint32_t gi = find_render_graph(graph_name);
      if (gi == INVALID_RESOURCE_SLOT) {
        utils::warn("graphics_base: resident graph '{}' not found — пропускаем", graph_name);
        continue;
      }
      graph_indices.push_back(gi);
    }

    if (!graph_indices.empty()) compute_active_masks(graph_indices);
    else utils::warn("graphics_base: no resident graphs found — создаём все ресурсы");
  }

  // начинаем создавать все подряд
  create_samplers(); // до layout'ов: immutable-сэмплеры зашиваются в descriptor set layout
  create_descriptor_set_layouts();
  create_resources();
  create_descriptor_sets();
  revalidate_pairs(draw_group_names);
  update_descriptors();
  create_fences();
  create_global_semaphores();

  // теперь кажется все

  return 0;
}

void graphics_base::recreate_swapchain(const uint32_t width, const uint32_t height) {
  if (presentation_engine_type != presentation_engine_type::main) return;

  if (surface == VK_NULL_HANDLE) utils::error{}("Could not recreate swapchain: no surface");

  if (swapchain_slot == INVALID_RESOURCE_SLOT) {
    utils::error{}(
      "Could not recreate swapchain: resource with role 'present' was not created, resources={}, graphs={}",
      resources.size(),
      graphs.size()
    );
  }
  auto& res = DS_ASSERT_ARRAY_GET(resources, swapchain_slot);

  vk::Device dev(device);
  vk::PhysicalDevice pd(physical_device);
  const auto presents = pd.getSurfacePresentModesKHR(surface);
  const auto formats = pd.getSurfaceFormatsKHR(surface);
  const auto caps = pd.getSurfaceCapabilitiesKHR(surface);

  const auto present_mode = choose_swapchain_present_mode(presents);
  const auto format = choose_swapchain_surface_format(formats);
  const auto extent = choose_swapchain_extent(width, height, caps);

  for (uint32_t i = 0; i < swapchain_images.size(); ++i) {
    dev.destroy(res.handles[i].view);
  }

  swapchain_images.clear();

  // sanity check
  assert(extent.width != 0 && extent.height != 0);
  this->swapchain_image_size = std::make_tuple(extent.width, extent.height);

  vk::SwapchainCreateInfoKHR sci{};
  sci.surface = surface;
  sci.minImageCount = frames_in_flight();
  sci.imageFormat = format.format;
  sci.imageColorSpace = format.colorSpace;
  sci.imageExtent = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = static_cast<vk::ImageUsageFlags>(res.usage_mask);
  sci.presentMode = present_mode;
  sci.oldSwapchain = swapchain;

  auto new_swapchain = vk::Device(device).createSwapchainKHR(sci);
  if (swapchain != VK_NULL_HANDLE) {
    vk::Device(device).destroy(swapchain);
    swapchain = VK_NULL_HANDLE;
  }

  swapchain = new_swapchain;

  set_name(device, vk::SwapchainKHR(swapchain), "graphics_base.swapchain");

  const auto vk_images = dev.getSwapchainImagesKHR(swapchain);
  swapchain_images.resize(vk_images.size(), VK_NULL_HANDLE);
  static_assert(sizeof(VkImage) == sizeof(vk::Image));
  memcpy(swapchain_images.data(), vk_images.data(), sizeof(vk_images[0]) * vk_images.size());

  // ImageView нужен ТОЛЬКО если usage образа поддерживает view (sampled / storage / любой attachment).
  // Для present-only свопчейна (его лишь блитят в конце) usage = TRANSFER_DST: view не нужен
  // (vkCmdBlitImage берёт сам образ), а его создание нарушает спеку (VUID-VkImageViewCreateInfo-None-02273
  // и связанные). subresourceRange всё равно сохраняем — он нужен для барьеров/смены layout образа.
  constexpr vk::ImageUsageFlags view_capable_usage =
    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eDepthStencilAttachment |
    vk::ImageUsageFlagBits::eInputAttachment | vk::ImageUsageFlagBits::eTransientAttachment;
  const bool needs_view = bool(static_cast<vk::ImageUsageFlags>(res.usage_mask) & view_capable_usage);

  vk::ImageSubresourceRange range{};
  range.aspectMask = vk::ImageAspectFlagBits::eColor;
  range.baseMipLevel = 0;
  range.levelCount = 1;
  range.baseArrayLayer = 0;
  range.layerCount = 1;

  for (uint32_t i = 0; i < swapchain_images.size(); ++i) {
    res.handles[i].index = INVALID_RESOURCE_SLOT;
    res.handles[i].subimage = std::bit_cast<subresource_image>(range);

    set_name(device, vk::Image(swapchain_images[i]), res.name + ".image" + std::to_string(i));

    if (!needs_view) { res.handles[i].view = VK_NULL_HANDLE; continue; }

    vk::ImageViewCreateInfo ivci{};
    ivci.image = swapchain_images[i];
    ivci.format = format.format;
    ivci.viewType = vk::ImageViewType::e2D;
    ivci.components = vk::ComponentMapping{};
    ivci.subresourceRange = range;
    res.handles[i].view = dev.createImageView(ivci);

    set_name(device, vk::ImageView(res.handles[i].view), res.name + ".view" + std::to_string(i));
  }
}

void graphics_base::clear_prev_resources() {
  vk::Device dev(device);
  vma::Allocator a(allocator);
  dev.resetDescriptorPool(descriptor_pool);
  dev.resetCommandPool(command_pool);

  clear();

  constant_values.clear();
  counters.clear();
  resource_containers.clear();
  resources.clear();
  constants.clear();
  render_targets.clear();
  descriptors.clear();
  materials.clear();
  geometries.clear();
  draw_groups.clear();
  steps.clear();
  passes.clear();
  graphs.clear();

  constants_memory[0].clear();
  constants_memory[1].clear();

  // пары не трогаем пока что
}

void graphics_base::create_samplers() {
  for (auto& s : samplers) {
    sampler_maker sm{ vk::Device(device) };
    sm.filter(vk::Filter(s.min_filter), vk::Filter(s.mag_filter));
    sm.mipmapMode(vk::SamplerMipmapMode(s.mipmap_mode));
    sm.addressMode(vk::SamplerAddressMode(s.address_u), vk::SamplerAddressMode(s.address_v), vk::SamplerAddressMode(s.address_w));
    s.handle = sm.create(s.name);
    utils::info("graphics_base: created sampler '{}'", s.name);
  }
}

void graphics_base::create_descriptor_set_layouts() {
  for (uint32_t di = 0; di < descriptors.size(); ++di) {
    if (!is_descriptor_active(di)) continue; // Фаза 3: дескриптор не нужен resident-графам
    auto& desc = descriptors[di];
    descriptor_set_layout_maker dslm(device);
    // combined() хранит pImmutableSamplers = samplers.data(); держим эти векторы живыми до create()
    std::vector<std::vector<vk::Sampler>> imm_keep;
    imm_keep.reserve(desc.layout.size() + 1);
    for (uint32_t i = 0; i < desc.layout.size(); ++i) {
      const auto& [slot, usage, sampler_index, stages] = desc.layout[i];
      const auto& res = resources[slot];
      const uint32_t buffers_count = res.compute_buffering(this);
      const auto stage = vk::ShaderStageFlags(stages);
      if (sampler_index != INVALID_RESOURCE_SLOT) {
        // sampled + sampler => combinedImageSampler с immutable-сэмплером (по одному на элемент массива)
        imm_keep.emplace_back(buffers_count, vk::Sampler(samplers[sampler_index].handle));
        dslm.combined(i, vk::DescriptorType::eCombinedImageSampler, stage, imm_keep.back());
      } else {
        dslm.binding(i, convertdt(usage), stage, buffers_count);
      }
    }
    // asset-текстурный binding идёт после resource-bindings (binding = layout.size())
    if (desc.texture_count > 0) {
      imm_keep.emplace_back(desc.texture_count, vk::Sampler(samplers[desc.texture_sampler].handle));
      dslm.combined(uint32_t(desc.layout.size()), vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlags(desc.texture_stage), imm_keep.back());
    }
    desc.setlayout = dslm.create(desc.name);
  }
}

// Фаза 3: транзитивный used-set resident-графов → resource_active_mask_/descriptor_active_mask_.
// Над-аппроксимация БЕЗОПАСНА (лишний активный ресурс = как раньше; пропущенный нужный = краш),
// поэтому берём с запасом. pass.read|write уже агрегирует барьеры шагов, ресурсы local-дескриптора,
// cmd-ресурсы и subpass/pass-барьеры (см. parse_execution_step2/parse_execution_pass2). Отдельно
// доносим то, что read/write НЕ покрывает: буферы draw_group и ресурсы дескрипторов из step.sets.
void graphics_base::compute_active_masks(const std::vector<uint32_t>& graph_indices) {
  // Маски — bitset<MAXIMUM_RENDERING_RESOURCES_COUNT>; больше 256 графов/ресурсов/дескрипторов сейчас
  // неразумно — падаем громко (позже придумаем динамику).
  if (graphs.size() > MAXIMUM_RENDERING_RESOURCES_COUNT)
    utils::error{}("graphics_base: graphs={} exceed mask capacity {}", graphs.size(), MAXIMUM_RENDERING_RESOURCES_COUNT);
  if (resources.size() > MAXIMUM_RENDERING_RESOURCES_COUNT)
    utils::error{}("graphics_base: resources={} exceed mask capacity {}", resources.size(), MAXIMUM_RENDERING_RESOURCES_COUNT);
  if (descriptors.size() > MAXIMUM_RENDERING_RESOURCES_COUNT)
    utils::error{}("graphics_base: descriptors={} exceed mask capacity {}", descriptors.size(), MAXIMUM_RENDERING_RESOURCES_COUNT);

  graph_active_mask_.reset();
  resource_active_mask_.reset();
  descriptor_active_mask_.reset();
  graph_filtered_ = true;

  const auto mark_res = [&](const uint32_t idx) {
    if (idx != INVALID_RESOURCE_SLOT && idx < resources.size()) resource_active_mask_.set(idx);
  };
  const auto mark_desc = [&](const uint32_t idx) {
    if (idx != INVALID_RESOURCE_SLOT && idx < descriptors.size()) descriptor_active_mask_.set(idx);
  };

  for (const uint32_t graph_index : graph_indices) {
    if (graph_index >= graphs.size()) continue;
    graph_active_mask_.set(graph_index);

    const auto& graph = DS_ASSERT_ARRAY_GET(graphs, graph_index);
    mark_res(graph.present_source);

    for (const uint32_t pass_index : graph.passes) {
      const auto& pass = DS_ASSERT_ARRAY_GET(passes, pass_index);

      // pass-level барьеры и subpass-вложения (все слоты; read/write НЕ покрывает transfer_dst/upload и т.п.)
      for (const auto& group : pass.barriers)  for (const auto& info : group) mark_res(info.slot);
      for (const auto& group : pass.subpasses) for (const auto& info : group) mark_res(info.slot);

      if (pass.render_target != INVALID_RESOURCE_SLOT) {
        const auto& rt = DS_ASSERT_ARRAY_GET(render_targets, pass.render_target);
        for (const auto& [res_idx, usage] : rt.resources) mark_res(res_idx);
      }

      for (const uint32_t step_index : pass.steps) {
        if (step_index == UINT32_MAX) continue; // маркер next_subpass
        const auto& step = DS_ASSERT_ARRAY_GET(steps, step_index);

        // step.barriers уже вбирает barriers + step.resources (local-descr) + cmd_params.resources
        // (см. parse_execution_step2), но берём ещё и cmd_params явно — над-аппроксимация безопасна.
        for (const auto& [res_idx, usage] : step.barriers) mark_res(res_idx);
        for (const auto& [res_idx, usage] : step.cmd_params.resources) mark_res(res_idx);

        mark_desc(step.descriptor);
        for (const uint32_t d : step.sets) mark_desc(d);

        if (step.draw_group != INVALID_RESOURCE_SLOT) {
          const auto& dg = DS_ASSERT_ARRAY_GET(draw_groups, step.draw_group);
          mark_res(dg.instances_buffer);
          mark_res(dg.indirect_buffer);
          mark_desc(dg.descriptor);
        }
      }
    }
  }

  // used-дескрипторы → их layout-ресурсы (ресурсы из sets, не попавшие в pass.read/write)
  for (uint32_t d = 0; d < descriptors.size(); ++d) {
    if (!descriptor_active_mask_[d]) continue;
    for (const auto& [slot, usage, sampler_index, stages] : descriptors[d].layout) mark_res(slot);
  }

  utils::info(
    "graphics_base: {} resident graph(s) use {}/{} resources, {}/{} descriptors",
    graph_active_mask_.count(), resource_active_mask_.count(), resources.size(), descriptor_active_mask_.count(), descriptors.size()
  );
}

void graphics_base::create_resources() {
  // Собираем Vulkan usage только по resident-графам, если включена фильтрация. Когда два графа
  // живут одновременно, общий ресурс получает union usage обоих графов и не требует пересоздания
  // при переключении между ними.
  std::vector<vk::BufferUsageFlags> buffer_flags(resources.size());
  std::vector<vk::ImageUsageFlags> image_flags(resources.size());

  const auto add_usage = [&](const uint32_t res_index, const usage::values usage) {
    const auto& res = DS_ASSERT_ARRAY_GET(resources, res_index);
    const bool is_image = role::is_image(res.role);
    if (is_image) {
      image_flags[res_index] = image_flags[res_index] | convertiuf(usage);
    } else {
      buffer_flags[res_index] = buffer_flags[res_index] | convertbuf(usage);
    }
  };

  const auto consume_step = [&](const step_base& step) {
    for (const auto& [res_index, usage] : step.barriers) {
      add_usage(res_index, usage);
    }
  };

  const auto consume_pass = [&](const execution_pass_base& pass) {
    for (const auto& subpass : pass.barriers) {
      for (const auto& info : subpass) {
        add_usage(info.slot, info.usage);
      }
    }

    for (const auto& subpass : pass.subpasses) {
      for (const auto& info : subpass) {
        add_usage(info.slot, info.usage);
      }
    }
  };

  if (graph_filtered_) {
    for (uint32_t gi = 0; gi < graphs.size(); ++gi) {
      if (!is_graph_active(gi)) continue;
      const auto& graph = graphs[gi];
      for (const uint32_t pass_index : graph.passes) {
        const auto& pass = DS_ASSERT_ARRAY_GET(passes, pass_index);
        for (const uint32_t step_index : pass.steps) {
          if (step_index == UINT32_MAX) continue;
          consume_step(DS_ASSERT_ARRAY_GET(steps, step_index));
        }
        consume_pass(pass);
      }
    }
  } else {
    for (const auto& step : steps) consume_step(step);
    for (const auto& pass : passes) consume_pass(pass);
  }

  for (uint32_t di = 0; di < descriptors.size(); ++di) {
    if (!is_descriptor_active(di)) continue;
    for (const auto& [res_index, usage, sampler_index, stages] : descriptors[di].layout) {
      add_usage(res_index, usage);
    }
  }

  //const auto& caps = vk::PhysicalDevice(physical_device).getSurfaceCapabilitiesKHR(surface);
  //const auto& ext = choose_swapchain_extent(caps.currentExtent.width, caps.currentExtent.height, caps);
  vma::Allocator al(allocator);

  // имя?
  struct matching_resources {
    struct { uint32_t x, y; } extent;
    uint32_t format;
    uint32_t usage_mask;
    uint32_t mips;
    bool is_buffer;
    bool is_host_resource;

    uint32_t layers;
    uint32_t layer_offset;

    size_t offset;
    size_t size;

    size_t layer_size;

    std::string name;
  };

  std::vector<matching_resources> matching;

  std::vector<uint32_t> container_table(resources.size(), 0);
  std::vector<size_t> frame_sizes(resources.size(), 0);
  std::vector<size_t> frame_strides(resources.size(), 0);

  for (uint32_t i = 0; i < resources.size(); ++i) {
    auto& res = resources[i];

    const bool is_image = role::is_image(res.role);

    if (is_image) {
      res.usage_mask = res.usage_mask | static_cast<uint32_t>(image_flags[i]);
    } else {
      res.usage_mask = res.usage_mask | static_cast<uint32_t>(buffer_flags[i]);
    }

    if (res.role == role::present) continue;
    if (!is_resource_active(i)) continue; // Фаза 3: ресурс не нужен активному графу — не создаём контейнер

    const bool is_buffer = role::is_buffer(res.role);
    //const auto& size_value = DS_ASSERT_ARRAY_GET(constant_values, res.size);

    const uint32_t res_mips = 1;
    const bool is_host_resource = role::is_host_visible(res.role);

    // тут бы мы хотели еще как то сгруппировать ресурсы
    // наверное придется ввести маленькую предварительную структуру 

    // размеры буферов так то должны быть aligned к 16
    // и размеры картинок тоже поди нужно алигнуть к чему то

    auto [buffer_size, img_ext] = res.compute_frame_size(this);
    const uint32_t buffering = res.compute_buffering(this);
    auto image_extent = vk::Extent2D{ std::get<0>(img_ext), std::get<1>(img_ext) };
    const size_t frame_size = buffer_size;
    const size_t frame_stride = is_buffer
      ? utils::align_to(frame_size, buffer_suballocation_alignment(physical_device, res.usage_mask))
      : frame_size;
    frame_sizes[i] = frame_size;
    frame_strides[i] = frame_stride;

    if (buffering == 0) {
      utils::error{}("Invalid buffering for resource '{}'", res.name);
    }

    // если у нас роль present то мы пропускаем этот ресурс и создадим его отдельно при создании свопчеина

    //auto image_extent = ext;
    //size_t buffer_size = 0;

    //if (size_value.type == value_type::screensize) {
    //  const auto& [xs, ys, zs] = size_value.current_scale;
    //  buffer_size = size_t(ext.width * xs) * size_t(ext.height * ys) * size_t(res.size_hint);
    //  image_extent = vk::Extent2D(ext.width * xs, ext.height * ys);
    //} else if (size_value.type == value_type::fixed) {
    //  const auto& [x, y, z] = size_value.current_value;
    //  const auto& [xs, ys, zs] = size_value.current_scale;
    //  buffer_size = size_t(x*xs) * size_t(res.size_hint);
    //  image_extent = vk::Extent2D(x * xs, x * xs);
    //} else if (size_value.type == value_type::fixed_2d) {
    //  const auto& [x, y, z] = size_value.current_value;
    //  const auto& [xs, ys, zs] = size_value.current_scale;
    //  buffer_size = size_t(x * xs) * size_t(y*ys) * size_t(res.size_hint);
    //  image_extent = vk::Extent2D(x * xs, y * ys);
    //} else if (size_value.type == value_type::fixed_3d) {
    //  const auto& [x, y, z] = size_value.current_value;
    //  const auto& [xs, ys, zs] = size_value.current_scale;
    //  buffer_size = size_t(x*xs) * size_t(y*ys) * size_t(z*zs) * size_t(res.size_hint);
    //  image_extent = vk::Extent2D(x * xs, y * ys);

    //  if (!is_buffer) utils::error{}("Not implemented");
    //} 
    //
    //// наверное указать что за глобальная юниформа? а сколько их бывает то
    //if (res.role == role::global_uniform) {
    //  // константа (данные камеры)
    //  buffer_size = res.size_hint; // ???
    //} else if (res.role == role::frame_constants) {
    //  // константа (счетчик фреймов + время + ???)
    //  buffer_size = res.size_hint; // ???
    //} else if (res.role == role::indirect) {
    //  // размер одного буфера фиксирован 
    //  buffer_size = sizeof(vk::DrawIndirectCommand) * 2;
    //}

    
    auto itr = std::find_if(matching.begin(), matching.end(), [&](const auto& data) -> bool {
      if (is_buffer != data.is_buffer) return false;
      if (is_host_resource != data.is_host_resource) return false;

      if (is_buffer && data.is_buffer) {
        return res.usage_mask == data.usage_mask;
      }

      const bool size_match = image_extent.width == data.extent.x && image_extent.height == data.extent.y;
      const bool format_match = res.format_hint == data.format;
      const bool usage_mask_match = res.usage_mask == data.usage_mask;
      const bool mips_match = res_mips == data.mips;
      return size_match && format_match && usage_mask_match && mips_match;
    });

    if (itr == matching.end()) {
      matching.emplace_back();
      matching.back().extent = { image_extent.width, image_extent.height };
      matching.back().format = res.format_hint;
      matching.back().usage_mask = res.usage_mask;
      matching.back().mips = res_mips;
      matching.back().is_buffer = is_buffer;
      matching.back().is_host_resource = is_host_resource;
      matching.back().layers = 0;
      matching.back().layer_offset = 0;
      matching.back().offset = 0;
      matching.back().size = 0;
      matching.back().layer_size = frame_stride;

      itr = matching.begin() + (matching.size()-1);
    }

    // буферизация присутствует для всех, но по слоям она только у картинок
    // на хосте никаких layers наверное не будет и нужно несколько ресурсов городить
    // не будет, да надо в принципе отказаться от host visible картинок в этом контексте
    if (!is_buffer) itr->layers += buffering;
    itr->size += frame_stride * buffering;
    if (itr->name.empty()) itr->name = res.name;
    else itr->name += " | " + res.name;

    container_table[i] = std::distance(matching.begin(), itr);
  }

  for (auto& data : matching) {
    auto& cont = resource_containers.emplace_back();
    cont.name = std::move(data.name);
    cont.format = data.format;
    cont.layers = data.layers;
    cont.mips = data.mips;
    cont.usage_mask = data.usage_mask;
    cont.extent = { data.extent.x, data.extent.y };
    cont.size = data.size;
    cont.mem_ptr = nullptr;

    cont.create_container(allocator, data.is_host_resource);
  }

  // вот мы создали контейнеры, теперь заполним сами ресурсы
  vk::Device dev(device);

  for (uint32_t i = 0; i < resources.size(); ++i) {
    auto& res = resources[i];

    if (res.role == role::present) continue; // swapchain создаётся отдельно в recreate_swapchain()
    if (!is_resource_active(i)) continue; // Фаза 3: пропускаем неактивные

    const uint32_t cont_index = container_table[i];

    auto& match = matching[cont_index];
    auto& cont = resource_containers[cont_index];

    const bool is_image = role::is_image(res.role);

    const uint32_t buffering = res.compute_buffering(this);
    const auto frame_size = frame_sizes[i];
    const auto frame_stride = frame_strides[i];

    for (uint32_t j = 0; j < buffering; ++j) {
      res.handles[j].index = cont_index;
      if (is_image) {
        const auto img_handle = std::bit_cast<VkImage>(cont.handle);

        const auto fmt = static_cast<vk::Format>(res.format_hint);
        vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;
        if (fmt == vk::Format::eD16Unorm) aspect = vk::ImageAspectFlagBits::eDepth;
        if (fmt == vk::Format::eD32Sfloat) aspect = vk::ImageAspectFlagBits::eDepth;
        if (fmt == vk::Format::eD16UnormS8Uint) aspect = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
        if (fmt == vk::Format::eD24UnormS8Uint) aspect = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
        if (fmt == vk::Format::eD32SfloatS8Uint) aspect = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;

        vk::ImageViewCreateInfo ivci{};
        ivci.image = img_handle;
        ivci.format = fmt;
        ivci.viewType = vk::ImageViewType::e2D;
        ivci.components = vk::ComponentMapping{};
        ivci.subresourceRange = vk::ImageSubresourceRange{};
        // определяется форматом? ролью?
        ivci.subresourceRange.aspectMask = aspect;
        ivci.subresourceRange.baseMipLevel = 0;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.baseArrayLayer = match.layer_offset + j;
        ivci.subresourceRange.layerCount = 1;

        res.handles[j].view = dev.createImageView(ivci);

        set_name(device, vk::ImageView(res.handles[i].view), res.name + ".view" + std::to_string(j));

        res.handles[j].subimage = std::bit_cast<subresource_image>(ivci.subresourceRange);
      } else {
        res.handles[j].subbuffer.offset = match.offset + frame_stride * j;
        res.handles[j].subbuffer.size = frame_size;
      }
    }

    match.offset += frame_stride * buffering;
    match.layer_offset += buffering;
  }
}

void graphics_base::create_descriptor_sets() {
  for (uint32_t di = 0; di < descriptors.size(); ++di) {
    if (!is_descriptor_active(di)) continue; // Фаза 3: дескриптор не нужен активному графу
    auto& d = descriptors[di];
    descriptor_set_maker dsm(device);
    for (uint32_t i = 0; i < frames_in_flight(); ++i) {
      dsm.layout(d.setlayout);
    }

    const auto arr = dsm.create(descriptor_pool, d.name + ".set");
    memcpy(d.sets.data(), arr.data(), arr.size() * sizeof(arr[0]));
  }
}

void graphics_base::revalidate_pairs(const std::vector<std::string>& prev_drav_group_names) {
  for (auto& pair : pairs) {
    const auto& old_name = prev_drav_group_names[pair.draw_group];
    const uint32_t index = check(find_draw_group(old_name), "draw_group", old_name, "revalidate_pairs");
    pair.draw_group = index;
  }
}

// предположим эта функция только про заполнить дескрипторы хоть чем то
void graphics_base::update_descriptors() {
  std::vector<vk::WriteDescriptorSet> writes;
  std::vector<std::tuple<size_t, bool>> offsets;
  std::vector<vk::DescriptorBufferInfo> buffers;
  std::vector<vk::DescriptorImageInfo> images;

  const uint32_t cur_frame_index = current_frame_index();
  const uint32_t frames_in_flight_count = frames_in_flight();
  const uint32_t current_set_index = cur_frame_index % frames_in_flight_count;

  for (uint32_t i = 0; i < descriptors.size(); ++i) {
    if (!is_descriptor_active(i)) continue; // Фаза 3: пропускаем дескрипторы вне активного графа
    const auto& d = descriptors[i];
    const auto set = d.sets[current_set_index];

    for (uint32_t bind = 0; bind < d.layout.size(); ++bind) {
      const auto& [res_index, usage, sampler_index, stages] = d.layout[bind];
      const auto& res = resources[res_index];
      const uint32_t buffering = res.compute_buffering(this);
      const bool is_image = role::is_image(res.role);
      const bool combined = sampler_index != INVALID_RESOURCE_SLOT;

      writes.emplace_back();
      writes.back().dstSet = set;
      writes.back().dstBinding = bind;
      writes.back().dstArrayElement = 0;
      writes.back().descriptorCount = buffering;
      writes.back().descriptorType = combined ? vk::DescriptorType::eCombinedImageSampler : convertdt(usage);

      if (is_image) {
        offsets.push_back(std::make_tuple(images.size(), is_image));
        for (uint32_t j = 0; j < buffering; ++j) {
          const auto& counter = DS_ASSERT_ARRAY_GET(counters, res.swap);
          const uint32_t current_clock = counter.get_value();
          const uint32_t final_index = (current_clock + j) % buffering;

          images.emplace_back();
          // immutable-сэмплер уже в layout; для combined достаточно view (sampler в write игнорируется)
          images.back().sampler = combined ? vk::Sampler(samplers[sampler_index].handle) : vk::Sampler(nullptr);
          images.back().imageView = res.handles[final_index].view;
          images.back().imageLayout = convertil(usage);
        }
      } else {
        offsets.push_back(std::make_tuple(buffers.size(), is_image));
        for (uint32_t j = 0; j < buffering; ++j) {
          const uint32_t current_clock = counters[res.swap].value;
          const uint32_t final_index = (current_clock + j) % buffering;

          const auto& cont = resource_containers[res.handles[final_index].index];
          auto handle = std::bit_cast<VkBuffer>(cont.handle);

          buffers.emplace_back();
          buffers.back().buffer = handle;
          const auto& sub = res.handles[final_index].subbuffer;
          const size_t available = sub.offset < cont.size ? cont.size - sub.offset : 0;
          buffers.back().offset = sub.offset;
          buffers.back().range = sub.size <= available ? sub.size : available;
        }
      }
    }
  }

  for (size_t i = 0; i < writes.size(); ++i) {
    const auto& [offset, is_image] = offsets[i];
    if (is_image) {
      writes[i].pImageInfo = &images[offset];
    } else {
      writes[i].pBufferInfo = &buffers[offset];
    }
  }

  vk::Device dev(device);
  dev.updateDescriptorSets(writes, nullptr);

  /*for (uint32_t i = 0; i < descriptors.size(); ++i) {
    const auto& d = descriptors[i];
    for (uint32_t bind = 0; bind < d.layout.size(); ++bind) {
      const auto& [res_index, usage] = d.layout[bind];
      const auto& res = resources[res_index];
      const uint32_t buffering = static_cast<uint32_t>(res.type);
      const bool is_image = role::is_image(res.role);

      for (uint32_t f = 0; f < frames_in_flight(); ++f) {
        writes.emplace_back();
        writes.back().dstSet = d.sets[f];
        writes.back().dstBinding = bind;
        writes.back().dstArrayElement = 0;
        writes.back().descriptorCount = buffering;
        writes.back().descriptorType = convertdt(usage);

        if (is_image) {
          offsets.push_back(std::make_tuple(images.size(), is_image));
          for (uint32_t j = 0; j < buffering; ++j) {
            const uint32_t current_clock = counters[res.swap].value;
            const uint32_t final_index = (current_clock + j) % buffering;

            images.emplace_back();
            images.back().sampler = nullptr;
            images.back().imageView = res.handles[final_index].view;
            images.back().imageLayout = convertil(usage);
          }
        } else {
          offsets.push_back(std::make_tuple(buffers.size(), is_image));
          for (uint32_t j = 0; j < buffering; ++j) {
            const uint32_t current_clock = counters[res.swap].value;
            const uint32_t final_index = (current_clock + j) % buffering;

            const auto& cont = resource_containers[res.handles[final_index].index];
            auto handle = std::bit_cast<VkBuffer>(cont.handle);

            buffers.emplace_back();
            buffers.back().buffer = handle;
            buffers.back().offset = res.handles[final_index].subbuffer.offset;
            buffers.back().range = res.handles[final_index].subbuffer.size;
          }
        }
      }
    }
  }

  for (size_t i = 0; i < writes.size(); ++i) {
    const auto& [offset, is_image] = offsets[i];
    if (is_image) {
      writes[i].pImageInfo = &images[offset];
    } else {
      writes[i].pBufferInfo = &buffers[offset];
    }
  }

  vk::Device dev(device);
  dev.updateDescriptorSets(writes, nullptr);*/
}

void graphics_base::wait_fence() {
  if (!can_draw()) return;

  auto f = fences[computed_current_frame_index];
  const size_t sec1 = size_t(1000) * size_t(1000) * size_t(1000);
  const auto res = vk::Device(device).waitForFences(vk::Fence(f), true, sec1);
  if (res != vk::Result::eSuccess) utils::error{}("waitForFences failed for current frame {}", current_frame_index());
  vk::Device(device).resetFences(vk::Fence(f));
}

render_graph_instance graphics_base::create_render_graph_instance(const uint32_t index) {
  if (index >= graphs.size()) utils::error{}("Try to get graph with index {}, but array has {} graphs", index, graphs.size());

  const auto& graph = DS_ASSERT_ARRAY_GET(graphs, index);

  render_graph_instance out;
  out.type = step_interface::type::render_graph;
  out.super = index;
  out.device = device;

  for (const auto pass_index : graph.passes) {
    const auto& pass = DS_ASSERT_ARRAY_GET(passes, pass_index);

    auto ptr = std::make_unique<execution_pass_instance>();
    ptr->type = step_interface::type::execution_pass;
    ptr->super = pass_index;
    ptr->device = device;

    // создаем рендер пасс и фреймбуферы
    ptr->create_related_primitives(this);

    auto ptr_raw = ptr.get();

    out.groups.emplace_back();
    auto& group = out.groups.back();
    group.device = device;
    group.pool = command_pool;
    group.steps.push_back(ptr_raw);
    group.frames.resize(frames_in_flight());
    group.populate_command_buffers();

    out.steps.emplace_back(std::move(ptr));
    out.viewport_steps.push_back(ptr_raw);

    uint32_t subpass_index = 0;
    for (const auto step_index : pass.steps) {
      // next_subpass ???
      if (step_index == UINT32_MAX) {
        // тут мы создадим специальную штуку которая барьеры подхватит

        auto ptr = std::make_unique<subpass_next>();
        ptr->type = step_interface::type::step;
        ptr->super = pass_index;
        ptr->renderpass = ptr_raw->renderpass;
        ptr->index = subpass_index;

        auto ptr_raw = ptr.get();
        group.steps.push_back(ptr_raw);
        out.steps.emplace_back(std::move(ptr));

        subpass_index += 1;
        continue;
      }

      const auto& step = DS_ASSERT_ARRAY_GET(steps, step_index);

      step_interface* cur_step = nullptr;
      switch (step.cmd_params.type) {
        case command::values::draw                 : cur_step = create_render_step<graphics_draw>                 (out, step_index, device, ptr_raw->renderpass, subpass_index, pass.render_target); break;
        case command::values::draw_indexed         : cur_step = create_render_step<graphics_draw_indexed>         (out, step_index, device, ptr_raw->renderpass, subpass_index, pass.render_target); break;
        case command::values::draw_indirect        : cur_step = create_render_step<graphics_draw_indirect>        (out, step_index, device, ptr_raw->renderpass, subpass_index, pass.render_target); break;
        case command::values::draw_indexed_indirect: cur_step = create_render_step<graphics_draw_indexed_indirect>(out, step_index, device, ptr_raw->renderpass, subpass_index, pass.render_target); break;
        case command::values::draw_constant        : cur_step = create_render_step<graphics_draw_constant>        (out, step_index, device, ptr_raw->renderpass, subpass_index, pass.render_target); break;
        case command::values::draw_indexed_constant: cur_step = create_render_step<graphics_draw_indexed_constant>(out, step_index, device, ptr_raw->renderpass, subpass_index, pass.render_target); break;
        case command::values::draw_ui               : cur_step = create_render_step<graphics_draw_ui>              (out, step_index, device, ptr_raw->renderpass, subpass_index, pass.render_target); break;
        case command::values::dispatch_indirect    : cur_step = create_render_step<compute_dispatch_indirect>     (out, step_index, device); break;
        case command::values::dispatch_constant    : cur_step = create_render_step<compute_dispatch_constant>     (out, step_index, device); break;
        case command::values::copy_buffer          : cur_step = create_render_step<transfer_copy_buffer>          (out, step_index); break;
        case command::values::copy_image           : cur_step = create_render_step<transfer_copy_image>           (out, step_index); break;
        case command::values::copy_buffer_image    : cur_step = create_render_step<transfer_copy_buffer_image>    (out, step_index); break;
        case command::values::copy_image_buffer    : cur_step = create_render_step<transfer_copy_image_buffer>    (out, step_index); break;
        case command::values::blit_linear          : cur_step = create_render_step<transfer_blit_linear>          (out, step_index); break;
        case command::values::blit_nearest         : cur_step = create_render_step<transfer_blit_nearest>         (out, step_index); break;
        case command::values::clear_color          : cur_step = create_render_step<transfer_clear_color>          (out, step_index); break;
        case command::values::clear_depth          : cur_step = create_render_step<transfer_clear_depth>          (out, step_index); break;
        default: break;
      }

      if (cur_step == nullptr) utils::error{}("Invalid command parameter {}", static_cast<uint32_t>(step.cmd_params.type));
      
      group.steps.push_back(cur_step);
    }

    {
      auto ptr = std::make_unique<execution_pass_end_instance>();
      ptr->type = step_interface::type::execution_pass;
      ptr->super = pass_index;

      auto ptr_raw = ptr.get();
      group.steps.push_back(ptr_raw);
      out.steps.emplace_back(std::move(ptr));
    }

    // для каждой группы создадим командные буферы
    // нужно еще прочитать настройки синхронизаций
    // по идее семафора это тоже ресурс = имя + пачка примитивов
    // как минимум у нас есть 1 внешняя семафора - имейдж эквайр

    for (const auto& name : pass.signal) {
      const uint32_t index = out.create_semaphore(name, frames_in_flight());
      const auto& sem = DS_ASSERT_ARRAY_GET(out.local_semaphores, index);
      for (uint32_t i = 0; i < group.frames.size(); ++i) {
        const uint32_t sem_index = i; // тот же кадр
        group.frames[i].signal.push_back(sem.handles[sem_index]);
      }
    }
  }

  // находим семафоры по которые указываем в качестве ожидания
  for (uint32_t i = 0; i < graph.passes.size(); ++i) {
    const uint32_t pass_index = graph.passes[i];
    const auto& pass = DS_ASSERT_ARRAY_GET(passes, pass_index);
    vk::PipelineStageFlags wait_for_stages{};
    if (pass.has_step_type(step_type::graphics)) wait_for_stages = wait_for_stages | vk::PipelineStageFlagBits::eAllGraphics;
    if (pass.has_step_type(step_type::compute)) wait_for_stages = wait_for_stages | vk::PipelineStageFlagBits::eComputeShader;
    if (pass.has_step_type(step_type::transfer)) wait_for_stages = wait_for_stages | vk::PipelineStageFlagBits::eTransfer;

    static_assert(step_type::count == 3);

    // тут походу не совпадают семафоры?
    auto& group = DS_ASSERT_ARRAY_GET(out.groups, i);
    for (const auto& name : pass.wait_for) {
      const uint32_t global_index = find_semaphore(name);
      if (global_index != INVALID_RESOURCE_SLOT) {
        const auto& sem = DS_ASSERT_ARRAY_GET(semaphores, global_index);
        for (uint32_t j = 0; j < group.frames.size(); ++j) {
          const uint32_t sem_index = j; // тот же кадр
          group.frames[j].wait_for.push_back(sem.handles[sem_index]);
          group.frames[j].wait_for_stages.push_back(static_cast<uint32_t>(wait_for_stages));
        }

        continue;
      }
      
      const uint32_t local_index = out.find_semaphore(name);
      if (local_index != INVALID_RESOURCE_SLOT) {
        const auto& sem = DS_ASSERT_ARRAY_GET(out.local_semaphores, local_index);
        for (uint32_t j = 0; j < group.frames.size(); ++j) {
          const uint32_t sem_index = j == 0 ? group.frames.size()-1 : j-1; // предыдущий кадр
          group.frames[j].wait_for.push_back(sem.handles[sem_index]);
          group.frames[j].wait_for_stages.push_back(static_cast<uint32_t>(wait_for_stages));
        }

        continue;
      }

      utils::error{}("Could not find semaphore resource '{}' for pass '{}'", name, pass.name);
    }
  }

  // так и дальше че? ну и все походу
  // наконецто... будем надеяться что это хотя бы сработает как надо
  return out;
}

void graphics_base::change_render_graph(const uint32_t index) {
  auto next_graph = create_render_graph_instance(index);

  // Смена графа может происходить поверх уже отправленных кадров. Несколько кадров простоя
  // допустимы: ждём graphics queue, меняем активный instance, затем чистим старые graph-local
  // Vulkan-объекты (pipelines/renderpasses/framebuffers/local semaphores).
  vk::Queue(graphics).waitIdle();

  auto old_graph = std::move(execution_graph);
  execution_graph = std::move(next_graph);
  current_render_graph_index = index;
  old_graph.clear();
}

bool graphics_base::presentable_state_stable() const {
  return presentation_engine_type != presentation_engine_type::main || 
    (presentation_engine_type == presentation_engine_type::main && current_presentable_state == static_cast<uint32_t>(vk::Result::eSuccess));
}

bool graphics_base::presentable_state_suboptimal() const {
  return presentation_engine_type == presentation_engine_type::main && current_presentable_state == static_cast<uint32_t>(vk::Result::eSuboptimalKHR);
}

bool graphics_base::presentable_state_waiting_host_event() const {
  return presentation_engine_type == presentation_engine_type::main && (current_presentable_state == static_cast<uint32_t>(vk::Result::eErrorOutOfDateKHR) || current_presentable_state == static_cast<uint32_t>(vk::Result::eErrorSurfaceLostKHR));
}

void graphics_ctx::prepare() {
  // тут нужно собрать текущие ресурсы и дескрипторы

  resources.clear();
  resources.resize(base->resources.size());

  // вопрос как быть с usage? желательно все таки наследовать
  // заполнять usage при входе?.. короч пока как нибудь
  for (uint32_t i = 0; i < base->resources.size(); ++i) {
    const auto& base_res = base->resources[i];
    const auto& counter = DS_ASSERT_ARRAY_GET(base->counters, base_res.swap);
    const bool is_image = role::is_image(base_res.role);
    const uint32_t buffering = base_res.compute_buffering(base);

    const uint32_t cur_index = counter.get_value() % buffering;
    const auto& cur_handle = base_res.handles[cur_index];

    if (base_res.role == role::present) {
      const uint32_t cur_img_index = base->current_swapchain_image_index();
      resources[i].img = DS_ASSERT_ARRAY_GET(base->swapchain_images, cur_img_index);
      resources[i].subimg = cur_handle.subimage;
      resources[i].view = cur_handle.view;
      const auto [x, y] = base->swapchain_extent();
      resources[i].extent = { x, y };
      resources[i].role = base_res.role;
      resources[i].usage = usage::undefined;
      continue;
    }

    const auto& base_res_container = DS_ASSERT_ARRAY_GET(base->resource_containers, cur_handle.index);

    if (is_image) {
      resources[i].img = std::bit_cast<VkImage>(base_res_container.handle);
      resources[i].subimg = cur_handle.subimage;
      resources[i].view = cur_handle.view;
      resources[i].extent = { base_res_container.extent.x, base_res_container.extent.y };
      resources[i].role = base_res.role;
      resources[i].usage = usage::undefined;
    } else {
      resources[i].buf = std::bit_cast<VkBuffer>(base_res_container.handle);
      resources[i].subbuf = cur_handle.subbuffer;
      resources[i].view = VK_NULL_HANDLE;
      resources[i].extent = { base_res_container.extent.x, base_res_container.extent.y };
      resources[i].role = base_res.role;
      resources[i].usage = usage::undefined;
    }
  }

  const uint32_t cur_frame_index = base->current_frame_index();
  const uint32_t frames_in_flight_count = base->frames_in_flight();
  const uint32_t current_descriptor_index = cur_frame_index % frames_in_flight_count;

  descriptors.clear();
  descriptors.resize(base->descriptors.size(), VK_NULL_HANDLE);
  for (uint32_t i = 0; i < base->descriptors.size(); ++i) {
    descriptors[i] = base->descriptors[i].sets[current_descriptor_index];
  }
}

void graphics_ctx::draw() {
  if (!base->can_draw()) return;
  base->execution_graph.process(this, VK_NULL_HANDLE);
}

}
}
