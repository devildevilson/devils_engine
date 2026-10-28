#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "adventure.h"

namespace gn09 {

// План искусственного места живёт в сантиметрах, отдельно от сэмплов пещеры.
// Оси u/v лежат в X/Z; u выбирается по свободному направлению от портов.
struct plan_rect {
  int32_t u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

struct built_zone {
  plan_rect box;
  std::string purpose;
};

struct built_opening {
  uint32_t a = 0, b = 0;
  // Отрезок на общей стене зон: либо u0==u1, либо v0==v1.
  plan_rect span;
};

struct built_port {
  uint32_t edge_index = 0;
  plan_rect span; // проём дока на его внешней стороне
};

enum class hatch_condition : uint8_t { operable, jammed, breached };
const char* name(hatch_condition condition);

struct emergency_exit {
  uint32_t zone = 0; // модуль, из которого дверь ведёт в пещеру
  plan_rect span;     // точный отрезок на наружной стене модуля
  hatch_condition condition = hatch_condition::operable;
};

// Руда, кристалл или кладка: локальный объект, не отдельный узел маршрута.
struct local_feature {
  uint32_t room = 0;
  point centre;
  std::string kind;
  uint32_t amount = 0;
};

struct local_structure {
  uint32_t node_id = 0;
  theme kind = theme::empty;
  site_recipe recipe = site_recipe::ore_cave;
  point centre;
  point forward{1, 0, 0};
  int8_t dock_axis = 0; // 0: боковой шлюз; +1: база над лодкой; -1: под ней
  int32_t level_cm = 0; // высота построенных зон относительно графового узла
  hatch_condition dock_hatch = hatch_condition::operable;
  // Заброшенная база: у этой зоны нет центральной панели крыши (0 — нет пролома).
  uint32_t roof_breach = 0;
  // Бит i: у модуля i центральная панель крыши — «стекло» (пока непрозрачное).
  uint16_t glazed_roofs = 0;
  std::vector<built_zone> zones;
  std::vector<built_opening> openings;
  std::vector<built_port> ports;
  std::vector<emergency_exit> emergency_exits;
  std::vector<local_feature> features;
};

// Цельные детали постройки в локальных сантиметрах. Стены — отдельный материал,
// поэтому вырезание скалы не может незаметно удалить перегородку.
struct built_solid {
  plan_rect footprint;
  int32_t y0 = 0, y1 = 0;
  uint8_t shade = 0;
  bool hatch_panel = false;
};

local_structure materialize_local_structure(const adventure& world, uint32_t node_id);
// План базы, выбранный в её канонической окрестности 3x3 сектора — тот же, что
// получит любое окно. Нужен уровню залов, чтобы горловина обходила детали баз.
local_structure canonical_outpost(uint64_t seed, const node& at, double clearance_radius);
int8_t preferred_dock_axis(uint64_t seed, const node& at);
// Проверяет прямоугольники, проёмы и привязку залежей независимо от генератора плана.
std::string verify_local_structure(const adventure& world, const site& local,
                                   const local_structure& structure);
point structure_to_world(const local_structure& structure, double u, double y, double v);
point docking_axis_point(const local_structure& structure, double progress, double along);
double outpost_envelope_field(const local_structure& structure, point position);
// false — в области [low,high] монтажный объём заведомо равен стартовому 3.0.
bool outpost_envelope_reaches(const local_structure& structure, point low, point high);
// Проверяет природный док до монтажа и долю корпуса, помещённого в породу.
std::string verify_outpost_environment(const adventure& world,
                                       const local_structure& structure);
uint32_t outpost_body_rock_percent(const adventure& world,
                                   const local_structure& structure);
std::vector<built_solid> materialize_outpost_solids(const local_structure& structure);
// Отделка крыш по тому, что над ними до монтажа. Пролом в скалу выглядел бы
// изнутри как квадрат камня в потолке, а стекло — как окно в камень, поэтому оба
// ставятся только на крыши, над которыми открыта природная пещера.
struct roof_finish {
  uint32_t breach = 0;  // заброшенная база: зона без панели, 0 — нет
  uint16_t glazed = 0;  // маска модулей со стеклом
};
// Маска зон 1.., над серединой крыши которых пещера открыта (люк в крыше исключён).
uint16_t open_roof_mask(const adventure& world, const local_structure& structure);
roof_finish choose_roof_finish(const adventure& world, const local_structure& structure);
// Вероятность застеклить открытый модуль: у жилого рецепта выше, у прочих ниже.
double glazing_chance(site_recipe recipe);
std::vector<built_solid> fit_outpost_solids(const adventure& world,
                                           const local_structure& structure);
// Проверяет уже построенные твёрдые детали относительно резервов графа.
std::string verify_outpost_solids(const adventure& world, const local_structure& structure,
                                  const std::vector<built_solid>& solids);
bool write_structure_svg(const adventure& world, uint32_t node_id, const std::string& path);

// Постоянная для ребра часть формы галереи: концы, рамка, ширины и две боковые
// полости. Считается один раз на область вместо каждой точки поля.
struct edge_shape {
  struct lobe {
    point centre;
    double axial = 0, lateral = 0, vertical = 0;
  };
  point a, b, axis, right, up;
  double distance = 0, core_radius = 0;
  double vertical_gain = 0, biome_width = 1;
  double first_t = 0, second_t = 0;
  double base_width = 0, first_width = 0, second_width = 0, base_roof = 0;
  bool constriction = false;
  lobe lobes[2];
};
edge_shape shape_edge(const adventure& world, const node& from, const node& to);

// Поле, подготовленное для одной прямоугольной области (обычно чанка). Из окна
// оставлены только рёбра, узлы, места и базы, чьё влияние может дотянуться до
// области, а места, карманы и планы баз материализованы один раз, а не в каждой
// точке. Отброшенное слагаемое минимума в области заведомо больше фоновой скалы
// (3 + неровность <= 3.46), поэтому значение побитово равно полю всего окна.
struct field_region {
  const adventure* world = nullptr;
  point low, high;
  std::vector<uint32_t> edges;
  std::vector<edge_shape> shapes;                  // параллельно edges
  std::vector<std::optional<edge_pocket>> pockets; // параллельно edges
  std::vector<uint32_t> nodes;
  std::vector<site> sites;
  std::vector<local_structure> bases;
  std::vector<uint8_t> envelope_reaches; // параллельно bases
  std::vector<uint32_t> halls;           // индексы adventure::halls
};
// with_structures = false готовит только природную пещеру для cave_density.
field_region prepare_field(const adventure& world, point low, point high,
                           bool with_structures = true);
// Чанк вместе с соседним слоем сэмплов, нужным поверхности для градиентов.
field_region prepare_chunk_field(const adventure& world, int x, int y, int z);
double cave_density(const field_region& region, point position);
// +1 — вся область сплошная скала (до неё не дотягивается ни одно слагаемое),
// -1 — вся область вода внутри зала, 0 — неизвестно, нужны сэмплы. Оценка точная:
// у однородного чанка все сэмплы имеют этот знак, и поверхности в нём нет.
int uniform_sign(const field_region& region);
double density(const field_region& region, point position);
chunk generate_chunk(const field_region& region, int x, int y, int z);

} // namespace gn09
