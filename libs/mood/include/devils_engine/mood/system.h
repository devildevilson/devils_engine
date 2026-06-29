#ifndef DEVILS_ENGINE_MOOD_SYSTEM_H
#define DEVILS_ENGINE_MOOD_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <span>
#include <gtl/phmap.hpp>

#include "devils_engine/utils/string_id.h"   // utils::id / string_hash (предпосчёт хешей)
#include "devils_engine/act/registry.h"      // act::registry (бывшая mood::table)
#include "devils_engine/act/function.h"      // act::predicate_function / act::effect_function
#include "devils_engine/act/exec_context.h"  // act::exec_context

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
// ТА САМАЯ "общая таблица функций между системами", о которой речь выше, теперь
// существует как act::registry (devils_engine::act): guard = act::predicate_function
// (bool), action = act::effect_function (void). mood больше не держит свой набор
// std::function — он резолвит имена guard/action в типизированные указатели реестра при
// сборке системы (см. конструктор) и зовёт их через общий act::exec_context.
//
// system — ТУПОЕ БЫСТРОЕ ХРАНИЛИЩЕ состояний и переходов: оно ничего не держит в рантайме,
// ни на что не реагирует и НЕ знает про конвенции имён (any_state / idle / on_entry /
// on_exit). Его задача — по (state, event) за O(1) вернуть отсортированный по исходному
// порядку строк список переходов-кандидатов. Вся логика конвенций, шаг автомата, проверка
// гвардов и валидация графа живут в helper-функциях (см. mood/runtime.h). Рантайм работает
// на ХЕШАХ (utils::id), строки в transition нужны только для диагностики/describe.

class system {
public:
  struct transition {
    std::string_view full_line;

    std::string_view current_state;
    std::string_view event;
    std::string_view next_state;
    std::array<std::string_view, 8> guards;
    std::array<std::string_view, 8> actions;

    // предпосчитанные хеши — рантайм сравнивает/ищет по ним, не по строкам.
    utils::id current_hash = utils::invalid_id;
    utils::id event_hash   = utils::invalid_id;
    utils::id next_hash    = utils::invalid_id; // invalid_id если next_state пуст (чистый эффект)

    std::array<const act::predicate_function*, 8> guards_ptr{};
    std::array<const act::effect_function*, 8> actions_ptr{};

    transition() noexcept = default;
    // гварды/эффекты ЭТОГО перехода — работают над собственными данными, конвенций не знают.
    int32_t is_valid(const act::exec_context& ctx) const; // все ли гварды прошли (1/0)
    int32_t process(const act::exec_context& ctx) const;  // выполнить эффекты перехода
  };

  // диапазон в m_transitions для одной (state, event) группы (группа непрерывна).
  struct range { uint32_t offset = 0; uint32_t count = 0; };

  system(const act::registry* registry, std::vector<std::string> lines) noexcept;

  // основной доступ — по хешам (горячий путь). Порядок внутри группы = порядок исходных
  // строк (АРХИВАЖНО для top-down проверки гвардов).
  std::span<const transition> find_transitions(const utils::id state_hash, const utils::id event_hash) const;
  // обёртки: хешируют и форвардят (парсинг/тесты/диагностика).
  std::span<const transition> find_transitions(const std::string_view& current_state, const std::string_view& event) const;
  std::span<const transition> transitions() const;
private:
  const act::registry* registry;
  std::vector<transition> m_transitions;
  gtl::flat_hash_map<uint64_t, range> m_index; // mix(state_hash, event_hash) -> диапазон
  std::vector<std::string> m_memory;
};
}
}

#endif
