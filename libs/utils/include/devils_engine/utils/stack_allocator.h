#ifndef DEVILS_ENGINE_UTILS_STACK_ALLOCATOR_H
#define DEVILS_ENGINE_UTILS_STACK_ALLOCATOR_H

#include <cstddef>
#include <atomic>
#include "utils/core.h"
#include "utils/type_traits.h"

namespace devils_engine {
  namespace utils {
    class stack_allocator {
    public:
      stack_allocator(const size_t size, const size_t aligment) noexcept;
      ~stack_allocator() noexcept;

      stack_allocator(const stack_allocator &allocator) noexcept = delete;
      stack_allocator(stack_allocator &&move) noexcept;
      stack_allocator & operator=(const stack_allocator &allocator) noexcept = delete;
      stack_allocator & operator=(stack_allocator &&move) noexcept;

      void* allocate(const size_t size) noexcept;

      template <typename T, typename... Args>
      T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>, "Must sutisfy is_trivially_destructible_v");
        auto ptr = allocate(sizeof(T));
        utils_assertf(ptr != nullptr, "Could not allocate memory for '{}', size: {} allocated: {}", utils::type_name<T>(), m_size, m_allocated);
        return new (ptr) T(std::forward<Args>(args)...);
      }

      void clear() noexcept;

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

      stack_allocator_mt(const stack_allocator_mt &allocator) noexcept = delete;
      stack_allocator_mt(stack_allocator_mt &&move) noexcept;
      stack_allocator_mt & operator=(const stack_allocator_mt &allocator) noexcept = delete;
      stack_allocator_mt & operator=(stack_allocator_mt &&move) noexcept;

      void* allocate(const size_t size) noexcept;

      template <typename T, typename... Args>
      T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>, "Must sutisfy is_trivially_destructible_v");
        auto ptr = allocate(sizeof(T));
        utils_assertf(ptr != nullptr, "Could not allocate memory for '{}', size: {} allocated: {}", utils::type_name<T>(), m_size, m_allocated);
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
  }
}

#endif
