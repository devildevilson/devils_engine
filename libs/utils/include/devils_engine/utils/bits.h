/** 
  Modified version of https://github.com/wroniasty/bits
*/
#ifndef DEVILS_ENGINE_UTILS_BITS_H
#define DEVILS_ENGINE_UTILS_BITS_H

#include <cstdint>
#include <string>
#include <iostream>
#include <cassert>
#include <bit>
#include <limits>
#include <span>
#include "core.h"

namespace devils_engine {
  namespace utils {
    constexpr size_t uint8_significant_bits_count = count_significant(std::numeric_limits<uint8_t>::max());
    static_assert(uint8_significant_bits_count == 8);

    constexpr char hexstr(const uint8_t v) {
      char c = '\0';

      switch (v) {
        case  0: c = '0'; break;
        case  1: c = '1'; break;
        case  2: c = '2'; break;
        case  3: c = '3'; break;
        case  4: c = '4'; break;
        case  5: c = '5'; break;
        case  6: c = '6'; break;
        case  7: c = '7'; break;
        case  8: c = '8'; break;
        case  9: c = '9'; break;
        case 10: c = 'a'; break;
        case 11: c = 'b'; break;
        case 12: c = 'c'; break;
        case 13: c = 'd'; break;
        case 14: c = 'e'; break;
        case 15: c = 'f'; break;
      }

      return c;
    }

    size_t binstr(const uint8_t v, char* buffer, const size_t max_size);
    size_t hexdump(const uint8_t* buffer, const size_t length, char* out, const size_t max_size);
    size_t binstr(const std::span<uint8_t> &arr, char* buffer, const size_t max_size);

    template <typename T> 
    size_t binstr(T v, char* buffer, const size_t max_size) {
      
      size_t count = 0;
      if constexpr (std::endian::native == std::endian::big) {
        for (int64_t i = sizeof(v) - 1; i >= 0 && count < max_size; --i) {
          const size_t size = binstr(reinterpret_cast<const uint8_t*>(&v)[i], &buffer[count], max_size-count);
          count += size;
        }
      } else if constexpr (std::endian::native == std::endian::little) {
        for (size_t i = 0; i < sizeof(v) && count < max_size; ++i) {
          const size_t size = binstr(reinterpret_cast<const uint8_t*>(&v)[i], &buffer[count], max_size-count);
          count += size;
        }
      } else {
        assert(false && "Invalid endian");
      }

      return count;
    }

    constexpr uint8_t setbits(const uint8_t c, const size_t offset, const size_t numbits, const uint8_t v) {
      uint8_t stamp = (v    << (uint8_significant_bits_count-numbits));
      uint8_t mask  = (0xff << (uint8_significant_bits_count-numbits));
      stamp >>= offset; 
      mask = ~(mask >> offset);
      return (c & mask) | stamp; 
    }

    constexpr void setbitvalue (uint8_t* buffer, const size_t v, const size_t m) {
      //static_assert(!std::numeric_limits<T>::is_signed);
      /**
      * Write the value of v in network byte order starting at the location
      * pointed to by buffer, using m as the bitmask.
      * The bitmask determines the bits to be preserved from the initial value
      * of memory at buffer. E.g.
      *             v     =  00001010
      *             m     =  11110001
      * initial buffer[0] =  10010100
      * final   buffer[0] =  10011010
      *                          ^^^  - bits written
      */
      size_t shift = sizeof(size_t) * uint8_significant_bits_count; 
      const size_t ff = 0xff;
      for (size_t j = 0; j < sizeof(size_t); ++j) {      
        shift -= uint8_significant_bits_count;
        const size_t ffs = ff << shift;
        const uint8_t vb = (v  & ffs) >> shift;
        const uint8_t mb = (m  & ffs) >> shift; 
        buffer[j] = (buffer[j] & mb) | vb;
      }
    }

    constexpr void setbitbuffer (uint8_t* buffer, const size_t offset, const size_t numbits, const size_t value) {
      /**
      * Write the numbits most significant bits of value to buffer, starting at bit offset 
      * in network byte order.
      */
      const size_t size = sizeof(size_t) * uint8_significant_bits_count;
      
      buffer += (offset / uint8_significant_bits_count);  // move to the first affected byte 
      size_t v = value << (size - numbits) >> (offset % uint8_significant_bits_count);    // shift value to match the bit position
      size_t m = ~(~(static_cast<size_t>(0)) << (size - numbits) >> (offset % uint8_significant_bits_count)); // mask to determine which bits are to be preserved
      setbitvalue(buffer, v, m); // update the first location
      /* 
      * Now check if the area spans multiple type T locations, and update the remaining bits.
      */
      const size_t remaining_bits = ((offset % uint8_significant_bits_count) + numbits) % size; 
      if ((remaining_bits > 0) && ((offset % uint8_significant_bits_count) + numbits > size)) {
        v = value << (size - remaining_bits);
        m = ~(~(static_cast<size_t>(0)) << (size - remaining_bits));
        setbitvalue (buffer + sizeof(size_t), v, m);
      }
    }

    constexpr size_t getbitvalue (uint8_t* buffer, const size_t m) {
      //static_assert(!std::numeric_limits<T>::is_signed);

      size_t rval = 0;
      size_t shift = sizeof(size_t) * uint8_significant_bits_count;
      for (size_t j = 0; j < sizeof(size_t); ++j) {
        shift -= uint8_significant_bits_count;
        const uint8_t mb = (m  & (static_cast<size_t>(0xff) << shift)) >> shift;
        rval |= (static_cast<size_t>(buffer[j]) & mb) << shift;
      }
      return rval;
    }

    constexpr size_t getbitbuffer (uint8_t* buffer, const size_t offset, const size_t numbits) {
      const size_t size = sizeof(size_t) * uint8_significant_bits_count;
    
      buffer += (offset / uint8_significant_bits_count);
      size_t m  = (~(static_cast<size_t>(0)) << (size - numbits) >> (offset % uint8_significant_bits_count));
      size_t rval = getbitvalue (buffer, m); 
      if ((offset % uint8_significant_bits_count) + numbits > size) {
        const size_t remaining_bits = ((offset % uint8_significant_bits_count) + numbits) % size;
        m =  (~(static_cast<size_t>(0)) << (size - remaining_bits));
        buffer += sizeof(size_t);
        rval <<= remaining_bits;
        rval += getbitvalue(buffer, m) >> (size - remaining_bits);
      } else {
        rval >>= (size - (offset % uint8_significant_bits_count) - numbits);
      }
      return rval;
    }

  }
}

#endif
