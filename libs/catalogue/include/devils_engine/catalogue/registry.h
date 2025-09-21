#ifndef DEVILS_ENGINE_CATALOGUE_REGISTRY_H
#define DEVILS_ENGINE_CATALOGUE_REGISTRY_H

#include <cstdint>
#include <cstddef>
#include <span>
#include <gtl/phmap.hpp>
#include "common.h"

namespace devils_engine {
namespace catalogue {
// заголовок поди потребуется
inline void default_invoke(const function_buffer_header& header, const std::span<uint8_t> &) {}

struct registry {
  struct info {
    using invoke_fn = decltype(&default_invoke);

    size_t id;
    std::string_view name;
    const invoke_fn fn;

    friend inline bool operator==(const info& a, const info& b) noexcept { return a.name == b.name; }
    friend inline bool operator!=(const info& a, const info& b) noexcept { return !(a.name == b.name); }
  };

  gtl::flat_hash_map<size_t, info> funcs;

  void reg(const size_t id, const std::string_view &name, const info::invoke_fn fn);
};
}
}

#endif