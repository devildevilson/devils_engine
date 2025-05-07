#ifndef DEVILS_ENGINE_UTILS_UNIQUE_PTR_H
#define DEVILS_ENGINE_UTILS_UNIQUE_PTR_H

#include <cstdint>
#include <cstddef>
#include <utility>
#include <new>

namespace devils_engine {
namespace utils {
template <typename T> //, typename Deleter = void
class unique_ptr {
public:
  unique_ptr() noexcept : m_memory(nullptr) {}
  template <typename... Args>
  unique_ptr(Args&&... args) : m_memory(nullptr) {
    reset(std::forward<Args>(args)...);
  }

  ~unique_ptr() noexcept { clear(); }

  unique_ptr(const unique_ptr &copy) noexcept : m_memory(nullptr) {
    reset(*copy.get());
  }

  unique_ptr(unique_ptr &&move) noexcept : m_memory(move.m_memory) {
    move.m_memory = nullptr;
  }

  unique_ptr & operator=(const unique_ptr &copy) noexcept {
    reset(*copy.get());
  }

  unique_ptr & operator=(unique_ptr &&move) noexcept {
    clear();

    m_memory = move.m_memory;
    move.m_memory = nullptr;
  }

  template <typename... Args>
  T* reset(Args&&... args) {
    if (m_memory != nullptr) {
      get()->~T();
      return new (m_memory) T(std::forward<Args>(args)...));
    }

    m_memory = new (std::align_val_t{alignof(T)}) char[sizeof(T)];
    return new (m_memory) T(std::forward<Args>(args)...));
  }

  void clear() noexcept {
    get()->~T();
    operator delete[] (m_memory, std::align_val_t{alignof(T)});
    m_memory = nullptr;
  }

  T* get() noexcept { return reinterpret_cast<T*>(m_memory); }
  const T* get() const noexcept { return reinterpret_cast<T*>(m_memory); }

  T* operator->() noexcept { return reinterpret_cast<T*>(m_memory); }
  const T* operator->() const noexcept { return reinterpret_cast<T*>(m_memory); }

  operator bool () const noexcept { return m_memory != nullptr; }
private:
  char* m_memory;
};
}
}

#endif
