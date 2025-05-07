#ifndef DEVILS_ENGINE_UTILS_TYPE_TRAITS_H
#define DEVILS_ENGINE_UTILS_TYPE_TRAITS_H

#include <type_traits>
#include <functional>
#include <string_view>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <set>
#include <span>
#include <list>

namespace devils_engine {
  namespace utils {
    enum class function_class {
      type,
      pointer,
      pointer_noexcept,
      std_function,
      member,
      member_noexcept,
      member_const,
      member_const_noexcept,
      count
    };

    namespace detail {
      // primary template.
      template<typename T>
      struct function_traits : public function_traits<decltype(&T::operator())> {};

      // partial specialization for function type
      template<typename R, typename... Args>
      struct function_traits<R(Args...)> {
        using member_of = void;
        using result_type = R;
        using argument_types = std::tuple<Args..., void>;
        using arguments_tuple_t = std::tuple<Args...>;
        static constexpr bool is_function = true;
        static constexpr function_class class_e = function_class::type;
        static constexpr size_t argument_count = std::tuple_size_v<argument_types> - 1;
      };

      // partial specialization for function pointer
      template<typename R, typename... Args>
      struct function_traits<R (*)(Args...)> {
        using member_of = void;
        using result_type = R;
        using argument_types = std::tuple<Args..., void>;
        using arguments_tuple_t = std::tuple<Args...>;
        static constexpr bool is_function = true;
        static constexpr function_class class_e = function_class::pointer;
        static constexpr size_t argument_count = std::tuple_size_v<argument_types> - 1;
      };

      template<typename R, typename... Args>
      struct function_traits<R (*)(Args...) noexcept> {
        using member_of = void;
        using result_type = R;
        using argument_types = std::tuple<Args..., void>;
        using arguments_tuple_t = std::tuple<Args...>;
        static constexpr bool is_function = true;
        static constexpr function_class class_e = function_class::pointer_noexcept;
        static constexpr size_t argument_count = std::tuple_size_v<argument_types> - 1;
      };

      // partial specialization for std::function
      template<typename R, typename... Args>
      struct function_traits<std::function<R(Args...)>> {
        using member_of = void;
        using result_type = R;
        using argument_types = std::tuple<Args..., void>;
        using arguments_tuple_t = std::tuple<Args...>;
        static constexpr bool is_function = true;
        static constexpr function_class class_e = function_class::std_function;
        static constexpr size_t argument_count = std::tuple_size_v<argument_types> - 1;
      };

      // partial specialization for pointer-to-member-function (i.e., operator()'s)
      template<typename T, typename R, typename... Args>
      struct function_traits<R (T::*)(Args...)> {
        using member_of = T;
        using result_type = R;
        using argument_types = std::tuple<Args..., void>;
        using arguments_tuple_t = std::tuple<Args...>;
        static constexpr bool is_function = true;
        static constexpr function_class class_e = function_class::member;
        static constexpr size_t argument_count = std::tuple_size_v<argument_types> - 1;
      };

      // operator() noexcept
      template<typename T, typename R, typename... Args>
      struct function_traits<R (T::*)(Args...) noexcept> {
        using member_of = T;
        using result_type = R;
        using argument_types = std::tuple<Args..., void>;
        using arguments_tuple_t = std::tuple<Args...>;
        static constexpr bool is_function = true;
        static constexpr function_class class_e = function_class::member_noexcept;
        static constexpr size_t argument_count = std::tuple_size_v<argument_types> - 1;
      };

      // operator() const
      template<typename T, typename R, typename... Args>
      struct function_traits<R (T::*)(Args...) const> {
        using member_of = T;
        using result_type = R;
        using argument_types = std::tuple<Args..., void>;
        using arguments_tuple_t = std::tuple<Args...>;
        static constexpr bool is_function = true;
        static constexpr function_class class_e = function_class::member_const;
        static constexpr size_t argument_count = std::tuple_size_v<argument_types> - 1;
      };

      // operator() const noexcept
      template<typename T, typename R, typename... Args>
      struct function_traits<R (T::*)(Args...) const noexcept> {
        using member_of = T;
        using result_type = R;
        using argument_types = std::tuple<Args..., void>;
        using arguments_tuple_t = std::tuple<Args...>;
        static constexpr bool is_function = true;
        static constexpr function_class class_e = function_class::member_const_noexcept;
        static constexpr size_t argument_count = std::tuple_size_v<argument_types> - 1;
      };

      /// Simple type introspection without RTTI.
      template <typename T>
      constexpr std::string_view get_type_name() {
#if defined(_MSC_VER)
        constexpr std::string_view start_char_seq = "get_type_name<";
        constexpr std::string_view end_char_seq = ">(void)";
        constexpr std::string_view function_type_pattern = ")(";
        constexpr std::string_view sig = __FUNCSIG__;
        constexpr size_t sig_size = sig.size()+1;
        constexpr size_t str_seq_name_start = sig.find(start_char_seq) + start_char_seq.size();
        constexpr size_t end_of_char_str = sig.rfind(start_char_seq);
        constexpr size_t count = sig_size - str_seq_name_start - end_char_seq.size() - 1; // отстается символ '>' в конце
        constexpr std::string_view substr = sig.substr(str_seq_name_start, count);
        if constexpr (substr.find(function_type_pattern) == std::string_view::npos) {
          constexpr std::string_view class_char_seq = "class ";
          constexpr std::string_view struct_char_seq = "struct ";
          const size_t class_seq_start = substr.find(class_char_seq);
          const size_t struct_seq_start = substr.find(struct_char_seq);
          if constexpr (class_seq_start == 0) return substr.substr(class_char_seq.size());
          if constexpr (struct_seq_start == 0) return substr.substr(struct_char_seq.size());;
        }
        return substr;
#elif defined(__clang__)
        constexpr std::string_view sig = __PRETTY_FUNCTION__;
        constexpr std::string_view start_char_seq = "T = ";
        constexpr size_t sig_size = sig.size()+1;
        constexpr size_t str_seq_name_start = sig.find(start_char_seq) + start_char_seq.size();
        constexpr size_t end_of_char_str = 2;
        constexpr size_t count = sig_size - str_seq_name_start - end_of_char_str;
        return sig.substr(str_seq_name_start, count);
#elif defined(__GNUC__)
        constexpr std::string_view sig = __PRETTY_FUNCTION__;
        constexpr std::string_view start_char_seq = "T = ";
        constexpr size_t sig_size = sig.size()+1;
        constexpr size_t str_seq_name_start = sig.find(start_char_seq) + start_char_seq.size();
        constexpr size_t end_of_char_str = sig_size - sig.find(';');
        constexpr size_t count = sig_size - str_seq_name_start - end_of_char_str;
        return sig.substr(str_seq_name_start, count);
#else
#error Compiler not supported for demangling
#endif
      }

      constexpr uint8_t to_u8(const char c) { return uint8_t(c); }

      constexpr uint64_t U8TO64_LE(const char* data) {
        return  uint64_t(to_u8(data[0]))        | (uint64_t(to_u8(data[1])) << 8)  | (uint64_t(to_u8(data[2])) << 16) |
              (uint64_t(to_u8(data[3])) << 24) | (uint64_t(to_u8(data[4])) << 32) | (uint64_t(to_u8(data[5])) << 40) |
              (uint64_t(to_u8(data[6])) << 48) | (uint64_t(to_u8(data[7])) << 56);
      }

      constexpr uint64_t U8TO64_LE(const uint8_t* data) {
        return  uint64_t(data[0])        | (uint64_t(data[1]) << 8)  | (uint64_t(data[2]) << 16) |
              (uint64_t(data[3]) << 24) | (uint64_t(data[4]) << 32) | (uint64_t(data[5]) << 40) |
              (uint64_t(data[6]) << 48) | (uint64_t(data[7]) << 56);
      }

      constexpr uint64_t to_u64(const char c) { return uint64_t(to_u8(c)); }

      constexpr uint64_t murmur_hash64A(const std::string_view& in_str, const uint64_t seed) {
        constexpr uint64_t m = 0xc6a4a7935bd1e995LLU;
        constexpr int r = 47;
        const size_t len = in_str.size();
        const size_t end = len - (len % sizeof(uint64_t));

        uint64_t h = seed ^ (len * m);

        for (size_t i = 0; i < end; i += 8) {
          uint64_t k = U8TO64_LE(&in_str[i]);
          k *= m;
          k ^= k >> r;
          k *= m;

          h ^= k;
          h *= m;
        }

        const auto key_end = in_str.substr(end);
        const size_t left = len & 7;
        switch (left) {
          case 7: h ^= to_u64(key_end[6]) << 48; [[fallthrough]];
          case 6: h ^= to_u64(key_end[5]) << 40; [[fallthrough]];
          case 5: h ^= to_u64(key_end[4]) << 32; [[fallthrough]];
          case 4: h ^= to_u64(key_end[3]) << 24; [[fallthrough]];
          case 3: h ^= to_u64(key_end[2]) << 16; [[fallthrough]];
          case 2: h ^= to_u64(key_end[1]) << 8;  [[fallthrough]];
          case 1: h ^= to_u64(key_end[0]);
            h *= m;
        };

        h ^= h >> r;
        h *= m;
        h ^= h >> r;

        return h;
      }
    }

    template<typename T, size_t N>
    using function_argument_type = typename std::tuple_element<N, typename detail::function_traits<T>::argument_types>::type;

    template<typename T>
    using function_result_type = typename detail::function_traits<T>::result_type;

    template<typename T>
    using function_member_of = typename detail::function_traits<T>::member_of;

    template <typename Container>
    struct is_container : std::false_type {};
    template <typename... Ts>
    struct is_container<std::array<Ts...>> : std::true_type {};
    template <typename... Ts>
    struct is_container<std::span<Ts...>> : std::true_type {};
    template <typename T, size_t N>
    struct is_container<std::array<T, N>> : std::true_type {};
    template <typename T, size_t N>
    struct is_container<std::span<T, N>> : std::true_type {};
    template <typename... Ts>
    struct is_container<std::vector<Ts...>> : std::true_type {};
    template <typename... Ts>
    struct is_container<std::list<Ts...>> : std::true_type {};
    template <typename... Ts>
    struct is_container<std::unordered_set<Ts...>> : std::true_type {};
    template <typename... Ts>
    struct is_container<std::set<Ts...>> : std::true_type {};

    template <typename... Ts>
    constexpr bool is_container_v = is_container<Ts...>::value;

    template <typename Container>
    struct is_map : std::false_type {};
    template <typename... Ts>
    struct is_map<std::unordered_map<Ts...>> : std::true_type {};
    template <typename... Ts>
    struct is_map<std::map<Ts...>> : std::true_type {};

    template <typename... Ts>
    constexpr bool is_map_v = is_map<Ts...>::value;

    template <typename T>
    constexpr std::string_view type_name() { return detail::get_type_name<T>(); }

    // тут это не должно быть
    constexpr uint64_t default_murmur_seed = 14695981039346656037ull;
    constexpr uint64_t murmur_hash64A(const std::string_view &in_str, const uint64_t seed) {
      return detail::murmur_hash64A(in_str, seed);
    }

    template <typename T>
    constexpr size_t type_id() {
      using type = std::remove_reference_t<std::remove_cv_t<T>>; // std::remove_pointer_t
      const auto name = detail::get_type_name<type>();
      return murmur_hash64A(name, default_murmur_seed);
    }
  }
}

#endif
