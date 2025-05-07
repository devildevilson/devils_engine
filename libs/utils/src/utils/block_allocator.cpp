#include "block_allocator.h"

#include "core.h"
#include <cassert>

namespace devils_engine {
  namespace utils {
    block_allocator::block_allocator(const size_t memory_size, const size_t block_size, const size_t block_align) noexcept :
      m_align(std::max(block_align, sizeof(void*))),
      m_block_size(utils::align_to(block_size, m_align)),
      m_current_memory(m_align),
      m_memory_size(utils::align_to(memory_size, m_align)),
      m_memory(nullptr),
      m_free_memory(nullptr)
    {
      //create_new_memory();
    }

    block_allocator::~block_allocator() noexcept {
      clear();
    }

    block_allocator::block_allocator(block_allocator &&move) noexcept :
      m_align(move.m_align),
      m_block_size(move.m_block_size),
      m_current_memory(move.m_current_memory),
      m_memory_size(move.m_memory_size),
      m_memory(move.m_memory),
      m_free_memory(move.m_free_memory)
    {
      move.m_memory = nullptr;
      move.m_free_memory = nullptr;
      move.m_current_memory = m_align;
    }

    block_allocator & block_allocator::operator=(block_allocator &&move) noexcept {
      clear();

      m_align = move.m_align;
      m_block_size = move.m_block_size;
      m_current_memory = move.m_current_memory;
      m_memory_size = move.m_memory_size;
      m_memory = move.m_memory;
      m_free_memory = move.m_free_memory;
      move.m_memory = nullptr;
      move.m_free_memory = nullptr;
      move.m_current_memory = m_align;

      return *this;
    }

    void* block_allocator::allocate() noexcept {
      if (m_free_memory != nullptr) {
        void* tmp = m_free_memory;
        m_free_memory = reinterpret_cast<char**>(m_free_memory)[0];
        return tmp;
      }

      if (m_memory == nullptr || m_current_memory + m_block_size > m_memory_size) {
        create_new_memory();
      }

      char* mem = &m_memory[m_current_memory];
      m_current_memory += m_block_size;
      return mem;
    }

    void* block_allocator::allocate1(const size_t count) noexcept {
      // имеет смысл реализовать аллокацию нескольких участков разом
      // но это в таком дизайне довольно сложно, нужно проверить
      // есть ли свободные участки памяти
      return nullptr;
    }

    void block_allocator::free(void* mem) noexcept {
      reinterpret_cast<void**>(mem)[0] = m_free_memory;
      m_free_memory = mem;
    }

    void block_allocator::clear() noexcept {
      char* current = m_memory;
      while (current != nullptr) {
        char* tmp = reinterpret_cast<char**>(current)[0];
        ::operator delete[] (current, std::align_val_t{m_align});
        current = tmp;
      }

      m_current_memory = m_align;
      m_memory = nullptr;
      m_free_memory = nullptr;
    }

    size_t block_allocator::alignment() const noexcept { return m_align; }
    size_t block_allocator::allocation_size() const noexcept { return m_block_size; }
    size_t block_allocator::size() const noexcept { return m_memory_size; }
    size_t block_allocator::compute_full_size() const noexcept {
      size_t counter = 0;
      char* current = m_memory;
      while (current != nullptr) {
        char* tmp = reinterpret_cast<char**>(current)[0];
        counter += m_memory_size;
        current = tmp;
      }

      return counter;
    }

    void block_allocator::create_new_memory() noexcept {
      // char* new_mem = new (std::align_val_t{ m_align }) char[m_memory_size + m_align];
      char* new_mem = reinterpret_cast<char*>(::operator new((m_memory_size + m_align) * sizeof(char), std::align_val_t{m_align}));
      assert(new_mem != nullptr);
      reinterpret_cast<char**>(new_mem)[0] = m_memory;
      m_memory = new_mem;
      m_current_memory = m_align;
    }

    char* const unallocated_memory_ptr = reinterpret_cast<char*>(SIZE_MAX);

    block_allocator_mt::block_allocator_mt(const size_t memory_size, const size_t block_size, const size_t block_align) noexcept :
      m_align(std::max(block_align, sizeof(void*))),
      m_block_size(utils::align_to(block_size, m_align)),
      m_current_memory(m_align),
      m_memory_size(utils::align_to(memory_size, m_align)),
      m_memory(unallocated_memory_ptr),
      m_free_memory(nullptr)
    {
      //create_new_memory();
    }

    block_allocator_mt::~block_allocator_mt() noexcept {
      clear();
    }

    block_allocator_mt::block_allocator_mt(block_allocator_mt &&move) noexcept :
      m_align(move.m_align),
      m_block_size(move.m_block_size),
      m_current_memory(move.m_current_memory),
      m_memory_size(move.m_memory_size),
      m_memory(move.m_memory.load()),
      m_free_memory(move.m_free_memory.load())
    {
      move.m_memory = nullptr;
      move.m_free_memory = nullptr;
      move.m_current_memory = m_align;
    }

    block_allocator_mt & block_allocator_mt::operator=(block_allocator_mt &&move) noexcept {
      clear();

      m_align = move.m_align;
      m_block_size = move.m_block_size;
      m_current_memory = move.m_current_memory;
      m_memory_size = move.m_memory_size;
      m_memory = move.m_memory.load();
      m_free_memory = move.m_free_memory.load();
      move.m_memory = nullptr;
      move.m_free_memory = nullptr;
      move.m_current_memory = m_align;

      return *this;
    }

    void* block_allocator_mt::allocate() noexcept {
      void* tmp = nullptr;
      while (!m_free_memory.compare_exchange_strong(tmp, nullptr)) {
        void* next = reinterpret_cast<char**>(tmp)[0];
        if (m_free_memory.compare_exchange_strong(tmp, next)) return tmp;

        tmp = nullptr;
      }

      auto mem = spin_lock();

      if (mem == unallocated_memory_ptr || m_current_memory + m_block_size > m_memory_size) {
        mem = create_new_memory(mem);
      }

      char* ptr = &mem[m_current_memory];
      m_current_memory += m_block_size;

      m_memory = mem; // один return, так что легко понятно когда присваивание должно происходить
      return ptr;
    }

    void block_allocator_mt::free(void* ptr) noexcept {
      reinterpret_cast<void**>(ptr)[0] = m_free_memory.exchange(ptr);
    }

    void block_allocator_mt::clear() noexcept {
      char* current = m_memory;
      while (current != nullptr) {
        char* tmp = reinterpret_cast<char**>(current)[0];
        operator delete[] (current, std::align_val_t{m_align});
        current = tmp;
      }

      m_current_memory = m_align;
      m_memory = nullptr;
      m_free_memory = nullptr;
    }

    size_t block_allocator_mt::alignment() const noexcept { return m_align; }
    size_t block_allocator_mt::allocation_size() const noexcept { return m_block_size; }
    size_t block_allocator_mt::size() const noexcept { return m_memory_size; }
    size_t block_allocator_mt::compute_full_size() const noexcept {
      size_t counter = 0;
      char* current = m_memory;
      while (current != nullptr) {
        char* tmp = reinterpret_cast<char**>(current)[0];
        counter += m_memory_size;
        current = tmp;
      }

      return counter;
    }

    char* block_allocator_mt::create_new_memory(char* old_mem) noexcept {
      //char* new_mem = new (std::align_val_t{m_align}) char[m_memory_size + m_align];
      char* new_mem = reinterpret_cast<char*>(::operator new((m_memory_size + m_align) * sizeof(char), std::align_val_t{ m_align }));
      assert(new_mem != nullptr);
      reinterpret_cast<char**>(new_mem)[0] = old_mem;
      //memory = new_mem;
      m_current_memory = m_align;

      return new_mem;
    }

    char* block_allocator_mt::spin_lock() noexcept {
      char* mem = nullptr;
      do {
        std::this_thread::sleep_for(std::chrono::microseconds(1)); // с этой строкой работает стабильнее
        mem = m_memory.exchange(nullptr);
      } while (mem == nullptr);

      return mem;
    }
  }
}
