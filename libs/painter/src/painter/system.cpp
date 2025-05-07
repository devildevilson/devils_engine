#include "system.h"

#include "vulkan_header.h"
#include "container.h"
#include "utils/core.h"
#include "utils/named_serializer.h"
#include "utils/fileio.h"
#include "pipelines_config_static_container.h"
#include "common.h"

#include <filesystem>
namespace fs = std::filesystem;

namespace devils_engine {
namespace painter {

const std::string_view image_target_strs[] = {
#define X(name) #name ,
  DEVILS_ENGINE_PAINTER_IMAGE_TARGET_LIST
#undef X
};
const size_t image_target_strs_count = sizeof(image_target_strs) / sizeof(image_target_strs[0]);

const gtl::flat_hash_map<std::string_view, image_target::values> image_target_map = {
#define X(name) std::make_pair(#name, image_target::name),
  DEVILS_ENGINE_PAINTER_IMAGE_TARGET_LIST
#undef X
};

std::string_view image_target::to_string(const image_target::values val) {
  if (uint32_t(val) >= image_target_strs_count) return std::string_view();
  return image_target_strs[val];
}

image_target::values image_target::from_string(const std::string_view &str) {
  const auto itr = image_target_map.find(str);
  if (itr == image_target_map.end()) return image_target::invalid;
  return itr->second;
}


system::system() : 
  surface(VK_NULL_HANDLE),
  recreating(nullptr),
  recompiling_shaders(nullptr),
  submiter_counter(0)
{}
system::~system() noexcept {
  const size_t timeout = 1000000000;
  for (const auto &ptr : frames_presenters) {
    ptr->wait(timeout);
  }
}

uint32_t system::recompile_shaders() {
  uint32_t return_code = 0;
  for (auto p = recompiling_shaders; p != nullptr; p = p->next()) {
    const auto ret = p->recompile_shaders();
    return_code = std::max(ret, return_code);
  }

  return return_code;
}

void system::recreate(const uint32_t width, const uint32_t height) {
  for (auto p = recreating; p != nullptr; p = p->next(recreating)) {
    p->recreate(width, height);
  }
}

uint32_t system::wait_frame(const size_t timeout) const {
  auto ptr = frames_presenters[submiter_counter].get();
  return ptr->wait(timeout);
}

uint32_t system::compute_frame() {
  submiter_counter = (submiter_counter + 1) % frames_presenters.size();
  auto ptr = frames_presenters[submiter_counter].get();

  ptr->begin();

  // попытаемся получить изображение, если eErrorOutOfDateKHR то пересоздадим свопчеин и еще раз попробуем взять картинку
  auto res = vk::Result::eSuccess;
  do {
    res = (vk::Result)ptr->acquire_next_image();
    switch (res) {
      case vk::Result::eSuccess:
      case vk::Result::eSuboptimalKHR:
        break;
      case vk::Result::eErrorOutOfDateKHR: {
        vk::PhysicalDevice pd(main_container->physical_device);
        const auto caps = pd.getSurfaceCapabilitiesKHR(surface);
        recreate(caps.currentExtent.width, caps.currentExtent.height);
        break;
      }
      default: utils::error("Acquiring image error '{}'", vk::to_string(vk::Result(res)));
    }
  } while (res != vk::Result::eSuccess && res != vk::Result::eSuboptimalKHR);

  ptr->process();

  res = vk::Result(ptr->present());
  switch (res) {
    case vk::Result::eSuccess:
      break;
    case vk::Result::eSuboptimalKHR:
    case vk::Result::eErrorOutOfDateKHR: {
      // chatgpt говорит что мы можем получить caps.currentExtent.width == UINT32_MAX 
      // и че делать? брать масимальные значения? ... как будто нет
      vk::PhysicalDevice pd(main_container->physical_device);
      const auto caps = pd.getSurfaceCapabilitiesKHR(surface);
      recreate(caps.currentExtent.width, caps.currentExtent.height);
      break;
    }
    default: utils::error("Presentation error '{}'", vk::to_string(vk::Result(res)));
  }

  return submiter_counter;
}

VkInstance system::get_instance() const { return main_container->instance; }
VkDevice system::get_device() const { return main_container->device; }
container* system::get_main_container() const { return main_container.get(); }
void system::set_window_surface(VkSurfaceKHR surface) {
  this->surface = surface;
}

const graphics_pipeline_create_config* system::get_graphics_pipeline_config(const std::string &name) const {
  auto itr = graphics_pipeline_configs.find(name);
  if (itr == graphics_pipeline_configs.end()) return nullptr;
  return &itr->second;
}

const compute_pipeline_create_config* system::get_compute_pipeline_config(const std::string &name) const {
  auto itr = compute_pipeline_configs.find(name);
  if (itr == compute_pipeline_configs.end()) return nullptr;
  return &itr->second;
}

const render_pass_data_t* system::get_render_pass_config(const std::string &name) const {
  auto itr = render_pass_configs.find(name);
  if (itr == render_pass_configs.end()) return nullptr;
  return &itr->second;
}

const std::vector<attachment_config_t> & system::get_attachments_config(const std::string &name) const {
  auto itr = attachments_configs.find(name);
  if (itr == attachments_configs.end()) return std::vector<attachment_config_t>();
  return itr->second;
}

const sampler_config_t* system::get_sampler_config(const std::string &name) const {
  auto itr = sampler_configs.find(name);
  if (itr == sampler_configs.end()) return nullptr;
  return &itr->second;
}

const descriptor_set_layouts_config_t* system::get_set_layouts_configs() const {
  return &set_layouts_configs;
}

const pipeline_layouts_t* system::get_pipeline_layouts_configs() const {
  return &pipeline_layouts_configs;
}

const auto path_to_cached_folder = utils::project_folder() + "cache/";
template <typename T>
void dump_configs(const T &map, const std::string &postfix) {
  for (const auto &[ name, conf ] : map) {
    const auto file_name = path_to_cached_folder + name + postfix + ".json";
    const auto json = utils::to_json<glz::opts{.prettify = true, .indentation_width=2}>(conf);
    // надо бы переделать file_io
    file_io::write(json.value(), file_name);
  }
}

const std::string graphics_pipeline_type = "_graphics_pipeline";
const std::string compute_pipeline_type = "_compute_pipeline";
const std::string render_pass_type = "_render_pass";
const std::string attachments_type = "_attachments";
const std::string sampler_type = "_sampler";
static bool check_type(const std::string_view &filename, const std::string_view &type_name) {
  if (filename.rfind(type_name) == std::string_view::npos) return false;
  return (filename.rfind(type_name) + type_name.size()) == filename.size();
}

static std::string_view get_config_name(const std::string_view &filename, const std::string_view &type_name) {
  return filename.substr(0, filename.rfind(type_name));
}

void system::dump_configs_to_disk() const {
  dump_configs(graphics_pipeline_configs, graphics_pipeline_type);
  dump_configs(compute_pipeline_configs, compute_pipeline_type);
  dump_configs(render_pass_configs, render_pass_type);
  dump_configs(attachments_configs, attachments_type);
  dump_configs(sampler_configs, sampler_type);
  {
    const auto file_name = path_to_cached_folder + "descriptor_set_layouts.json";
    const auto json = utils::to_json<glz::opts{.prettify=true,.indentation_width=2}>(set_layouts_configs);
    file_io::write(json.value(), file_name);
  }

  {
    const auto file_name = path_to_cached_folder + "pipeline_layouts.json";
    const auto json = utils::to_json<glz::opts{.prettify=true,.indentation_width=2}>(pipeline_layouts_configs);
    file_io::write(json.value(), file_name);
  }
}

template <typename T, typename Fnames, typename Fconfigs>
void load_default_configs(T &map, const Fnames &fnames, const Fconfigs &fconfigs) {
  const auto names = fnames();
  for (const auto &name : names) {
    const auto ptr = fconfigs(name);
    if (ptr == nullptr) continue;

    // можем ли мы воспользоваться map.insert_or_assign
    auto itr = map.find(name);
    if (itr == map.end()) {
      map.insert(std::make_pair(name, *ptr));
    } else {
      itr->second = *ptr;
    }
  }
}

template <typename T>
bool load_config_from_json(T &map, const std::string &name, const std::string &content) {
  using conf_type = typename T::mapped_type;

  conf_type conf;
  const auto err = utils::from_json(conf, content);
  if (err) {
    utils::warn("Could not parse config '{}' for type '{}'. Skipped", name, utils::type_name<conf_type>());
    return false;
  }

  auto itr = map.find(name);
  if (itr == map.end()) {
    map.insert(std::make_pair(name, conf));
  } else {
    itr->second = conf;
  }

  return true;
}

template <typename T>
bool check_and_load_config_from_json(T &map, const std::string_view &file_name, const std::string &type_name, const std::string &content) {
  if (!check_type(file_name, type_name)) return false;

  const auto name = get_config_name(file_name, type_name);
  return load_config_from_json(map, std::string(name), content);
}

const std::string json_format = ".json";
void system::reload_configs() {
  // сначала грузим дефолтные конфиги?
  load_default_configs(graphics_pipeline_configs, available_default_graphics_pipeline_configs, get_default_graphics_pipeline_config);
  load_default_configs(compute_pipeline_configs, available_default_compute_pipeline_configs, get_default_compute_pipeline_config);
  load_default_configs(render_pass_configs, available_default_render_pass_configs, get_default_render_pass_config);
  load_default_configs(attachments_configs, available_default_attachments_configs, get_default_attachments_config);
  load_default_configs(sampler_configs, available_default_samplers_configs, get_default_sampler_config);
  set_layouts_configs = *reinterpret_cast<const descriptor_set_layouts_config_t*>(get_default_descriptor_set_layout_configs());
  pipeline_layouts_configs = *reinterpret_cast<const pipeline_layouts_t*>(get_default_pipeline_layout_configs());

  // теперь попытаемся загрузить кофиги с диска
  if (!file_io::exists(path_to_cached_folder)) return;
  for (const auto &entry : fs::directory_iterator(path_to_cached_folder)) {
    if (!entry.is_regular_file()) continue;
    const auto path = entry.path().generic_string();
    const size_t format_index = path.rfind(json_format);
    if (format_index == std::string::npos) continue;
    if (format_index + json_format.size() != path.size()) continue;

    const auto content = file_io::read(path);
    auto file_name = std::string_view(path).substr(path.rfind('/')+1);
    file_name = file_name.substr(0, file_name.rfind('.'));

    if (file_name == "descriptor_set_layouts") {
      descriptor_set_layouts_config_t conf;
      const auto err = utils::from_json(conf, content);
      if (err) {
        utils::warn("Could not parse config '{}' for type '{}'. Skipped", path, "descriptor_set_layouts_config_t");
        continue;
      }

      set_layouts_configs = conf;
      continue;
    }

    if (file_name == "pipeline_layouts") {
      pipeline_layouts_t conf;
      const auto err = utils::from_json(conf, content);
      if (err) {
        utils::warn("Could not parse config '{}' for type '{}'. Skipped", path, "pipeline_layouts_t");
        continue;
      }

      pipeline_layouts_configs = conf;
      continue;
    }

    check_and_load_config_from_json(graphics_pipeline_configs, file_name, graphics_pipeline_type, content);
    check_and_load_config_from_json(compute_pipeline_configs, file_name, compute_pipeline_type, content);
    check_and_load_config_from_json(render_pass_configs, file_name, render_pass_type, content);
    check_and_load_config_from_json(attachments_configs, file_name, attachments_type, content);
    check_and_load_config_from_json(sampler_configs, file_name, sampler_type, content);
  }
}

// короч с конфигами как то вот так

}
}