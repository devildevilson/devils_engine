#ifndef DEVILS_ENGINE_PAINTER_STAGE_BUFFER_H
#define DEVILS_ENGINE_PAINTER_STAGE_BUFFER_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <atomic>

// обычно такого рода буферы это пара из буфера в памяти хоста и буфера в памяти устройства
// и обычно это просто статический буфер в который заполняется структура данных последовательно
// к ним нужно еще присобачить дескриптор
// было бы неплохо прочитать контент шейдера и понять в какой слот это дело пойдет
// но если я могу на лету компилировать шейдеры, то можно для них задать кучу разных define'ов
// в том числе дефайн с полным описанием буфера 

namespace devils_engine {
namespace painter {
// напоминает обычный стак аллокатор
// очень малое количество буферов обладает еще небольшими данными в самом начале
// по идее эти буферы могут быть как инпутами так и аутпутами
class stage_buffer {
public:
  std::string name;

  // device
  // buffer_host
  // buffer_gpu

  stage_buffer(std::string name, void* device, const size_t offset, const size_t initial_capacity);
  ~stage_buffer() noexcept;

  char* allocate(const size_t size);
  char* raw_memory(const size_t offset);

  template <typename T>
  T* header() {
    if (sizeof(T) > offset()) return nullptr;
    auto ptr = raw_memory(0);
    return reinterpret_cast<T*>(ptr);
  }

  template <typename T, typename... Args>
  T* push(Args&&... args) {
    auto ptr = allocate(sizeof(T));
    if (ptr == nullptr) return nullptr;
    new (ptr) T(std::forward<Args>(args)...);
    return ptr;
  }

  template <typename T>
  T* at(const size_t index) {
    auto ptr = raw_memory(offset() + index * sizeof(T));
    return reinterpret_cast<T*>(ptr);
  }

  inline void clear() { _size = 0; }

  inline size_t offset() const { return _offset; }
  inline size_t capacity() const { return _capacity; }
  inline size_t size() const { return _size; }
  template <typename T>
  size_t count() const { return _size / sizeof(T); }
private:
  size_t _offset;
  size_t _capacity;
  std::atomic<size_t> _size; // размер лучше наверное указывать в байтах
};
}
}



#endif