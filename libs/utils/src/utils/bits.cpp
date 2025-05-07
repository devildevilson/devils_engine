/* Modified version of https://github.com/wroniasty/bits */
#include <cstdint>
#include <cstring>
#include "bits.h"

namespace devils_engine {
  namespace utils {
    size_t hexdump(const uint8_t* buffer, const size_t length, char* out, const size_t max_size) {
      const uint8_t mask = 0xff;

      size_t count = 0;
      for (size_t i = 0; i < length && count < max_size; ++i) {
        for (size_t j = 0; j < sizeof(uint8_t) * 2 && count < max_size; ++j) {
          const uint8_t byte_half = (buffer[j] >> 4 * j) & mask;
          out[count] = hexstr(byte_half);
          count += 1;
        }
      }

      return count;
    }
    
    size_t binstr(const uint8_t v, char* buffer, const size_t max_size) {
      uint8_t m = 128;

      size_t count = 0;
      while (m > 0 && count < max_size) {
        buffer[count] = (v & m) ? '1' : '0';
        count += 1;
        m >>= 1;
      }

      return count;
    } 

    size_t binstr(const std::span<uint8_t> &arr, char* buffer, const size_t max_size) {
      size_t count = 0;
      for (size_t i = 0; i < arr.size() && count < max_size; ++i) {
        count += binstr(arr[i], &buffer[count], max_size-count); 
      }
      return count;
    }

    // uint8_t setbits(const uint8_t c, const size_t offset, const size_t numbits, const uint8_t v) {
    //   uint8_t stamp = (v    << (uint8_significant_bits_count-numbits)), 
    //           mask  = (0xff << (uint8_significant_bits_count-numbits));
    //   stamp >>= offset; 
    //   mask = ~(mask >>= offset);
    //   return (c & mask) | stamp; 
    // }
  }
}
