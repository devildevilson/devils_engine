#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "adventure.h"
#include "station.h"
#include "devils_engine/originator/primitive_field.h"

namespace gn09 {

struct built_tunnel;

// Руда, кристалл или кладка: локальный объект, не отдельный узел маршрута.
struct local_feature {
  uint32_t room = 0;
  point centre;
  std::string kind;
  uint32_t amount = 0;
};

// Наполнение природного места. Базы строятся станциями (station.h).
struct local_structure {
  uint32_t node_id = 0;
  theme kind = theme::empty;
  site_recipe recipe = site_recipe::ore_cave;
  point centre;
  std::vector<local_feature> features;
};

local_structure materialize_local_structure(const adventure& world, uint32_t node_id);
// Поза станции, которую рецепт и биом предпочитают: 0 — у стены, +1 — над лодкой
// (под сводом), -1 — под лодкой (на дне).
int8_t preferred_dock_axis(uint64_t seed, const node& at);
// Проверяет привязку залежей природного места независимо от генератора.
std::string verify_local_structure(const adventure& world, const site& local,
                                   const local_structure& structure);
// План природного места или станции базы.
bool write_structure_svg(const adventure& world, uint32_t node_id, const std::string& path);

// Постоянная для ребра часть формы галереи: концы, рамка, ширины и две боковые
// полости. Считается один раз на область вместо каждой точки поля.
struct edge_shape {
  struct lobe {
    point centre;
    double axial = 0, lateral = 0, vertical = 0;
  };
  point a, b, axis, right, up;
  cave_biome biome = cave_biome::karst;
  double distance = 0, core_radius = 0;
  double reach = 24; // за ядро + reach поле ребра больше фоновой скалы
  double vertical_gain = 0, biome_width = 1;
  double first_t = 0, second_t = 0;
  double base_width = 0, first_width = 0, second_width = 0, base_roof = 0;
  bool constriction = false;
  lobe lobes[2];
};
edge_shape shape_edge(const adventure& world, const node& from, const node& to);

// Поле, подготовленное для одной прямоугольной области (обычно чанка). Из окна
// оставлены только рёбра, узлы, места и базы, чьё влияние может дотянуться до
// области; их строки собраны в поле библиотеки (originator::primitive_field) со
// словарём форм world_shapes — та же формула, что у поля из буферов цепочки.
// Отброшенное слагаемое минимума в области заведомо больше фоновой скалы
// (3 + неровность <= 3.46), поэтому значение побитово равно полю всего окна.
struct field_region {
  const adventure* world = nullptr;
  point low, high;
  bool structures = false;
  std::vector<uint32_t> edges;
  std::vector<uint32_t> nodes;
  std::vector<uint32_t> halls; // индексы adventure::halls
  size_t sites = 0, stations = 0, tunnels = 0;
  // Поле области из словаря форм (world_shapes) и подготовленная по её коробке выборка.
  std::shared_ptr<const devils_engine::originator::primitive_field> field;
  std::shared_ptr<const devils_engine::originator::primitive_field::region> view;
};
// Станции и туннели, касающиеся коробки, для поля, собранного хостом (prepare_field). Чанк цепочки
// берёт их fan-in-ом (chunk_anchors, chunk_tunnels) — того же отбора надмножество.
struct structures {
  std::vector<std::shared_ptr<const built_station>> stations;
  std::vector<std::shared_ptr<const built_tunnel>> tunnels;
};
structures structures_near(const adventure& world, point low, point high);

// with_structures = false готовит только природную пещеру для cave_density;
// with_sites = false — ещё и без пещер природных мест: по этому полю место
// выбирает свою пещеру (cave.h), не завися от других мест.
field_region prepare_field(const adventure& world, point low, point high,
                           bool with_structures = true, bool with_sites = true);
// Чанк вместе с соседним слоем сэмплов, нужным поверхности для градиентов.
field_region prepare_chunk_field(const adventure& world, int x, int y, int z);
double cave_density(const field_region& region, point position);
// Вид полости, чьё слагаемое даёт минимум поля в точке (с вырезом станций).
rock_kind rock_at(const field_region& region, point position);
// +1 — вся область сплошная скала (до неё не дотягивается ни одно слагаемое),
// -1 — вся область вода внутри зала, 0 — неизвестно, нужны сэмплы. Оценка точная:
// у однородного чанка все сэмплы имеют этот знак, и поверхности в нём нет.
int uniform_sign(const field_region& region);
double density(const field_region& region, point position);
chunk generate_chunk(const field_region& region, int x, int y, int z);

// Чанк из цепочки (генератор chunk_field, ключ — номер чанка): сэмплы его решётки и область поля
// `solid` для запросов между узлами (поверхность). Окно чанка — 5x5 секторов вокруг его сектора;
// экземпляры держат и поле, и мир окна (region.world).
struct chunk_instance {
  chunk volume;
  field_region region;
  std::shared_ptr<const devils_engine::originator::generator_result> instance, window;
};
chunk_instance chunk_from_chain(uint64_t seed, double clearance_radius, int x, int y, int z);

// Поле по точкам подряд: одна подготовленная область (куб со стороной 2 * half)
// обслуживает соседние точки, а точка вне неё пересобирает область вокруг себя.
// Значения те же, что у поточечного вызова, — область лишь отсекает слагаемые,
// которые до неё заведомо не дотягиваются.
class field_cache {
public:
  field_cache(const adventure& world, bool structures, double half = 16.0, bool sites = true)
    : world_(&world), structures_(structures), sites_(sites), half_(half) {}
  double operator()(point position);
private:
  const adventure* world_;
  bool structures_, sites_;
  double half_;
  std::optional<field_region> region_;
};

} // namespace gn09
