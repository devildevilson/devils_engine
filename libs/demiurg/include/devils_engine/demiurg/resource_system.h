#ifndef DEVILS_ENGINE_DEMIURG_SYSTEM_H
#define DEVILS_ENGINE_DEMIURG_SYSTEM_H

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtl/phmap.hpp>

#include "devils_engine/utils/block_allocator.h"
#include "devils_engine/utils/memory_pool.h"
#include "devils_engine/utils/string_id.h"
#include "devils_engine/utils/type_traits.h"
#include "resource_base.h"
#include "resource_manifest.h"

namespace devils_engine {
namespace demiurg {
class module_system;
class resource_system;

struct resource_handle {
  const resource_system* system = nullptr;
  utils::id hash = utils::invalid_id;

  resource_interface* get() const noexcept;

  template <typename T>
  T* get() const noexcept;

  explicit operator bool() const noexcept;
};

// тут нужно проверить попадаем ли мы в тип в который пытаемся это дело преобразовать?
template <typename T = resource_interface>
class view_iterator : public std::span<resource_interface* const>::iterator {
public:
  using super = typename std::span<resource_interface* const>::iterator;

  template <typename... Args>
  view_iterator(Args&&... args);

  T* operator*() const noexcept;
  T* operator->() const noexcept;
};

template <typename T = resource_interface>
class view : public std::span<resource_interface* const> {
public:
  using super = std::span<resource_interface* const>;
  using iterator = view_iterator<T>;
  using reverse_iterator = std::reverse_iterator<iterator>;

  template <typename... Args>
  view(Args&&... args);

  T* operator[](const size_t index) const;

  iterator begin() const noexcept;
  iterator end() const noexcept;
  reverse_iterator rbegin() const noexcept;
  reverse_iterator rend() const noexcept;
};

class resource_system {
public:
  struct type {
    using resource_producer = std::function<resource_interface*(utils::block_allocator&)>;

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
      resource_producer create) noexcept;

    resource_interface* create();
    void destroy(resource_interface* ptr);
    size_t find_ext(const std::string_view& str) const;
  };

  resource_system() noexcept;
  ~resource_system() noexcept;

  // ext - укажем через запятую или другой знак, например png,bmp,jpg, то что первое будет считать за основной ресурс
  // и если втретится составной тип например obj,mtl, то сначала будет идти obj а потом mtl
  // здесь в аргументы можно сложить вещи которые нужны при загрузке
  // текстурки возможно все равно придется загружать с помощью хендла
  template <typename T, typename... Args>
  void register_type(std::string name, std::string ext, Args&&... args);

  // как раз было бы неплохо использовать концепт
  template <typename BaseT, typename T, typename... Args>
    requires(std::derived_from<T, BaseT>)
  void register_type(std::string name, std::string ext, Args&&... args);

  resource_interface* create(const std::string_view& id, const std::string_view& extension);

  resource_interface* get(const std::string_view& id) const;
  resource_interface* get(utils::id hash) const noexcept;

  static utils::id resource_hash(const std::string_view& id) noexcept;
  resource_handle handle(const std::string_view& id) const noexcept;
  resource_handle handle(utils::id hash) const noexcept;

  template <typename T>
  T* get(const std::string_view& id) const;

  // не указывать расширение файла!
  // поиск по отсортированному массиву поди O(logN + N)
  view<> find(const std::string_view& filter) const;
  // так работать это дело не будет, нужно отдельный контейнер делать
  template <typename T>
  size_t find(const std::string_view& filter_str, std::vector<T*>& arr) const;

  // здесь мы будем искать именно подстроку
  template <typename T>
  size_t filter(const std::string_view& filter_str, std::vector<T*>& arr) const;

  template <typename T>
  size_t filter_typed(const std::string_view& filter_str, std::vector<T*>& arr) const;

  void parse_resources(module_system* sys);

  // Дописать ресурсы module_system'а в СУЩЕСТВУЮЩИЙ реестр (без clear). В отличие от
  // parse_resources НЕ переигрывает override/dedup (ring-списки) по всему набору — рассчитан на
  // ПОДРЕЕСТРЫ с id, не пересекающимися с уже загруженными (напр. отдельный cache-модуль движка).
  // При коллизии id новый ресурс пропускается с warn (override в append не поддержан).
  void append_resources(module_system* sys);

  void clear();

  size_t resources_count() const noexcept;
  size_t all_resources_count() const noexcept;

private:
  struct typed_candidate {
    const resource_candidate* candidate;
    type* resource_type;
    size_t ext_index;
    size_t order;
  };

  struct manifest_entry {
    typed_candidate primary;
    std::vector<typed_candidate> supplementary;
    std::vector<std::string> aliases;
    uint32_t list_index_override = invalid_list_index;
  };

  utils::memory_pool<type, sizeof(type) * 16> types_pool;
  gtl::flat_hash_map<std::string_view, type*> types;
  std::vector<resource_interface*> resources;
  std::vector<resource_interface*> all_resources;
  std::deque<std::string> alias_storage;
  gtl::flat_hash_map<std::string_view, resource_interface*> aliases;
  struct hashed_resource {
    std::string_view id;
    resource_interface* res;
  };
  gtl::flat_hash_map<utils::id, hashed_resource> resources_by_hash;

  resource_system::type* find_proper_type(const std::string_view& id, const std::string_view& extension) const;
  std::vector<manifest_entry> resolve_manifest(const std::vector<resource_candidate>& candidates) const;
  void instantiate_manifest(const std::vector<manifest_entry>& manifest, std::vector<resource_interface*>* pending = nullptr);
  void register_alias(std::string alias, resource_interface* res);
  void rebuild_hash_index();
  void register_hash_key(std::string_view id, resource_interface* res);
  void parse_resources_impl(module_system* sys);
  void append_resources_impl(module_system* sys);
  static void sort_active_resources(std::vector<resource_interface*>& resources);
  std::span<resource_interface* const> raw_find(const std::string_view& filter) const;
};

// Template implementation

template <typename T>
T* resource_handle::get() const noexcept {
  constexpr auto type_id = utils::type_id<T>();
  auto* ptr = get();
  if (ptr == nullptr || !ptr->is_type(type_id)) {
    return nullptr;
  }
  return static_cast<T*>(ptr);
}

template <typename T>
template <typename... Args>
view_iterator<T>::view_iterator(Args&&... args) : super(std::forward<Args>(args)...) {}

template <typename T>
T* view_iterator<T>::operator*() const noexcept {
  return static_cast<T*>(super::operator*());
}

template <typename T>
T* view_iterator<T>::operator->() const noexcept {
  return static_cast<T*>(super::operator*());
}

template <typename T>
template <typename... Args>
view<T>::view(Args&&... args) : super(std::forward<Args>(args)...) {}

template <typename T>
T* view<T>::operator[](const size_t index) const {
  return static_cast<T*>(super::operator[](index));
}

template <typename T>
typename view<T>::iterator view<T>::begin() const noexcept {
  return iterator(super::begin());
}

template <typename T>
typename view<T>::iterator view<T>::end() const noexcept {
  return iterator(super::end());
}

template <typename T>
typename view<T>::reverse_iterator view<T>::rbegin() const noexcept {
  return reverse_iterator(super::rbegin());
}

template <typename T>
typename view<T>::reverse_iterator view<T>::rend() const noexcept {
  return reverse_iterator(super::rend());
}

template <typename T, typename... Args>
void resource_system::register_type(std::string name, std::string ext, Args&&... args) {
  auto constructor = [args = std::make_tuple(std::forward<Args>(args)...)](
                       utils::block_allocator& allocator) -> resource_interface* {
    auto* ptr = std::apply([&allocator](auto&&... ctor_args) {
      return allocator.create<T>(std::forward<decltype(ctor_args)>(ctor_args)...);
    },
                           args);
    ptr->type_id = utils::type_id<T>();
    ptr->loading_type_id = utils::type_id<T>();
    return ptr;
  };

  auto* type = types_pool.create(std::move(name), std::move(ext), sizeof(T) * 100, sizeof(T), alignof(T), std::move(constructor));
  types[type->name] = type;
}

template <typename BaseT, typename T, typename... Args>
  requires(std::derived_from<T, BaseT>)
void resource_system::register_type(std::string name, std::string ext, Args&&... args) {
  auto constructor = [args = std::make_tuple(std::forward<Args>(args)...)](
                       utils::block_allocator& allocator) -> resource_interface* {
    auto* ptr = std::apply([&allocator](auto&&... ctor_args) {
      return allocator.create<T>(std::forward<decltype(ctor_args)>(ctor_args)...);
    },
                           args);
    ptr->type_id = utils::type_id<T>();
    ptr->loading_type_id = utils::type_id<BaseT>();
    return ptr;
  };

  auto* type = types_pool.create(std::move(name), std::move(ext), sizeof(T) * 100, sizeof(T), alignof(T), std::move(constructor));
  types[type->name] = type;
}

template <typename T>
T* resource_system::get(const std::string_view& id) const {
  constexpr auto type_id = utils::type_id<T>();
  auto* ptr = get(id);
  if (ptr == nullptr || !ptr->is_type(type_id)) {
    return nullptr;
  }
  return static_cast<T*>(ptr);
}

template <typename T>
size_t resource_system::find(const std::string_view& filter_str, std::vector<T*>& arr) const {
  constexpr auto type_id = utils::type_id<T>();
  const auto found = find(filter_str);

  size_t i = 0;
  for (; i < found.size(); ++i) {
    if (!std::is_same_v<T, resource_interface> && !found[i]->is_type(type_id)) {
      continue;
    }
    arr.push_back(static_cast<T*>(found[i]));
  }
  return i;
}

template <typename T>
size_t resource_system::filter(const std::string_view& filter_str, std::vector<T*>& arr) const {
  constexpr auto type_id = utils::type_id<T>();
  size_t counter = 0;
  for (auto* resource : resources) {
    if (!std::is_same_v<T, resource_interface> && !resource->is_type(type_id)) {
      continue;
    }
    if (resource->id.find(filter_str) == std::string_view::npos) {
      continue;
    }

    ++counter;
    arr.push_back(static_cast<T*>(resource));
  }
  return counter;
}

template <typename T>
size_t resource_system::filter_typed(const std::string_view& filter_str, std::vector<T*>& arr) const {
  const auto itr = types.find(filter_str);
  if (itr == types.end()) {
    return 0;
  }

  constexpr auto type_id = utils::type_id<T>();
  if (itr->second->type_list == nullptr || !itr->second->type_list->is_type(type_id)) {
    return 0;
  }

  size_t counter = 0;
  for (auto* resource = itr->second->type_list; resource != nullptr; resource = resource->exemplary_next(itr->second->type_list)) {
    arr.push_back(static_cast<T*>(resource));
    ++counter;
  }
  return counter;
}

} // namespace demiurg
} // namespace devils_engine

#endif
