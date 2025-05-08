#include "dynamic_stack_allocator.h"

#include <cassert>

namespace devils_engine {
  namespace utils {
    dynamic_stack_allocator::dynamic_stack_allocator(const size_t size, const size_t aligment, const float grow_policy) noexcept :
      m_aligment(aligment), m_size(utils::align_to(size, m_aligment)), m_memory(nullptr), m_allocated(0), m_grow_policy(grow_policy)
    {}

    dynamic_stack_allocator::~dynamic_stack_allocator() noexcept {
      operator delete[](m_memory, std::align_val_t{m_aligment});
    }

    dynamic_stack_allocator::dynamic_stack_allocator(dynamic_stack_allocator &&move) noexcept :
      m_aligment(move.m_aligment), m_size(move.m_size), m_memory(move.m_memory), m_allocated(move.m_allocated), m_grow_policy(move.m_grow_policy)
    {
      move.m_memory = nullptr;
    }

    dynamic_stack_allocator & dynamic_stack_allocator::operator=(dynamic_stack_allocator &&move) noexcept {
      operator delete[](m_memory, std::align_val_t{m_aligment});

      m_aligment = move.m_aligment; 
      m_size = move.m_size;
      m_memory = move.m_memory;
      m_allocated = move.m_allocated;
      m_grow_policy = move.m_grow_policy;
      move.m_memory = nullptr;

      return *this;
    }

    void* dynamic_stack_allocator::allocate(const size_t size) {
      const size_t final_size = utils::align_to(size, m_aligment);

      if (m_memory == nullptr || m_allocated + final_size > m_size) {
        const auto [ new_mem, new_size ] = grow_memory(m_memory);
        m_memory = new_mem;
        m_size = new_size;
      }

      auto ptr = &m_memory[m_allocated];
      m_allocated += final_size;
      return ptr;
    }

    void dynamic_stack_allocator::clear() noexcept {
      m_allocated = 0;
    }

    size_t dynamic_stack_allocator::capacity() const noexcept { return m_size; }
    size_t dynamic_stack_allocator::aligment() const noexcept { return m_aligment; }
    size_t dynamic_stack_allocator::size() const noexcept { return m_allocated; }
    char* dynamic_stack_allocator::data() noexcept { return m_memory; }
    const char* dynamic_stack_allocator::data() const noexcept { return m_memory; }

    std::tuple<char*, size_t> dynamic_stack_allocator::grow_memory(char* mem) const noexcept {
      if (mem == nullptr) {
        char* new_mem = new (std::align_val_t{m_aligment}) char[m_size];
        return std::make_tuple(new_mem, m_size);
      }

      assert(m_grow_policy > 1.0f);

      const size_t new_size = m_size * m_grow_policy;
      char* new_mem = ::new (std::align_val_t{m_aligment}) char[new_size];
      assert(new_mem != nullptr);
      memcpy(new_mem, mem, m_allocated);
      operator delete[](mem, std::align_val_t{m_aligment});

      return std::make_tuple(new_mem, new_size);
    }
  }
}