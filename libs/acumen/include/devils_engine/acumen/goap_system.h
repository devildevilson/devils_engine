#ifndef DEVILS_ENGINE_ACUMEN_GOAP_SYSTEM_H
#define DEVILS_ENGINE_ACUMEN_GOAP_SYSTEM_H

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goap_resource.h"
#include "system.h"

namespace devils_engine {
namespace acumen {

// Собрать system из плоского goap_config (после resolve_goap_config). Нумерация битов: метрики —
// 0..N-1 в порядке конфига, символические биты (встречаются только в next_state/goal, напр.
// "resolved") — следом при первой встрече. Битов больше, чем держит acumen::state, —
// utils::error с именем мозга (scoped_state::set — noexcept над bitset::set, выход за размер там был
// бы terminate без диагностики). Функции метрик и действий system резолвит по имени в `functions`;
// регистрирует их потребитель (см. goap_function_names).
system build_system(const act::registry* functions, std::string_view brain, const goap_config& config);

// Имена act-функций, которые объявляют GOAP-конфиги нескольких мозгов одного реестра. Мозги обычно
// наследуют общую базу, поэтому одноимённые метрики и действия приходят копиями одного скрипта:
// тот же origin — функция уже зарегистрирована, другой origin — конфликт имён, utils::error.
class goap_function_names {
public:
  // true — имя новое, потребитель регистрирует функцию; false — это имя с тем же origin уже есть.
  bool claim(std::string_view key, std::string_view origin);
  void clear() noexcept;

private:
  std::vector<std::pair<std::string, std::string>> origins_; // key -> origin; мозгов и имён единицы-десятки
};

} // namespace acumen
} // namespace devils_engine

#endif
