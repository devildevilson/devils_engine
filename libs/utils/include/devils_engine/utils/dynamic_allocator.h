#ifndef DEVILS_ENGINE_UTILS_DYNAMIC_ALLOCATOR_H
#define DEVILS_ENGINE_UTILS_DYNAMIC_ALLOCATOR_H

#include <cstddef>
#include <tuple>

// чем хорош динамический аллокатор? тем что это похоже на стак + мы держим всю память
// на одном куске памяти, и можем по ней быстро пробежаться
// практически как в случае с вектором, только тут можно данные разного размера создавать
// тут довольно сложно сделать мультитрединг
// единственное что у меня нет представления о том какой тип тут содержится

#ifndef DE_DYN_ALLOCATOR_INIT_CAPACITY
#  define DE_DYN_ALLOCATOR_INIT_CAPACITY 160
#endif

namespace devils_engine {
  namespace utils {
    class dynamic_allocator {
    public:
      class iterator {
      public:
        iterator(char* mem) noexcept;
        ~iterator() noexcept = default;

        iterator(const iterator &copy) noexcept = default;
        iterator(iterator &&move) noexcept = default;
        iterator & operator=(const iterator &copy) noexcept = default;
        iterator & operator=(iterator &&move) noexcept = default;

        iterator & operator++() noexcept;
        iterator operator++(int) noexcept;
        // можно ли назад?
        // iterator & operator--() noexcept;
        // iterator operator--(int) noexcept;

        void* operator*() const noexcept;
        size_t size() const noexcept;
        iterator next() const noexcept;

        friend bool operator==(const iterator &a, const iterator &b);
        friend bool operator!=(const iterator &a, const iterator &b);
      private:
        char* cur;
      };

      dynamic_allocator(const size_t alignment, const float grow_policy = 2.0f) noexcept;
      dynamic_allocator(const size_t alignment, const size_t capacity, const float grow_policy = 2.0f) noexcept;
      ~dynamic_allocator() noexcept;

      dynamic_allocator(const dynamic_allocator &copy) noexcept = delete;
      dynamic_allocator(dynamic_allocator &&move) noexcept;
      dynamic_allocator & operator=(const dynamic_allocator &copy) noexcept = delete;
      dynamic_allocator & operator=(dynamic_allocator &&move) noexcept;

      // с таким аллокатором фигня заключается в том что довольно быстро указатель на память отвалится
      // и че делать тогда? мне бы все равно не помешал бы способ пробежаться по массиву памяти
      std::tuple<void*, size_t> allocate(const size_t size) noexcept;
      // можно ли почистить? тут очистка такая же как у стак аллокатора
      void clear() noexcept;

      template <typename T, typename... Args>
      std::tuple<T*, size_t> create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>);
        static_assert(std::is_trivially_copyable_v<T>);

        auto [ ptr, index ] = allocate(sizeof(T));
        auto obj_ptr = new (ptr) T(std::forward<Args>(args)...);

        return std::tie(obj_ptr, index);
      }

      // тут придется делать итератор
      iterator begin() const noexcept;
      iterator seek(const size_t index) const noexcept;
      iterator end() const noexcept;

      std::tuple<void*, size_t> front() const;
      std::tuple<void*, size_t> back() const; // O(count)
      std::tuple<void*, size_t> pop_back(); // O(count)

      float grow_policy() const noexcept;
      size_t alignment() const noexcept;
      size_t size() const noexcept;
      size_t capacity() const noexcept;
    private:
      float m_grow_policy;
      size_t m_alignment;
      size_t m_count;
      size_t m_size;
      size_t m_capacity;
      char* m_memory;

      void new_memory_and_copy();
    };
  }
}

#endif
