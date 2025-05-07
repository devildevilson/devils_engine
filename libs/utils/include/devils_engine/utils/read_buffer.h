#ifndef DEVILS_ENGINE_UTILS_READ_BUFFER_H
#define DEVILS_ENGINE_UTILS_READ_BUFFER_H

#include <cstddef>
#include "utils/core.h"

namespace devils_engine {
  namespace utils {
    template <typename T>
    class read_buffer {
    public:
      read_buffer(const T* buf) noexcept : buffer(buf), size(0) {}
      
      template <typename S>
      S read() {
        const size_t read_size = sizeof(S);
        auto ptr = &reinterpret_cast<const char*>(buffer.data())[size];
        size += read_size;
        return *reinterpret_cast<S*>(ptr);
      }

      template <size_t maximum = 255>
      size_t read() {
        
      }
    private:
      const T* buffer;
      size_t size;
    };
  }
} 

#endif