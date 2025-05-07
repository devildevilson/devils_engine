#ifndef DEVILS_ENGINE_UTILS_ARENA_ALLOCATOR_H
#define DEVILS_ENGINE_UTILS_ARENA_ALLOCATOR_H

#include <cstdint>
#include <cstddef>
#include <new>
#include <utility>
#include "core.h"

// хорошая идея как можно сделать быструю арену памяти: pointer bumping (проталкивание указателя?)
// при аллокации пушим указатель на количество байт, при деаллокации уменьшаем
// непосредственно уменьшение происходит только когда мы удаляем "верхний" объект
// но мы можем пометить участок памяти к удалению, и когда деаллокация верхнего объекта все таки произойдет
// пропихнуть счетчик назад, на все ранее удаленные объекты
// супер хорошая идея на самом деле
// осталось понять как пометить участок, для каждого участка памяти должен быть хеадер, где мы укажем размер
// и наверное начало предыдущего участка?
// безопаснее конечно хранить указатель для разрушения объекта

//const auto align = alignof(std::max_align_t);

template <typename T>
inline void destroyer(void* ptr) noexcept { reinterpret_cast<T*>(ptr)->~T(); }
inline void destroyer(void*) noexcept {}

namespace devils_engine {
  namespace utils {
    class arena_allocator {
    public:
      // тут нужен довольно большой кусок памяти и алигмент, который в большинстве случаев будет 8 (не меньше)
      // алигмент редко будет больше 8 (в моем конкретном случае только когда я буду использовать simd)
      arena_allocator(const size_t size, const size_t alignment = 8) noexcept;
      ~arena_allocator() noexcept;

      arena_allocator() noexcept = delete;
      arena_allocator(const arena_allocator &copy) noexcept = delete;
      arena_allocator(arena_allocator &&move) noexcept;
      arena_allocator & operator=(const arena_allocator &copy) noexcept = delete;
      arena_allocator & operator=(arena_allocator &&move) noexcept;

      // имеет смысл выдавать как минимум sizeof(memory_header_t)*2
      void* allocate(const size_t size, const size_t alignment = 8) noexcept;
      // нужно выкинуть ошибку если указатель не отсюда
      // может быть еще передавать сюда алигнмент?
      void free(void* ptr);

      template <typename T, typename... Args>
      T* create(Args&&... args) {
        static_assert(alignof(T) <= 16, "ALignment > 16 is not supported");
        auto ptr = allocate(sizeof(T), alignof(T));
        utils_assertf(ptr != nullptr, "Out of memory: remaining_size {}", remaining_size());
        return new (ptr) T(std::forward<Args>(args)...);
      }

      template <typename T>
      void destroy(T* ptr) {
        ptr->~T();
        free(ptr);
      }

      size_t size() const noexcept;
      size_t alignment() const noexcept;
      size_t remaining_size() const noexcept;
    private:
      // наверное чутка позже сделаю версию с деаллокатором
      // другое дело что memory_header_t тогда станет слишком большим
      //using destructor_ptr = decltype(&(destroyer<arena_allocator>));

      struct memory_header_t {
        size_t size;
        char* prev;

        inline memory_header_t(const size_t size, char* prev) noexcept : size(size), prev(prev) {}
      };

      // вообще по идее можно освобождать и с другого конца тоже
      // нужно 2 указателя, точнее теперь память контролируется 3 указателями
      // указатель слева - в большинстве случаев будет совпадать с m_memory
      // центральный указатель - где разрушены объекты слева
      // и правый указатель - обычный, единственная проблема этой штуки
      // нужно в хедере обязательно хранить предыдущий и следующий объект (размер)
      char* m_current;
      char* m_prev;
      char* m_memory;
      size_t m_size;
      size_t m_alignment;

      void init() noexcept;
    };
  }
}

#endif
