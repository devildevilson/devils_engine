#include "stack_allocator.h"

#include "core.h"

namespace devils_engine {
  namespace utils {
    stack_allocator::stack_allocator(const size_t size, const size_t aligment) noexcept :
      m_aligment(aligment),
      m_size(utils::align_to(size, m_aligment)),
      m_memory(reinterpret_cast<char*>(::operator new(m_size, std::align_val_t{m_aligment}))),
      m_allocated(0)
    {}

    stack_allocator::~stack_allocator() noexcept { ::operator delete[] (m_memory, std::align_val_t{m_aligment}); }

    stack_allocator::stack_allocator(stack_allocator &&move) noexcept :
      m_aligment(move.m_aligment),
      m_size(move.m_size),
      m_memory(move.m_memory),
      m_allocated(move.m_allocated)
    {
      move.m_memory = nullptr; // нужно ли новый создать? хотя это мув
      move.m_size = 0;
      move.m_allocated = 0;
    }

    stack_allocator & stack_allocator::operator=(stack_allocator &&move) noexcept {
      ::operator delete[] (m_memory, std::align_val_t{m_aligment});

      m_aligment = move.m_aligment;
      m_size = move.m_size;
      m_memory = move.m_memory;
      m_allocated = move.m_allocated;
      move.m_memory = nullptr; // нужно ли новый создать? хотя это мув
      move.m_size = 0;
      move.m_allocated = 0;

      return *this;
    }

    void* stack_allocator::allocate(const size_t size) noexcept {
      if (m_memory == nullptr || size == 0) return nullptr;

      const size_t final_size = utils::align_to(size, m_aligment);
      const size_t offset = m_allocated;
      if (offset + final_size > m_size) return nullptr;
      m_allocated += final_size;
      return &m_memory[offset];
    }

    void stack_allocator::clear() noexcept { m_allocated = 0; }
    size_t stack_allocator::capacity() const noexcept { return m_size; }
    size_t stack_allocator::aligment() const noexcept { return m_aligment; }
    size_t stack_allocator::size() const noexcept { return m_allocated; }
    char* stack_allocator::data() noexcept { return m_memory; }
    const char* stack_allocator::data() const noexcept { return m_memory; }

    stack_allocator_mt::stack_allocator_mt(const size_t size, const size_t aligment) noexcept :
      m_aligment(aligment),
      m_size(utils::align_to(size, m_aligment)),
      m_memory(reinterpret_cast<char*>(::operator new(m_size, std::align_val_t{m_aligment}))),
      m_allocated(0)
    {}

    stack_allocator_mt::~stack_allocator_mt() noexcept { ::operator delete[] (m_memory, std::align_val_t{m_aligment}); }

    stack_allocator_mt::stack_allocator_mt(stack_allocator_mt &&move) noexcept :
      m_aligment(move.m_aligment),
      m_size(move.m_size),
      m_memory(move.m_memory),
      m_allocated(move.m_allocated.load())
    {
      move.m_memory = nullptr; // нужно ли новый создать? хотя это мув
      move.m_size = 0;
      move.m_allocated = 0;
    }

    stack_allocator_mt & stack_allocator_mt::operator=(stack_allocator_mt &&move) noexcept {
      ::operator delete[] (m_memory, std::align_val_t{m_aligment});

      m_aligment = move.m_aligment;
      m_size = move.m_size;
      m_memory = move.m_memory;
      m_allocated = move.m_allocated.load();
      move.m_memory = nullptr; // нужно ли новый создать? хотя это мув
      move.m_size = 0;
      move.m_allocated = 0;

      return *this;
    }

    void* stack_allocator_mt::allocate(const size_t size) noexcept {
      if (m_memory == nullptr || size == 0) return nullptr;

      const size_t final_size = utils::align_to(size, m_aligment);
      const size_t offset = m_allocated.fetch_add(final_size);
      if (offset + final_size > m_size) return nullptr;
      return &m_memory[offset];
    }

    void stack_allocator_mt::clear() noexcept { m_allocated = 0; }
    size_t stack_allocator_mt::size() const noexcept { return m_size; }
    size_t stack_allocator_mt::aligment() const noexcept { return m_aligment; }
    size_t stack_allocator_mt::allocated_size() const noexcept { return m_allocated; }
    char* stack_allocator_mt::data() noexcept { return m_memory; }
    const char* stack_allocator_mt::data() const noexcept { return m_memory; }
  }
}
