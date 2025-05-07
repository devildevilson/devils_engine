#include "dynamic_allocator.h"

#include <algorithm>
#include <cstring>
#include "core.h"

namespace devils_engine {
  namespace utils {
    dynamic_allocator::iterator::iterator(char* mem) noexcept : cur(mem) {}

    dynamic_allocator::iterator & dynamic_allocator::iterator::operator++() noexcept {
      const size_t advance = reinterpret_cast<size_t*>(cur)[0];
      cur += advance + sizeof(size_t);
      return *this;
    }

    dynamic_allocator::iterator dynamic_allocator::iterator::operator++(int) noexcept {
      const auto itr = *this;
      const size_t advance = reinterpret_cast<size_t*>(cur)[0];
      cur += advance + sizeof(size_t);
      return itr;
    }

//     dynamic_allocator::iterator & dynamic_allocator::iterator::operator--() noexcept {
//
//     }
//
//     dynamic_allocator::iterator dynamic_allocator::iterator::operator--(int) noexcept {
//
//     }

    void* dynamic_allocator::iterator::operator*() const noexcept { return cur + sizeof(size_t); }
    size_t dynamic_allocator::iterator::size() const noexcept { return reinterpret_cast<size_t*>(cur)[0]; }
    dynamic_allocator::iterator dynamic_allocator::iterator::next() const noexcept {
      const size_t advance = reinterpret_cast<size_t*>(cur)[0];
      auto next_ptr = cur + advance + sizeof(size_t);
      return iterator(next_ptr);
    }

    bool operator==(const dynamic_allocator::iterator &a, const dynamic_allocator::iterator &b) {
      return a.cur == b.cur;
    }

    bool operator!=(const dynamic_allocator::iterator &a, const dynamic_allocator::iterator &b) {
      return a.cur != b.cur;
    }

    dynamic_allocator::dynamic_allocator(const size_t alignment, const float grow_policy) noexcept :
      m_grow_policy(grow_policy),
      m_alignment(std::max(alignment, sizeof(size_t))),
      m_count(0),
      m_size(0),
      m_capacity(0),
      m_memory(nullptr)
    {}

    dynamic_allocator::dynamic_allocator(const size_t alignment, const size_t capacity, const float grow_policy) noexcept :
      m_grow_policy(grow_policy),
      m_alignment(std::max(alignment, sizeof(size_t))),
      m_count(0),
      m_size(0),
      m_capacity(capacity),
      m_memory(nullptr)
    {
      new_memory_and_copy();
    }

    dynamic_allocator::~dynamic_allocator() noexcept {
      operator delete[] (m_memory, std::align_val_t{m_alignment});
    }

    dynamic_allocator::dynamic_allocator(dynamic_allocator &&move) noexcept :
      m_grow_policy(move.m_grow_policy),
      m_alignment(move.m_alignment),
      m_count(move.m_count),
      m_size(move.m_size),
      m_capacity(move.m_capacity),
      m_memory(move.m_memory)
    {
      move.m_memory = nullptr;
      move.m_count = 0;
      move.m_size = 0;
      move.m_capacity = 0;
    }

    dynamic_allocator & dynamic_allocator::operator=(dynamic_allocator &&move) noexcept {
      operator delete[] (m_memory, std::align_val_t{m_alignment});

      m_grow_policy = move.m_grow_policy;
      m_alignment = move.m_alignment;
      m_size = move.m_size;
      m_capacity = move.m_capacity;
      m_memory = move.m_memory;
      move.m_memory = nullptr;
      move.m_count = 0;
      move.m_size = 0;
      move.m_capacity = 0;

      return *this;
    }

    std::tuple<void*, size_t> dynamic_allocator::allocate(const size_t size) noexcept {
      const size_t final_size = utils::align_to(size, m_alignment);

      while (m_memory == nullptr || m_size + sizeof(size_t) + final_size > m_capacity) {
        new_memory_and_copy();
      }

      const size_t cur = m_size;
      auto ptr = &m_memory[cur];
      reinterpret_cast<size_t*>(ptr)[0] = final_size;
      ptr += sizeof(size_t);
      m_size += sizeof(size_t) + final_size;
      const size_t cur_index = m_count;
      m_count += 1;

      return std::tie(ptr, cur_index);
    }

    void dynamic_allocator::clear() noexcept {
      m_size = 0;
      m_count = 0;
    }

    // тут придется делать итератор
    dynamic_allocator::iterator dynamic_allocator::begin() const noexcept {
      return iterator(m_memory);
    }

    dynamic_allocator::iterator dynamic_allocator::seek(const size_t index) const noexcept {
      auto itr = begin();
      for (size_t i = 0; i < index && itr != end(); ++i, ++itr) {}
      return itr;
    }

    dynamic_allocator::iterator dynamic_allocator::end() const noexcept {
      return iterator(&m_memory[m_size]);
    }

    std::tuple<void*, size_t> dynamic_allocator::front() const {
      return std::make_tuple(m_memory + sizeof(size_t), reinterpret_cast<size_t*>(m_memory)[0]);
    }
    std::tuple<void*, size_t> dynamic_allocator::back() const {
      if (m_count == 0) utils::error("empty dynamic_allocator");
      const auto itr = seek(m_count-1);
      if (itr == end()) utils::error("Unexpected dynamic_allocator behaviour: count {} size {}", m_count, m_size);
      return std::make_tuple(*itr, itr.size());
    }

    std::tuple<void*, size_t> dynamic_allocator::pop_back() {
      if (m_count == 0) utils::error("empty dynamic_allocator");
      if (m_count == 1) { clear(); return std::make_tuple(nullptr, 0); }

      const auto new_last = seek(m_count-2);
      const auto last = new_last.next();
      m_size -= last.size() + sizeof(size_t);
      m_count -= 1;
      return std::make_tuple(*new_last, new_last.size());
    }

    float dynamic_allocator::grow_policy() const noexcept { return m_grow_policy; }
    size_t dynamic_allocator::alignment() const noexcept { return m_alignment; }
    size_t dynamic_allocator::size() const noexcept { return m_size; }
    size_t dynamic_allocator::capacity() const noexcept { return m_capacity; }

    void dynamic_allocator::new_memory_and_copy() {
      if (m_grow_policy <= 1.0f) utils::error("dynamic_allocator has m_grow_policy {}", m_grow_policy);

      auto old_memory = m_memory;
      const size_t old_capacity = m_capacity;

      m_capacity = m_grow_policy * (m_capacity == 0 ? DE_DYN_ALLOCATOR_INIT_CAPACITY : m_capacity);
      m_memory = new (std::align_val_t{m_alignment}) char[m_capacity];

      if (old_memory == nullptr) return;

      memcpy(m_memory, old_memory, old_capacity);
      operator delete[] (old_memory, std::align_val_t{m_alignment});
    }
  }
}
