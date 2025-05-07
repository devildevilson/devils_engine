#ifndef DEVILS_ENGINE_PAINTER_BUFFER_RESOURCES_H
#define DEVILS_ENGINE_PAINTER_BUFFER_RESOURCES_H

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utils/reflect>
#include <atomic>
#include <concepts>
#include <type_traits>
#include "utils/core.h"
#include "vulkan_minimal.h"
#include "primitives.h"

namespace devils_engine {
namespace painter {

template<class Tuple>
struct remove_last;

template<>
struct remove_last<std::tuple<>>; // Define as you wish or leave undefined

template<class... Args>
struct remove_last<std::tuple<Args...>> {
private:
  using Tuple = std::tuple<Args...>;

  template<std::size_t... n>
  static std::tuple<std::tuple_element_t<n, Tuple>...> extract(std::index_sequence<n...>);
public:
  using type = decltype(extract(std::make_index_sequence<sizeof...(Args) - 1>()));
};

template<class Tuple>
using remove_last_t = typename remove_last<Tuple>::type;

template <typename T>
class clever_buffer {
public:
  using raw_data_t = decltype(reflect::to<std::tuple>(T{}));
  constexpr static const size_t element_count = std::tuple_size_v<raw_data_t>;
  static_assert(element_count > 0);
  using last_element_t = typename std::tuple_element_t<element_count-1, raw_data_t>;
  using data_part_t = typename std::conditional_t<std::is_pointer_v<last_element_t>, remove_last_t<raw_data_t>, raw_data_t>;
  using array_part_t = typename std::conditional_t<std::is_pointer_v<last_element_t>, std::remove_pointer_t<last_element_t>, void>;
  constexpr static const size_t at_least_size = utils::align_to(sizeof(data_part_t), 16) + sizeof(array_part_t);

  using counter_t = uint32_t;

  clever_buffer() noexcept : counter(0), mapped_pointer(nullptr) {}

  template <std::same_as<array_part_t> U = array_part_t>
    requires (!std::is_same_v<array_part_t, void>)
  void* interpretate_place(const counter_t index) {
    //if constexpr (std::is_same_v<array_part_t, void>) return nullptr;
    auto ptr = reinterpret_cast<char*>(mapped_pointer);
    ptr += sizeof(data_part_t);
    ptr += index * sizeof(array_part_t);
    return ptr;
  }

  template <typename... Args>
    requires (!std::same_as<array_part_t, void>)
  counter_t add(Args&&... args) {
    const auto index = counter.fetch_add(1);
    auto ptr = interpretate_place(index);
    new (ptr) T{std::forward<Args>(args)...};
    return index;
  }

  template <std::same_as<array_part_t> U = array_part_t>
    requires (!std::is_same_v<array_part_t, void>)
  array_part_t & arr_at(const counter_t index) {
    auto ptr = interpretate_place(index);
    return reinterpret_cast<array_part_t*>(ptr);
  }

  template <std::same_as<array_part_t> U = array_part_t>
    requires (!std::is_same_v<last_element_t, void>)
  const array_part_t & arr_at(const counter_t index) const {
    auto ptr = interpretate_place(index);
    return reinterpret_cast<array_part_t*>(ptr);
  }

  template <size_t N>
  auto & get() { 
    return reflect::get<N>(*reinterpret_cast<T*>(mapped_pointer));
  }

  template <size_t N>
  const auto & get() const { 
    return reflect::get<N>(*reinterpret_cast<T*>(mapped_pointer));
  }

  template <reflect::fixed_string Name>
  auto & get() { 
    return reflect::get<Name>(*reinterpret_cast<T*>(mapped_pointer));
  }

  template <reflect::fixed_string Name>
  const auto & get() const { 
    return reflect::get<Name>(*reinterpret_cast<T*>(mapped_pointer));
  }

  auto & get() { return *reinterpret_cast<T*>(mapped_pointer); }
  const auto & get() const { return *reinterpret_cast<T*>(mapped_pointer); }

  size_t data_size() const { return sizeof(data_part_t); }
  size_t array_size() const { return counter * sizeof(array_part_t); }
protected:
  std::atomic<counter_t> counter;
  void* mapped_pointer;
};

// значицца в чем прикол:
// практически все буферы на гпу имеют примерно схожую сигнатуру
// struct_t { elem1, elem2, elem3, elem4* };
// где последний элемент можно сделать указателем чтобы тот смотрел на остаток буфера
// эту сигнатуру удобно выводить средствами reflect
// из нее можно получить собственно удачный маппинг между хостом и гпу
// из нее так же можно составить объявление в шейдер (большую часть шейдера можно будет не переписывать по миллиарду раз)
// должно быть наверное в обратную сторону наследование
// буфер может быть и чисто гпушный

// так все буферы еще нужно расположить в дескрипторе

// еще сделать маски для трансфера?
enum usage { undefined = 0, uniform = 0b1, storage = 0b10, indirect = 0b100, vertex = 0b1000, index = 0b10000, transfer_src = 0b100000, transfer_dst = 0b1000000 };
enum class reside { host = 0, gpu };
constexpr size_t standart_buffer_data_aligment = 16;
constexpr uint32_t standart_storage_usage = usage::storage | usage::transfer_src | usage::transfer_dst;

// пересоздание с потерей данных
std::tuple<VkBuffer, VmaAllocation, void*> create_buffer(VmaAllocator allocator, const size_t size, const uint32_t usg, enum reside rsd);
void destroy_buffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation);
void copy(VkCommandBuffer cbuf, VkBuffer src, VkBuffer dst, size_t srcoffset, size_t dstoffset, size_t size);

size_t align_to_device(const size_t size, VmaAllocator allocator, const uint32_t usage);

class common_buffer : public arbitrary_data, public buffer_provider {
public:
  common_buffer(VmaAllocator allocator, const size_t size, const uint32_t usg, enum reside rsd);
  ~common_buffer() noexcept;
  size_t orig_size() const;
  void* mapped_data();
  void flush_memory() const;
  void resize(const size_t new_size);
protected:
  VmaAllocator allocator;
  size_t _orig_size;
  VmaAllocation allocation;
  void* _mapped_data;

  uint32_t usage; 
  enum reside reside;
};

template <uint32_t usg, enum reside rsd>
class templated_buffer : public common_buffer {
public:
  templated_buffer(VmaAllocator allocator, size_t size) : common_buffer(allocator, size, usg, rsd) {}
};

// некоторое количество буферов создадим таким образом
using storage_buffer = templated_buffer<standart_storage_usage, reside::gpu>;
using uniform_buffer = templated_buffer<usage::uniform | usage::transfer_dst, reside::gpu>;
using vertex_buffer = templated_buffer<usage::vertex | usage::transfer_dst, reside::gpu>;
using index_buffer = templated_buffer<usage::index | usage::transfer_dst, reside::gpu>;
using indirect_buffer = templated_buffer<usage::indirect | usage::storage, reside::gpu>;
using host_buffer = templated_buffer<usage::transfer_src, reside::host>;

size_t reduce(const std::initializer_list<size_t> &sizes, const size_t aligment);

// это гпу буферы, в них нужно еще копировать, откуда?
class packed_buffer : public common_buffer {
public:
  constexpr static const size_t buffer_providers_count = 16;

  packed_buffer(VmaAllocator allocator, const std::initializer_list<size_t> &sizes, const uint32_t usage, enum reside reside);
  ~packed_buffer() noexcept;

  const buffer_provider* get(const size_t index) const;
  size_t count() const;
protected:
  size_t _count;
  buffer_provider providers[buffer_providers_count];
};

template <uint32_t usage, enum reside reside>
class templated_packed_buffer : public packed_buffer {
public:
  templated_packed_buffer(VmaAllocator allocator, const std::initializer_list<size_t> &sizes) : packed_buffer(allocator, sizes, usage, reside) {}
};

using storage_packed_buffer = templated_packed_buffer<standart_storage_usage, reside::gpu>;
using uniform_packed_buffer = templated_packed_buffer<usage::uniform | usage::transfer_dst, reside::gpu>;
using indirect_packed_buffer = templated_packed_buffer<usage::indirect | usage::storage, reside::gpu>;
using host_packed_buffer = templated_packed_buffer<usage::undefined, reside::host>;

class similar_buffer : public packed_buffer {
public:
  similar_buffer(VmaAllocator allocator, const size_t individual_size, const size_t count, const uint32_t usage, enum reside reside);
};

template <uint32_t usage, enum reside reside>
class templated_similar_buffer : public similar_buffer {
public:
  templated_similar_buffer(VmaAllocator allocator, const size_t individual_size, const size_t count) : similar_buffer(allocator, individual_size, count, usage, reside) {}
};

using storage_similar_buffer = templated_similar_buffer<standart_storage_usage, reside::gpu>;
using uniform_similar_buffer = templated_similar_buffer<usage::uniform | usage::transfer_dst, reside::gpu>;
using indirect_similar_buffer = templated_similar_buffer<usage::indirect | usage::storage, reside::gpu>;
using host_similar_buffer = templated_similar_buffer<usage::undefined, reside::host>;

// создадим пару буферов: на хосте + на гпу
template <typename T, uint32_t usage = standart_storage_usage>
class pair_buffer : public templated_buffer<usage, reside::gpu>, public clever_buffer<T> {
public:
  using super = common_buffer;

  pair_buffer(VmaAllocator allocator, const size_t size) : templated_buffer<usage, reside::gpu>(allocator, size) {
    auto [ b, a, m ] = create_buffer(allocator, super::_orig_size, usage::transfer_src, reside::host);
    host_buffer = b;
    host_allocation = a;
    super::_mapped_data = m;
    clever_buffer<T>::mapped_pointer = m;
  }

  ~pair_buffer() noexcept {
    destroy_buffer(super::allocator, host_buffer, host_allocation);
  }

  void resize(const size_t size) {
    const size_t new_buffer_size = align_to_device(size, super::allocator, usage);
    if (super::_orig_size == new_buffer_size) return;

    destroy_buffer(super::allocator, buffer_provider::buffer, super::allocation);
    destroy_buffer(super::allocator, host_buffer, host_allocation);

    super::_orig_size = new_buffer_size;

    {
      auto [ b, a, m ] = create_buffer(super::allocator, super::_orig_size, usage, reside::gpu);
      super::buffer = b;
      super::allocation = a;
      super::size = super::_orig_size;
    }

    {
      auto [ b, a, m ] = create_buffer(super::allocator, super::_orig_size, usage::transfer_src, reside::host);
      host_buffer = b;
      host_allocation = a;
      super::_mapped_data = m;
      clever_buffer<T>::mapped_pointer = m;
    }
  }

  void copy(VkCommandBuffer cbuf) {
    copy(cbuf, host_buffer, super::buffer, 0, 0, super::_orig_size);
  }
protected:
  VkBuffer host_buffer;
  VmaAllocation host_allocation;
};

// где то до этого момента закрываем ивент, затем после того как все скопируется открываем ивент
template <typename T, uint32_t usage = standart_storage_usage>
class pair_buffer_simple_stage : public pair_buffer<T, usage>, public sibling_stage {
public:
  using super = pair_buffer<T, usage>;

  pair_buffer_simple_stage(VmaAllocator allocator, const size_t size) : pair_buffer<T, usage>(allocator, size) {}
  void begin() {}

  void process(VkCommandBuffer buffer) {
    const uint32_t current_index =  pair_buffer<T, usage>::counter.exchange(0);
    if (current_index == 0) return;
    super::copy(buffer);
  }

  void clear() {}
};

}
}

#endif