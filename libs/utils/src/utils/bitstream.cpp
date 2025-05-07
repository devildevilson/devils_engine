/* Modified version of https://github.com/wroniasty/bits */
#include <cstdint>
#include <cstring>
#include <vector>
#include "bitstream.h"

namespace devils_engine {
  namespace utils {
    bitstream::bitstream(uint8_t* data) : buffer(data), offset(0) {}

    uint8_t* bitstream::ptr() { return buffer; }
    uint8_t* bitstream::current() { return buffer + position() / 8; }
    const uint8_t* bitstream::ptr() const { return buffer; }
    const uint8_t* bitstream::current() const { return buffer + position() / 8; }
    bool bitstream::aligned() const { return position() % 8 == 0; }

    void bitstream::seek(const size_t position) { offset = position; }
    void bitstream::rewind() { offset = 0; }
    void bitstream::skip(const size_t bits) { offset += bits; }
    size_t bitstream::position() const { return offset; } 

    void bitstream::peek_at(uint8_t* dst, const size_t offset, const size_t numbits) const {
      int64_t remain = numbits;
      if (offset % uint8_significant_bits_count != 0 || numbits % uint8_significant_bits_count != 0) {
        size_t current_offset = offset;
        while (remain > 0) {
          *(dst++) = peek_at(current_offset, std::min(remain, int64_t(uint8_significant_bits_count)));
          remain -= uint8_significant_bits_count;
          current_offset += uint8_significant_bits_count;
        }
      } else {
        const size_t current_offset = offset;
        memcpy (dst, (buffer + current_offset / uint8_significant_bits_count), numbits / uint8_significant_bits_count);
      }
    }

    void bitstream::peek(uint8_t *dst, const size_t numbits) const {
      // int64_t remain = numbits;
      // size_t current_offset = position();
      // if (position() % uint8_significant_bits_count != 0 || numbits % uint8_significant_bits_count != 0) {
      //   while (remain > 0) {
      //     *(dst++) = read<uint8_t>(std::min(remain, int64_t(uint8_significant_bits_count)));
      //     remain -= uint8_significant_bits_count;
      //   }
      //   seek(current_offset);
      // } else {
      //   memcpy (dst, (buffer + current_offset / uint8_significant_bits_count), numbits / uint8_significant_bits_count);
      // }
      peek_at(dst, offset, numbits);
    }

    // std::vector<uint8_t> bitstream::peek(const size_t numbits) const {
    //   const size_t dst_size = numbits / uint8_significant_bits_count + (numbits % uint8_significant_bits_count != 0 ? 1 : 0);
    //   std::vector<uint8_t> res(dst_size, 0);
    //   peek(res.data(), numbits);
    //   return res;
    // }

    void bitstream::read(uint8_t* dst, const size_t numbits) {
      peek(dst, numbits);
      skip(numbits);
    }

    // std::vector<uint8_t> bitstream::read(const size_t numbits) {
    //   auto s = peek(numbits);
    //   skip(numbits);
    //   return s;
    // }

    void bitstream::write_at(const size_t offset, const size_t numbits, const uint8_t* src) {
      int64_t remain = numbits;
      if (position() % 8 != 0 || numbits % 8 != 0) {
        // if the write is not aligned, write byte by byte (slowish)
        size_t current_offset = offset;
        while (remain > 0) {
          write_at(current_offset, std::min(remain, int64_t(uint8_significant_bits_count)), *(src++));
          remain -= uint8_significant_bits_count;
          current_offset += uint8_significant_bits_count;
        }
      } else {
        // otherwise just copy the buffer
        memcpy(buffer+(offset / uint8_significant_bits_count), src, numbits / uint8_significant_bits_count);
      }
    }

    void bitstream::write(const size_t numbits, const uint8_t* src) {
      write_at(offset, numbits, src);
      skip(numbits);
    }

    void bitstream::write(const std::span<uint8_t> &s, const size_t max_bytes) {
      const size_t numbits = std::min(max_bytes * uint8_significant_bits_count, s.size() * uint8_significant_bits_count);
      write(numbits, s.data());
      if (max_bytes > s.size()) zero((max_bytes - s.size()) * uint8_significant_bits_count);
    }

    void bitstream::write(const std::span<uint8_t> &s) {
      write(s, s.size());
    }

    void bitstream::zero(const size_t numbits) {
      memset(numbits, 0);
    }

    void bitstream::memset(const size_t numbits, uint8_t value) {
      if ( position() % uint8_significant_bits_count != 0 || numbits % uint8_significant_bits_count != 0 ) {
        // if the write is not aligned, write byte by byte (slowish)
        int64_t remain = numbits;
        while (remain > 0) {
          write(std::min(remain, int64_t(uint8_significant_bits_count)), value);
          remain -= 8;
        }
      } else {
        // otherwise just copy the buffer
        ::memset(current(), value, numbits/8 );
        skip(numbits);
      }
    }

    void bitstream::write_at(const size_t offset, const std::span<uint8_t> &s, const size_t max_bytes) {
      const size_t numbits = std::min(max_bytes * uint8_significant_bits_count, s.size() * uint8_significant_bits_count);
      write_at(offset, numbits, s.data());
      if (max_bytes > s.size()) zero((max_bytes - s.size()) * uint8_significant_bits_count);
    }

    void bitstream::write_at(const size_t offset, const std::span<uint8_t> &s) {
      write_at(offset, s, s.size());
    }

  }
}
