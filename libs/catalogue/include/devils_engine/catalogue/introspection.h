#ifndef DEVILS_ENGINE_CATALOGUE_INTROSPECTION_H
#define DEVILS_ENGINE_CATALOGUE_INTROSPECTION_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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
  std::string value;
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

template <typename T>
std::string stringify_arg(const T& value, bool& printable) {
  using U = std::remove_cvref_t<T>;
  printable = true;

  if constexpr (std::is_same_v<U, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_integral_v<U> && !std::is_same_v<U, char>) {
    return std::to_string(value);
  } else if constexpr (std::is_floating_point_v<U>) {
    return std::to_string(value);
  } else if constexpr (std::is_enum_v<U>) {
    return std::to_string(static_cast<std::underlying_type_t<U>>(value));
  } else if constexpr (std::is_same_v<U, std::string>) {
    return value;
  } else if constexpr (std::is_same_v<U, std::string_view>) {
    return std::string(value);
  } else if constexpr (std::is_convertible_v<T, std::string_view>) {
    return std::string(std::string_view(value));
  } else {
    printable = false;
    return {};
  }
}

template <size_t I, utils::template_string_t... Names>
consteval std::string_view argument_name() {
  constexpr std::array<std::string_view, sizeof...(Names)> names{Names.sv()...};
  if constexpr (I < names.size()) return names[I];
  else return {};
}

inline std::string fallback_argument_name(const size_t index) {
  return "arg" + std::to_string(index);
}

template <size_t I, utils::template_string_t... Names, typename T>
argument_view make_argument_view(const T& value) {
  bool printable = false;
  std::string str = stringify_arg(value, printable);
  constexpr std::string_view fixed_name = argument_name<I, Names...>();
  return argument_view{
    fixed_name,
    utils::type_name<std::remove_cvref_t<T>>(),
    std::move(str),
    printable
  };
}

template <utils::template_string_t... Names, typename Tuple, size_t... I>
auto make_argument_views_impl(Tuple&& tuple, std::index_sequence<I...>) {
  return std::array<argument_view, sizeof...(I)>{
    make_argument_view<I, Names...>(std::get<I>(std::forward<Tuple>(tuple)))...
  };
}

template <utils::template_string_t... Names, typename... Args>
auto make_argument_views(const Args&... args) {
  auto tuple = std::forward_as_tuple(args...);
  auto arr = make_argument_views_impl<Names...>(tuple, std::index_sequence_for<Args...>{});
  for (size_t i = 0; i < arr.size(); ++i) {
    if (!arr[i].name.empty()) continue;
    // Store fallback names in a small static ring to keep string_view valid for
    // the duration of a synchronous introspection call.
    static thread_local std::array<std::string, 64> fallback_names;
    fallback_names[i % fallback_names.size()] = fallback_argument_name(i);
    arr[i].name = fallback_names[i % fallback_names.size()];
  }
  return arr;
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

    template <typename... Args>
    static return_t call_free(Args... args) {
      introspection_interface* intro = intro_i;
      if (intro == nullptr) {
        if constexpr (std::is_void_v<return_t>) {
          Fn(std::forward<Args>(args)...);
          return;
        } else {
          return Fn(std::forward<Args>(args)...);
        }
      }

      auto arg_views = detail::make_argument_views<ArgNames...>(args...);
      const call_info info{
        domain_id,
        function_id,
        name,
        utils::type_name<return_t>(),
        {},
        0,
        std::span<const argument_view>(arg_views.data(), arg_views.size())
      };

      return detail::invoke_with_introspection<return_t>(info, intro, [&]() -> return_t {
        if constexpr (std::is_void_v<return_t>) {
          Fn(std::forward<Args>(args)...);
          return;
        } else {
          return Fn(std::forward<Args>(args)...);
        }
      });
    }

    template <typename Obj, typename... Args>
    static return_t call_member(Obj&& obj, Args... args) {
      introspection_interface* intro = intro_i;
      if (intro == nullptr) {
        if constexpr (std::is_void_v<return_t>) {
          (std::forward<Obj>(obj).*Fn)(std::forward<Args>(args)...);
          return;
        } else {
          return (std::forward<Obj>(obj).*Fn)(std::forward<Args>(args)...);
        }
      }

      auto arg_views = detail::make_argument_views<ArgNames...>(obj, args...);
      const call_info info{
        domain_id,
        function_id,
        name,
        utils::type_name<return_t>(),
        {},
        0,
        std::span<const argument_view>(arg_views.data(), arg_views.size())
      };

      return detail::invoke_with_introspection<return_t>(info, intro, [&]() -> return_t {
        if constexpr (std::is_void_v<return_t>) {
          (std::forward<Obj>(obj).*Fn)(std::forward<Args>(args)...);
          return;
        } else {
          return (std::forward<Obj>(obj).*Fn)(std::forward<Args>(args)...);
        }
      });
    }

    template <typename... Args>
    static return_t call_functor(Args... args) {
      introspection_interface* intro = intro_i;
      if (intro == nullptr) {
        if constexpr (std::is_void_v<return_t>) {
          Fn(std::forward<Args>(args)...);
          return;
        } else {
          return Fn(std::forward<Args>(args)...);
        }
      }

      auto arg_views = detail::make_argument_views<ArgNames...>(args...);
      const call_info info{
        domain_id,
        function_id,
        name,
        utils::type_name<return_t>(),
        {},
        0,
        std::span<const argument_view>(arg_views.data(), arg_views.size())
      };

      return detail::invoke_with_introspection<return_t>(info, intro, [&]() -> return_t {
        if constexpr (std::is_void_v<return_t>) {
          Fn(std::forward<Args>(args)...);
          return;
        } else {
          return Fn(std::forward<Args>(args)...);
        }
      });
    }

    template <typename T, typename = void>
    struct maker;

    template <typename Ret, typename... Args>
    struct maker<Ret (*)(Args...)> {
      using pointer_t = Ret (*)(Args...);

      static Ret call(Args... args) {
        return call_free(std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename... Args>
    struct maker<Ret (*)(Args...) noexcept> {
      using pointer_t = Ret (*)(Args...);

      static Ret call(Args... args) {
        return call_free(std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename C, typename... Args>
    struct maker<Ret (C::*)(Args...)> {
      using pointer_t = Ret (*)(C&, Args...);

      static Ret call(C& obj, Args... args) {
        return call_member(obj, std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename C, typename... Args>
    struct maker<Ret (C::*)(Args...) noexcept> {
      using pointer_t = Ret (*)(C&, Args...);

      static Ret call(C& obj, Args... args) {
        return call_member(obj, std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename C, typename... Args>
    struct maker<Ret (C::*)(Args...) const> {
      using pointer_t = Ret (*)(const C&, Args...);

      static Ret call(const C& obj, Args... args) {
        return call_member(obj, std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename C, typename... Args>
    struct maker<Ret (C::*)(Args...) const noexcept> {
      using pointer_t = Ret (*)(const C&, Args...);

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
      };

      template <typename Ret, typename C, typename... Args>
      struct impl<Ret (C::*)(Args...) const noexcept> {
        using pointer_t = Ret (*)(Args...);
      };

      template <typename Ret, typename C, typename... Args>
      struct impl<Ret (C::*)(Args...)> {
        using pointer_t = Ret (*)(Args...);
      };

      template <typename Ret, typename C, typename... Args>
      struct impl<Ret (C::*)(Args...) noexcept> {
        using pointer_t = Ret (*)(Args...);
      };

      using pointer_t = typename impl<decltype(&T::operator())>::pointer_t;

      template <typename... Args>
      static return_t call(Args... args) {
        return call_functor(std::forward<Args>(args)...);
      }
    };

    using pointer_t = typename maker<fn_t>::pointer_t;
    static constexpr pointer_t fn_ptr = &maker<fn_t>::call;
  };
};

}
}

#endif
