/*
    MIT License

    Copyright (c) 2020 LekKit https://github.com/LekKit

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#ifndef DEVILS_ENGINE_UTILS_SHA256_CPP_H
#define DEVILS_ENGINE_UTILS_SHA256_CPP_H

#include <string>
#include <string_view>
#include <array>
#include "sha256.h"

namespace devils_engine {
namespace utils {
  using digest = std::array<uint8_t, 32>;

  class SHA256 {
  public:
    static std::string easy(const std::string_view& str);
    static std::string easy(const void* data, const size_t size);

    SHA256() noexcept;
    void update(const void* data, const size_t size);
    digest finalize();
    std::string hash();
  private:
    struct sha256_buff buff;
  };
}
}

#endif
