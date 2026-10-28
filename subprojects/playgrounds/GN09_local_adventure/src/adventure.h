#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gn09 {

struct point {
  double x = 0;
  double y = 0;
  double z = 0;
};

enum class theme : uint8_t { empty, outpost, derelict, grotto, vent };
const char* name(theme value);
bool is_base(theme value);

enum class cave_biome : uint8_t { karst, rift, chimney };
const char* name(cave_biome value);
cave_biome biome_at(uint64_t seed, point position);

// Рецепт малого места выбирается по постоянному ключу узла. Это локальный
// генератор, а не новый вид узла в макрографе.
enum class site_recipe : uint8_t {
  ore_cave, nest_cave, thermal_cave,
  mining_base, residential_base, military_base
};
const char* name(site_recipe value);

struct node {
  uint32_t id = 0;
  uint64_t key = 0; // постоянный ключ точки, не зависящий от запрошенного окна
  point centre;
  theme kind = theme::empty;
  uint8_t rank = 0; // назначается до рёбер; обязан совпасть с итоговой степенью
};
site_recipe recipe_of(uint64_t seed, const node& at);

struct edge {
  uint32_t a = 0;
  uint32_t b = 0;
};

struct graph {
  struct window {
    int min_x = 0, min_z = 0, max_x = 2, max_z = 2; // клетки мира, [min,max)
  } area;
  std::vector<node> nodes;
  std::vector<edge> edges;
  uint32_t entry = 0;
  uint32_t destination = 0;
};

struct portal {
  uint32_t edge_index = 0;
  point centre;
};

struct room {
  point centre;
  double radius = 0;
  std::string purpose;
};

struct room_link {
  uint32_t a = 0;
  uint32_t b = 0;
};

// Локальный генератор знает только место, его порты и seed. Ему не нужен граф всей игры.
struct site {
  uint32_t node_id = 0;
  theme kind = theme::grotto;
  site_recipe recipe = site_recipe::ore_cave;
  point centre;
  std::vector<portal> portals;
  std::vector<room> rooms;
  std::vector<room_link> links;
};

struct edge_pocket {
  point mouth;
  point centre;
  double radius = 0;
};
// Небольшой декоративный карман привязан к ребру, не создавая новый узел графа.
std::optional<edge_pocket> materialize_edge_pocket(uint64_t seed,
                                                    const node& a, const node& b);

// Большой зал регионального уровня. Он не ребро графа: это объём пещеры со своим
// гарантированным коридором для крупных существ и одной горловиной в обжитой слой.
// Координаты зала локальные: u вдоль forward, y вверх, v вправо.
enum class hall_kind : uint8_t { pillared, lair };
const char* name(hall_kind value);

struct hall_pillar {
  double u = 0, v = 0, radius = 0; // у пола и потолка шире: radius * (1 + 0.35 (y/H)^2)
};

struct hall_ledge {
  point centre; // локальная коробка уступа, частью уходящая в стену
  point half;
};

struct hall {
  uint64_t key = 0;
  int region_x = 0, region_z = 0;
  hall_kind kind = hall_kind::pillared;
  point centre;
  point forward{1, 0, 0};
  double half_length = 0, half_width = 0, half_height = 0;
  double floor = 0; // локальная высота плоского дна
  std::vector<hall_pillar> pillars;
  std::vector<hall_ledge> ledges;
  // Коридор существ: капсула вдоль u на высоте core_y, |u| <= core_half.
  double core_radius = 0, core_y = 0, core_half = 0;
  // Горловина: вертикальная шахта от пустого узла обжитого графа до зала.
  uint64_t throat_node = 0;
  point throat_top, throat_end;
  double throat_radius = 0;
};

struct adventure {
  uint64_t seed = 0;
  graph route;
  double clearance_radius = 1.5; // радиус гарантированно проходящей сферы, метры
  std::vector<uint8_t> outpost_layout; // ориентация/сторона, вычисляется один раз на окно
  // Отделка крыш, выбранная вместе с планом по открытой пещере над крышей до
  // монтажа: зона заброшенной базы без центральной панели (0 — крыша цела) и
  // битовая маска застеклённых модулей.
  std::vector<uint8_t> outpost_breach;
  std::vector<uint16_t> outpost_glazing;
  // Залы всех регионов, чьё влияние может дотянуться до окна. Чистая функция от
  // seed и координат региона, поэтому совпадает в любых окнах.
  std::vector<hall> halls;
};

inline constexpr int sector_size = 96;
// Полная высота мира. Граф баз и галерей занимает только обжитой слой
// settled_y_*; выше и ниже лежат большие залы региона.
inline constexpr int world_y_min = -500;
inline constexpr int world_y_max = 500;
inline constexpr int settled_y_min = -40;
inline constexpr int settled_y_max = 40;
inline constexpr double surface_guard = 2.0; // > диагонали клетки 1 м: √3
inline constexpr double starter_sub_length = 17.0; // временный масштаб, нос–корма
inline constexpr double starter_sub_beam = 6.0;   // временный максимальный поперечник
// Корпус — капсула общей длиной 17 м: ось 11 м, радиус 3 м.
inline constexpr double starter_sub_axis_half = (starter_sub_length - starter_sub_beam) / 2.0;
inline constexpr double starter_sub_path_radius = starter_sub_beam / 2.0 + surface_guard;
inline constexpr double starter_sub_turn_radius = starter_sub_length / 2.0 + surface_guard;
// Естественная полость у базы содержит мокрый док 24x24x7 м с запасом под сетку.
inline constexpr double base_cavern_radius = 19.5;
static_assert(surface_guard * surface_guard > 3.0);
// Регион — ячейка уровня залов, 8x8 секторов.
inline constexpr int region_size = 768;
inline constexpr uint32_t min_base_intermediates = 2;
inline constexpr uint32_t max_deadend_nodes = 5;

graph generate_graph(uint64_t seed, graph::window area = {});
site generate_site(uint64_t seed, const node& at, std::vector<portal> ports,
                   double clearance_radius = 1.5);
site materialize_site(const adventure& world, uint32_t node_id);
// Поле одного локального природного места без магистральной пещеры вокруг него.
double local_cave_field(uint64_t seed, const site& local,
                        double clearance_radius, point position);
adventure generate(uint64_t seed, graph::window area = {}, double clearance_radius = 1.5);
// Абсолютное окружение 3x3 сектора вокруг базы: в нём решения по пещере не
// зависят от окна, а края запрошенного окна могут быть обрезаны. Без плана баз.
adventure canonical_context(uint64_t seed, point centre, double clearance_radius);
// Возвращает конечный фрагмент бесконечной по X/Z сети с запасом за дальностью видимости.
adventure generate_for_view(uint64_t seed, point observer, double visibility,
                            double clearance_radius = 1.5);

// Небольшой 3D value noise в [-1,1]; ключ — мировой узел решётки.
double value_noise(uint64_t seed, point p, double period);

// Залы одного региона: форма, коридор существ и горловина в обжитой граф.
// sealed (если задан) получает число залов, отброшенных без безопасной горловины.
std::vector<hall> region_halls(uint64_t seed, int region_x, int region_z,
                               double clearance_radius, uint32_t* sealed = nullptr);
// Поле зала вместе с коридором и горловиной (без природных галерей вокруг).
double hall_field(uint64_t seed, const hall& h, point position);
// Может ли поле зала в области [low,high] опуститься ниже фоновой скалы.
bool hall_reaches(const hall& h, point low, point high);
// Вся область [low,high] заведомо вода: внутри внутреннего эллипсоида, выше дна,
// вдали от колонн и уступов. Точная оценка, без сэмплов поля.
bool hall_contains_air(const hall& h, point low, point high);
// Дальность по вертикали, ниже которой поле зала может быть меньше фоновой скалы.
double hall_vertical_reach(const hall& h);
// Независимая проверка зала: отделение от обжитого слоя, горловина, коридор.
std::string verify_hall(uint64_t seed, const hall& h, double clearance_radius);

// Положительное поле — скала, отрицательное — свободное пространство.
// Чанк берёт только мировые координаты; порядок запросов и место наблюдателя не являются входами.
double density(const adventure& world, point position);
// Поле пещеры ДО установки локальных структур. По нему принимается место дока
// и оценивается, насколько построенный корпус окружён породой.
double cave_density(const adventure& world, point position);

struct chunk {
  static constexpr int cells = 16;
  int x = 0;
  int y = 0;
  int z = 0;
  std::vector<double> samples; // (cells + 1)^3, включая общие грани
  double at(int local_x, int local_y, int local_z) const;
};
chunk generate_chunk(const adventure& world, int x, int y, int z);

struct verification {
  bool ok = false;
  std::string cause;
  uint32_t reached_rooms = 0;
  uint32_t total_rooms = 0;
  uint32_t checked_samples = 0;
  uint32_t max_deadend_nodes = 0;
  uint32_t wide_route_percent = 0; // доля длины рёбер с просторным сечением
  uint32_t halls = 0;
};
verification verify(const adventure& world);
bool write_svg(const adventure& world, const std::string& path);
bool write_site_svg(const adventure& world, uint32_t node_id, const std::string& path);

} // namespace gn09
