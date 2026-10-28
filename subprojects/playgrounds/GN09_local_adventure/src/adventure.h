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

struct adventure {
  uint64_t seed = 0;
  graph route;
  double clearance_radius = 1.5; // радиус гарантированно проходящей сферы, метры
  std::vector<uint8_t> outpost_layout; // ориентация/сторона, вычисляется один раз на окно
};

inline constexpr int sector_size = 96;
inline constexpr int world_y_min = -40;
inline constexpr int world_y_max = 40;
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
// Возвращает конечный фрагмент бесконечной по X/Z сети с запасом за дальностью видимости.
adventure generate_for_view(uint64_t seed, point observer, double visibility,
                            double clearance_radius = 1.5);

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
};
verification verify(const adventure& world);
bool write_svg(const adventure& world, const std::string& path);
bool write_site_svg(const adventure& world, uint32_t node_id, const std::string& path);

} // namespace gn09
