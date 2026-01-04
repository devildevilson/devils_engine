#ifndef DEVILS_ENGINE_MOOD_SYSTEM_H
#define DEVILS_ENGINE_MOOD_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <span>
#include <functional>
#include "gtl/phmap.hpp"

// реально думаю что у нас скорее всего будет общий реестр функций, более того
// наверное будет общий реестр базовых геймплейных функций + реестр функций которые мы составим из них
// функции у нас будут отличаться по типу: функция экшен, функция гвард, функция которая возвращает какое то значение
// какое значение? да вообще наверное любое - мы можем вернуть практически любое значение by value
// в рамках игры у нас довольно ограниченные запросы: мы бы хотели иметь возможность ответить да или нет на какой то продолжительный скрипт
// посчитать какое то значение (или вектор), посчитать строчку, возможно отдать один из заранее определенных типов
// не сказать чтобы много разных применений, другое дело что в эти функции нужно запихнуть 
// входные данные, то есть например чтобы посчитать отношение одного персонажа к другому, то мне как минимум нужно указать 
// энтитиА, энтитиБ и например найти у этих энтити несколько похожих перков которые дают бонусы к отношениям
// в этом случае как будто не участвуют доп данные
// но например в договор о мире я должен вложить ряд предложений другой стороне в том числе:
// провинции, отношения, репарации, хумилиатион и так далее, как это сделать?
// я так подозреваю что эта система - это не скрипт, а что то совершенно отдельное
// которое состоит из пачки разных скриптов
// но например все равно для того чтобы проверить валидность передачи провки после войны как будто нужно 
// указать запрашивающего + владельца + провку, то есть нужна некая форма контекста
// в скриптах которые сделал я это будет, в остальных местах - врядли

namespace devils_engine {
namespace mood {
// по идее эту таблицу можно шарить между системами
// более того у меня наверное будет общая таблица со всеми возможными функциями
// будут возвращать либо bool, либо число, либо строку, либо это функция эффект
// как обычно таблицу тут лучше сделать через std vector
struct table {
  using action_f = std::function<int32_t(void* userdata)>; // non-zero return == error
  using guard_f = std::function<int32_t(void* userdata)>; // less zero return == error

  gtl::node_hash_map<std::string, action_f> actions;
  gtl::node_hash_map<std::string, guard_f> guards;
};

class system {
public:
  struct transition {
    std::string_view full_line;

    std::string_view current_state;
    std::string_view event;
    std::string_view next_state;
    std::array<std::string_view, 8> guards;
    std::array<std::string_view, 8> actions;

    std::span<const transition> current_state_on_exit;
    std::span<const transition> next_state_on_entry;
    std::array<const table::guard_f*, 8> guards_ptr;
    std::array<const table::action_f*, 8> actions_ptr;

    transition() noexcept = default;
    int32_t is_valid(void* userdata) const;
    int32_t on_exit(void* userdata) const;
    int32_t process(void* userdata) const;
    int32_t on_entry(void* userdata) const;
  };
  
  system(const struct table* table, std::vector<std::string> lines) noexcept;

  std::span<const transition> find_transitions(const std::string_view& current_state, const std::string_view& event) const;
  std::span<const transition> transitions() const;
private:
  const struct table* table;
  std::vector<transition> m_transitions; // gtl::flat_hash_map ?
  std::vector<std::string> m_memory;
};
}
}

#endif