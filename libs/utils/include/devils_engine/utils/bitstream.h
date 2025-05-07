/** 
 *  Modified version of https://github.com/wroniasty/bits
 */
#ifndef DEVILS_ENGINE_UTILS_BITSTREAM_H
#define DEVILS_ENGINE_UTILS_BITSTREAM_H

#include <cstdint>
#include <vector>
#include <span>

#include "bits.h"

namespace devils_engine {
  namespace utils {
    class bitstream {
    public:
      bitstream(uint8_t* data); // размер?

      uint8_t* ptr();
      uint8_t* current();
      const uint8_t* ptr() const;
      const uint8_t* current() const;
      bool aligned() const;

      void seek(const size_t position);
      void rewind();
      void skip(const size_t bits);
      size_t position() const;

      size_t peek_at(const size_t offset, const size_t numbits) const {
        return getbitbuffer(buffer, offset, numbits);
      }

      size_t peek(const size_t numbits) const {
        return peek_at(offset, numbits);
      }

      size_t read(const size_t numbits) {
        const size_t v = peek(numbits);
        offset += numbits;
        return v;
      }

      void peek_at(uint8_t* dst, const size_t offset, const size_t numbits) const;
      void peek(uint8_t* dst, const size_t numbits) const;
      void read(uint8_t* dst, const size_t numbits);
      //std::vector<uint8_t> read(const size_t numbits);
      //std::vector<uint8_t> peek(const size_t numbits) const;
  
      void write_at(const size_t offset, const size_t numbits, const uint8_t* v); 
      void write_at(const size_t offset, const std::span<uint8_t> &s);
      void write_at(const size_t offset, const std::span<uint8_t> &s, const size_t max_bytes);
      void write(const size_t numbits, const uint8_t* v); 

      void write_at(const size_t offset, const size_t numbits, const size_t v) {
        setbitbuffer(buffer, offset, numbits, v);
      }

      void write(const size_t numbits, const size_t v) {
        write_at(offset, numbits, v);
        offset += numbits;
      }

      void write(const std::span<uint8_t> &s);
      void write(const std::span<uint8_t> &s, const size_t max_bytes);

      void zero(const size_t numbits);
      void memset(const size_t numbits, uint8_t value);
    private:
      uint8_t* buffer;
      size_t offset;
    };

  }
}

#endif
