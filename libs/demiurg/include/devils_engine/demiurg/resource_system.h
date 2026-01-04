#ifndef DEVILS_ENGINE_DEMIURG_SYSTEM_H
#define DEVILS_ENGINE_DEMIURG_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <type_traits>
#include <utility>
#include <string>
#include <vector>
#include <array>
#include <span>
#include <functional>
#include <concepts>
#include "resource_base.h"
#include "devils_engine/utils/block_allocator.h"
#include "devils_engine/utils/memory_pool.h"
#include "devils_engine/utils/type_traits.h"
#include <gtl/phmap.hpp>

namespace devils_engine {
namespace demiurg {
class module_system;

// тут нужно проверить попадаем ли мы в тип в который пытаемся это дело преобразовать?
template <typename T = resource_interface>
class view_iterator : public std::span<resource_interface *const>::iterator {
public:
  using super = typename std::span<resource_interface *const>::iterator;

  template <typename... Args>
  view_iterator(Args &&...args) : super(std::forward<Args>(args)...) {}

  T* &operator*() noexcept { return static_cast<T*>(super::operator*()); }
  T* const &operator*() const noexcept { return static_cast<T*>(super::operator*()); }
  T& operator->() noexcept { return *static_cast<T*>(super::operator*()); }
  const T& operator->() const noexcept { return *static_cast<T*>(super::operator*()); }
};

template <typename T = resource_interface>
class view : public std::span<resource_interface *const> {
public:
  using super = std::span<resource_interface *const>;
  using iterator = view_iterator<T>;
  using reverse_iterator = std::reverse_iterator<iterator>;

  template<typename... Args>
  view(Args &&...args) : super(std::forward<Args>(args)...) {}

  T* const& operator[](const size_t index) const { return static_cast<T*>(super::operator[](index)); }

  iterator begin() const noexcept { return iterator(super::begin()); }
  iterator end() const noexcept { return iterator(super::end()); }
  reverse_iterator rbegin() const noexcept { return reverse_iterator(super::rbegin()); }
  reverse_iterator rend() const noexcept { return reverse_iterator(super::rend()); }
};

class resource_system {
public:
  struct type {
    using resource_producer = std::function<resource_interface *(utils::block_allocator &)>;

    std::string name;
    std::string ext;
    std::array<std::string_view, 16> exts;
    resource_interface* type_list;
    utils::block_allocator allocator;
    resource_producer createf;

    type(
      std::string name,
      std::string ext,
      const size_t allocator_size,
      const size_t block_size,
      const size_t allocator_align,
      resource_producer create
    ) noexcept;

    resource_interface *create();
    void destroy(resource_interface * ptr);
    size_t find_ext(const std::string_view &str) const;
  };

  resource_system() noexcept;
  ~resource_system() noexcept;

  // ext - укажем через запятую или другой знак, например png,bmp,jpg, то что первое будет считать за основной ресурс
  // и если втретится составной тип например obj,mtl, то сначала будет идти obj а потом mtl
  // здесь в аргументы можно сложить вещи которые нужны при загрузке
  // текстурки возможно все равно придется загружать с помощью хендла
  template <typename T, typename... Args>
  void register_type(std::string name, std::string ext, Args&&... args) {
    auto constructor = [args = std::make_tuple(std::forward<Args>(args)...)](
            utils::block_allocator &allocator
    ) -> resource_interface * {
      auto ptr = std::apply(&utils::block_allocator::create<T>, std::tuple_cat(std::make_tuple(std::ref(allocator)), args));
      ptr->loading_type_id = utils::type_id<T>();
      ptr->loading_type = utils::type_name<T>();
      return ptr;
    };

    auto type = types_pool.create(std::move(name), std::move(ext), sizeof(T) * 100, sizeof(T), alignof(T), std::move(constructor));
        
    types[type->name] = type;
  }

  // как раз было бы неплохо использовать концепт
  template <typename BaseT, typename T, typename... Args>
    requires(std::derived_from<T, BaseT>)
  void register_type(std::string name, std::string ext, Args&&... args) {
    auto constructor = [args = std::make_tuple(std::forward<Args>(args)...)](
            utils::block_allocator &allocator
    ) -> resource_interface * {
      auto ptr = std::apply(&utils::block_allocator::create<T>, std::tuple_cat(std::make_tuple(std::ref(allocator)), args));
      ptr->loading_type_id = utils::type_id<BaseT>();
      ptr->loading_type = utils::type_name<T>();
      return ptr;
    };

    auto type = types_pool.create(std::move(name), std::move(ext), sizeof(T) * 100, sizeof(T), alignof(T), std::move(constructor));
        
    types[type->name] = type;
  }

  resource_interface *create(const std::string_view &id, const std::string_view &extension);

  resource_interface* get(const std::string_view& id) const;

  template <typename T>
  T* get(const std::string_view& id) const {
    constexpr size_t type_id = utils::type_id<T>();

    auto ptr = get(id);
    if (ptr == nullptr || ptr->loading_type_id != type_id) return nullptr;
    return static_cast<T*>(ptr);
  }

  // не указывать расширение файла!
  // поиск по отсортированному массиву поди O(logN + N)
  view<> find(const std::string_view &filter) const;
  // так работать это дело не будет, нужно отдельный контейнер делать
  template <typename T>
  size_t find(const std::string_view &filter_str, std::vector<T*> &arr) const {
    constexpr size_t type_id = utils::type_id<T>();

    const auto v = find(filter_str);
    size_t i = 0;
    for (; i < v.size(); ++i) { // arr.capacity() - arr.size()
      if (!std::is_same_v<T, resource_interface> && v[i]->loading_type_id != type_id) continue;
      auto ptr = v[i];
      arr.push_back(static_cast<T*>(ptr));
    }

    return i;
  }

  // здесь мы будем искать именно подстроку
  template <typename T>
  size_t filter(const std::string_view &filter_str, std::vector<T *> &arr) const {
    constexpr size_t type_id = utils::type_id<T>();

    size_t counter = 0;
    for (size_t i = 0; i < resources.size(); ++i) { // && arr.size() < arr.capacity()
      auto ptr = resources[i];
      if (!std::is_same_v<T, resource_interface> && ptr->loading_type_id != type_id) continue;
      if (ptr->id.find(filter_str) == std::string_view::npos) continue;

      counter += 1;
      arr.push_back(static_cast<T *>(ptr));
    }

    return counter;
  }

  template <typename T>
  size_t filter_typed(const std::string_view& filter_str, std::vector<T*>& arr) const {
    const auto itr = types.find(filter_str);
    if (itr == types.end()) return 0;

    constexpr size_t type_id = utils::type_id<T>();
    if (itr->second->type_list == nullptr || itr->second->type_list->loading_type_id != type_id) return 0;

    size_t counter = 0;
    for (auto res = itr->second->type_list; res != nullptr; res = res->exemplary_next(itr->second->type_list)) {
      arr.push_back(static_cast<T*>(res));
      counter += 1;
    }

    return counter;
  }

  void parse_resources(module_system* sys);

  void clear();

  size_t resources_count() const noexcept;
  size_t all_resources_count() const noexcept;
private:
  utils::memory_pool<type, sizeof(type)*16> types_pool;
  gtl::flat_hash_map<std::string_view, type*> types;
  std::vector<resource_interface *> resources;
  std::vector<resource_interface *> all_resources;

  resource_system::type *find_proper_type(const std::string_view &id, const std::string_view &extension) const;
  std::span<resource_interface * const> raw_find(const std::string_view &filter) const;
};
}
}

#endif