#ifndef GN07_LOCATION_MODEL_H
#define GN07_LOCATION_MODEL_H

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <devils_script/container.h>

// Данные площадки.
//
//   фрагмент мира — подделка графа А (ландшафт и окружение): узлы с положением и фактами
//                   и физические связи между ними. Связи уже решены верхним уровнем, в том
//                   числе какая из них тайная; локальный решатель их не выбирает;
//   архетип       — вариант рода места (шахта, глубокая шахта): виды узлов, места, порты,
//                   теги и параметры роста. Каждый вариант — свой файл, без наследования;
//   задание       — обязательства конкретной локации: размещения с бюджетами, лорные связи
//                   между ними и допуск по общему контенту;
//   вход локации  — то, что решатель выводит из фрагмента: границы, факты с наследованием
//                   и порты, направление которых задаёт положение соседа;
//   граф          — результат, до геометрии.
//
// У узла графа есть клетка грубой решётки только для того, чтобы у слов «сторона границы»
// и «рядом» был смысл, а воплощение потом было возможным. Сторона — деталь семейства,
// в данных её нет: она выводится из того, где стоит сосед.
//
// Вычисленные свойства — функции ВСЕГО графа. После любой правки их пересчитывают заново
// через compute_properties, а не обновляют по месту.

namespace gn07 {

inline constexpr uint32_t no_node = std::numeric_limits<uint32_t>::max();

enum class side : uint8_t { none, north, east, south, west };
std::string_view to_string(side value) noexcept;
side side_from_string(std::string_view text) noexcept;

// Очевидность прохода в узел от его родителя.
enum class passage : uint8_t { open, hidden };
std::string_view to_string(passage value) noexcept;

// Род связи с соседом: обычный вход ждут, тайный — преимущество, назначенное верхним уровнем.
enum class link_kind : uint8_t { ordinary, secret };
std::string_view to_string(link_kind value) noexcept;
bool link_kind_from_string(std::string_view text, link_kind& out) noexcept;

struct fact {
  std::string name;
  std::string value;
};

// ---- Архетип -------------------------------------------------------------------------------

// Геометрия плана — в сантиметрах: целые числа, чтобы проверки стыков и стен были точными и
// не зависели от машины. В данных размеры пишутся в единицах мира (метрах) с дробью и читаются
// как фиксированная точка, без double.
inline constexpr int32_t plan_scale = 100;

// Чем вид узла становится в каркасе. Коридор — сечение шириной `width`; камера — прямоугольник
// `size` с несколькими пристроенными «наростами» (`lobes`), и устье шириной `width` к соседям.
enum class body_shape : uint8_t { corridor, chamber };
std::string_view to_string(body_shape value) noexcept;

struct body_spec {
  body_shape shape = body_shape::corridor;
  int32_t width_min = 0;        // см: сечение коридора или устья камеры
  int32_t width_max = 0;
  int32_t size_min = 0;         // см: сторона камеры
  int32_t size_max = 0;
  int64_t lobes_min = 0;
  int64_t lobes_max = 0;
};

struct node_kind {
  std::string name;
  std::string function;
  int64_t content = 0;          // ёмкость контента: из неё считаются бюджеты, глубины и допуск
  std::vector<std::string> tags;
  char glyph = '?';
  body_spec body;
};

// Правила воплощения графа в каркас, общие для всех видов узлов варианта (см).
struct frame_rules {
  int32_t wall = 0;             // наименьшая толща породы между несвязанными выработками
  int32_t passage = 0;          // самый узкий проём, в который проходит игрок
  int32_t hidden_min = 0;       // ширина скрытого прохода; верх обязан быть уже любого открытого
  int32_t hidden_max = 0;
  int32_t wander = 0;           // насколько выработка уходит вбок на шаге решётки
};

// Именованное место — предикат автора архетипа над мета-информацией узла.
struct place {
  std::string name;
  devils_script::container where;
};

// Порт какого рода вариант умеет и откуда внутри он выходит. Для обычного входа места нет:
// это корень графа. Для тайного — место узла, от которого уходит ход.
struct port_kind {
  link_kind kind = link_kind::ordinary;
  std::string place;
};

// Параметры роста. Их читает только семейство `mine`; другое семейство объявит свои.
struct mine_growth {
  int64_t turn_percent = 30;
  int64_t branches_min = 3;
  int64_t branches_max = 6;
  int64_t branch_length_min = 2;
  int64_t branch_length_max = 5;
  int64_t abandoned_percent = 35;
  int64_t face_percent = 60;
};

struct archetype {
  std::string name;
  std::string family;           // какой код строит граф
  int64_t cell = 12;            // единиц плана на шаг решётки
  std::vector<std::string> tags; // что верхний уровень ищет в этом варианте (deep, shallow...)
  std::vector<node_kind> kinds;
  std::vector<place> places;
  std::vector<port_kind> ports;
  mine_growth growth;
  frame_rules frame;

  uint32_t kind_index(std::string_view name) const noexcept;       // no_node, если нет
  const place* find_place(std::string_view name) const noexcept;
  const port_kind* find_port(link_kind kind) const noexcept;
  bool knows_tag(std::string_view tag) const noexcept;
  bool knows_function(std::string_view function) const noexcept;
};

// ---- Фрагмент мира (граф А) ----------------------------------------------------------------

struct world_node {
  std::string name;
  std::string parent;
  std::string archetype;        // пусто — узел не локация (дорога, тоннель, замок)
  int64_t at_x = 0;             // мировые единицы; x на восток, y на север
  int64_t at_y = 0;
  int64_t bounds_width = 0;     // только у локаций: участок вокруг `at`
  int64_t bounds_height = 0;
  std::vector<fact> facts;
};

struct world_link {
  std::string a;
  std::string b;
  link_kind kind = link_kind::ordinary;
};

struct world_fragment {
  std::vector<world_node> nodes;
  std::vector<world_link> links;

  const world_node* find(std::string_view name) const noexcept;
  // Факты узла с наследованием от предков: потомок перекрывает значение предка. Порядок —
  // по имени, чтобы отчёт и самопроверки не зависели от порядка строк в файле.
  std::vector<fact> facts_of(std::string_view name) const;
};

// ---- Задание -------------------------------------------------------------------------------

struct placement {
  std::string name;
  std::string what;             // метаданные для прохода наполнения; решатель их не читает
  int64_t count = 1;
  std::string place;            // имя места архетипа
  int64_t budget_min = 0;       // контент по пути от обычного входа до привязки
  int64_t budget_max = 0;
};

// Лорная связь между обязательствами: «шахтёры знают, где предмет». Пространства она не
// ограничивает, решатель проносит её в результат для слоя диалогов.
struct lore_link {
  std::string from;
  std::string to;
  std::string kind;
};

struct brief {
  std::string name;
  std::string location;         // узел фрагмента
  std::vector<placement> placements;
  std::vector<lore_link> lore;
  bool has_allowance = false;   // допуск по общему контенту всей локации
  int64_t content_min = 0;
  int64_t content_max = 0;
};

// ---- Вход локации --------------------------------------------------------------------------

struct port_request {
  std::string neighbour;
  link_kind kind = link_kind::ordinary;
  side at = side::none;         // сторона, обращённая к соседу
  int32_t along = 0;            // клетка вдоль этой стороны, куда смотрит сосед
};

struct location_input {
  const world_node* node = nullptr;
  int32_t width = 0;            // решётка в клетках
  int32_t height = 0;
  int64_t cell = 0;
  std::vector<fact> facts;
  std::vector<port_request> ports;
};

// ---- Граф ----------------------------------------------------------------------------------

struct graph_node {
  uint32_t kind = 0;
  uint32_t parent = no_node;    // дерево с корнем в обычном входе
  passage entry = passage::open;
  int32_t x = 0;
  int32_t y = 0;
};

struct location_graph {
  int32_t width = 0;
  int32_t height = 0;
  int64_t cell = 0;
  std::vector<graph_node> nodes; // узел 0 — обычный вход
  uint32_t secret_entrance = no_node;

  // Вычисленное (compute_properties).
  std::vector<std::vector<uint32_t>> neighbours;
  std::vector<int64_t> depth;         // контент от обычного входа, без стартового узла
  std::vector<int64_t> secret_depth;  // то же от особого входа; -1, если его нет
  std::vector<int64_t> route_steps;   // шаги решётки от обычного входа
  std::vector<int64_t> subtree;       // узлов, которые открываются только через этот
  std::vector<uint8_t> on_secret_way;
  std::vector<side> boundary;
  int64_t max_depth = 0;
  int64_t total_content = 0;
};

// Контент на пути от `from` ко всем узлам, без стартового узла и с конечным.
std::vector<int64_t> content_distances(const location_graph& graph, const archetype& arch, uint32_t from);

void compute_properties(location_graph& graph, const archetype& arch);

// Окно вдоль стороны, в которое обязан попасть тайный порт: сосед задаёт направление, а не клетку.
int32_t port_window(int32_t side_length) noexcept;

// Сторона и клетка, обращённые к соседу: луч из центра решётки в сторону соседа. Только целая
// арифметика, чтобы ответ не зависел от машины. Ноль при совпадающих координатах — отказ.
bool facing(int32_t width, int32_t height, int64_t dx_east, int64_t dy_north, side& at, int32_t& along) noexcept;

} // namespace gn07

#endif
