#ifndef DEVILS_ENGINE_UTILS_MEMORY_POOL_H
#define DEVILS_ENGINE_UTILS_MEMORY_POOL_H

#include <cstddef>
#include <algorithm>
#include <new>
#include <utility>

namespace devils_engine {
  namespace utils {
    template <typename T, size_t N = 4096>
    class memory_pool {
    public:
      using elem_ptr = T*;
      using const_elem_ptr = const T*;
      
      static_assert(sizeof(T) >= sizeof(T*), "Small element");
      
      memory_pool() : memory(nullptr), current_memory(nullptr), last_memory(nullptr), free_memory(nullptr) {}
      memory_pool(const memory_pool &pool) = delete;
      memory_pool(memory_pool &&pool) : memory(pool.memory), current_memory(pool.current_memory), last_memory(pool.last_memory), free_memory(pool.free_memory) {
        pool.memory = nullptr;
        pool.current_memory = nullptr;
        pool.last_memory = nullptr;
        pool.free_memory = nullptr;
      }
      
      ~memory_pool() { clear(); }
      
      memory_pool & operator=(const memory_pool &pool) = delete;
      void operator=(memory_pool &&pool) {
        clear();
        
        memory = pool.memory;
        current_memory = pool.current_memory;
        last_memory = pool.last_memory;
        free_memory = pool.free_memory;
        pool.memory = nullptr;
        pool.current_memory = nullptr;
        pool.last_memory = nullptr;
        pool.free_memory = nullptr;
      }
      
      char* allocate() {
        if (free_memory != nullptr) {
          auto ptr = free_memory;
          auto ptr_mem = reinterpret_cast<char**>(free_memory);
          free_memory = ptr_mem[0];
          return ptr;
        }
        
        if (current_memory + sizeof(T) > last_memory) allocate_memory();
        auto ptr = current_memory;
        current_memory += sizeof(T);
        return ptr;
      }
      
      template <typename ...Args>
      elem_ptr create(Args&& ...args) {
        auto ptr = allocate();
        elem_ptr p = new (ptr) T(std::forward<Args>(args)...);
        return p;
      }
      
      void destroy(elem_ptr ptr) {
        if (ptr == nullptr) return;
        ptr->~T();
        auto ptr_mem = reinterpret_cast<char**>(ptr);
        ptr_mem[0] = free_memory;
        free_memory = reinterpret_cast<char*>(ptr);
      }
      
      constexpr size_t block_elem_count() const {
        return usable_bytes() / sizeof(T);
      }
      
      void clear() {
        char* old_mem = memory;
        while (old_mem != nullptr) {
          auto ptr_mem = reinterpret_cast<char**>(old_mem);
          char* tmp = ptr_mem[0];
          
          operator delete(old_mem, std::align_val_t{block_alignment()});
          old_mem = tmp;
        }
        
        memory = nullptr;
        current_memory = nullptr;
        last_memory = nullptr;
        free_memory = nullptr;
      }
      
      size_t blocks_allocated() const {
        size_t counter = 0;
        char* old_mem = memory;
        while (old_mem != nullptr) {
          auto ptr_mem = reinterpret_cast<char**>(old_mem);
          char* tmp = ptr_mem[0];
          
          ++counter;
          old_mem = tmp;
        }
        
        return counter;
      }
      
      size_t block_size() const {
        return final_block_size();
      }
    private:
      char* memory;
      char* current_memory;
      char* last_memory;
      char* free_memory;
      
      constexpr static size_t align_to(const size_t &mem, const size_t &align) {
        return (mem + align - 1) / align * align;
      }
      
      constexpr size_t block_alignment() const {
        return std::max(alignof(T), alignof(char*));
      }

      constexpr size_t block_header_size() const {
        return align_to(sizeof(char*), alignof(T));
      }

      constexpr size_t min_block_size() const {
        return block_header_size() + sizeof(T);
      }

      constexpr size_t usable_bytes() const {
        return final_block_size() - block_header_size();
      }
      
      size_t final_block_size() const {
        return align_to(std::max(N, min_block_size()), block_alignment());
      }
      
      void allocate_memory() {
        const size_t block_size = final_block_size();
        char* new_memory = reinterpret_cast<char*>(operator new(block_size, std::align_val_t{block_alignment()}));
        auto ptr_mem = reinterpret_cast<char**>(new_memory);
        ptr_mem[0] = memory;
        memory = new_memory;
        current_memory = new_memory + block_header_size();
        last_memory = new_memory + block_size;
      }
    };
  }
}

#endif
