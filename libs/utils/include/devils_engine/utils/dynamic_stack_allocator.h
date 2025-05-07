#ifndef DEVILS_ENGINE_UTILS_DYNAMIC_STACK_ALLOCATOR_H
#define DEVILS_ENGINE_UTILS_DYNAMIC_STACK_ALLOCATOR_H

#include <cstddef>
#include <atomic>
#include <tuple>
#include "utils/core.h"
#include "utils/type_traits.h"

namespace devils_engine {
  namespace utils {
    class dynamic_stack_allocator {
    public:
      dynamic_stack_allocator(const size_t size, const size_t aligment, const float grow_policy = 2.0f) noexcept;
      ~dynamic_stack_allocator() noexcept;

      dynamic_stack_allocator(const dynamic_stack_allocator &allocator) noexcept = delete;
      dynamic_stack_allocator(dynamic_stack_allocator &&move) noexcept;
      dynamic_stack_allocator & operator=(const dynamic_stack_allocator &allocator) noexcept = delete;
      dynamic_stack_allocator & operator=(dynamic_stack_allocator &&move) noexcept;

      void* allocate(const size_t size);

      template <typename T, typename... Args>
      T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T> && std::is_trivially_copiable_v<T>, "Must sutisfy is_trivially_destructible_v && is_trivially_copiable_v");
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
      float m_grow_policy;

      std::tuple<char*, size_t> grow_memory(char* mem) const noexcept;
    };
  }
}

#endif // !DEVILS_ENGINE_UTILS_DYNAMIC_STACK_ALLOCATOR_H