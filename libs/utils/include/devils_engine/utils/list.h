#ifndef DEVILS_ENGINE_UTILS_LIST_H
#define DEVILS_ENGINE_UTILS_LIST_H

#include <type_traits>
#include <cstddef>
#include <atomic>

// кажется никакого динамического каста не используется

namespace devils_engine {
  namespace utils {

    namespace forw {
      template <typename T, size_t t>
      struct list {
        using current_list_p = list<T, t>*;
        T* m_next;

        list() noexcept : m_next(nullptr) {}
        ~list() noexcept { invalidate(); }

        void add(T* obj) noexcept {
          static_assert(std::is_base_of_v<list<T, t>, T>);
          current_list_p l = obj;
          l->m_next = m_next;
          m_next = obj;
        }

        void set(T* obj) noexcept {
          static_assert(std::is_base_of_v<list<T, t>, T>);
          m_next = obj;
        }

        void remove(T* prev) noexcept {
          static_assert(std::is_base_of_v<list<T, t>, T>);
          current_list_p l = prev;
          l->m_next = m_next;
          m_next = nullptr;
        }

        void invalidate() noexcept { m_next = nullptr; }
        bool empty() const noexcept { return m_next == nullptr; }
      };

      template <size_t t, typename T>
      void list_add(T* cur, T* obj) noexcept {
        list<T, t>* l = cur;
        l->add(obj);
      }

      template <size_t t, typename T>
      void list_set(T* cur, T* obj) noexcept {
        list<T, t>* l = cur;
        l->set(obj);
      }

      template <size_t t, typename T>
      T* list_next(T* cur) noexcept {
        const list<T, t>* l = cur;
        return l->m_next;
      }

      template <size_t t, typename T>
      void list_remove(T* root, T* cur) noexcept {
        auto ptr = root;
        while (ptr != nullptr) {
          list<T, t>* l = ptr;
          if (l->m_next == cur) {
            list<T, t>* cur_l = cur;
            cur_l->remove(ptr);
            break;
          }
          ptr = list_next<t>(ptr);
        }
      }
      
      template <size_t t, typename T>
      bool list_empty(const T* cur) noexcept {
        const list<T, t>* l = cur;
        return l->empty();
      }
      
      template <size_t t, typename T>
      void list_invalidate(T* cur) noexcept {
        list<T, t>* l = cur;
        l->invalidate();
      }
      
      template <size_t t, typename T>
      size_t list_count(T* cur) noexcept {
        size_t counter = 0;
        for (auto c = cur; c != nullptr; c = list_next<t>(c)) { ++counter; }
        return counter;
      }

      // бегать по такому стэку не получится
      // как сделать из этого queue?
      // stack
      template <size_t t, typename T>
      void atomic_list_push(std::atomic<T*> &cur, T* obj) noexcept {
        list<T, t>* l = obj;
        l->m_next = cur.load(std::memory_order_relaxed);
        while (!cur.compare_exchange_weak(
          l->m_next, obj,
          std::memory_order_release,
          std::memory_order_relaxed
        ));
      }

      // stack
      template <size_t t, typename T>
      T* atomic_list_pop(std::atomic<T*> &cur) noexcept {
        auto cur_obj = cur.load(std::memory_order_relaxed);
        list<T, t>* l = cur_obj;
        while (!cur.compare_exchange_weak(
          cur_obj, l->m_next,
          std::memory_order_release,
          std::memory_order_relaxed
        )) {
          l = cur_obj;
        }

        list_invalidate<t>(cur_obj);
        return cur_obj;
      }
    }
    
    namespace ring {
      template <typename T, size_t t>
      struct list {
        using current_list_p = list<T, t>*;
        
        T* m_next;
        T* m_prev;
        
        // использовать this плохая идея для контейнеров вроде std vector
        // хотя в этом случае вся структура не имеет смысла
        list() noexcept : m_next(static_cast<T*>(this)), m_prev(static_cast<T*>(this)) {}
        // пока непонятно насколько это испортит разные списки в игре
        // но с другой стороны, если мы в этот момент не будем ничего обходить
        // то так аккуратно уберем невалидные данные
        ~list() noexcept { remove(); }
    
        void add(T* obj) noexcept {
          static_assert(std::is_base_of_v<list<T, t>, T>);
          current_list_p l = obj;
          l->remove();

          auto cur = static_cast<T*>(this);
          l->m_next = m_next;
          l->m_prev = cur;
          current_list_p n = m_next;
          m_next = obj;
          n->m_prev = obj;
        }
        
        void radd(T* obj) noexcept {
          static_assert(std::is_base_of_v<list<T, t>, T>);
          current_list_p l = obj;
          l->remove();

          auto cur = static_cast<T*>(this);
          l->m_next = cur;
          l->m_prev = m_prev;
          current_list_p n = m_prev;
          m_prev = obj;
          n->m_next = obj;
        }
        
        void remove() noexcept {
          static_assert(std::is_base_of_v<list<T, t>, T>);
          auto l_next = static_cast<current_list_p>(m_next);
          auto l_prev = static_cast<current_list_p>(m_prev);
          l_next->m_prev = m_prev;
          l_prev->m_next = m_next;
          m_prev = static_cast<T*>(this);
          m_next = static_cast<T*>(this);
        }
        
        void invalidate() noexcept { m_next = static_cast<T*>(this); m_prev = static_cast<T*>(this); }
        bool empty() const noexcept { return m_next == static_cast<const T*>(this) && m_prev == static_cast<const T*>(this); }
      };
    
      template <size_t t, typename T>
      void list_add(T* cur, T* obj) noexcept {
        if (obj == nullptr) return;
        list<T, t>* l = cur;
        l->add(obj);
      }
      
      template <size_t t, typename T>
      void list_radd(T* cur, T* obj) noexcept {
        if (obj == nullptr) return;
        list<T, t>* l = cur;
        l->radd(obj);
      }

      template <size_t t, typename T>
      void list_remove(T* cur) noexcept {
        list<T, t>* l = cur;
        l->remove();
      }

      template <size_t t, typename T>
      T* list_next(const T* cur, const T* ref) noexcept {
        const list<T, t>* l = cur;
        return cur != nullptr && l->m_next != ref ? l->m_next : nullptr;
      }

      template <size_t t, typename T>
      T* list_prev(const T* cur, const T* ref) noexcept {
        const list<T, t>* l = cur;
        return cur != nullptr && l->m_prev != ref ? l->m_prev : nullptr;
      }
      
      template <size_t t, typename T>
      bool list_empty(const T* cur) noexcept {
        const list<T, t>* l = cur;
        return l->empty();
      }
      
      template <size_t t, typename T>
      void list_invalidate(T* cur) noexcept {
        list<T, t>* l = cur;
        l->invalidate();
      }
      
      template <size_t t, typename T>
      size_t list_count(T* cur) noexcept {
        size_t counter = 0;
        for (auto c = cur; c != nullptr; c = list_next<t>(c, cur)) { ++counter; }
        return counter;
      }
    }
  }
}

#endif
