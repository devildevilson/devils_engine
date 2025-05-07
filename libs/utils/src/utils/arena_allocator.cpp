#include "arena_allocator.h"

#include <cassert>

namespace devils_engine {
  namespace utils {
    arena_allocator::arena_allocator(const size_t size, const size_t alignment) noexcept :
      m_current(nullptr),
      m_prev(nullptr),
      m_memory(nullptr),
      m_size(utils::align_to(size, aligment)),
      m_alignment(alignment)
    {}

    arena_allocator::~arena_allocator() noexcept {
      // предполагаем что аккуратно разрушили все объекты
      operator delete[] (m_memory, std::align_val_t{m_alignment});
    }

    arena_allocator::arena_allocator(arena_allocator &&move) noexcept :
      m_current(move.m_current),
      m_prev(move.m_prev),
      m_memory(move.m_memory),
      m_size(move.m_size),
      m_alignment(move.m_alignment)
    {
      move.m_current = nullptr;
      move.m_prev = nullptr;
      move.m_memory = nullptr;
    }

    arena_allocator & arena_allocator::operator=(arena_allocator &&move) noexcept {
      operator delete[] (m_memory, std::align_val_t{m_alignment});

      m_current = move.m_current;
      m_prev = move.m_prev;
      m_memory = move.m_memory;
      m_size = move.m_size;
      m_alignment = move.m_alignment;
      move.m_current = nullptr;
      move.m_prev = nullptr;
      move.m_memory = nullptr;

      return *this;
    }

    void* arena_allocator::allocate(const size_t size, const size_t alignment) noexcept {
      // эта проверка делается каждый божий раз, зачем? чтобы аккуратно сделать move
      // иначе все равно проверять видимо придется
      if (m_current == nullptr) init();

      // как мы теперь делаем аллокацию? мы должны выделить память для memory_header_t и размер
      const size_t final_alignment = std::max(alignment, m_alignment);
      const size_t final_obj_size = utils::align_to(size, final_alignment);
      const size_t final_size = final_obj_size + sizeof(memory_header_t);

      if (m_current + final_size > m_memory + m_size) return nullptr;

      char* start = m_current;
      char* ptr = start + sizeof(memory_header_t);

      // для того чтобы сделать версию с деструктором,
      // мы можем заранее создавать memory_header_t
      // где указывать предыдущий блок, не работает только когда m_current + final_size == m_memory + m_size
      new (start) memory_header_t(final_size, m_prev);
      m_prev = m_current;
      m_current += final_size;

      return ptr;
    }

    // предполагаем что мы аккуратно используем аллокатор и удаляем объекты только через destroy
    void arena_allocator::free(void* ptr) {
      auto mem = reinterpret_cast<char*>(ptr);
      mem -= sizeof(memory_header_t);

      utils_assertf(
        !(m_memory <= mem && mem <= m_memory + m_size),
        "Seems like ptr {} in not in range of this memory arena {} - {}",
        size_t(ptr), size_t(m_memory), size_t(m_memory + m_size)
      );

      auto h = reinterpret_cast<memory_header_t*>(mem);
      h->size = SIZE_MAX;
      // m_current всегда указывает на еще не созданный объект,
      // а значит m_prev указывает на последний созданный
      if (mem != m_prev) return;

      // последовательно обходим все предыдущие блоки, передвигаем указатель если предыдущие боки тоже разрушены
      do {
        m_prev = h->prev;
        m_current = reinterpret_cast<char*>(h);
        h = reinterpret_cast<memory_header_t*>(h->prev);
      } while (h != nullptr && h->size == SIZE_MAX);
    }

    size_t arena_allocator::size() const noexcept { return m_size; }
    size_t arena_allocator::alignment() const noexcept { return m_alignment; }
    size_t arena_allocator::remaining_size() const noexcept { return m_size - size_t(m_current - m_memory); }

    void arena_allocator::init() noexcept {
      m_current = new (std::align_val_t{m_alignment}) char[m_size];
      m_memory = m_current;
      assert(m_memory != nullptr);
    }
  }
}
