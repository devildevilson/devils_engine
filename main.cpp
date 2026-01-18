#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/assets_base.h"
#include "devils_engine/painter/makers.h"
#include "devils_engine/painter/auxiliary.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/input/core.h"
#include "devils_engine/utils/time-utils.hpp"

#include <chrono>

using namespace devils_engine;

const char* app_name = "fast_test";
const char* engine_name = "fast_test";
const uint32_t app_version = VK_MAKE_VERSION(0,0,1);
const uint32_t engine_version = VK_MAKE_VERSION(0, 0, 1);
const uint32_t api_version = VK_API_VERSION_1_0;

const uint32_t g_width = 800;
const uint32_t g_height = 600;

static std::string join_arr(const std::span<const std::string>& arr, const std::string_view& token) {
  std::string str;

  size_t capacity = 0;
  for (const auto& s : arr) { capacity += s.size() + token.size(); }

  str.reserve(capacity);
  for (size_t i = 0; i < arr.size()-1; ++i) {
    str += arr[i];
    str += std::string(token);
  }

  str += arr.back();
  return str;
}

static std::string join_arr(const std::span<const std::string_view>& arr, const std::string_view& token) {
  std::string str;

  size_t capacity = 0;
  for (const auto& s : arr) { capacity += s.size() + token.size(); }

  str.reserve(capacity);
  for (size_t i = 0; i < arr.size() - 1; ++i) {
    str += arr[i];
    str += std::string(token);
  }

  str += arr.back();
  return str;
}

static std::string join_arr(const std::span<const char* const>& arr, const std::string_view& token) {
  std::string str;

  size_t capacity = 0;
  for (const auto& s : arr) { capacity += strlen(s) + token.size(); }

  str.reserve(capacity);
  for (size_t i = 0; i < arr.size() - 1; ++i) {
    str += arr[i];
    str += std::string(token);
  }

  str += arr.back();
  return str;
}

struct input_init : public input::init {
  GLFWwindow* w;

  input_init(const input::error_callback cb) noexcept : input::init(cb), w(nullptr) {}
  ~input_init() noexcept {
    if (w != nullptr) input::destroy(w);
  }
};

struct vulkan_init {
  VkInstance instance;
  VkDebugUtilsMessengerEXT mess;
  painter::physical_device_data p_data;
  VkDevice device;
  VkSurfaceKHR surface;

  vulkan_init() : instance(VK_NULL_HANDLE), mess(VK_NULL_HANDLE), device(VK_NULL_HANDLE), surface(VK_NULL_HANDLE) {
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

    {
      const auto layers_str = join_arr(painter::default_validation_layers, ", ");
      const auto ext_str = join_arr(exts, ", ");
      utils::info("Created vulkan instance with layers '{}' and extensions '{}', api version {}", layers_str, ext_str, api_version);
    }

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
    vk::Instance(instance).destroy(surface);
    vk::Device(device).destroy();
    painter::destroy_debug_messenger(instance, mess);
    vk::Instance(instance).destroy();
  }
};

static void error_callback(int err, const char* msg) noexcept {
  utils::info("Error: {}", msg);
}

static void print_vec(const std::span<const uint32_t>& arr) {
  utils::print("{ ");
  for (const auto val : arr) { utils::print(val, " "); }
  utils::println("}");
}

static uint32_t make_color(const float r, const float g, const float b, const float a) {
  return (uint32_t(std::round(std::clamp(r, 0.0f, 1.0f) * 255.0f)) << 0) | (uint32_t(std::round(std::clamp(g, 0.0f, 1.0f) * 255.0f)) << 8) | (uint32_t(std::round(std::clamp(b, 0.0f, 1.0f) * 255.0f)) << 16) | (uint32_t(std::round(std::clamp(a, 0.0f, 1.0f) * 255.0f)) << 24);
}

int main() {
  const auto cur_folder = utils::project_folder();
  utils::println(cur_folder);

  input_init ii(&error_callback);
  vulkan_init vk;
  vk::Device dev(vk.device);

  // получим queue, надо бы сделать еще проверку на несколько queue в одной семье
  // к сожалению queue придется еще синхронизировать через мьютекс
  auto graphics_queue = dev.getQueue(vk.p_data.graphics_queue, 0);
  auto compute_queue = dev.getQueue(vk.p_data.compute_queue, 0);
  auto transfer_queue = dev.getQueue(vk.p_data.transfer_queue, 0);

  painter::set_name(dev, graphics_queue, "main_graphics_queue");

  // если по итогу мне нужны треугольники, то нужно сделать что?
  // нужно задать файлики описаний ресурсов, создаю папку тест рендер граф получается

  painter::graphics_base base(vk.instance, vk.device, vk.p_data.handle, painter::presentation_engine_type::main);

  // создать аллокатор, дескриптор пул, комманд пул, фенсы
  base.create_allocator();
  base.create_command_pool(vk.p_data.graphics_queue, graphics_queue);
  base.create_descriptor_pool();
  // свопчеин зависит от пачки ресурсов с диска
  //base.recreate_swapchain(640, 480);
  base.get_or_create_pipeline_cache(utils::cache_folder() + "graphics_pipeline_cache");

  // нужны количество картинок для всех ресурсов которые зависят от свопчейна (кроме презент)
  // ... блин а обычные то ресурсы не будут пересоздаваться количественно
  // и все таки скорее должны зависесть от frames_in_flight 
  // нельзя их делать зависимыми от свопчейна 
  const auto res = base.recreate_basic_resources(cur_folder + "tests/render_config/");
  if (res != 0) {
    utils::error{}("Could not parse folder '{}'", "tests/render_config/");
  }

  // так теперь нужно создать сюрфейс и свопчеин
  ii.w = input::create_window(g_width, g_height, "triangle_test");
  auto result = input::create_window_surface(vk.instance, ii.w, nullptr, &vk.surface);
  if (result != static_cast<uint32_t>(vk::Result::eSuccess)) {
    utils::error{}("Could not create surface, got: {}", vk::to_string(static_cast<vk::Result>(result)));
  }

  base.set_surface(vk.surface, g_width, g_height);
  base.resize_viewport(g_width, g_height);

  for (const auto& res : base.resources) {
    if (painter::role::is_image(res.role)) {
      utils::info("Created image  resource '{}', role '{}', buffering '{}' ({})", res.name, painter::role::to_string(res.role), painter::type::to_string(res.type), res.compute_buffering(&base));
    } else {
      utils::info("Created buffer resource '{}', role '{}', buffering '{}' ({}), stride {}", res.name, painter::role::to_string(res.role), painter::type::to_string(res.type), res.compute_buffering(&base), res.size_hint);
    }
  }

  for (const auto& res : base.resource_containers) {
    if (res.is_image()) {
      utils::info("Created image container '{}', extent ({}, {}), layers {}, format '{}'", res.name, res.extent.x, res.extent.y, res.layers, vk::to_string(static_cast<vk::Format>(res.format)));
    } else {
      utils::info("Created buffer container '{}', size {}", res.name, res.size);
    }
  }

  // теперь нужно создать рендер граф
  const uint32_t graph_index = base.find_render_graph("graphics1");
  if (graph_index == painter::INVALID_RESOURCE_SLOT) utils::error{}("Could not find render graph 'graphics1'");

  base.populate_constant_default_values();
  base.change_render_graph(graph_index);

  print_vec(base.constants_memory[1]);

  painter::assets_base assets(dev, vk.p_data.handle);
  assets.create_fence();
  assets.create_allocator(vk.instance);
  assets.create_command_buffer(transfer_queue, vk.p_data.transfer_queue);
  assets.set_graphics_base(&base);

  // зададим ресурс для треугольника
  uint32_t pair_index = painter::INVALID_RESOURCE_SLOT;
  {
    const auto tri_h = assets.register_buffer_storage("triangle");
    painter::buffer_create_info bci{ "g1", 3, 0 };
    assets.create_buffer_storage(tri_h, bci);
    struct buffer_data { float x, y, z; uint32_t c; };
    const buffer_data buffer_mem[] = { { -1, -1, 0, make_color(1,0,0,1) }, { 1, -1, 0, make_color(0,1,0,1) }, { 0, 1, 0, make_color(0,0,1,1) } };
    const size_t buffer_mem_size = sizeof(buffer_mem);
    assets.populate_buffer_storage(tri_h, std::span(reinterpret_cast<const uint8_t*>(&buffer_mem[0]), buffer_mem_size), std::span<const uint8_t>());

    assets.mark_ready_buffer_slot(tri_h);

    const uint32_t dg_index = base.find_draw_group("dg1");
    pair_index = base.register_pair(dg_index, tri_h, 500);
  }

  // как сделать аккуратное заполнение vertexCount и firstInstance?
  // при регистрации пары мы бы хотели закинуть данные через fillbuffer?
  // наверное если есть cpu доступ, то можно поди закинуть данные из меша наместе
  // а если только gpu то в компут шейдере обновим...
  {
    const auto inst = base.get_current_instance_resource_frame(pair_index, 1);
    const auto indi = base.get_current_indirect_resource_frame(pair_index, 1);

    // хочу 4 треугольника по разным осям
    utils::info("Instance buffer: name '{}', format '{}', offset '{}', stride '{}', role '{}'", inst.name, inst.format, inst.sub.offset, inst.stride, painter::role::to_string(inst.role));
    utils::info("Indirect buffer: name '{}', format '{}', offset '{}', stride '{}', role '{}'", indi.name, indi.format, indi.sub.offset, indi.stride, painter::role::to_string(indi.role));

    struct vec4 { float x,y,z,w; };
    auto ptr = reinterpret_cast<vec4*>(&reinterpret_cast<uint8_t*>(inst.mapped)[inst.sub.offset]);
    auto com_ptr = reinterpret_cast<VkDrawIndirectCommand*>(&reinterpret_cast<uint8_t*>(indi.mapped)[indi.sub.offset]);

    ptr[0] = vec4{ 0, 0.5, 0, 0};
    ptr[1] = vec4{ 0.5, 0, 0, 0};
    ptr[2] = vec4{ 0,-0.5, 0, 0};
    ptr[3] = vec4{-0.5, 0, 0, 0};

    com_ptr[0].vertexCount = 3;
    com_ptr[0].instanceCount = 4;
    com_ptr[0].firstVertex = 0;
    com_ptr[0].firstInstance = 0;
  }

  // теперь если я хочу закинуть UI данные в рендер я что должен сделать?
  // вызываю get_current_buffer_resource_frame и копирую сырые данные через memcpy?

  base.update_event();

  constexpr auto fps60 = std::chrono::microseconds(size_t(utils::round(double(std::chrono::microseconds(std::chrono::seconds(1)).count()) / 60.0)));

  painter::graphics_ctx ctx;
  ctx.base = &base;
  ctx.assets = &assets;

  auto tp = std::chrono::steady_clock::now();
  size_t counter = 0;
  while (counter < 100) {
    ++counter;

    base.prepare_frame();
    ctx.prepare(); // нужен дескриптор
    ctx.draw();
    base.submit_frame();

    //utils::info("Submited frame");

    // что делать если лагает? желательно все равно выводить картинку, но пропускать кадры
    std::this_thread::sleep_until(tp + fps60 * counter);
  }

  utils::println("Exit");
}