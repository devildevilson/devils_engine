#ifndef GN07_SOLVER_H
#define GN07_SOLVER_H

#include <cstdint>
#include <string>
#include <vector>

#include "frame.h"
#include "location_model.h"
#include "node_scope.h"

// Решатель: сводит обязательства конкретной локации к графу.
//
//   1. До генерации — то, что перебор seed не исправит: у архетипа нет порта нужного рода,
//      обычной связи нет или их несколько, тайных связей больше одной, семейство неизвестно.
//   2. K попыток; у попытки k свой seed = splitmix(seed, k). Семейство получает от решателя
//      только целевую глубину (бюджет самого требовательного размещения) и допуск.
//   3. Порты проверяются по вычисленным свойствам окончательного графа: обычный вход на
//      стороне соседа по обычной связи; тайный — на стороне соседа в окне вокруг направления,
//      со скрытым проходом в начале и с началом в месте порта из архетипа.
//   4. Размещения привязываются по предикату места и бюджету. Одно размещение может занять тот
//      же узел, что и другое, поэтому они независимы и отката между ними не нужно.
//   5. Общий контент локации — в допуске, если он задан.
//   6. Каркас: граф воплощается в план участка. Каркас, который не может дать проём шире прохода
//      игрока, — отказ попытки, как и любой другой.
//   7. Первая попытка, где всё сошлось, — результат. Если ни одной — отказ с самой частой
//      причиной (числа в причинах при подсчёте не различаются) и числом её попыток.
//
// Выбор среди подходящих узлов зависит от seed попытки и ИМЕНИ размещения, а не от порядка
// строк задания.

namespace gn07 {

struct binding {
  uint32_t placement = 0;
  uint32_t node = no_node;
  size_t candidates = 0;
};

struct port_outcome {
  uint32_t port = 0;             // индекс в location_input::ports
  uint32_t node = no_node;       // узел входа
  uint32_t attach = no_node;     // для тайного — узел шахты, от которого уходит ход
  int64_t way_nodes = 0;
  int64_t way_content = 0;
};

struct solve_result {
  bool ok = false;
  std::string refusal;
  uint32_t attempt = 0;
  uint32_t attempts = 0;
  uint64_t attempt_seed = 0;
  int64_t target_depth = 0;
  int64_t branches = 0;
  location_graph graph;
  std::vector<binding> bindings;
  std::vector<port_outcome> ports;
  location_frame frame;
};

solve_result solve(const brief& task, const location_input& input, const archetype& arch, const script_env& env,
                   uint64_t seed, uint32_t attempts);

// Независимая проверка результата: дерево, решётка, свойства пересчитаны на копии, глубины
// пересчитаны по цепочке родителей, предикаты вычислены повторно, порты и допуск проверены
// по графу. Пустой список — всё верно.
std::vector<std::string> verify_result(const solve_result& result, const brief& task, const location_input& input,
                                       const archetype& arch, const script_env& env);

// Отчёт и карта решётки. Отчёт полностью определяется результатом, поэтому его же сравнивает
// проверка воспроизводимости.
std::string format_report(const solve_result& result, const brief& task, const location_input& input,
                          const archetype& arch);
std::string format_map(const solve_result& result, const archetype& arch);
std::string format_frame_svg(const solve_result& result, const brief& task, const archetype& arch);

} // namespace gn07

#endif
