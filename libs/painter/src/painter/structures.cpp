#include <algorithm>
#include <filesystem>
#include <functional>

#include "auxiliary.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/string-utils.hpp"
#include "devils_engine/utils/type_traits.h"
#include "graphics_base.h"
#include "gtl/phmap.hpp"
#include "makers.h"
#include "render_config_source.h"
#include "structures.h"
#include "tavl/tavl.h"
#include "vulkan_header.h"

namespace fs = std::filesystem;

namespace devils_engine {
namespace painter {

bool resource_container::is_image() const noexcept {
  return layers >= 1;
}

bool resource_container::host_visible() const noexcept {
  return mem_ptr != nullptr;
}

mesh_draw_group_pair::mesh_draw_group_pair() noexcept
  : mesh(UINT32_MAX), draw_group(UINT32_MAX), max_size(0), indirect_offset(0), instance_offset(0) {}

step_base::step_base() noexcept
  : descriptor(invalid_resource_slot),
    material(invalid_resource_slot),
    geometry(invalid_resource_slot),
    draw_group(invalid_resource_slot) {}

execution_pass_base::execution_pass_base() noexcept : render_target(invalid_resource_slot) {}

bool execution_pass_base::is_graphics_pass() const noexcept {
  return render_target != invalid_resource_slot;
}

bool execution_pass_base::has_step_type(const step_type::values t) const noexcept {
  return step_mask.test(static_cast<uint32_t>(t));
}

void execution_pass_base::set_step_type(const step_type::values t) noexcept {
  step_mask.set(static_cast<uint32_t>(t), true);
}

render_graph_base::render_graph_base() noexcept : present_source(UINT32_MAX) {}

#define DE_PAINTER_FIND(fn, vec)                                               \
  uint32_t render_config_storage::fn(const std::string_view& name) const {     \
    auto i = size_t{0};                                                        \
    for (; i < vec.size() && vec[i].name != name; ++i) {                       \
    }                                                                          \
    return i >= vec.size() ? invalid_resource_slot : static_cast<uint32_t>(i); \
  }
DE_PAINTER_FIND(find_constant_value, constant_values)
DE_PAINTER_FIND(find_resource, resources)
DE_PAINTER_FIND(find_counter, counters)
DE_PAINTER_FIND(find_constant, constants)
DE_PAINTER_FIND(find_render_target, render_targets)
DE_PAINTER_FIND(find_descriptor, descriptors)
DE_PAINTER_FIND(find_sampler, samplers)
DE_PAINTER_FIND(find_material, materials)
DE_PAINTER_FIND(find_geometry, geometries)
DE_PAINTER_FIND(find_draw_group, draw_groups)
DE_PAINTER_FIND(find_execution_step, steps)
DE_PAINTER_FIND(find_execution_pass, passes)
DE_PAINTER_FIND(find_render_graph, graphs)
#undef DE_PAINTER_FIND

constant_value::constant_value() noexcept : type(value_type::fixed), value{0, 0, 0}, scale{1, 1, 1}, presets_count(0), scale_presets_count(0), presets{}, scale_presets{}, current_value{0, 0, 0}, current_scale{1, 1, 1} {
  presets.fill(std::make_pair(preset::low, value_t{0, 0, 0}));
  for (auto& v : scale_presets) {
    v = std::make_pair(preset::low, scale_t{1, 1, 1});
  }
}
counter::counter() noexcept : value(0), next_value(0) {}
counter::counter(const counter& copy) noexcept : name(copy.name), value(copy.value.load(std::memory_order_acquire)), next_value(copy.next_value.load(std::memory_order_acquire)) {}
counter::counter(counter&& move) noexcept : name(std::move(move.name)), value(move.value.load(std::memory_order_acquire)), next_value(move.next_value.load(std::memory_order_acquire)) {}
counter& counter::operator=(const counter& copy) noexcept {
  name = copy.name;
  value.store(copy.value.load(std::memory_order_acquire), std::memory_order_release);
  next_value.store(copy.next_value.load(std::memory_order_acquire), std::memory_order_release);
  return *this;
}

counter& counter::operator=(counter&& move) noexcept {
  name = std::move(move.name);
  value.store(move.value.load(std::memory_order_acquire), std::memory_order_release);
  next_value.store(move.next_value.load(std::memory_order_acquire), std::memory_order_release);
  return *this;
}

void counter::push_value() noexcept {
  value.store(next_value.load(std::memory_order_acquire), std::memory_order_release);
}
uint32_t counter::get_value() const noexcept {
  return value.load(std::memory_order_acquire);
}
void counter::inc_next_value() noexcept {
  next_value.fetch_add(1, std::memory_order_release);
}
void counter::set_value(const uint32_t val) noexcept {
  next_value.store(val, std::memory_order_release);
}
resource::frame::frame() noexcept : index(0), view(VK_NULL_HANDLE), subimage{0, 0, 0, 0, 0}, subbuffer{0, 0} {}
resource::resource() noexcept : format_hint(VK_FORMAT_UNDEFINED), size_hint(0), size(UINT32_MAX), role(role::values::count), type(type::values::count), swap(0), usage_mask(0) {}
constant::constant() noexcept : size(0), offset(0) {}
uint32_t render_target::resource_index(const uint32_t res_id) const {
  uint32_t i = 0;
  for (; i < resources.size() && std::get<0>(resources[i]) != res_id; ++i) {
  }
  return i >= resources.size() ? UINT32_MAX : i;
}
descriptor::descriptor() noexcept : texture_count(0), texture_stage(VK_SHADER_STAGE_ALL), setlayout(VK_NULL_HANDLE) {
  sets.fill(VK_NULL_HANDLE);
}
sampler::sampler() noexcept : mag_filter(VK_FILTER_LINEAR), min_filter(VK_FILTER_LINEAR),
                              address_u(VK_SAMPLER_ADDRESS_MODE_REPEAT), address_v(VK_SAMPLER_ADDRESS_MODE_REPEAT), address_w(VK_SAMPLER_ADDRESS_MODE_REPEAT),
                              mipmap_mode(VK_SAMPLER_MIPMAP_MODE_LINEAR), handle(VK_NULL_HANDLE) {}
material::material() noexcept {}
geometry::geometry() noexcept : index_type(index_type::u32), topology_type(0), restart(false), stride(0) {}
draw_group::draw_group() noexcept : budget_constant(UINT32_MAX), types_constant(UINT32_MAX), type(type::device_local), instances_buffer(UINT32_MAX), indirect_buffer(UINT32_MAX), descriptor(UINT32_MAX), stride(0) {}
execution_pass_base::resource_info::resource_info() noexcept : slot(invalid_resource_slot), usage(usage::undefined), action(store_op::none) {}
execution_pass_base::resource_info::resource_info(const uint32_t slot, const usage::values usage, const store_op::values action) noexcept : slot(slot), usage(usage), action(action) {}
constexpr uint32_t default_color_blending = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
blend_data::blend_data() noexcept : enable(false),
                                    srcColorBlendFactor(0),
                                    dstColorBlendFactor(0),
                                    colorBlendOp(0),
                                    srcAlphaBlendFactor(0),
                                    dstAlphaBlendFactor(0),
                                    alphaBlendOp(0),
                                    colorWriteMask(default_color_blending) {}

buffer_frame::buffer_frame() noexcept : role(role::count), stride(0), sub({0, 0}), handle(VK_NULL_HANDLE), mapped(nullptr) {}
image_frame::image_frame() noexcept : role(role::count), vk_format(0), sub({0, 0, 0, 0, 0}), view(VK_NULL_HANDLE), handle(VK_NULL_HANDLE), mapped(nullptr) {}

constant_value::value_t constant_value::compute_value() const {
  const auto& [x, y, z] = current_value;
  const auto& [xs, ys, zs] = current_scale;
  return std::make_tuple(x * xs, y * ys, z * zs);
}

size_t constant_value::reduce_value() const {
  const auto& [x, y, z] = compute_value();
  return size_t(x) + size_t(y) + size_t(z);
}

void resource_container::create_container(VmaAllocator alc, const uint32_t host_visible) {
  vma::Allocator al(alc);

  if (is_image()) {
    vk::ImageCreateInfo ici{};
    ici.usage = static_cast<vk::ImageUsageFlags>(usage_mask);
    ici.format = static_cast<vk::Format>(format);
    ici.imageType = vk::ImageType::e2D;
    ici.initialLayout = vk::ImageLayout::eUndefined; // general?
    ici.samples = vk::SampleCountFlagBits::e1;
    ici.arrayLayers = layers;
    ici.mipLevels = 1;
    ici.extent = vk::Extent3D{extent.x, extent.y, 1u};
    ici.tiling = bool(host_visible) ? vk::ImageTiling::eLinear : vk::ImageTiling::eOptimal;

    // patterns from roles
    vma::AllocationCreateInfo aci{};
    aci.usage = bool(host_visible) ? vma::MemoryUsage::eCpuOnly : vma::MemoryUsage::eGpuOnly;

    if (
      aci.usage == vma::MemoryUsage::eCpuOnly ||
      aci.usage == vma::MemoryUsage::eCpuCopy ||
      aci.usage == vma::MemoryUsage::eCpuToGpu ||
      aci.usage == vma::MemoryUsage::eGpuToCpu) {
      aci.flags = aci.flags | vma::AllocationCreateFlagBits::eMapped;
    }

    auto [img_handle, allocation] = al.createImage(ici, aci);
    if (bool(host_visible)) {
      mem_ptr = al.mapMemory(allocation);
    }
    alloc = allocation;
    handle = std::bit_cast<size_t>(img_handle);

    set_name(allocator_device(alc), img_handle, name);
  } else {
    vk::BufferCreateInfo bci{};
    bci.usage = static_cast<vk::BufferUsageFlags>(usage_mask);
    bci.size = size;

    vma::AllocationCreateInfo aci{};
    aci.usage = bool(host_visible) ? vma::MemoryUsage::eCpuOnly : vma::MemoryUsage::eGpuOnly;
    if (
      aci.usage == vma::MemoryUsage::eCpuOnly ||
      aci.usage == vma::MemoryUsage::eCpuCopy ||
      aci.usage == vma::MemoryUsage::eCpuToGpu ||
      aci.usage == vma::MemoryUsage::eGpuToCpu) {
      aci.flags = aci.flags | vma::AllocationCreateFlagBits::eMapped;
    }

    auto [buf_handle, allocation] = al.createBuffer(bci, aci);
    if (bool(host_visible)) {
      mem_ptr = al.mapMemory(allocation);
    }

    alloc = allocation;
    handle = std::bit_cast<size_t>(buf_handle);

    set_name(allocator_device(alc), buf_handle, name);
  }
}

std::tuple<size_t, std::tuple<uint32_t, uint32_t>> resource::compute_frame_size(const graphics_base* base) const {
  const auto& size_value = DS_ASSERT_ARRAY_GET(base->constant_values, size);

  const auto [width, height] = base->swapchain_extent();

  auto image_extent = vk::Extent2D{width, height};
  size_t buffer_size = 0;
  if (role == role::present && size_value.type != value_type::screensize) {
    buffer_size = size_t(width) * size_t(height) * size_t(size_hint);
    image_extent = vk::Extent2D(width, height);
  } else if (size_value.type == value_type::screensize) {
    const auto& [xs, ys, zs] = size_value.current_scale;
    buffer_size = size_t(width * xs) * size_t(height * ys) * size_t(size_hint);
    image_extent = vk::Extent2D(width * xs, height * ys);
  } else if (size_value.type == value_type::fixed) {
    const auto& [x, y, z] = size_value.current_value;
    const auto& [xs, ys, zs] = size_value.current_scale;
    buffer_size = size_t(x * xs) * size_t(size_hint);
    image_extent = vk::Extent2D(x * xs, x * xs);
  } else if (size_value.type == value_type::fixed_2d) {
    const auto& [x, y, z] = size_value.current_value;
    const auto& [xs, ys, zs] = size_value.current_scale;
    buffer_size = size_t(x * xs) * size_t(y * ys) * size_t(size_hint);
    image_extent = vk::Extent2D(x * xs, y * ys);
  } else if (size_value.type == value_type::fixed_3d) {
    const auto& [x, y, z] = size_value.current_value;
    const auto& [xs, ys, zs] = size_value.current_scale;
    buffer_size = size_t(x * xs) * size_t(y * ys) * size_t(z * zs) * size_t(size_hint);
    image_extent = vk::Extent2D(x * xs, y * ys);
  }

  // наверное указать что за глобальная юниформа? а сколько их бывает то
  if (role == role::global_uniform) {
    // константа (данные камеры)
    buffer_size = size_hint; // ???
  } else if (role == role::frame_constants) {
    // константа (счетчик фреймов + время + ???)
    buffer_size = size_hint; // ???
  } else if (role::is_indirect(role)) {
    // размер одного буфера фиксирован
    buffer_size = indirect_buffer_size * size_hint;
  }

  return std::make_tuple(buffer_size, std::make_tuple(image_extent.width, image_extent.height));
}

size_t resource::compute_size(const graphics_base* base) const {
  const auto [buffer_size, ext] = compute_frame_size(base);
  return buffer_size * compute_buffering(base);
}

uint32_t resource::compute_buffering(const graphics_base* base) const {
  return type::compute_buffering(base, type);
}

size_t geometry::index_size() const {
  switch (index_type) {
    case index_type::u32: return sizeof(uint32_t);
    case index_type::u16: return sizeof(uint16_t);
    case index_type::u8: return sizeof(uint8_t);
    default: break;
  }

  return 0;
}

struct base_layout {
  format::values fmt_id;
  uint32_t components;
  uint32_t size;
  vk::Format format;
};

static std::vector<format::values> parse_layout(const std::string_view& str) {
  if (str.empty()) {
    return {};
  }
  if (isdigit(str[0])) {
    return {};
  }

  // нас инресуют сиквенсы букв, после них сиквенсы цифр
  size_t i = 0;
  std::vector<format::values> parts;
  while (i < str.size()) {
    const size_t start = i;
    for (; i < str.size() && !isdigit(str[i]); ++i) {
    }
    for (; i < str.size() && isdigit(str[i]); ++i) {
    }

    const auto part = str.substr(start, i - start);
    const auto fmt_id = format::from_string(part);
    if (fmt_id >= format::count) {
      utils::error{}("Could not parse format part '{}' from format '{}'", part, str);
    }

    if (fmt_id == format::mat4 || fmt_id == format::mat44) {
      parts.insert(parts.end(), 4, format::v4);
      continue;
    }

    parts.push_back(fmt_id);
  }

  /*for (; i < str.size(); ++i) {
    if (isdigit(str[i])) cur_letter = false;

    if (!isdigit(str[i]) && !cur_letter) {
      const auto part = str.substr(k, i - k);
      const uint32_t fmt_id = format::from_string(part);
      const auto l = base_layout{
        format::el_count(fmt_id),
        format::size(fmt_id),
        static_cast<vk::Format>(format::to_vulkan_format(fmt_id))
      };
      parts.push_back(l);
      cur_letter = true;
      k = i;
    }
  }*/

  /*const auto part = str.substr(k, i - k);
  const uint32_t fmt_id = format::from_string(part);
  const auto l = base_layout{
    format::el_count(fmt_id),
    format::size(fmt_id),
    static_cast<vk::Format>(format::to_vulkan_format(fmt_id))
  };

  parts.push_back(l);
  cur_letter = true;
  k = i;*/

  return parts;
}

// цель этой функции? однозначно посчитать размер в байтах
// по идее некоторые типы без паддинга могут привести к фигне и навреное нужно ворнинг кидать?
static size_t compute_size(const std::span<const format::values>& layout, const std::string_view& type_hint, const std::string_view& name_hint) {
  size_t size = 0;
  auto prev_fmt = format::count;
  uint32_t prev_channels = 0;
  uint32_t prev_elem_size = 0;
  bool prev_is_pad_or_indirect = true;
  for (const auto fmt : layout) {
    if (fmt == format::pad1) {
      prev_elem_size = 1 * sizeof(uint32_t);
      size += prev_elem_size;
      prev_fmt = fmt;
      prev_channels = 1;
      prev_is_pad_or_indirect = true;
      continue;
    }

    if (fmt == format::pad2) {
      prev_elem_size = 2 * sizeof(uint32_t);
      size += prev_elem_size;
      prev_fmt = fmt;
      prev_channels = 2;
      prev_is_pad_or_indirect = true;
      continue;
    }

    if (fmt == format::pad3) {
      prev_elem_size = 3 * sizeof(uint32_t);
      size += prev_elem_size;
      prev_fmt = fmt;
      prev_channels = 3;
      prev_is_pad_or_indirect = true;
      continue;
    }

    if (fmt == format::dispatch3) {
      size += 3 * sizeof(uint32_t);
      prev_fmt = fmt;
      prev_channels = 3;
      prev_is_pad_or_indirect = true;
      continue;
    }

    if (fmt == format::draw4) {
      size += 4 * sizeof(uint32_t);
      prev_fmt = fmt;
      prev_channels = 4;
      prev_is_pad_or_indirect = true;
      continue;
    }

    if (fmt == format::indexed5) {
      size += 5 * sizeof(uint32_t);
      prev_fmt = fmt;
      prev_channels = 5;
      prev_is_pad_or_indirect = true;
      continue;
    }

    // вообще может быть много чем...
    // а и потом 4 * sizeof(uint32_t) это много...
    if (fmt == format::swapchain4) {
      size += 4 * sizeof(uint32_t);
      prev_fmt = fmt;
      prev_channels = 4;
      prev_is_pad_or_indirect = true;
      continue;
    }

    const uint32_t vk_fmt = format::to_vulkan_format(fmt);
    const uint32_t channels = format_channel_count(vk_fmt);
    const uint32_t elem_size = format_element_size(vk_fmt, format::to_vulkan_aspect(fmt));
    // alignof(vec4) == 16, alignof(vec3) == 16, alignof(vec2) == 8, alignof(vec1) == 4
    const uint32_t final_elem_size = utils::align_to(std::max(size_t(elem_size), sizeof(uint32_t)), sizeof(uint32_t));

    // warning?
    if (!prev_is_pad_or_indirect && (prev_elem_size / prev_channels) >= sizeof(uint32_t) && (prev_elem_size / prev_channels) < (elem_size / channels)) {
      utils::warn("Previous fmt '{}' in layout of {} '{}' might need explicit padding before '{}' in some contexts", format::to_string(prev_fmt), type_hint, name_hint, format::to_string(fmt));
    }

    size += final_elem_size;

    prev_fmt = fmt;
    prev_channels = channels;
    prev_elem_size = elem_size;
    prev_is_pad_or_indirect = false;
  }

  return size;
}

struct constant_value_mirror {
  std::string name;
  std::string type;
  constant_value::value_t value = {0, 0, 0};
  constant_value::scale_t scale = {1, 1, 1};
  std::vector<std::tuple<std::string, constant_value::value_t>> presets;
  std::vector<std::tuple<std::string, constant_value::value_t>> scale_presets;

  constant_value convert(const render_config_storage& ctx) const {
    const uint32_t exists = ctx.find_constant_value(name);
    if (exists != UINT32_MAX) {
      utils::error{}("Constant value '{}' is already exists", name);
    }

    constant_value cv;
    cv.name = name;
    cv.type = check(value_type::from_string(type), type, name);
    cv.value = value;
    cv.scale = scale;
    cv.presets_count = presets.size();
    cv.scale_presets_count = scale_presets.size();
    if (cv.presets_count > preset::count) {
      utils::error{}("Too many presets in constant value '{}'", name);
    }
    if (cv.scale_presets_count > preset::count) {
      utils::error{}("Too many scale_presets in constant value '{}'", name);
    }
    for (size_t i = 0; i < presets.size(); ++i) {
      const auto& [p_name, value] = presets[i];
      cv.presets[i] = std::make_tuple(check(preset::from_string(p_name), p_name, name), value);
    }
    for (size_t i = 0; i < scale_presets.size(); ++i) {
      const auto& [p_name, value] = scale_presets[i];
      cv.scale_presets[i] = std::make_tuple(check(preset::from_string(p_name), p_name, name), value);
    }

    cv.current_value = cv.value;
    cv.current_scale = cv.scale;

    return cv;
  }
};

struct resource_mirror {
  std::string name;
  std::string format;
  std::string role;
  std::string size;
  std::string type;
  std::string swap;

  // mirror context
  resource convert(const render_config_storage& ctx) const {
    const uint32_t exists = ctx.find_resource(name);
    if (exists != UINT32_MAX) {
      utils::error{}("Resource '{}' is already exists", name);
    }

    resource res;
    res.name = name;
    res.format = format;
    res.role = check(role::from_string(role), role, name);
    //res.size = check(size::from_string(size), size, name);
    res.size = check(ctx.find_constant_value(size), "constant_value", size, name);
    res.type = check(type::from_string(type), type, name);
    // создавать на месте? хардкодить? честно говоря было бы неплохо задекларировать где то
    res.swap = check(ctx.find_counter(swap), "counter", swap, name);
    res.usage_mask = 0;

    if (role::is_image(res.role) || role::is_attachment(res.role)) {
      const auto fmt_id = format::from_string(res.format);
      if (fmt_id >= format::count) {
        utils::error{}("Could not parse format '{}' for role '{}', resource '{}'", res.format, role, res.name);
      }
      res.format_hint = format::to_vulkan_format(fmt_id);
      res.size_hint = format::size(fmt_id);
    } else {
      const auto& meta = parse_layout(res.format);
      res.format_hint = UINT32_MAX;
      res.size_hint = compute_size(meta, "resource", name);
    }

    return res;
  }
};

struct constant_mirror {
  std::string name;
  std::string layout;
  std::vector<double> value;

  constant convert(const render_config_storage& ctx) const {
    const uint32_t exists = ctx.find_constant(name);
    if (exists != UINT32_MAX) {
      utils::error{}("Constant '{}' is already exists", name);
    }

    constant c;
    c.name = name;
    c.layout_str = layout;
    c.size = 0;
    c.offset = 0;
    c.value = value;

    c.layout = parse_layout(layout);
    c.size = compute_size(c.layout, "constant", name);

    return c;
  }
};

static std::tuple<uint32_t, uint32_t, uint32_t> parse_blend_exp(const std::string_view& exp) {
  if (exp.empty()) {
    return std::make_tuple(UINT32_MAX, UINT32_MAX, UINT32_MAX);
  }
  std::array<std::string_view, 3> parts;
  const auto& [part1, remaining] = utils::string::split_prefix(utils::string::trim(exp), " ");
  const auto& [part2, part3_raw] = utils::string::split_prefix(utils::string::trim(remaining), " ");
  const auto part3 = utils::string::trim(part3_raw);
  const uint32_t f1 = check(blend_factor::from_string(part1), "blend_factor", parts[0]);
  const uint32_t op = check(blend_op::from_string(part2), "blend_op", parts[1]);
  const uint32_t f2 = check(blend_factor::from_string(part3), "blend_factor", parts[2]);
  return std::make_tuple(f1, op, f2);
}

struct blend_data_mirror {
  bool enable = false;
  std::string color;
  std::string alpha;
  std::string mask;

  blend_data convert() const {
    blend_data bd;
    bd.enable = enable;
    const auto& [srcColorBlendFactor, colorBlendOp, dstColorBlendFactor] = parse_blend_exp(color);
    const auto& [srcAlphaBlendFactor, alphaBlendOp, dstAlphaBlendFactor] = parse_blend_exp(alpha);
    bd.srcColorBlendFactor = srcColorBlendFactor;
    bd.colorBlendOp = colorBlendOp;
    bd.dstColorBlendFactor = dstColorBlendFactor;
    bd.srcAlphaBlendFactor = srcAlphaBlendFactor;
    bd.alphaBlendOp = alphaBlendOp;
    bd.dstAlphaBlendFactor = dstAlphaBlendFactor;
    if (mask.empty()) {
      bd.colorWriteMask = default_color_blending;
    }
    for (const auto c : mask) {
      if (c == 'r') {
        bd.colorWriteMask = bd.colorWriteMask | VK_COLOR_COMPONENT_R_BIT;
      }
      if (c == 'g') {
        bd.colorWriteMask = bd.colorWriteMask | VK_COLOR_COMPONENT_G_BIT;
      }
      if (c == 'b') {
        bd.colorWriteMask = bd.colorWriteMask | VK_COLOR_COMPONENT_B_BIT;
      }
      if (c == 'a') {
        bd.colorWriteMask = bd.colorWriteMask | VK_COLOR_COMPONENT_A_BIT;
      }
    }

    return bd;
  }
};

// вторым аргументом тут бы задать clearvalue
struct render_target_mirror {
  std::string name;
  std::vector<std::tuple<std::string, std::string>> resources;
  std::vector<blend_data_mirror> blending;

  render_target convert(const render_config_storage& ctx) const {
    const uint32_t exists = ctx.find_render_target(name);
    if (exists != UINT32_MAX) {
      utils::error{}("Render target '{}' is already exists", name);
    }

    render_target rt;
    rt.name = name;
    for (const auto& [res, type] : resources) {
      const uint32_t index = check(ctx.find_resource(res), "resource", res, name);
      const auto type_id = check(usage::from_string(type), type, name);
      if (!usage::is_attachment(type_id)) {
        utils::error{}("Could not parse attachment type '{}', render target: {}", type, name);
      }
      rt.resources.push_back(std::make_tuple(index, type_id));
    }

    rt.default_blending.resize(rt.resources.size());
    for (size_t i = 0; i < std::min(blending.size(), rt.default_blending.size()); ++i) {
      rt.default_blending[i] = blending[i].convert();
    }

    return rt;
  }
};

static uint32_t parse_filter(const std::string_view& s, const std::string_view& owner) {
  if (s == "linear") {
    return VK_FILTER_LINEAR;
  }
  if (s == "nearest") {
    return VK_FILTER_NEAREST;
  }
  utils::error{}("Unknown sampler filter '{}' (sampler '{}')", s, owner);
  return VK_FILTER_LINEAR;
}

static uint32_t parse_address(const std::string_view& s, const std::string_view& owner) {
  if (s == "repeat") {
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
  if (s == "mirror") {
    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  }
  if (s == "clamp") {
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  }
  if (s == "border") {
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  }
  utils::error{}("Unknown sampler address mode '{}' (sampler '{}')", s, owner);
  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

// 'fragment | vertex | ...' или 'all'. Пустая строка => all.
static uint32_t parse_shader_stages(const std::string_view& s) {
  if (s.empty()) {
    return VK_SHADER_STAGE_ALL;
  }
  uint32_t flags = 0;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find('|', i);
    if (j == std::string_view::npos) {
      j = s.size();
    }
    auto tok = s.substr(i, j - i);
    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) {
      tok.remove_prefix(1);
    }
    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) {
      tok.remove_suffix(1);
    }
    if (tok == "all") {
      return VK_SHADER_STAGE_ALL;
    } else if (tok == "vertex") {
      flags |= VK_SHADER_STAGE_VERTEX_BIT;
    } else if (tok == "fragment") {
      flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    } else if (tok == "compute") {
      flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    } else if (tok == "geometry") {
      flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
    } else if (tok == "tess_control") {
      flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    } else if (tok == "tess_eval") {
      flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    } else if (!tok.empty()) {
      utils::error{}("Unknown shader stage '{}'", tok);
    }
    i = j + 1;
  }
  return flags == 0 ? uint32_t(VK_SHADER_STAGE_ALL) : flags;
}

struct sampler_mirror {
  std::string name;
  std::string filter = "linear";
  std::string address = "repeat";
  // под доращивание: mipmap, anisotropy, lod, border_color, compare

  sampler convert(const render_config_storage& /*ctx*/) const {
    sampler s;
    s.name = name;
    const uint32_t f = parse_filter(filter, name);
    s.mag_filter = f;
    s.min_filter = f;
    const uint32_t a = parse_address(address, name);
    s.address_u = a;
    s.address_v = a;
    s.address_w = a;
    return s;
  }
};

struct descriptor_mirror {
  // запись layout: resource + usage (+ опц. sampler, +опц. shader-стадии).
  // если задан sampler — binding станет combinedImageSampler (см. create_descriptor_set_layouts).
  struct entry {
    std::string resource;
    std::string usage;
    std::string sampler;       // пусто => без сэмплера
    std::string stage = "all"; // 'fragment | vertex | ...' либо 'all'
  };

  std::string name;
  std::vector<entry> layout;

  // asset-текстурный binding (опционально): texture_count картинок из assets_base + ПУЛ семплеров
  // (sampler_pool — immutable, binding L+1; шейдер берёт по sampler_id из id). bindless v2.
  uint32_t texture_count = 0;
  std::vector<std::string> sampler_pool; // напр. [linear, nearest]; индекс = sampler_id в tex_id
  std::string texture_stage = "fragment";

  descriptor convert(const render_config_storage& ctx) const {
    const uint32_t exists = ctx.find_descriptor(name);
    if (exists != UINT32_MAX) {
      utils::error{}("Descriptor '{}' is already exists", name);
    }

    descriptor d;
    d.name = name;
    if (texture_count > 0) {
      d.texture_count = texture_count;
      d.texture_stage = parse_shader_stages(texture_stage);
      if (sampler_pool.empty()) {
        utils::error{}("Descriptor '{}' has texture_count but empty sampler_pool", name);
      }
      for (const auto& sname : sampler_pool) {
        const uint32_t si = ctx.find_sampler(sname);
        if (si == UINT32_MAX) {
          utils::error{}("Sampler '{}' not found (descriptor '{}')", sname, name);
        }
        d.texture_samplers.push_back(si);
      }
    }
    for (const auto& e : layout) {
      const uint32_t res_index = check(ctx.find_resource(e.resource), "resource", e.resource, name);
      const auto usage_value = check(usage::from_string(e.usage), e.usage, name);
      uint32_t sampler_index = UINT32_MAX;
      if (!e.sampler.empty()) {
        sampler_index = ctx.find_sampler(e.sampler);
        if (sampler_index == UINT32_MAX) {
          utils::error{}("Sampler '{}' not found (descriptor '{}')", e.sampler, name);
        }
      }
      const uint32_t stages = parse_shader_stages(e.stage);
      d.layout.push_back(std::make_tuple(res_index, usage_value, sampler_index, stages));
    }
    return d;
  }
};

struct material_mirror {
  struct shaders {
    std::string vertex;
    std::string tesselation_control;
    std::string tesselation_evaluation;
    std::string geometry;
    std::string fragment;
    std::string compute;
  };

  struct raster {
    bool depth_clamp = false;
    bool raster_discard = false;
    bool depth_bias = false;
    std::string polygon;
    std::string cull;
    std::string front_face;
    float bias_constant = 0.0f;
    float bias_clamp = 0.0f;
    float bias_slope = 0.0f;
    float line_width = 0.0f;
  };

  struct depth {
    struct stencil_op_state {
      std::string fail_op;
      std::string pass_op;
      std::string depth_fail_op;
      std::string compare_op;
      uint32_t compare_mask = 0;
      uint32_t write_mask = 0;
      uint32_t reference = 0;
    };

    bool test = false;
    bool write = false;
    bool bounds_test = false;
    bool stencil_test = false;
    std::string compare;
    stencil_op_state front;
    stencil_op_state back;
    float min_bounds = 0.0f;
    float max_bounds = 0.0f;
  };

  std::string name;
  struct material::shaders shaders;
  struct raster raster;
  struct depth depth;

  material convert(const render_config_storage& ctx) const {
    const uint32_t exists = ctx.find_material(name);
    if (exists != UINT32_MAX) {
      utils::error{}("Material '{}' is already exists", name);
    }

    material m;
    m.name = name;
    m.shaders = shaders;
    m.raster.depth_clamp = raster.depth_clamp;
    m.raster.raster_discard = raster.raster_discard;
    m.raster.depth_bias = raster.depth_bias;
    m.raster.polygon = check(polygon_mode::from_string(raster.polygon), "polygon_mode", raster.polygon, m.name);
    m.raster.cull = check(cull_mode::from_string(raster.cull), "cull_mode", raster.cull, m.name);
    m.raster.front_face = check(front_face::from_string(raster.front_face), "front_face", raster.front_face, m.name);
    m.raster.bias_constant = raster.bias_constant;
    m.raster.bias_clamp = raster.bias_clamp;
    m.raster.bias_slope = raster.bias_slope;
    m.raster.line_width = raster.line_width;
    m.depth.test = depth.test;
    m.depth.write = depth.write;
    m.depth.compare = check(compare_op::from_string(depth.compare), "compare_op", depth.compare, m.name);
    m.depth.bounds_test = depth.bounds_test;
    m.depth.stencil_test = depth.stencil_test;
    if (m.depth.stencil_test) {
      m.depth.front.fail_op = check(stencil_op::from_string(depth.front.fail_op), "stencil_op", depth.front.fail_op, m.name);
      m.depth.front.pass_op = check(stencil_op::from_string(depth.front.pass_op), "stencil_op", depth.front.pass_op, m.name);
      m.depth.front.depth_fail_op = check(stencil_op::from_string(depth.front.depth_fail_op), "stencil_op", depth.front.depth_fail_op, m.name);
      m.depth.front.compare_op = check(compare_op::from_string(depth.front.compare_op), "compare_op", depth.front.compare_op, m.name);
      m.depth.front.compare_mask = depth.front.compare_mask;
      m.depth.front.write_mask = depth.front.write_mask;
      m.depth.front.reference = depth.front.reference;
      m.depth.back.fail_op = check(stencil_op::from_string(depth.back.fail_op), "stencil_op", depth.back.fail_op, m.name);
      m.depth.back.pass_op = check(stencil_op::from_string(depth.back.pass_op), "stencil_op", depth.back.pass_op, m.name);
      m.depth.back.depth_fail_op = check(stencil_op::from_string(depth.back.depth_fail_op), "stencil_op", depth.back.depth_fail_op, m.name);
      m.depth.back.compare_op = check(compare_op::from_string(depth.back.compare_op), "compare_op", depth.back.compare_op, m.name);
      m.depth.back.compare_mask = depth.back.compare_mask;
      m.depth.back.write_mask = depth.back.write_mask;
      m.depth.back.reference = depth.back.reference;
    }
    m.depth.min_bounds = depth.min_bounds;
    m.depth.max_bounds = depth.max_bounds;
    return m;
  }
};

struct geometry_mirror {
  std::string name;
  std::string vertex_layout;
  std::string index_type;
  std::string topology;
  bool restart = false;

  geometry convert(const render_config_storage& ctx) const {
    const uint32_t exists = ctx.find_geometry(name);
    if (exists != UINT32_MAX) {
      utils::error{}("Geometry '{}' is already exists", name);
    }

    geometry g;
    g.name = name;
    g.layout_str = vertex_layout;
    g.vertex_layout = parse_layout(vertex_layout);
    g.stride = compute_size(g.vertex_layout, "geometry", name);

    g.index_type = geometry::index_type::none;
    g.index_type = index_type == "u32" ? geometry::index_type::u32 : g.index_type;
    g.index_type = index_type == "u16" ? geometry::index_type::u16 : g.index_type;
    g.index_type = index_type == "u8" ? geometry::index_type::u8 : g.index_type;
    if (g.index_type == geometry::index_type::none && !index_type.empty() && index_type != "none") {
      utils::error{}("Could not parse geometry '{}' index type '{}'", name, index_type);
    }
    g.topology_type = check(primitive_topology::from_string(topology), "primitive_topology", topology, name);
    g.restart = restart;
    return g;
  }
};

struct draw_group_mirror {
  std::string name;
  std::string layout;
  std::string budget;
  std::string draw_capacity;
  std::string type;

  draw_group convert(const render_config_storage& ctx) const {
    const uint32_t exists = ctx.find_draw_group(name);
    if (exists != UINT32_MAX) {
      utils::error{}("Draw group '{}' is already exists", name);
    }

    draw_group dg;
    dg.name = name;
    dg.layout_str = layout; // не может быть пустым
    dg.budget_constant = check(ctx.find_constant_value(budget), "constant_value", budget, name);
    dg.types_constant = check(ctx.find_constant_value(draw_capacity), "constant_value", draw_capacity, name);
    dg.type = draw_group::type::device_local;
    if (type == "host_visible") {
      dg.type = draw_group::type::host_visible;
    }
    dg.instance_layout = parse_layout(layout);
    dg.stride = compute_size(dg.instance_layout, "draw_group", name);
    return dg;
  }
};

struct pass2_mirror {
  std::string name;
  std::string render_target;
  std::vector<std::unordered_map<std::string, std::tuple<std::string, std::string>>> subpasses;
  std::vector<std::string> steps; // ключевое слово next_subpass

  std::vector<std::string> wait_for;
  std::vector<std::string> signal;
};

struct pass_step2_mirror {
  std::string name;
  //std::string type;
  std::unordered_map<std::string, blend_data_mirror> blending;
  std::vector<std::tuple<std::string, std::string>> barriers;
  std::vector<std::tuple<std::string, std::string>> resources;
  std::vector<std::string> sets;
  std::vector<std::string> push_constants;
  std::string material;
  std::string geometry;
  std::string draw_group;
  std::string command;
};

struct render_graph2_mirror {
  std::string name;
  std::vector<pass2_mirror> passes;
  std::string present_source;
};

static void parse_step2(
  const pass_step2_mirror& data,
  render_config_storage& ctx,
  step_base& step) {
  if (ctx.find_execution_step(data.name) != UINT32_MAX) {
    utils::error{}("Execution step with name '{}' is already exists", data.name);
  }

  step.name = data.name;
  for (const auto& [res_name, data] : data.blending) {
    const uint32_t index = check(ctx.find_resource(res_name), "resource", res_name, step.name);
    step.blending.emplace_back(std::make_tuple(index, data.convert()));
  }

  for (const auto& [res_name, usage_str] : data.barriers) {
    const uint32_t index = check(ctx.find_resource(res_name), "resource", res_name, step.name);
    const auto usage = check(usage::from_string(usage_str), usage_str, step.name);
    step.barriers.push_back(std::make_tuple(index, usage));

    if (usage::is_read(usage)) {
      step.read.set(index, true);
    }
    if (usage::is_write(usage)) {
      step.write.set(index, true);
    }
  }

  step.descriptor = UINT32_MAX;
  if (!data.resources.empty()) {
    auto name = step.name + ".local_descriptor";
    if (ctx.find_descriptor(name) != UINT32_MAX) {
      utils::error{}("Descriptor name '{}' is already exists, step '{}'", name, step.name);
    }
    step.descriptor = ctx.descriptors.size();
    ctx.descriptors.emplace_back();
    ctx.descriptors.back().name = std::move(name);
  }

  for (const auto& [res_name, usage_str] : data.resources) {
    const uint32_t index = check(ctx.find_resource(res_name), "resource", res_name, step.name);
    const auto usage = check(usage::from_string(usage_str), usage_str, step.name);
    step.barriers.push_back(std::make_tuple(index, usage));
    ctx.descriptors[step.descriptor].layout.push_back(std::make_tuple(index, usage, UINT32_MAX, uint32_t(VK_SHADER_STAGE_ALL)));

    if (usage::is_read(usage)) {
      step.read.set(index, true);
    }
    if (usage::is_write(usage)) {
      step.write.set(index, true);
    }
  }

  for (const auto& descriptor_name : data.sets) {
    if (step.descriptor != UINT32_MAX && descriptor_name == "local") {
      step.sets.push_back(step.descriptor);
      continue;
    }

    const uint32_t index = check(ctx.find_descriptor(descriptor_name), "descriptor", descriptor_name, step.name);
    step.sets.push_back(index);
  }

  for (const auto& constant_name : data.push_constants) {
    const uint32_t index = check(ctx.find_constant(constant_name), "constant", constant_name, step.name);
    step.push_constants.push_back(index);
  }

  if (!data.material.empty()) {
    step.material = check(ctx.find_material(data.material), "material", data.material, step.name);
  }
  if (!data.geometry.empty()) {
    step.geometry = check(ctx.find_geometry(data.geometry), "geometry", data.geometry, step.name);
  }
  if (!data.draw_group.empty()) {
    step.draw_group = check(ctx.find_draw_group(data.draw_group), "draw_group", data.draw_group, step.name);
  }
  step.command = data.command;

  if (step.command.empty() && data.draw_group.empty()) {
    utils::error{}("Either command or draw group must be specified, step '{}'", step.name);
  }
  const auto csv = std::string_view(step.command).substr(0, step.command.find(' '));
  const bool is_graphics = csv == "draw" || step.draw_group != UINT32_MAX;
  const bool is_compute = csv == "dispatch";

  if (is_graphics && step.geometry == UINT32_MAX) {
    utils::error{}("Graphics step must specify geometry, step '{}'", step.name);
  }

  if ((is_graphics || is_compute) && step.material == UINT32_MAX) {
    utils::error{}("Graphics or compute step must specify material, step '{}'", step.name);
  }

  // парсим команду где? да можно и тут так то
  step.cmd_params = parse_command(&ctx, step, step.command);

  for (uint32_t i = 0; i < step.cmd_params.resources.size() && std::get<0>(step.cmd_params.resources[i]) != invalid_resource_slot; ++i) {
    const auto& [cmd_res, cmd_usage] = step.cmd_params.resources[i];

    const auto itr = std::find_if(step.barriers.begin(), step.barriers.end(), [&](const auto& data) {
      return cmd_res == std::get<0>(data);
    });
    if (itr != step.barriers.end()) {
      const auto& [index, usage] = *itr;
      if (cmd_usage != usage) {
        const auto& res = ctx.resources[index];
        utils::error{}("Execution step '{}' has resource '{}' barriers conflict, '{}' != '{}'", step.name, res.name, usage::to_string(cmd_usage), usage::to_string(usage));
      } else {
        step.barriers.push_back(step.cmd_params.resources[i]);
      }
    } else {
      step.barriers.push_back(step.cmd_params.resources[i]);
    }

    if (usage::is_read(cmd_usage)) {
      step.read.set(cmd_res, true);
    }
    if (usage::is_write(cmd_usage)) {
      step.write.set(cmd_res, true);
    }
  }

  // так нужно еще юсаджи собрать в ресурсы
}

static void parse_execution_pass2(
  const pass2_mirror& data,
  render_config_storage& ctx,
  execution_pass_base& pass) {
  using ri = execution_pass_base::resource_info;
  if (ctx.find_execution_pass(data.name) != invalid_resource_slot) {
    utils::error{}("Execution pass with name '{}' is already exists", data.name);
  }

  pass.name = data.name;
  if (!data.render_target.empty()) {
    pass.render_target = check(ctx.find_render_target(data.render_target), "render_target", data.render_target, pass.name);
  }
  const bool is_render_pass = pass.render_target != invalid_resource_slot;

  pass.wait_for = data.wait_for;
  pass.signal = data.signal;

  for (const auto& step_name : data.steps) {
    if (step_name == "next_subpass") {
      pass.steps.push_back(UINT32_MAX);
      continue;
    }

    //const uint32_t index = check(ctx.find_execution_pass(step_name), "execution_pass", step_name, pass.name);
    const uint32_t index = check(ctx.find_execution_step(step_name), "execution_step", step_name, pass.name);
    const auto& step = ctx.steps[index];

    if (is_render_pass && command::is_compute(step.cmd_params.type)) {
      utils::error{}("Trying to add compute step '{}' to graphics execution pass '{}'", step.name, pass.name);
    }

    if (!is_render_pass && command::is_graphics(step.cmd_params.type)) {
      utils::error{}("Trying to add graphics step '{}' to compute execution pass '{}'", step.name, pass.name);
    }

    pass.steps.push_back(index);

    pass.read = pass.read | step.read;
    pass.write = pass.write | step.write;

    const auto t = step.type();
    pass.step_mask.set(t, true);
  }

  for (const auto& subpass : data.subpasses) {
    if (is_render_pass) {
      const auto& rt = DS_ASSERT_ARRAY_GET(ctx.render_targets, pass.render_target);
      pass.subpasses.emplace_back();
      pass.subpasses.back().resize(rt.resources.size());
    }

    pass.barriers.emplace_back();
    for (const auto& [res_name, data] : subpass) {
      const auto& [usage_str, action_str] = data;
      const uint32_t index = check(ctx.find_resource(res_name), "resource", res_name, pass.name);
      const auto usage = check(usage::from_string(usage_str), usage_str, pass.name);
      const auto action = check(store_op::from_string(action_str), action_str, pass.name);

      const auto& info = ri{index, usage, action};

      if (usage::is_read(usage)) {
        pass.read.set(index, true);
      }
      if (usage::is_write(usage)) {
        pass.write.set(index, true);
      }

      if (is_render_pass) {
        const auto& rt = ctx.render_targets[pass.render_target];
        const uint32_t id = rt.resource_index(index);
        if (id < rt.resources.size()) {
          pass.subpasses.back()[id] = info;
        } else {
          pass.barriers.back().push_back(info);
        }
      } else {
        pass.barriers.back().push_back(info);
      }
    }
  }
}

static void parse_render_graph2(
  const render_graph2_mirror& data,
  render_config_storage& ctx,
  render_graph_base& graph) {
  if (ctx.find_render_graph(data.name) != UINT32_MAX) {
    utils::error{}("Render graph with name '{}' is already exists", data.name);
  }

  graph.name = data.name;
  for (const auto& pass : data.passes) {
    const uint32_t index = ctx.passes.size();
    ctx.passes.emplace_back();

    parse_execution_pass2(pass, ctx, ctx.passes.back());
    graph.passes.push_back(index);
  }

  graph.present_source = check(ctx.find_resource(data.present_source), "resource", data.present_source, graph.name);
}

// tavl: каждый top-level блок/строка в файле = один инстанс T (см. deserialize_next).
// один и тот же цикл читает и одиночную запись, и "list" файл из нескольких записей.
template <typename T>
static std::vector<T> parse_config_content(const std::string_view content, const std::string& label, const uint32_t line_offset = 0) {
  tavl::parser p;
  p.add_default_operator();
  p.flush(std::string(content));
  p.finish();

  tavl::ct_context ctx;
  std::vector<T> arr;
  T val{};
  while (tavl::deserialize_next(p, ctx, val)) {
    arr.emplace_back(std::move(val));
    val = T{};
  }

  for (const auto& d : ctx.diagnostics) {
    if (!d.error.is_critical()) {
      continue;
    }
    const uint32_t line = d.error.span.line == 0 ? 0 : static_cast<uint32_t>(d.error.span.line) + line_offset;
    utils::warn("Could not parse config '{}' as type '{}': error '{}' at {}:{} field '{}'",
                label, utils::type_name<T>(), tavl::to_string(d.error.type), line, d.error.span.column, d.field);
  }

  return arr;
}

template <typename T>
std::vector<T> parse_file(const std::string& file) {
  if (!file_io::exists(file)) {
    utils::warn("File '{}' not found", file);
    return {};
  }

  const auto content = file_io::read(file);
  return parse_config_content<T>(content, file);
}

template <typename T>
std::vector<T> parse_folder(const std::string& folder) {
  std::vector<T> arr;

  // папки может не быть (напр. descriptors/ или execution_passes/) - directory_iterator кинул бы исключение
  if (!fs::exists(folder) || !fs::is_directory(folder)) {
    return arr;
  }

  for (const auto& entry : fs::directory_iterator(folder)) {
    if (!entry.is_regular_file()) {
      utils::warn("Ignore '{}'", entry.path().generic_string());
      continue;
    }

    const auto content = file_io::read(entry.path().generic_string());
    auto local_arr = parse_config_content<T>(content, entry.path().generic_string());
    for (auto& el : local_arr) {
      arr.emplace_back(std::move(el));
    }
  }

  return arr;
}

// Источник текста render-config, абстрагированный от места хранения. parse_data кормится
// им, не зная, откуда пришёл текст (папка на диске или движковый demiurg-реестр).
//   read_file("declare_values.tavl") -> текст одиночного файла ("" если нет)
//   read_folder("constants/")        -> тексты всех файлов категории
struct config_source {
  struct config_text {
    std::string label;
    std::string content;
    uint32_t line_offset = 0;
  };

  std::function<std::string(const std::string&)> read_file;
  std::function<std::vector<config_text>(const std::string&)> read_folder;
};

// распарсить все тексты одной категории в vector<T>
template <typename T>
static std::vector<T> parse_texts(const std::vector<config_source::config_text>& texts, const std::string& label) {
  std::vector<T> arr;
  for (const auto& text : texts) {
    auto local = parse_config_content<T>(text.content, text.label.empty() ? label : text.label, text.line_offset);
    for (auto& el : local) {
      arr.emplace_back(std::move(el));
    }
  }
  return arr;
}

// источник на файловой системе (сырой io) — используется fast_test'ом
static config_source make_fs_config_source(std::string path) {
  config_source src;
  src.read_file = [path](const std::string& name) -> std::string {
    const auto file = path + name;
    if (!file_io::exists(file)) {
      utils::warn("File '{}' not found", file);
      return {};
    }
    return file_io::read(file);
  };
  src.read_folder = [path](const std::string& name) -> std::vector<config_source::config_text> {
    std::vector<config_source::config_text> out;
    const auto folder = path + name;
    if (!fs::exists(folder) || !fs::is_directory(folder)) {
      return out; // категории может не быть
    }
    for (const auto& entry : fs::directory_iterator(folder)) {
      if (!entry.is_regular_file()) {
        utils::warn("Ignore '{}'", entry.path().generic_string());
        continue;
      }
      const auto file = entry.path().generic_string();
      out.push_back(config_source::config_text{file, file_io::read(file), 0});
    }
    return out;
  };
  return src;
}

// источник в движковом demiurg-реестре: prefix вида "render_config/"; ресурсы предварительно
// доведены до warm (текст в памяти) на этапе инициализации, здесь только читаем text.
static config_source make_demiurg_config_source(const demiurg::resource_system* reg, std::string prefix) {
  config_source src;
  src.read_file = [reg, prefix](const std::string& name) -> std::string {
    // "declare_values.tavl" -> id "render_config/declare_values"
    std::string id = prefix + name;
    const auto dot = id.rfind('.');
    if (dot != std::string::npos) {
      id = id.substr(0, dot);
    }
    auto* r = reg->get<render_config_source>(id);
    if (r == nullptr) {
      utils::warn("render config resource '{}' not found in engine registry", id);
      return {};
    }
    r->ensure_text_loaded();
    std::string text = r->text;
    r->drop_text();
    return text;
  };
  src.read_folder = [reg, prefix](const std::string& name) -> std::vector<config_source::config_text> {
    std::vector<render_config_source*> arr;
    reg->find<render_config_source>(prefix + name, arr); // префиксный поиск по id
    std::sort(arr.begin(), arr.end(), [](const auto* a, const auto* b) {
      std::less<std::string_view> less;
      if (less(a->path, b->path)) {
        return true;
      }
      if (less(b->path, a->path)) {
        return false;
      }
      if (a->list_index != b->list_index) {
        return a->list_index < b->list_index;
      }
      return less(a->id, b->id);
    });

    std::vector<config_source::config_text> out;
    out.reserve(arr.size());
    for (auto* r : arr) {
      r->ensure_text_loaded();
      out.push_back(config_source::config_text{
        std::string(r->id),
        r->text,
        r->list_start_line > 0 ? r->list_start_line - 1 : 0});
      r->drop_text();
    }
    return out;
  };
  return src;
}

static void parse_data_impl(render_config_storage& lctx, const config_source& src);

// локальный контекст, потому что очень лениво делать отельный класс
template <typename OUT, typename T>
std::vector<OUT> convert(render_config_storage& ctx, const std::vector<T>& in) {
  std::vector<OUT> arr;
  for (const auto& el : in) {
    arr.emplace_back(el.convert(ctx));
  }
  return arr;
}

template <typename OUT, typename T, typename F>
std::vector<OUT> convert(render_config_storage& ctx, const std::vector<T>& in, const F& f) {
  std::vector<OUT> arr;
  for (const auto& el : in) {
    f(el, ctx, arr.emplace_back());
  }
  return arr;
}

semaphore::semaphore() noexcept {
  handles.fill(VK_NULL_HANDLE);
}

static void parse_data_impl(render_config_storage& lctx, const config_source& src) {
  const auto constant_values = parse_config_content<constant_value_mirror>(src.read_file("declare_values.tavl"), "declare_values.tavl");
  const auto counter_names = parse_config_content<std::string>(src.read_file("declare_counters.tavl"), "declare_counters.tavl");
  const auto constants = parse_texts<constant_mirror>(src.read_folder("constants/"), "constants/");
  const auto resources = parse_texts<resource_mirror>(src.read_folder("resources/"), "resources/");
  const auto samplers = parse_texts<sampler_mirror>(src.read_folder("samplers/"), "samplers/");
  const auto descriptors = parse_texts<descriptor_mirror>(src.read_folder("descriptors/"), "descriptors/");
  const auto render_targets = parse_texts<render_target_mirror>(src.read_folder("render_targets/"), "render_targets/");
  const auto geometries = parse_texts<geometry_mirror>(src.read_folder("geometries/"), "geometries/");
  const auto materials = parse_texts<material_mirror>(src.read_folder("materials/"), "materials/");
  const auto draw_groups = parse_texts<draw_group_mirror>(src.read_folder("draw_groups/"), "draw_groups/");
  const auto steps = parse_texts<pass_step2_mirror>(src.read_folder("steps/"), "steps/");
  const auto execution_passes = parse_texts<pass2_mirror>(src.read_folder("execution_passes/"), "execution_passes/");
  const auto render_graphs = parse_texts<render_graph2_mirror>(src.read_folder("render_graphs/"), "render_graphs/");

  for (const auto& name : counter_names) {
    lctx.counters.emplace_back();
    lctx.counters.back().name = name;
  }

  // не будем особенно душить, сами создадим
  //const uint32_t per_frame_index = check(ctx->find_counter("per_frame"), "counter", "per_frame", "parse_data");
  //const uint32_t per_update_index = check(ctx->find_counter("per_update"), "counter", "per_update", "parse_data");
  lctx.per_frame_counter_index = lctx.find_counter("per_frame");
  if (lctx.per_frame_counter_index == invalid_resource_slot) {
    lctx.per_frame_counter_index = lctx.counters.size();
    lctx.counters.emplace_back();
    lctx.counters.back().name = "per_frame";
  }

  lctx.per_update_counter_index = lctx.find_counter("per_update");
  if (lctx.per_update_counter_index == invalid_resource_slot) {
    lctx.per_update_counter_index = lctx.counters.size();
    lctx.counters.emplace_back();
    lctx.counters.back().name = "per_update";
  }

  lctx.swapchain_counter_index = lctx.find_counter("swapchain");
  if (lctx.swapchain_counter_index == invalid_resource_slot) {
    lctx.swapchain_counter_index = lctx.counters.size();
    lctx.counters.emplace_back();
    lctx.counters.back().name = "swapchain";
  }

  lctx.constant_values = convert<constant_value>(lctx, constant_values);
  lctx.constants = convert<constant>(lctx, constants);
  lctx.resources = convert<resource>(lctx, resources);
  lctx.samplers = convert<sampler>(lctx, samplers); // до дескрипторов: их convert ищет sampler по имени
  lctx.descriptors = convert<descriptor>(lctx, descriptors);
  lctx.render_targets = convert<render_target>(lctx, render_targets);
  lctx.geometries = convert<geometry>(lctx, geometries);
  lctx.materials = convert<material>(lctx, materials);
  lctx.draw_groups = convert<draw_group>(lctx, draw_groups);
  lctx.steps = convert<step_base>(lctx, steps, &parse_step2);
  lctx.passes = convert<execution_pass_base>(lctx, execution_passes, &parse_execution_pass2);
  lctx.graphs = convert<render_graph_base>(lctx, render_graphs, &parse_render_graph2);

  // найдем ресурс с ролью present
  for (uint32_t i = 0; i < lctx.resources.size(); ++i) {
    const auto& res = lctx.resources[i];
    if (res.role != role::present) {
      continue;
    }
    if (lctx.swapchain_slot != invalid_resource_slot) {
      const auto& first = lctx.resources[lctx.swapchain_slot];
      utils::error{}("Found several resources with role present, first '{}' current '{}'", first.name, res.name);
    }

    // не тут проверяем на то является ли базовый класс презентабельным или нет
    //if (lctx.presentation_engine_type != presentation_engine_type::main) {
    //  utils::error{}("This base class does not support presentation");
    //}

    lctx.swapchain_slot = i;

    const auto& value = DS_ASSERT_ARRAY_GET(lctx.constant_values, res.size);

    // должно быть что? счетчик swapchain, размер скринсайз
    if (res.swap != lctx.swapchain_counter_index) {
      utils::warn("Swapchain resource depends on non swapchain counter");
    }
    if (value.type != value_type::screensize) {
      utils::warn("Swapchain resource automagically would be screen size");
    }
    // тут еще нужно указать свопчеин формат
    // тут нужно будет вызвать choose_swapchain_surface_format, а это наверное в другом месте
  }

  // тут нужно еще пройтись например по драв группам и создать для них буферы
  for (auto& group : lctx.draw_groups) {
    // нужно создать 3 ресурса: 2 буфера и дескриптор

    // если тип host_visible то роль соответственно должна быть input
    auto inst_counter_index = lctx.per_frame_counter_index;
    auto indi_counter_index = lctx.per_frame_counter_index;
    auto inst_type = type::frames_in_flight;
    auto indi_type = type::frames_in_flight;
    auto inst_role = role::instance_output;
    auto indi_role = role::indirect_output;
    if (group.type == draw_group::type::host_visible) {
      inst_role = role::instance_input;
      indi_role = role::indirect_input;
      inst_type = type::doublebuffer;
      indi_type = type::doublebuffer;
      inst_counter_index = lctx.per_update_counter_index;
      indi_counter_index = lctx.per_update_counter_index;
    }

    {
      resource r;
      r.name = group.name + ".instances";
      r.format = group.layout_str;
      r.format_hint = 0;
      r.size_hint = group.stride;
      r.size = group.budget_constant; // как указать байты? наверное тут поставим UINT32_MAX + рядом укажем
      r.role = inst_role;
      r.type = inst_type;
      r.swap = inst_counter_index;
      r.usage_mask = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; // ?

      group.instances_buffer = lctx.resources.size();
      lctx.resources.emplace_back(std::move(r));
    }

    // формат зависит от геометрии .... =(
    // а драв группа понятно дело может использоваться хоть где
    // можем ввести дополнительный флаг, а можем ничего не вводить и оставить как есть
    {
      resource r;
      r.name = group.name + ".indirect";
      r.format = "uv4uv4";
      r.format_hint = 0;
      r.size_hint = 16 + 16; // создадим буфер хотя бы на один элемент?
      r.size = group.types_constant;
      r.role = indi_role;
      r.type = indi_type;
      r.swap = indi_counter_index;
      r.usage_mask = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; // ?

      group.indirect_buffer = lctx.resources.size();
      lctx.resources.emplace_back(std::move(r));
    }

    {
      descriptor d;
      d.name = group.name + ".descriptor";
      d.layout = {std::make_tuple(group.instances_buffer, usage::storage_write, UINT32_MAX, uint32_t(VK_SHADER_STAGE_ALL)), std::make_tuple(group.indirect_buffer, usage::storage_write, UINT32_MAX, uint32_t(VK_SHADER_STAGE_ALL))};

      group.descriptor = lctx.descriptors.size();
      lctx.descriptors.emplace_back(std::move(d));
    }
  }
}

render_config_storage build_render_config(std::string path) {
  render_config_storage storage;
  parse_data_impl(storage, make_fs_config_source(std::move(path)));
  return storage;
}

render_config_storage build_render_config(const demiurg::resource_system* reg, std::string prefix) {
  render_config_storage storage;
  parse_data_impl(storage, make_demiurg_config_source(reg, std::move(prefix)));
  return storage;
}

command_params::command_params() noexcept : type(static_cast<command::values>(UINT32_MAX)),
                                            resources{},
                                            constants{} {
  resources.fill(std::make_tuple(invalid_resource_slot, static_cast<usage::values>(UINT32_MAX)));
  constants.fill(UINT32_MAX);
}
step_type::values step_base::type() const {
  return command::convert(cmd_params.type);
}

// это капец
// мы к сожалению не угадаем с форматом индирект буфера до того как эта функция будет запущена
// многое зависит от текстовой команды, можем хотя бы проверить чтобы буфер был не меньше чем структура
command_params parse_command(render_config_storage* ctx, const step_base& step, const std::string_view& command_str) {
  const uint32_t geometry_index = step.geometry;
  const bool is_indexed = geometry_index < ctx->geometries.size() && ctx->geometries[geometry_index].index_type != geometry::index_type::none;

  const auto& [part, remaining] = utils::string::split_prefix(utils::string::trim(command_str), " ");
  //const auto name_type = check(command::name_from_string(part), part, step.name);
  if (utils::string::trim(remaining).empty()) {
    if (part != "draw") {
      utils::error{}("Error while parsing command for step '{}': command '{}' must have arguments", step.name, part);
    }
    // нужно проверить чтобы существовала драв группа
    if (step.draw_group == invalid_resource_slot) {
      utils::error{}("Command 'draw' cannot find draw group in step '{}'", step.name);
    }
    command_params p;
    p.type = is_indexed ? command::values::draw_indexed : command::values::draw;
    return p;
  }

  // UI: "draw_ui <vertices> <indices> <commands>". Без draw_group/пар — буферы это обычные
  // per_update host-visible ресурсы рендер-графа. vertices/indices биндятся как vertex/index,
  // commands читается на CPU (mapped) в graphics_draw_ui::process. Usage буферов задаётся
  // отдельно в step.barriers (vertex/index/host_read), здесь только резолвим имена в слоты.
  if (part == "draw_ui") {
    command_params p;
    p.type = command::values::draw_ui;
    auto rest = utils::string::trim(remaining);
    // commands читается только на CPU (.mapped) — реального GPU-usage нет, но Vulkan требует
    // ненулевой usage у буфера; transfer_dst — безобидный валидный плейсхолдер. ДОЛЖЕН совпадать
    // с usage этого ресурса в step.barriers (иначе конфликт при сборке).
    const usage::values usages[3] = {usage::vertex, usage::index, usage::transfer_dst};
    for (int i = 0; i < 3; ++i) {
      const auto& [tok, rem] = utils::string::split_prefix(rest, " ");
      const auto nm = utils::string::trim(tok);
      if (nm.empty()) {
        utils::error{}("Command 'draw_ui' in step '{}' expects 3 resources: <vertices> <indices> <commands>", step.name);
      }
      const uint32_t index = check(ctx->find_resource(nm), "resource", nm, step.name);
      p.resources[i] = std::make_tuple(index, usages[i]);
      rest = utils::string::trim(rem);
    }
    return p;
  }

  const auto& [type, raw_res_name] = utils::string::split_prefix(utils::string::trim(remaining), " ");
  const auto res_name = utils::string::trim(raw_res_name);

  command_params p;

  if (part == "draw" && type == "constant") {
    p.type = is_indexed ? command::values::draw_indexed_constant : command::values::draw_constant;
  }

  if (part == "draw" && type == "indirect") {
    p.type = is_indexed ? command::values::draw_indexed_indirect : command::values::draw_indirect;
  }

  if (part == "dispatch" && type == "constant") {
    p.type = command::values::dispatch_constant;
  }

  if (part == "dispatch" && type == "indirect") {
    p.type = command::values::dispatch_indirect;
  }

  if (p.type == command::values::draw_indexed_constant) {
    const uint32_t index = check(ctx->find_constant(res_name), "constant", res_name, step.name);
    p.constants[0] = index;

    const auto& constant = DS_ASSERT_ARRAY_GET(ctx->constants, index);
    const size_t place = constant.layout_str.find(format::to_string(format::indexed5));
    if (place != 0) {
      utils::error{}("Constant '{}' has wrong layout, layout must starts from 'indexed5' if using as draw command constant", constant.name);
    }
    return p;
  }

  if (p.type == command::values::draw_constant) {
    const uint32_t index = check(ctx->find_constant(res_name), "constant", res_name, step.name);
    p.constants[0] = index;

    const auto& constant = DS_ASSERT_ARRAY_GET(ctx->constants, index);
    const size_t place = constant.layout_str.find(format::to_string(format::draw4));
    if (place != 0) {
      utils::error{}("Constant '{}' has wrong layout, layout must starts from 'draw4' if using as draw command constant", constant.name);
    }
    return p;
  }

  if (p.type == command::values::draw_indexed_indirect) {
    const uint32_t index = check(ctx->find_resource(res_name), "resource", res_name, step.name);
    p.resources[0] = std::make_tuple(index, usage::indirect_read);

    const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, index);
    if (!role::is_indirect(res.role)) {
      utils::error{}("Resource '{}' has role '{}'? Must be 'indirect'", res.name, role::to_string(res.role));
    }
    //const size_t place = res.format.find(format::to_string(format::indexed5));
    //if (place != 0) utils::error{}("Resource '{}' has wrong format, format must starts from 'indexed5' if using as draw command indirect buffer", res.name);
    if (res.size_hint < format::size(format::indexed5)) {
      utils::error{}("Resource '{}' element size too small, expected at least {}", res.size_hint, format::size(format::indexed5));
    }
    return p;
  }

  if (p.type == command::values::draw_indirect) {
    const uint32_t index = check(ctx->find_resource(res_name), "resource", res_name, step.name);
    p.resources[0] = std::make_tuple(index, usage::indirect_read);

    const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, index);
    if (!role::is_indirect(res.role)) {
      utils::error{}("Resource '{}' has role '{}'? Must be 'indirect'", res.name, role::to_string(res.role));
    }
    //const size_t place = res.format.find(format::to_string(format::draw4));
    //if (place != 0) utils::error{}("Resource '{}' has wrong format, format must starts from 'draw4' if using as draw command indirect buffer", res.name);
    if (res.size_hint < format::size(format::draw4)) {
      utils::error{}("Resource '{}' element size too small, expected at least {}", res.size_hint, format::size(format::draw4));
    }
    return p;
  }

  if (p.type == command::values::dispatch_constant) {
    const uint32_t index = check(ctx->find_constant(res_name), "constant", res_name, step.name);
    p.constants[0] = index;

    const auto& constant = DS_ASSERT_ARRAY_GET(ctx->constants, index);
    const size_t place = constant.layout_str.find(format::to_string(format::dispatch3));
    if (place != 0) {
      utils::error{}("Constant '{}' has wrong layout, layout must starts from 'dispatch3' if using as draw command constant", constant.name);
    }
    return p;
  }

  if (p.type == command::values::dispatch_indirect) {
    const uint32_t index = check(ctx->find_resource(res_name), "resource", res_name, step.name);
    p.resources[0] = std::make_tuple(index, usage::indirect_read);

    const auto& res = DS_ASSERT_ARRAY_GET(ctx->resources, index);
    if (!role::is_indirect(res.role)) {
      utils::error{}("Resource '{}' has role '{}'? Must be 'indirect'", res.name, role::to_string(res.role));
    }
    //const size_t place = res.format.find(format::to_string(format::dispatch3));
    //if (place != 0) utils::error{}("Resource '{}' has wrong format, format must starts from 'dispatch3' if using as draw command indirect buffer", res.name);
    if (res.size_hint < format::size(format::dispatch3)) {
      utils::error{}("Resource '{}' element size too small, expected at least {}", res.size_hint, format::size(format::dispatch3));
    }
    return p;
  }

  uint32_t res_index1 = UINT32_MAX;
  uint32_t res_index2 = UINT32_MAX;

  // тут всегда есть 2 ресурса + тип блита?
  const auto& [blit_type, remaining2] = utils::string::split_prefix(res_name, " ");

  if (part == "copy") {
    res_index1 = check(ctx->find_resource(type), "resource", type, step.name);
    res_index2 = check(ctx->find_resource(res_name), "resource", res_name, step.name);

    const auto& res1 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index1);
    const auto& res2 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index2);

    // как валидировать? блин, одного размера поди должны быть
    // но при этом могут оказаться как картинкой так и буфером
    // требуем чтобы совпали форматы? вообще выглядит как план
    // особенно с учетом того что это копия

    // хотя строго говоря это не ошибка...
    // нужно научиться расчитывать размер ресурса не только при создании
    if (res1.format_hint != res2.format_hint) {
      utils::error{}("Resource1 '{}' has format '{}', resource2 '{}' has format '{}'. Invalid copy command", res1.name, res1.format, res2.name, res2.format);
    }

    p.resources[0] = std::make_tuple(res_index1, usage::transfer_src);
    p.resources[1] = std::make_tuple(res_index2, usage::transfer_dst);

    const bool res1_is_image = role::is_image(res1.role);
    const bool res2_is_image = role::is_image(res2.role);
    if (res1_is_image && res2_is_image) {
      p.type = command::values::copy_image;
      return p;
    }

    if (!res1_is_image && !res2_is_image) {
      p.type = command::values::copy_buffer;
      return p;
    }

    if (res1_is_image && !res2_is_image) {
      p.type = command::values::copy_image_buffer;
      return p;
    }

    if (!res1_is_image && res2_is_image) {
      p.type = command::values::copy_buffer_image;
      return p;
    }
  }

  if (part == "blit") {
    res_index1 = check(ctx->find_resource(type), "resource", type, step.name);
    res_index2 = check(ctx->find_resource(res_name), "resource", res_name, step.name);

    const auto& res1 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index1);
    const auto& res2 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index2);

    // тут совсем пофиг на формат

    p.type = command::values::blit_nearest;
    if (blit_type == "linear") {
      p.type = command::values::blit_linear;
    }

    p.resources[0] = std::make_tuple(res_index1, usage::transfer_src);
    p.resources[1] = std::make_tuple(res_index2, usage::transfer_dst);

    const bool res1_is_image = role::is_image(res1.role);
    const bool res2_is_image = role::is_image(res2.role);

    if (!(res1_is_image && res2_is_image)) {
      utils::error{}("Blit command requires resource1 '{}' and resource2 '{}' to be an image", res1.name, res2.name);
    }

    return p;
  }

  if (part == "clear") {
    res_index1 = check(ctx->find_resource(type), "resource", type, step.name);
    res_index2 = check(ctx->find_constant(res_name), "constant", res_name, step.name);

    // можно проверить что данных в константе хотя бы хватает

    const auto& res1 = DS_ASSERT_ARRAY_GET(ctx->resources, res_index1);
    const auto& const2 = DS_ASSERT_ARRAY_GET(ctx->constants, res_index2);

    if (role::is_image(res1.role)) {
      const bool is_color_image = format_is_color(res1.format_hint);

      uint32_t aspect = 0;
      if (is_color_image) {
        aspect = VK_IMAGE_ASPECT_COLOR_BIT;
      }
      if (format_has_depth(res1.format_hint)) {
        aspect = aspect | VK_IMAGE_ASPECT_DEPTH_BIT;
      }
      if (format_has_stencil(res1.format_hint)) {
        aspect = aspect | VK_IMAGE_ASPECT_STENCIL_BIT;
      }

      const size_t size = format_element_size(res1.format_hint, aspect);
      if (const2.size < size) {
        utils::error{}("Constant layout '{}' must be at least element size {} of image resource '{}', step: '{}'", const2.layout_str, size, res1.name, step.name);
      }

      p.type = is_color_image ? command::values::clear_color : command::values::clear_depth;
      p.resources[0] = std::make_tuple(res_index1, usage::transfer_dst);
      p.constants[0] = res_index2;
      return p;
    } else {
      utils::error{}("Clearing buffer is not implemented yet, step: '{}'", step.name);
    }
  }

  if (p.type >= command::values::count) {
    utils::error{}("Could not parse command '{}' for step '{}'", command_str, step.name);
  }

  return command_params{};
}

} // namespace painter
} // namespace devils_engine
