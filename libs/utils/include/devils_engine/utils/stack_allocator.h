#ifndef DEVILS_ENGINE_UTILS_STACK_ALLOCATOR_H
#define DEVILS_ENGINE_UTILS_STACK_ALLOCATOR_H

#include <atomic>
#include <cstddef>

#include "core.h"
#include "list.h"
#include "type_traits.h"

namespace devils_engine {
namespace utils {
class stack_allocator {
public:
  stack_allocator(const size_t size, const size_t aligment) noexcept;
  ~stack_allocator() noexcept;

  stack_allocator(const stack_allocator& allocator) noexcept = delete;
  stack_allocator(stack_allocator&& move) noexcept;
  stack_allocator& operator=(const stack_allocator& allocator) noexcept = delete;
  stack_allocator& operator=(stack_allocator&& move) noexcept;

  void* allocate(const size_t size) noexcept;

  template <typename T, typename... Args>
  T* create(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>, "Must sutisfy is_trivially_destructible_v");
    auto ptr = allocate(sizeof(T));
    if (ptr == nullptr) {
      utils::error{}("Could not allocate memory for '{}', size: {} allocated: {}", utils::type_name<T>(), m_size, m_allocated);
    }
    return new (ptr) T(std::forward<Args>(args)...);
  }

  void clear() noexcept;

  // смещение аллокации в арене и обратное разрешение. Удобно хранить «ссылку» на данные как
  // число (напр. в nk_handle.id), а потом достать указатель: create -> offset_of -> ... -> at.
  size_t offset_of(const void* ptr) const noexcept;
  void* at(const size_t offset) noexcept;
  const void* at(const size_t offset) const noexcept;

  size_t capacity() const noexcept;
  size_t aligment() const noexcept;
  size_t size() const noexcept;
  char* data() noexcept;
  const char* data() const noexcept;

private:
  size_t m_aligment;
  size_t m_size;
  char* m_memory;
  size_t m_allocated;
};

class stack_allocator_mt {
public:
  stack_allocator_mt(const size_t size, const size_t aligment) noexcept;
  ~stack_allocator_mt() noexcept;

  stack_allocator_mt(const stack_allocator_mt& allocator) noexcept = delete;
  stack_allocator_mt(stack_allocator_mt&& move) noexcept;
  stack_allocator_mt& operator=(const stack_allocator_mt& allocator) noexcept = delete;
  stack_allocator_mt& operator=(stack_allocator_mt&& move) noexcept;

  void* allocate(const size_t size) noexcept;

  template <typename T, typename... Args>
  T* create(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>, "Must sutisfy is_trivially_destructible_v");
    auto ptr = allocate(sizeof(T));
    if (ptr == nullptr) {
      utils::error{}("Could not allocate memory for '{}', size: {} allocated: {}", utils::type_name<T>(), m_size, m_allocated);
    }
    return new (ptr) T(std::forward<Args>(args)...);
  }

  void clear() noexcept;

  size_t size() const noexcept;
  size_t aligment() const noexcept;
  size_t allocated_size() const noexcept;
  char* data() noexcept;
  const char* data() const noexcept;

private:
  size_t m_aligment;
  size_t m_size;
  char* m_memory;
  std::atomic<size_t> m_allocated;
};

// фиксированные куски памяти, но можно использовать как memory_pool
class fixed_pool_mt {
public:
  fixed_pool_mt(const size_t size, const size_t block_size, const size_t aligment) noexcept;
  ~fixed_pool_mt() noexcept;

  fixed_pool_mt(const fixed_pool_mt& allocator) noexcept = delete;
  fixed_pool_mt(fixed_pool_mt&& move) noexcept;
  fixed_pool_mt& operator=(const fixed_pool_mt& allocator) noexcept = delete;
  fixed_pool_mt& operator=(fixed_pool_mt&& move) noexcept;

  void* allocate() noexcept;

  template <typename T, typename... Args>
  T* create(Args&&... args) {
    if (sizeof(T) > m_block_size) {
      utils::error{}("Object '{}' size {} is bigger than fixed_pool_mt block size {}", utils::type_name<T>(), sizeof(T), m_block_size);
    }
    if (m_aligment % alignof(T) != 0) {
      utils::error{}("Object '{}' alignment {} is not compatible with fixed_pool_mt alignment {}", utils::type_name<T>(), alignof(T), m_aligment);
    }
    auto ptr = allocate();
    if (ptr == nullptr) {
      utils::error{}("Could not allocate memory for '{}', size: {}", utils::type_name<T>(), m_size);
    }
    return new (ptr) T(std::forward<Args>(args)...);
  }

  template <typename T>
  void destroy(T* ptr) {
    ptr->~T();
    free(ptr);
  }

  void free(void* ptr) noexcept;

  size_t size() const noexcept;
  size_t block_size() const noexcept;
  size_t aligment() const noexcept;

private:
  struct stack_element_t : public utils::forw::list<stack_element_t, 0> {};

  size_t m_aligment;
  size_t m_block_size;
  size_t m_size;
  char* m_memory;
  std::atomic<stack_element_t*> m_stack;
};
} // namespace utils
} // namespace devils_engine

#endif
