#include "resource_system.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cassert>
#include "module_interface.h"
#include "module_system.h"
#include "folder_module.h"
#include "devils_engine/utils/time-utils.hpp"
#include "devils_engine/utils/string-utils.hpp"
#include "devils_engine/utils/named_serializer.h"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/sha256.h"
#include "devils_engine/thread/atomic.h"

namespace fs = std::filesystem;

namespace devils_engine {
  namespace demiurg {
    resource_system::type::type(
      std::string name,
      std::string ext,
      const size_t allocator_size,
      const size_t block_size,
      const size_t allocator_align,
      resource_producer create
    ) noexcept : 
      name(std::move(name)),
      ext(std::move(ext)),
      type_list(nullptr),
      allocator(allocator_size, block_size, allocator_align),
      createf(std::move(create))
    {
      auto sp = std::span(exts.data(), exts.size());
      const size_t count = utils::string::split(this->ext, ",", sp);
      if (count == SIZE_MAX) utils::error{}("Found to many extensions in str '{}' when creating demiurg resource type '{}'", this->ext, this->name);
    }

    resource_interface *resource_system::type::create() { 
      auto ptr = createf(allocator);
      ptr->type = name;
      if (type_list == nullptr) type_list = ptr;
      else type_list->exemplary_radd(ptr);
      return ptr;
    }

    void resource_system::type::destroy(resource_interface *ptr) {
      if (type_list == ptr) type_list = type_list->exemplary_next(type_list);
      allocator.destroy(ptr);
    }

    size_t resource_system::type::find_ext(const std::string_view& str) const {
      size_t i = 0;
      for (; i < exts.size() && exts[i] != str; ++i) {}
      return i >= exts.size() ? SIZE_MAX : i;
    }

    resource_system::resource_system() noexcept {}

    resource_system::~resource_system() noexcept { 
      clear();
      for (auto & [name, ptr] : types) {
        types_pool.destroy(ptr);
      }
    }

    resource_interface *resource_system::create(const std::string_view &id, const std::string_view &extension) {
      auto t = find_proper_type(id, extension);
      if (t == nullptr) return nullptr;
      auto ptr = t->create();
      all_resources.push_back(ptr);
      return ptr;
    }

    static bool get_f(const resource_interface* const res, const std::string_view& value) {
      std::less<std::string_view> l;
      return l(res->id, value);
    }

    resource_interface* resource_system::get(const std::string_view& id) const {
      if (id == "") return nullptr;

      const auto itr = std::lower_bound(resources.begin(), resources.end(), id, &get_f);
      if ((*itr)->id != id) return nullptr;
      return (*itr);
    }

    static bool lazy_compare(const std::string_view &a, const std::string_view &b) {
      return a.substr(0, b.size()) == b;
    }

    view<> resource_system::find(const std::string_view &filter) const {
      const auto span = raw_find(filter);
      return view<>(span.begin(), span.end());
    }

    std::span<resource_interface * const> resource_system::raw_find(const std::string_view &filter) const {
      if (filter == "") return std::span(resources);

      const auto itr = std::lower_bound(resources.begin(), resources.end(), filter, [] (const resource_interface* const res, const std::string_view &value) {
        std::less<std::string_view> l;
        return l(res->id.substr(0, value.size()), value);
      });

      if (itr == resources.end()) return std::span<resource_interface *const>();

      //auto start = resources.begin();
      auto start = itr;
      auto prev = itr;
      auto end = itr;
      // (*start)->id.substr(0, filter.size()) != filter
      // (*end)->id.substr(0, filter.size()) == filter
      for (; start != resources.begin() && lazy_compare((*start)->id, filter); prev = start, --start) {}
      if (lazy_compare((*start)->id, filter)) prev = start;

      for (; end != resources.end() && lazy_compare((*end)->id, filter); ++end) {}
      return std::span(prev, end);
    }

    static void parse_path(
      const std::string &path, 
      const std::string_view &root_path,
      std::string_view &module_name,
      std::string_view &file_name, 
      std::string_view &ext,
      std::string_view &id
    ) {
      std::string_view full_path = path;
      utils_assertf(full_path.find(root_path) == 0, "Path to resource must have root folder part. Current path: {}", path);
      full_path = full_path.substr(root_path.size()+1);

      const size_t first_slash = full_path.find('/');
      module_name = first_slash != std::string_view::npos ? full_path.substr(0, first_slash) : "";
      const size_t last_slash_index = full_path.rfind('/');
      file_name = last_slash_index != std::string_view::npos ? full_path.substr(last_slash_index) : full_path;
      if (file_name == "." || file_name == "..") return;

      const size_t dot_index = file_name.rfind('.');
      ext = dot_index != 0 && dot_index != std::string_view::npos ? file_name.substr(dot_index+1) : "";
      const size_t module_size = module_name == "" ? 0 : module_name.size()+1;
      const size_t ext_size = ext == "" ? 0 : ext.size()+1;
      id = full_path.substr(module_size, full_path.size() - module_size - ext_size);
    }

    static void make_forward_slash(std::string &path) {
      const char backslash = '\\';
      const char forwslash = '/';
      std::replace(path.begin(), path.end(), backslash, forwslash);
      for (auto itr = path.begin() + 1, prev = path.begin(); itr != path.end(); prev = itr, ++itr) {
        if (*itr == forwslash && *prev == forwslash) itr = path.erase(itr);
      }
    }

void resource_system::parse_resources(module_system* sys) {
  clear();

  sys->open_modules();
  sys->parse_resources(this);
  sys->close_modules();

  gtl::flat_hash_map<std::string_view, resource_interface *> loaded;
  for (const auto &res : all_resources) {
    auto t = find_proper_type(res->id, res->ext);
    if (t == nullptr) {
      utils::warn("Could not find proper type for resource '{}' extension '{}'. Skip", res->id, res->ext);
      continue;
    }

    // проверим загружали ли мы уже вещи
    auto itr = loaded.find(res->id);
    if (itr == loaded.end()) {
      loaded[res->id] = res;
      resources.push_back(res);
    } else {
      auto other_ptr = itr->second;
      for (; other_ptr != nullptr &&
            other_ptr->module_name != res->module_name;
          other_ptr = other_ptr->replacement_next(itr->second)) {}

      // тут мы реплейсмент меняем и с ним уходит сапплиментари
      utils::println("res", res->module_name, res->id, "other_ptr", other_ptr != nullptr);
      if (other_ptr != nullptr) {
        // модули совпали, найдем у кого меньший индекс среди расширений
        // умрем на expr,exp например
        //const size_t other_place = t->ext.find(other_ptr->ext);
        //const size_t res_place = t->ext.find(res->ext);
        const size_t other_place = t->find_ext(other_ptr->ext);
        const size_t res_place = t->find_ext(res->ext);
        if (res_place < other_place) {
          if (other_ptr == itr->second) {
            auto arr_itr = std::find(resources.begin(), resources.end(), other_ptr);
            (*arr_itr) = res;
          }

          auto old_repl = other_ptr->replacement_next(other_ptr);
          other_ptr->replacement_remove();
          res->replacement_radd(old_repl);

          res->supplementary_radd(other_ptr);

          // забыл поменять указатель в хеш мапе
          itr->second = res;
        } else {
          other_ptr->supplementary_radd(res);
        }
      } else {
        // новый ресурс по модулю
        // тут теперь нужно определить у кого меньший индекс 
        // примерно так же как и в случае с расширениями
        itr->second->replacement_radd(res);
      }
    }
  }

  std::sort(resources.begin(), resources.end(), [] (auto a, auto b) {
    std::less<std::string_view> l;
    return l(a->id, b->id);
  });

  for (const auto ptr : resources) {
    utils::println(ptr->module_name, ptr->id, ptr->ext);
  }
}

    void resource_system::clear() {
      for (auto ptr : all_resources) {
        const auto itr = types.find(ptr->type);
        assert(itr != types.end());
        itr->second->destroy(ptr);
      }
      resources.clear();
      all_resources.clear();
    }

    size_t resource_system::resources_count() const noexcept { return resources.size(); }
    size_t resource_system::all_resources_count() const noexcept { return all_resources.size(); }

    // if type is models/monster1
    // then something/somesome/abc/models/monster1 is prior over models/monster1/something/somesome/abc
    resource_system::type * resource_system::find_proper_type(const std::string_view &id, const std::string_view &extension) const {
      type *t = nullptr;
      std::string_view current_full_str = id;
      while (current_full_str.size() > 0 && t == nullptr) {
        size_t end = current_full_str.size();
        while (end != std::string_view::npos && t == nullptr) {
          const auto cur_id = current_full_str.substr(0, end);
          const auto itr = types.find(cur_id);
          if (itr != types.end()) {
            auto found_t = itr->second;
            const size_t ext_index = found_t->find_ext(extension);
            if (ext_index != SIZE_MAX) t = itr->second;
          }

          end = current_full_str.rfind('/', end-1);
        }

        const size_t slash_index = current_full_str.find('/');
        current_full_str = slash_index == std::string_view::npos ? "" : current_full_str.substr(slash_index+1);
      }

      return t;
    }

    void resource_interface::set_path(std::string path, const std::string_view &root) {
      this->path = std::move(path);
      std::string_view file_name;
      parse_path(this->path, root, module_name, file_name, ext, id);
    }

    static std::tuple<std::string_view, std::string_view, std::string_view> parse_path(const std::string_view &path) {
      const size_t last_slash = path.rfind('/');
      const size_t last_dot = path.rfind('.');
      const auto ext = last_dot != std::string_view::npos ? path.substr(last_dot+1) : std::string_view();
      const auto id = path.substr(0, last_dot);
      const auto name = path.substr(last_slash+1).substr(0, last_dot);
      return std::make_tuple(id, name, ext);
    }

    void resource_interface::set(std::string path, const std::string_view &module_name, const std::string_view &id, const std::string_view &ext) {
      this->path = std::move(path);
      this->module_name = module_name;
      const auto [ local_id, local_name, local_ext ] = parse_path(this->path);
      this->id = local_id;
      this->ext = local_ext;
    }

    resource_interface *resource_interface::replacement_next(const resource_interface *ptr) const {
      return utils::ring::list_next<list_type::replacement>(this, ptr);
    }

    resource_interface *resource_interface::supplementary_next(const resource_interface *ptr) const {
      return utils::ring::list_next<list_type::supplementary>(this, ptr);
    }

    resource_interface *resource_interface::exemplary_next(const resource_interface *ptr) const {
      return utils::ring::list_next<list_type::exemplary>(this, ptr);
    }

    void resource_interface::replacement_add(resource_interface *ptr) {
      utils::ring::list_add<list_type::replacement>(this, ptr);
    }

    void resource_interface::supplementary_add(resource_interface *ptr) {
      utils::ring::list_add<list_type::supplementary>(this, ptr);
    }

    void resource_interface::exemplary_add(resource_interface *ptr) {
      utils::ring::list_add<list_type::exemplary>(this, ptr);
    }

    void resource_interface::replacement_radd(resource_interface *ptr) {
      utils::ring::list_radd<list_type::replacement>(this, ptr);
    }

    void resource_interface::supplementary_radd(resource_interface *ptr) {
      utils::ring::list_radd<list_type::supplementary>(this, ptr);
    }

    void resource_interface::exemplary_radd(resource_interface *ptr) {
      utils::ring::list_radd<list_type::exemplary>(this, ptr);
    }

    void resource_interface::replacement_remove() {
      utils::ring::list_remove<list_type::replacement>(this);
    }

    void resource_interface::supplementary_remove() {
      utils::ring::list_remove<list_type::supplementary>(this);
    }

    void resource_interface::exemplary_remove() {
      utils::ring::list_remove<list_type::exemplary>(this);
    }

    void resource_interface::load(const utils::safe_handle_t& handle) {
      switch (state()) {
        case state::cold: load_cold(handle); break;
        case state::warm: load_warm(handle); break;
        case state::hot : return; // не нужно увеличивать стейт
      }
      
      //_state = std::min(_state + 1, 2);
      _state.fetch_add(1, std::memory_order_relaxed);
      thread::atomic_min(_state, static_cast<int32_t>(state::count));
    }

    void resource_interface::unload(const utils::safe_handle_t& handle) {
      const auto cur = static_cast<state::values>(_state.load(std::memory_order_relaxed));
      switch (cur) {
        case state::cold: break;
        case state::warm: if (!flag(resource_flags::force_unload_warm)) {unload_warm(handle);} break;
        case state::hot : unload_hot(handle); break;
      }

      //_state = std::max(_state - 1, 0);
      _state.fetch_add(-1, std::memory_order_relaxed);
      thread::atomic_max(_state, 0);
    }

    void resource_interface::force_unload(const utils::safe_handle_t& handle) {
      const auto cur = static_cast<state::values>(_state.load(std::memory_order_relaxed));
      switch (cur) {
        case state::cold: break;
        case state::warm: unload_warm(handle); break;
        case state::hot : unload_hot(handle);  break;
      }

      //_state = std::max(_state - 1, 0);
      _state.fetch_add(-1, std::memory_order_relaxed);
      thread::atomic_max(_state, 0);
    }

    enum state::values resource_interface::state() const { 
      const auto cur = static_cast<state::values>(_state.load(std::memory_order_relaxed));
      if (cur == state::warm && flag(resource_flags::warm_and_hot_same)) return state::hot;
      return cur;
    }

    bool resource_interface::usable() const {
      return state() == state::hot;
    }
  }
}