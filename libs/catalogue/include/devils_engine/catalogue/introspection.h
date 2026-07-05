#ifndef DEVILS_ENGINE_CATALOGUE_INTROSPECTION_H
#define DEVILS_ENGINE_CATALOGUE_INTROSPECTION_H

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/string_id.h"
#include "devils_engine/utils/type_traits.h"

namespace devils_engine {
namespace catalogue {

struct argument_view {
  std::string_view name;
  std::string_view type;
  std::string_view value;
  bool printable = false;
};

struct call_info {
  utils::id domain = utils::invalid_id;
  utils::id function = utils::invalid_id;
  std::string_view function_name;
  std::string_view return_type;
  std::string_view file;
  uint32_t line = 0;
  std::span<const argument_view> arguments;
};

enum class call_decision : uint8_t {
  execute,
  skip
};

class introspection_interface {
public:
  virtual ~introspection_interface() noexcept = default;

  virtual call_decision enter(const call_info& info) = 0;
  virtual void exit(const call_info& info, uint64_t elapsed_mcs) = 0;
  virtual void skipped(const call_info& info) = 0;
};

class trace_introspection final : public introspection_interface {
public:
  call_decision enter(const call_info& info) override;
  void exit(const call_info& info, uint64_t elapsed_mcs) override;
  void skipped(const call_info& info) override;
};

class timing_introspection final : public introspection_interface {
public:
  call_decision enter(const call_info& info) override;
  void exit(const call_info& info, uint64_t elapsed_mcs) override;
  void skipped(const call_info& info) override;
};

class dry_run_introspection final : public introspection_interface {
public:
  call_decision enter(const call_info& info) override;
  void exit(const call_info& info, uint64_t elapsed_mcs) override;
  void skipped(const call_info& info) override;
};

template <size_t Capacity = 128>
class statistics_introspection final : public introspection_interface {
public:
  struct entry {
    utils::id function = utils::invalid_id;
    std::string_view name;
    uint64_t elapsed_mcs = 0;
  };

  call_decision enter(const call_info&) override { return call_decision::execute; }

  void exit(const call_info& info, const uint64_t elapsed_mcs) override {
    entries_[cursor_] = entry{info.function, info.function_name, elapsed_mcs};
    cursor_ = (cursor_ + 1) % Capacity;
    if (count_ < Capacity) ++count_;
  }

  void skipped(const call_info&) override {}

  size_t count() const noexcept { return count_; }

  double average_mcs(const utils::id fn) const noexcept {
    uint64_t sum = 0;
    size_t n = 0;
    for (size_t i = 0; i < count_; ++i) {
      if (entries_[i].function != fn) continue;
      sum += entries_[i].elapsed_mcs;
      ++n;
    }
    return n != 0 ? double(sum) / double(n) : 0.0;
  }

private:
  std::array<entry, Capacity> entries_{};
  size_t cursor_ = 0;
  size_t count_ = 0;
};

namespace detail {

struct argument_value_buffer {
  std::array<char, 64> chars{};
};

inline std::string_view shrink_placeholder(argument_value_buffer& buffer, const std::string_view type) {
  constexpr std::string_view prefix = "devils_engine::";
  constexpr std::string_view dots = "...";
  constexpr size_t capacity = std::tuple_size_v<decltype(buffer.chars)>;

  const auto write_wrapped = [&] (const std::string_view src) -> std::string_view {
    if (src.size() + 2 > capacity) return {};
    char* out = buffer.chars.data();
    *out++ = '<';
    for (const char c : src) *out++ = c;
    *out++ = '>';
    return std::string_view(buffer.chars.data(), size_t(out - buffer.chars.data()));
  };

  if (const auto full = write_wrapped(type); !full.empty()) return full;

  char* out = buffer.chars.data();
  size_t pos = 0;
  while (pos < type.size()) {
    if (type.substr(pos, prefix.size()) == prefix) {
      pos += prefix.size();
      continue;
    }

    if (out == buffer.chars.data() + capacity) break;
    *out++ = type[pos++];
  }

  const size_t without_engine_size = size_t(out - buffer.chars.data());
  if (without_engine_size + 2 <= capacity) {
    for (size_t i = without_engine_size; i > 0; --i) {
      buffer.chars[i] = buffer.chars[i - 1];
    }
    buffer.chars[0] = '<';
    buffer.chars[without_engine_size + 1] = '>';
    return std::string_view(buffer.chars.data(), without_engine_size + 2);
  }

  constexpr size_t payload_capacity = capacity - 2;
  constexpr size_t prefix_capacity = payload_capacity - dots.size();
  const size_t copy_count = std::min(without_engine_size, prefix_capacity);
  for (size_t i = copy_count; i > 0; --i) {
    buffer.chars[i] = buffer.chars[i - 1];
  }
  out = buffer.chars.data();
  *out++ = '<';
  out += copy_count;
  for (const char c : dots) *out++ = c;
  *out++ = '>';
  return std::string_view(buffer.chars.data(), size_t(out - buffer.chars.data()));
}

template <typename T>
std::string_view buffered_number(argument_value_buffer& buffer, const T value, bool& printable) {
  const auto res = std::to_chars(buffer.chars.data(), buffer.chars.data() + buffer.chars.size(), value);
  if (res.ec != std::errc{}) {
    printable = false;
    return {};
  }

  printable = true;
  return std::string_view(buffer.chars.data(), size_t(res.ptr - buffer.chars.data()));
}

template <typename T>
std::string_view stringify_arg(argument_value_buffer& buffer, const T& value, bool& printable) {
  using U = std::remove_cvref_t<T>;
  printable = true;

  if constexpr (std::is_same_v<U, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_integral_v<U> && !std::is_same_v<U, char>) {
    return buffered_number(buffer, value, printable);
  } else if constexpr (std::is_floating_point_v<U>) {
    return buffered_number(buffer, value, printable);
  } else if constexpr (std::is_enum_v<U>) {
    return buffered_number(buffer, static_cast<std::underlying_type_t<U>>(value), printable);
  } else if constexpr (std::is_same_v<U, std::string>) {
    return value;
  } else if constexpr (std::is_same_v<U, std::string_view>) {
    return value;
  } else if constexpr (std::is_convertible_v<T, std::string_view>) {
    return std::string_view(value);
  } else {
    printable = false;
    return shrink_placeholder(buffer, utils::type_name<U>());
  }
}

template <typename Ret>
Ret skipped_return() {
  if constexpr (std::is_void_v<Ret>) {
    return;
  } else {
    static_assert(std::is_default_constructible_v<Ret>, "dry-run skipped non-void functions require default-constructible return type");
    return Ret{};
  }
}

template <typename Ret, typename Invoke>
Ret invoke_with_introspection(const call_info& info, introspection_interface* intro, Invoke&& invoke) {
  using clock = std::chrono::steady_clock;

  if (intro == nullptr) {
    if constexpr (std::is_void_v<Ret>) {
      std::forward<Invoke>(invoke)();
      return;
    } else {
      return std::forward<Invoke>(invoke)();
    }
  }

  const call_decision decision = intro->enter(info);
  if (decision == call_decision::skip) {
    intro->skipped(info);
    return skipped_return<Ret>();
  }

  const auto start = clock::now();
  if constexpr (std::is_void_v<Ret>) {
    std::forward<Invoke>(invoke)();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start).count();
    intro->exit(info, static_cast<uint64_t>(elapsed));
    return;
  } else {
    Ret ret = std::forward<Invoke>(invoke)();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start).count();
    intro->exit(info, static_cast<uint64_t>(elapsed));
    return ret;
  }
}

template <typename T>
struct callable_traits : utils::detail::function_traits_v2<T> {};

template <typename T>
  requires requires { &T::operator(); }
struct callable_traits<T> : utils::detail::function_traits_v2<decltype(&T::operator())> {};

template <typename T>
using member_object_t = std::conditional_t<callable_traits<T>::is_const, const typename callable_traits<T>::member_of&, typename callable_traits<T>::member_of&>;

}

template <auto Domain>
struct domain {
  inline static introspection_interface* intro_i = nullptr;

  static void set_introspection(introspection_interface* ptr) noexcept {
    intro_i = ptr;
  }

  static introspection_interface* introspection() noexcept {
    return intro_i;
  }

  template <auto Fn, utils::template_string_t Name, utils::template_string_t... ArgNames>
  struct fn_traits {
    using fn_t = decltype(Fn);
    using traits = detail::callable_traits<fn_t>;
    using return_t = typename traits::result_type;
    using class_t = typename traits::member_of;

    static constexpr std::string_view name = Name.sv();
    static constexpr utils::id function_id = utils::murmur_hash64A(name);
    static constexpr utils::id domain_id = static_cast<utils::id>(Domain);
    static constexpr size_t argument_count = traits::argument_count;
    static constexpr std::array<std::string_view, sizeof...(ArgNames)> argument_names{ArgNames.sv()...};

    template <size_t N>
    struct argument_pack {
      std::array<argument_view, N> views{};
      std::array<detail::argument_value_buffer, N> buffers{};
    };

    template <size_t I>
    static consteval std::string_view argument_name() {
      if constexpr (I < argument_names.size()) return argument_names[I];
      else return {};
    }

    template <size_t I, typename T, size_t N>
    static void make_argument_view(argument_pack<N>& pack, const T& value) {
      bool printable = false;
      const std::string_view str = detail::stringify_arg(pack.buffers[I], value, printable);
      pack.views[I] = argument_view{
        argument_name<I>(),
        utils::type_name<std::remove_cvref_t<T>>(),
        str,
        printable
      };
    }

    template <typename... Args, size_t... I>
    static auto make_argument_views_impl(std::index_sequence<I...>, const Args&... args) {
      argument_pack<sizeof...(Args)> pack{};
      (make_argument_view<I>(pack, args), ...);
      return pack;
    }

    template <typename... Args>
    static auto make_argument_views(const Args&... args) {
      return make_argument_views_impl<Args...>(std::index_sequence_for<Args...>{}, args...);
    }

    template <typename... Args>
    static return_t invoke_free(Args&&... args) {
      if constexpr (std::is_void_v<return_t>) {
        std::invoke(Fn, std::forward<Args>(args)...);
        return;
      } else {
        return std::invoke(Fn, std::forward<Args>(args)...);
      }
    }

    template <typename Obj, typename... Args>
    static return_t invoke_member(Obj&& obj, Args&&... args) {
      if constexpr (std::is_void_v<return_t>) {
        std::invoke(Fn, std::forward<Obj>(obj), std::forward<Args>(args)...);
        return;
      } else {
        return std::invoke(Fn, std::forward<Obj>(obj), std::forward<Args>(args)...);
      }
    }

    template <typename... Args>
    static return_t invoke_functor(Args&&... args) {
      if constexpr (std::is_void_v<return_t>) {
        std::invoke(Fn, std::forward<Args>(args)...);
        return;
      } else {
        return std::invoke(Fn, std::forward<Args>(args)...);
      }
    }

    template <typename... Args>
    static return_t call_free(Args&&... args) {
      return call_free_at(std::source_location{}, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static return_t call_free_at(const std::source_location loc, Args&&... args) {
      introspection_interface* intro = intro_i;
      if (intro == nullptr) {
        return invoke_free(std::forward<Args>(args)...);
      }

      auto arg_views = make_argument_views(args...);
      const call_info info{
        domain_id,
        function_id,
        name,
        utils::type_name<return_t>(),
        loc.file_name(),
        loc.line(),
        std::span<const argument_view>(arg_views.views.data(), arg_views.views.size())
      };

      return detail::invoke_with_introspection<return_t>(info, intro, [&]() -> return_t {
        return invoke_free(std::forward<Args>(args)...);
      });
    }

    template <typename Obj, typename... Args>
    static return_t call_member(Obj&& obj, Args&&... args) {
      return call_member_at(std::source_location{}, std::forward<Obj>(obj), std::forward<Args>(args)...);
    }

    template <typename Obj, typename... Args>
    static return_t call_member_at(const std::source_location loc, Obj&& obj, Args&&... args) {
      introspection_interface* intro = intro_i;
      if (intro == nullptr) {
        return invoke_member(std::forward<Obj>(obj), std::forward<Args>(args)...);
      }

      auto arg_views = make_argument_views(obj, args...);
      const call_info info{
        domain_id,
        function_id,
        name,
        utils::type_name<return_t>(),
        loc.file_name(),
        loc.line(),
        std::span<const argument_view>(arg_views.views.data(), arg_views.views.size())
      };

      return detail::invoke_with_introspection<return_t>(info, intro, [&]() -> return_t {
        return invoke_member(std::forward<Obj>(obj), std::forward<Args>(args)...);
      });
    }

    template <typename... Args>
    static return_t call_functor(Args&&... args) {
      return call_functor_at(std::source_location{}, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static return_t call_functor_at(const std::source_location loc, Args&&... args) {
      introspection_interface* intro = intro_i;
      if (intro == nullptr) {
        return invoke_functor(std::forward<Args>(args)...);
      }

      auto arg_views = make_argument_views(args...);
      const call_info info{
        domain_id,
        function_id,
        name,
        utils::type_name<return_t>(),
        loc.file_name(),
        loc.line(),
        std::span<const argument_view>(arg_views.views.data(), arg_views.views.size())
      };

      return detail::invoke_with_introspection<return_t>(info, intro, [&]() -> return_t {
        return invoke_functor(std::forward<Args>(args)...);
      });
    }

    template <typename T, typename = void>
    struct maker;

    template <typename Ret, typename... Args>
    struct maker<Ret (*)(Args...)> {
      using pointer_t = Ret (*)(Args...);
      struct loc_fn_t {
        std::source_location loc;

        explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

        template <typename... CallArgs>
        Ret operator()(CallArgs&&... args) const {
          return call_free_at(loc, std::forward<CallArgs>(args)...);
        }
      };

      static Ret call(Args... args) {
        return call_free(std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename... Args>
    struct maker<Ret (*)(Args...) noexcept> {
      using pointer_t = Ret (*)(Args...);
      struct loc_fn_t {
        std::source_location loc;

        explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

        template <typename... CallArgs>
        Ret operator()(CallArgs&&... args) const {
          return call_free_at(loc, std::forward<CallArgs>(args)...);
        }
      };

      static Ret call(Args... args) {
        return call_free(std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename C, typename... Args>
    struct maker<Ret (C::*)(Args...)> {
      using pointer_t = Ret (*)(C&, Args...);
      struct loc_fn_t {
        std::source_location loc;

        explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

        template <typename Obj, typename... CallArgs>
        Ret operator()(Obj&& obj, CallArgs&&... args) const {
          return call_member_at(loc, std::forward<Obj>(obj), std::forward<CallArgs>(args)...);
        }
      };

      static Ret call(C& obj, Args... args) {
        return call_member(obj, std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename C, typename... Args>
    struct maker<Ret (C::*)(Args...) noexcept> {
      using pointer_t = Ret (*)(C&, Args...);
      struct loc_fn_t {
        std::source_location loc;

        explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

        template <typename Obj, typename... CallArgs>
        Ret operator()(Obj&& obj, CallArgs&&... args) const {
          return call_member_at(loc, std::forward<Obj>(obj), std::forward<CallArgs>(args)...);
        }
      };

      static Ret call(C& obj, Args... args) {
        return call_member(obj, std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename C, typename... Args>
    struct maker<Ret (C::*)(Args...) const> {
      using pointer_t = Ret (*)(const C&, Args...);
      struct loc_fn_t {
        std::source_location loc;

        explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

        template <typename Obj, typename... CallArgs>
        Ret operator()(Obj&& obj, CallArgs&&... args) const {
          return call_member_at(loc, std::forward<Obj>(obj), std::forward<CallArgs>(args)...);
        }
      };

      static Ret call(const C& obj, Args... args) {
        return call_member(obj, std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename C, typename... Args>
    struct maker<Ret (C::*)(Args...) const noexcept> {
      using pointer_t = Ret (*)(const C&, Args...);
      struct loc_fn_t {
        std::source_location loc;

        explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

        template <typename Obj, typename... CallArgs>
        Ret operator()(Obj&& obj, CallArgs&&... args) const {
          return call_member_at(loc, std::forward<Obj>(obj), std::forward<CallArgs>(args)...);
        }
      };

      static Ret call(const C& obj, Args... args) {
        return call_member(obj, std::forward<Args>(args)...);
      }
    };

    template <typename T>
      requires (!std::is_pointer_v<T> && !std::is_member_function_pointer_v<T> && requires { &T::operator(); })
    struct maker<T> {
      template <typename Sig>
      struct impl;

      template <typename Ret, typename C, typename... Args>
      struct impl<Ret (C::*)(Args...) const> {
        using pointer_t = Ret (*)(Args...);
        struct loc_fn_t {
          std::source_location loc;

          explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

          template <typename... CallArgs>
          Ret operator()(CallArgs&&... args) const {
            return call_functor_at(loc, std::forward<CallArgs>(args)...);
          }
        };
      };

      template <typename Ret, typename C, typename... Args>
      struct impl<Ret (C::*)(Args...) const noexcept> {
        using pointer_t = Ret (*)(Args...);
        struct loc_fn_t {
          std::source_location loc;

          explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

          template <typename... CallArgs>
          Ret operator()(CallArgs&&... args) const {
            return call_functor_at(loc, std::forward<CallArgs>(args)...);
          }
        };
      };

      template <typename Ret, typename C, typename... Args>
      struct impl<Ret (C::*)(Args...)> {
        using pointer_t = Ret (*)(Args...);
        struct loc_fn_t {
          std::source_location loc;

          explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

          template <typename... CallArgs>
          Ret operator()(CallArgs&&... args) const {
            return call_functor_at(loc, std::forward<CallArgs>(args)...);
          }
        };
      };

      template <typename Ret, typename C, typename... Args>
      struct impl<Ret (C::*)(Args...) noexcept> {
        using pointer_t = Ret (*)(Args...);
        struct loc_fn_t {
          std::source_location loc;

          explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

          template <typename... CallArgs>
          Ret operator()(CallArgs&&... args) const {
            return call_functor_at(loc, std::forward<CallArgs>(args)...);
          }
        };
      };

      using pointer_t = typename impl<decltype(&T::operator())>::pointer_t;
      using loc_fn_t = typename impl<decltype(&T::operator())>::loc_fn_t;

      template <typename... Args>
      static return_t call(Args... args) {
        return call_functor(std::forward<Args>(args)...);
      }
    };

    using pointer_t = typename maker<fn_t>::pointer_t;
    using loc_fn_t = typename maker<fn_t>::loc_fn_t;
    static constexpr pointer_t fn_ptr = &maker<fn_t>::call;
  };
};

}
}

#endif
