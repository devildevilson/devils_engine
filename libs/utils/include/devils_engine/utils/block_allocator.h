#ifndef DEVILS_ENGINE_UTILS_BLOCK_ALLOCATOR_H
#define DEVILS_ENGINE_UTILS_BLOCK_ALLOCATOR_H

#include <cstddef>
#include <cstdint>
#include <atomic>
#include "core.h"

namespace devils_engine {
  namespace utils {
    class block_allocator {
    public:
      block_allocator(const size_t memory_size, const size_t block_size, const size_t block_align) noexcept;
      ~block_allocator() noexcept;

      block_allocator(const block_allocator &copy) noexcept = delete;
      block_allocator(block_allocator &&move) noexcept;
      block_allocator & operator=(const block_allocator &copy) noexcept = delete;
      block_allocator & operator=(block_allocator &&move) noexcept;

      void* allocate() noexcept;
      void* allocate1(const size_t count) noexcept;
      void free(void* mem) noexcept;

      template <typename T, typename... Args>
      T* create(Args&&... args) {
        utils::assertf(sizeof(T) <= allocation_size(), "Type size {} must be lesser or equal than allocation size {}", sizeof(T), allocation_size());
        auto ptr = allocate();
        return new (ptr) T(std::forward<Args>(args)...);
      }

      template <typename T>
      void destroy(T* ptr) {
        ptr->~T();
        free(ptr);
      }

      void clear() noexcept;

      size_t alignment() const noexcept;
      size_t allocation_size() const noexcept;
      size_t size() const noexcept;
      size_t compute_full_size() const noexcept;
    private:
      size_t m_align;
      size_t m_block_size;
      size_t m_current_memory;
      size_t m_memory_size;
      char* m_memory;
      void* m_free_memory;

      void create_new_memory() noexcept;
    };

    class block_allocator_mt {
    public:
      block_allocator_mt(const size_t memory_size, const size_t block_size, const size_t block_align) noexcept;
      ~block_allocator_mt() noexcept;

      block_allocator_mt(const block_allocator_mt &copy) noexcept = delete;
      block_allocator_mt(block_allocator_mt &&move) noexcept;
      block_allocator_mt & operator=(const block_allocator_mt &copy) noexcept = delete;
      block_allocator_mt & operator=(block_allocator_mt &&move) noexcept;

      void* allocate() noexcept;
      void free(void* mem) noexcept;

      template <typename T, typename... Args>
      T* create(Args&&... args) {
        utils::assertf(sizeof(T) <= allocation_size(), "Type size {} must be lesser or equal than allocation size {}", sizeof(T), allocation_size());
        auto ptr = allocate();
        return new (ptr) T(std::forward<Args>(args)...);
      }

      template <typename T>
      void destroy(T* ptr) {
        ptr->~T();
        free(ptr);
      }

      void clear() noexcept;

      size_t alignment() const noexcept;
      size_t allocation_size() const noexcept;
      size_t size() const noexcept;
      size_t compute_full_size() const noexcept;
    private:
      size_t m_align;
      size_t m_block_size;
      size_t m_current_memory;
      size_t m_memory_size;
      std::atomic<char*> m_memory;
      std::atomic<void*> m_free_memory;

      char* create_new_memory(char* old_mem) noexcept;
      char* spin_lock() noexcept;
    };
  }
}

#endif
