#ifndef DEVILS_ENGINE_BINDINGS_GENERIC_H
#define DEVILS_ENGINE_BINDINGS_GENERIC_H

#include <cstddef>
#include <cstdint>
#include <reflect>
#include <string>
#include "utils/core.h"
#include "utils/type_traits.h"
#include "lua_header.h"

namespace devils_engine {
namespace bindings {
// тут нужно передать какой то объект
// reflect НЕ БЕРЕТ МЕТОДЫ КЛАССА !!! только поля
// блин, а как теперь?
// придется делать по старому =(
template <typename T>
auto type(sol::table t, const std::string_view &prefix, const T &default_obj = T{}) -> sol::usertype<T> {
  auto lua_type = t.new_usertype<T>(utils::type_name<T>(), sol::no_constructor);
  reflect::for_each([&](auto I) {
    using value_type = decltype(reflect::get<I>(default_obj));
    using mem_type = std::remove_cvref_t<value_type>;
    const auto type_name = utils::type_name<mem_type>();
    const std::string_view name = reflect::member_name<I>(default_obj);
    
    if constexpr (std::is_function_v<mem_type>) {
      if (name.find(prefix) == 0) {
        // если это функция и если префикс стоит в начале названия
        const auto final_member_name = name.substr(prefix.size());
        utils::println(final_member_name, name, type_name);
        lua_type[final_member_name] = &reflect::get<I>(default_obj);
      }
    }

    // тут мы ищем функцию у которой есть префикс в имени и ее добавляем в биндинг
    // было бы еще неплохо понять что является именно функцией, а что является get/set парой
  }, default_obj);

  return lua_type;
}

template <typename T>
sol::table make_table_raw(sol::table &t, const T &obj, const std::string_view &prefix = "");

template <typename T>
sol::table make_table_array_raw(sol::table &t, const T &obj, const std::string_view &prefix = "");

template <typename T>
sol::table make_table_map_raw(sol::table &t, const T &obj, const std::string_view &prefix = "") {
  static_assert(utils::is_map_v<T>);

  for (const auto &[ key, val ] : obj) {
    using mem_type = std::remove_cvref_t<decltype(val)>;
    if constexpr (utils::is_container_v<mem_type>) {
      auto t1 = t.create();
      t[key] = make_table_array_raw(t1, val, prefix);
    } else if constexpr (utils::is_map_v<mem_type>) {
      auto t1 = t.create();
      t[key] = make_table_map_raw(t1, val, prefix);
    } else if constexpr (std::is_aggregate_v<mem_type>) {
      auto t1 = t.create();
      t[key] = make_table_raw(t1, val, prefix);
    } else {
      t[key] = val;
    }
  }

  return t;
}

template <typename T>
sol::table make_table_array_raw(sol::table &t, const T &obj, const std::string_view &prefix) {
  size_t counter = 0;
  for (const auto &val : obj) {
    using mem_type = std::remove_cvref_t<decltype(val)>;
    counter += 1;
    if constexpr (utils::is_container_v<mem_type>) {
      auto t1 = t.create();
      t[counter] = make_table_array_raw(t1, val, prefix);
    } else if constexpr (utils::is_map_v<mem_type>) {
      auto t1 = t.create();
      t[counter] = make_table_map_raw(t1, val, prefix);
    } else if constexpr (std::is_aggregate_v<mem_type>) {
      auto t1 = t.create();
      t[counter] = make_table_raw(t1, val, prefix);
    } else {
      t[counter] = val;
    }
  }

  return t;
}

template <typename T>
sol::table make_table_raw(sol::table &t, const T &obj, const std::string_view &prefix) {
  reflect::for_each([&](auto I) {
    using value_type = decltype(reflect::get<I>(obj));
    using mem_type = std::remove_cvref_t<value_type>;
    const auto type_name = utils::type_name<mem_type>();
    const std::string_view name = reflect::member_name<I>(obj);
    
    if (prefix.empty() || name.find(prefix) == 0) {
      const auto final_member_name = name.substr(prefix.size());
      if constexpr (utils::is_container_v<mem_type>) {
        auto t1 = t.create();
        t[final_member_name] = make_table_array_raw(t1, reflect::get<I>(obj), prefix);
      } else if constexpr (utils::is_map_v<mem_type>) {
        auto t1 = t.create();
        t[final_member_name] = make_table_map_raw(t1, reflect::get<I>(obj), prefix);
      } else if constexpr (std::is_aggregate_v<mem_type>) {
        auto t1 = t.create_named(final_member_name);
        make_table_raw(t, reflect::get<I>(obj), prefix);
      } 
      // хотя разве он не поймет?
      //else if constexpr (std::is_same_v<std::string, mem_type> || std::is_same_v<std::string_view, mem_type> || std::is_same_v<const char*, mem_type>) {
      //  t[final_member_name] = std::string(reflect::get<I>(default_obj));
      //} 
      else {
        t[final_member_name] = reflect::get<I>(obj);
      }
    }
  }, obj);

  return t;
}

// было бы неплохо придумать какое то более сносное название
template <typename T>
sol::table create_table(const sol::environment &e, const T &obj, const std::string_view &prefix = "") {
  auto t = e.create(e.lua_state());
  if constexpr (utils::is_container_v<T>) {
    return make_table_array_raw(t, obj, prefix);
  } else if constexpr (utils::is_map_v<T>) {
    return make_table_map_raw(t, obj, prefix);
  } else if constexpr (std::is_aggregate_v<T>) {
    return make_table_raw(t, obj, prefix);
  } else {
    t[1] = obj;
  }

  return t;
}

template <typename T>
void get_value_aggregate(const sol::table &t, T &obj, const std::string_view &prefix = "");

template <typename T>
void get_value_map(const sol::table &t, T &obj, const std::string_view &prefix = "");

template <typename T>
void get_value_array(const sol::table &t, T &obj, const std::string_view &prefix = "");

template <typename T>
void get_value_map(const sol::table &t, T &obj, const std::string_view &prefix) {
  using key_type = std::remove_cvref_t<decltype(obj.begin()->first)>;
  using mem_type = std::remove_cvref_t<decltype(obj.begin()->second)>;

  for (const auto &[ key, val ] : t) {
    if (!key.is<key_type>()) continue;

    auto key_v = key.as<key_type>();
    if constexpr (utils::is_container_v<mem_type>) {
      mem_type tmp{};
      get_value_array(t, tmp, prefix);
      obj.insert(std::make_pair(std::move(key_v), std::move(tmp)));
    } else if constexpr (utils::is_map_v<mem_type>) {
      mem_type tmp{};
      get_value_map(t, tmp, prefix);
      obj.insert(std::make_pair(std::move(key_v), std::move(tmp)));
    } else if constexpr (std::is_aggregate_v<mem_type>) {
      mem_type tmp{};
      get_value_aggregate(val, tmp, prefix);
      obj.insert(std::make_pair(std::move(key_v), std::move(tmp)));
    } else {
      obj.insert(std::make_pair(std::move(key_v), val.as<mem_type>()));
    }
  }
}

template <typename T>
void get_value_array(const sol::table &t, T &obj, const std::string_view &prefix) {
  using mem_type = std::remove_cvref_t<decltype(obj[0])>;
  size_t counter = 0;
  for (const auto &[ key, val ] : t) {
    if (key.is<int64_t>() && key.as<int64_t>() == DEVILS_ENGINE_TO_LUA_INDEX(counter)) {
      if constexpr (utils::is_container_v<mem_type>) {
        mem_type tmp{};
        get_value_array(t, tmp, prefix);
        obj.push_back(std::move(tmp));
      } else if constexpr (utils::is_map_v<mem_type>) {
        mem_type tmp{};
        get_value_map(t, tmp, prefix);
        obj.push_back(std::move(tmp));
      } else if constexpr (std::is_aggregate_v<mem_type>) {
        mem_type tmp{};
        get_value_aggregate(val, tmp, prefix);
        obj.push_back(std::move(tmp));
      } else {
        obj.push_back(val.as<mem_type>());
      }
      counter += 1;
    }
  }
}

template <typename T>
void get_value_aggregate(const sol::table &t, T &obj, const std::string_view &prefix) {
  reflect::for_each([&](auto I) {
    using value_type = decltype(reflect::get<I>(obj));
    using mem_type = std::remove_cvref_t<value_type>;
    const auto type_name = utils::type_name<mem_type>();
    const std::string_view name = reflect::member_name<I>(obj);

    if (prefix.empty() || name.find(prefix) == 0) {
      const auto final_member_name = name.substr(prefix.size());
      auto val = t[final_member_name];
      if (!val.valid()) return;

      if constexpr (utils::is_container_v<mem_type>) {
        sol::table t1 = val;
        get_value_array(t1, reflect::get<I>(obj), prefix);
      } else if constexpr (utils::is_map_v<mem_type>) {
        sol::table t1 = val;
        get_value_map(t1, reflect::get<I>(obj), prefix);
      } else if constexpr (std::is_aggregate_v<mem_type>) {
        sol::table t1 = val;
        get_value_aggregate(t1, reflect::get<I>(obj), prefix);
      } else {
        reflect::get<I>(obj) = t[final_member_name];
      }
    }
  }, obj);
}

// было бы неплохо придумать какое то более сносное название
template <typename T>
void get_value(const sol::table &t, T &obj, const std::string_view &prefix = "") {
  if constexpr (utils::is_container_v<T>) {
    get_value_array(t, obj, prefix);
  } else if constexpr (utils::is_map_v<T>) {
    get_value_map(t, obj, prefix);
  } else if constexpr (std::is_aggregate_v<T>) {
    get_value_aggregate(t, obj, prefix);
  } else {
    obj = t[1];
  }
}
}
}

#endif