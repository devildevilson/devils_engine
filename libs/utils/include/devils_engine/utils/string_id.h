#ifndef DEVILS_ENGINE_UTILS_STRING_ID_H
#define DEVILS_ENGINE_UTILS_STRING_ID_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtl/phmap.hpp>

#include "core.h"

namespace devils_engine {
namespace utils {

// в этом файле две независимые штуки:
//
// 1. string_hash - свободная функция для строк, которым НЕ нужен последовательный id.
//    отдает стабильный 64 битный хеш строки (rapidhash). никакого состояния, ничего регистрировать
//    не нужно, можно звать откуда угодно и из любого потока. для compile time строк есть отдельный
//    constexpr murmur в type_traits.h - runtime и compile time строки мы намеренно не смешиваем.
//
// 2. string_pool - класс для строк, которым нужен ПОСЛЕДОВАТЕЛЬНЫЙ плотный id (0..N).
//    такой id удобно использовать как позицию бита (флаги состояний GOAP и тому подобное).
//    контракт по тредам: вся регистрация (reg) проходит ДО использования в однопоточной фазе
//    (например при загрузке модулей), после чего lookup/name только читают и потокобезопасны.

using id = uint64_t;
constexpr id invalid_id = UINT64_MAX;

// --- 1. непоследовательный хеш ---

[[nodiscard]] id string_hash(const std::string_view& str) noexcept;
[[nodiscard]] id string_hash(const std::string_view& str, const uint64_t seed) noexcept;

// --- 2. последовательный пул строк ---

// TAG разделяет независимые пулы на уровне типа: id из одного пула нельзя случайно скормить другому.
template <size_t TAG = 0>
class string_pool {
public:
  // регистрирует строку и возвращает ее плотный id. повторный reg той же строки отдает тот же id.
  // НЕ потокобезопасно - звать только в однопоточной фазе загрузки.
  id reg(const std::string_view& str) {
    // offset/size в entry это uint32_t - страхуемся заранее, чтобы обскурный баг не всплыл потом
    if (str.size() >= UINT32_MAX) {
      utils::error{}("string_pool: string is too long ({} bytes), max is {}", str.size(), UINT32_MAX);
    }

    const auto hash = string_hash(str);
    const auto itr = m_lookup.find(hash);
    if (itr != m_lookup.end()) {
      if (str != view(itr->second)) {
        utils::error{}("String hash collision in string_pool: '{}' vs '{}'. You are winner =)", str, view(itr->second));
      }
      return itr->second;
    }

    const uint32_t offset = static_cast<uint32_t>(m_memory.size());
    m_memory.insert(m_memory.end(), str.begin(), str.end());
    m_memory.push_back('\0');

    const id index = m_entries.size();
    m_entries.emplace_back(entry{offset, static_cast<uint32_t>(str.size())});
    m_lookup.emplace(hash, index);
    return index;
  }

  // потокобезопасно после завершения регистрации. invalid_id если строка не зарегистрирована.
  [[nodiscard]] id lookup(const std::string_view& str) const {
    const auto hash = string_hash(str);
    const auto itr = m_lookup.find(hash);
    if (itr == m_lookup.end()) {
      return invalid_id;
    }
    if (str != view(itr->second)) {
      utils::error{}("String hash collision in string_pool: '{}' vs '{}'. You are winner =)", str, view(itr->second));
    }
    return itr->second;
  }

  // потокобезопасно после завершения регистрации. пустой view если id невалиден.
  [[nodiscard]] std::string_view name(const id val) const {
    if (val >= m_entries.size()) {
      return std::string_view();
    }
    return view(val);
  }

  [[nodiscard]] bool contains(const std::string_view& str) const {
    return lookup(str) != invalid_id;
  }
  [[nodiscard]] size_t size() const noexcept {
    return m_entries.size();
  }
  [[nodiscard]] bool empty() const noexcept {
    return m_entries.empty();
  }

  void clear() noexcept {
    m_lookup.clear();
    m_entries.clear();
    m_memory.clear();
  }

private:
  struct entry {
    uint32_t offset;
    uint32_t size;
  };

  [[nodiscard]] std::string_view view(const id index) const noexcept {
    const auto& e = m_entries[index];
    return std::string_view(&m_memory[e.offset], e.size);
  }

  gtl::flat_hash_map<uint64_t, id> m_lookup;
  std::vector<entry> m_entries;
  std::vector<char> m_memory;
};

} // namespace utils
} // namespace devils_engine

#endif
