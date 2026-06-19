#ifndef DEVILS_ENGINE_UTILS_STRING_ID_H
#define DEVILS_ENGINE_UTILS_STRING_ID_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>
#include <gtl/phmap.hpp>

#include "core.h"
#include "rapidhash.h"

namespace devils_engine {
namespace utils {
/**
 * несколько замечаний: система должна выдать стабильный uint64_t айдишник на произвольную строку
 * желательно сделать систему thread safe
 * для этого лучше добавить регистрацию отдельно и заранее все строки зарегистрировать 
 * порядок регистрации строк не должен иметь значения... хотя стоп
 * а как же последовательность?
 * 
 * мы сделаем вот что - есть минимум две системы:
 * зависимая от последовательности
 * независимая от последовательности
 * + к этому добавим возможность иметь системам маленькую lookup table для независимых id
 * 
 * как обойтись без предварительной регистрации? она по идее не должна требоваться хотя бы для 
 * order_independent варианта... ощущение что для thread safe варианта особо никак
 */ 

using id = uint64_t;
constexpr id invalid_id = UINT64_MAX;

namespace hash_string {
using value = uint64_t;

// дает случайное uint64_t число
template <size_t ID>
class order_independent {
public:
  id reg(const std::string_view& str);

  // thread safe
  id lookup(const std::string_view &str) const;
  std::string_view lookup(const id val) const;
private:
  struct data {
    uint32_t offset;
    uint32_t size;
  };

  gtl::flat_hash_map<value, uint64_t> lookup_table;
  std::vector<data> lookup_string;
  std::vector<char> memory;
};

// дает последовательное uint64_t число
template <size_t ID>
class order_dependent {
public:
  id reg(const std::string_view& str);

  // thread safe
  id lookup(const std::string_view &str) const;
  std::string_view lookup(const id val) const;
private:
  struct data {
    uint32_t offset;
    uint32_t size;
    value hash_value;
  };

  gtl::flat_hash_map<value, uint64_t> lookup_table;
  std::vector<data> lookup_string;
  std::vector<char> memory;
};

template <size_t ID>
id order_independent<ID>::reg(const std::string_view& str) {
  const auto hash = rapidhash_withSeed(str.data(), str.size(), ID);
  const auto itr = lookup_table.find(hash);
  if (itr != lookup_table.end()) {
    // тут строку надо сравнить и если не совпадает то вылетаем =(
    const auto& d = lookup_string[itr->second];
    const auto contained_str = std::string_view(&memory[d.offset], d.size);
    if (str != contained_str) utils::error{}("String hash collision: hash('{}') == hash('{}'). You are winner =)", str, contained_str);
    return hash;
  }

  const size_t cur_pos = memory.size();
  memory.resize(cur_pos + str.size()+1, 0);
  memcpy(&memory[cur_pos], str.data(), str.size());
  memory[cur_pos + str.size()] = '\0';

  const uint64_t index = lookup_string.size();
  lookup_string.emplace_back(data{cur_pos, str.size(), hash});

  lookup_table.emplace(std::make_pair(hash, index));

  return hash;
}

template <size_t ID>
id order_independent<ID>::lookup(const std::string_view& str) const {
  const auto hash = rapidhash_withSeed(str.data(), str.size(), ID);
  const auto itr = lookup_table.find(hash);
  if (itr == lookup_table.end()) return invalid_id;

  const auto& d = lookup_string[itr->second];
  const auto contained_str = std::string_view(&memory[d.offset], d.size);
  if (str != contained_str) utils::error{}("String hash collision: hash('{}') == hash('{}'). You are winner =)", str, contained_str);

  return hash;
}

template <size_t ID>
std::string_view order_independent<ID>::lookup(const id val) const {
  const auto itr = lookup_table.find(val);
  if (itr == lookup_table.end()) return std::string_view();

  const auto& d = lookup_string[itr->second];
  return std::string_view(&memory[d.offset], d.size);
}

template <size_t ID>
id order_dependent<ID>::reg(const std::string_view& str) {
  const auto hash = rapidhash_withSeed(str.data(), str.size(), ID);
  const auto itr = lookup_table.find(hash);
  if (itr != lookup_table.end()) {
    // тут строку надо сравнить и если не совпадает то вылетаем =(
    const auto& d = lookup_string[itr->second];
    const auto contained_str = std::string_view(&memory[d.offset], d.size);
    if (str != contained_str) utils::error{}("String hash collision: hash('{}') == hash('{}'). You are winner =)", str, contained_str);
    return hash;
  }

  const size_t cur_pos = memory.size();
  memory.resize(cur_pos + str.size()+1, 0);
  memcpy(&memory[cur_pos], str.data(), str.size());
  memory[cur_pos + str.size()] = '\0';

  const uint64_t index = lookup_string.size();
  lookup_string.emplace_back(data{cur_pos, str.size(), hash});

  lookup_table.emplace(std::make_pair(hash, index));

  return index;
}

template <size_t ID>
id order_dependent<ID>::lookup(const std::string_view& str) const {
  const auto hash = rapidhash_withSeed(str.data(), str.size(), ID);
  const auto itr = lookup_table.find(hash);
  if (itr == lookup_table.end()) return invalid_id;

  const auto& d = lookup_string[itr->second];
  const auto contained_str = std::string_view(&memory[d.offset], d.size);
  if (str != contained_str) utils::error{}("String hash collision: hash('{}') == hash('{}'). You are winner =)", str, contained_str);

  return itr->second;
}

template <size_t ID>
std::string_view order_dependent<ID>::lookup(const id val) const {
  if (val >= lookup_string.size()) return std::string_view();
  const auto& d = lookup_string[val];
  return std::string_view(&memory[d.offset], d.size);
}

}

template <size_t ID>
using arbitrary_hash = hash_string::order_independent<ID>;
template <size_t ID>
using monotonic_hash = hash_string::order_dependent<ID>;

}
}

#endif