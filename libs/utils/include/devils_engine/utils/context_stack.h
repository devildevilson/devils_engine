#ifndef DEVILS_ENGINE_UTILS_CONTEXT_STACK_H
#define DEVILS_ENGINE_UTILS_CONTEXT_STACK_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>
#include "utils/list.h"

namespace devils_engine {
namespace utils {
template <typename T>
class context_stack {
public:
  using underlying_t = T;

  template <typename... Args>
  context_stack(const size_t size, Args&&... args);

  T* pop() noexcept;
  void push(T* ptr) noexcept;

  size_t capacity() const noexcept;
private:
  struct storage : public utils::forw::list<storage, 0> {
    T obj;
    template <typename... Args>
    storage(Args&&... args) : obj(std::forward<Args>(args)...) {}
    T* get_ptr() const { return std::addressof(obj); }
  };

  static storage* make_storage(T* ptr) noexcept;

  std::vector<std::unique_ptr<storage>> m_memory;
  std::atomic<storage*> m_stack;
};

template <typename T>
class acquire_context {
public:
  using ctx_t = T::underlying_t;

  ctx_t* ptr;

  acquire_context(T& ref) noexcept;
  ~acquire_context() noexcept;
private:
  T& m_ref;
};

template <typename T>
template <typename... Args>
context_stack<T>::context_stack(const size_t size, Args&&... args) : m_memory(size, nullptr), m_stack(nullptr) {
  m_memory.clear();

  for (size_t i = 0; i < size; ++i) {
    m_memory.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
    assert(m_memory.back() == make_storage(m_memory.back()->get_ptr()));
  }

  for (auto itr = m_memory.rbegin(); itr != m_memory.rend(); ++itr) {
    utils::forw::atomic_list_push<0>(m_stack, (*itr));
  }
}

template <typename T>
T* context_stack<T>::pop() noexcept {
  auto storage_ptr = utils::forw::atomic_list_pop<0>(m_stack);
  return storage_ptr->get_ptr();
}

template <typename T>
void context_stack<T>::push(T* ptr) noexcept {
  auto storage_ptr = make_storage(ptr);
  utils::forw::atomic_list_push<0>(m_stack, storage_ptr);
}

template <typename T>
size_t context_stack<T>::capacity() const noexcept { return m_memory.size(); }

template <typename T>
context_stack<T>::storage* context_stack<T>::make_storage(T* ptr) noexcept {
  const size_t offset = offsetof(context_stack<T>::storage, obj);
  return reinterpret_cast<storage*>( reinterpret_cast<char*>(ptr) - offset );
}

template <typename T>
acquire_context<T>::acquire_context(T& ref) noexcept : ptr(nullptr), m_ref(ref) {
  while (ctx == nullptr) {
    ptr = m_ref.pop();
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }
}

template <typename T>
acquire_context<T>::~acquire_context() noexcept {
  m_ref.push(ptr);
}
}
}

#endif