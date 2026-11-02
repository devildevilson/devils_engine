#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "adventure.h"
#include "devils_engine/originator/generator_chain.h"
#include "devils_engine/originator/tools.h"

namespace gn09 {

// Станция — три вложенных объёма: тело в породе (внутренние помещения за линией заведомой породы),
// наружная часть перед ней, и шлюз на конце вытянутого коридора («хобота») у мокрого дока в заведомой
// воде полости базы. План растёт графом помещений, геометрия — воксели 0.5 м с четвертями клетки.
//
// Система координат станции: u — горизонтальная ось «в породу» (поворот yaw вокруг Y,
// любой угол), v — вправо, y — вверх от уровня пола. Клетка плана — 0.5 м, клетка (i,k)
// занимает u in [i/2, (i+1)/2), v in [k/2, (k+1)/2).

enum class station_pose : uint8_t {
  wall,    // станция врезана в стену, лодка у бокового люка
  floor,   // станция стоит на дне, лодка сверху у люка в крыше
  ceiling, // станция прикреплена к своду, лодка снизу у люка в полу
  ledge    // аванпост зала на уступе: под полом уступ, лодка сверху у люка в крыше
};
// Люк в крыше: лодка над шлюзом.
inline bool roof_hatch(const station_pose pose) { return pose == station_pose::floor || pose == station_pose::ledge; }
const char* name(station_pose value);

inline constexpr int32_t station_interior_layers = 6; // 3 м между полом и потолком
inline constexpr int32_t station_plinth_layers = 6;   // фундамент/подвес до 3 м
// Оттенки построенного (>= 112 в знаковом байте вершины): сталь, плиты, люк, стекло.
inline constexpr uint8_t station_glass_shade = 113;

struct station_site {
  station_pose pose = station_pose::wall;
  point origin;       // опорная точка люка; y — уровень пола станции (верх плиты)
  double yaw = 0.0;   // ось u = (cos yaw, 0, sin yaw)
  int32_t outer_cells = 0; // боковая поза: шлюз выступает из стены на столько клеток
  point node_centre;  // поворотный узел базы: отсюда лодка идёт к причалу
  point berth;        // центр лодки у люка
  point berth_axis;   // ось лодки, горизонтальная
  double tilt_deg = 0.0, residual = 0.0, reach = 0.0; // замер опоры по лучам
  double rock_distance = 0.0; // длина коридора шлюза: от шлюза до линии породы вдоль u
};

point station_to_world(const station_site& site, double u, double y, double v);

// Чем станция привязана к миру. У базы графа это её узел; у аванпоста зала —
// уступ у стены: узла графа там нет, лодка подходит через открытую воду зала.
// Все поля — чистая функция seed и постоянного ключа, поэтому якорь один в любом окне.
struct station_anchor {
  uint64_t key = 0;     // ключ узла базы или ключ аванпоста зала
  point centre;         // точка разворота лодки перед причалом
  theme kind = theme::outpost;
  site_recipe recipe = site_recipe::residential_base;
  int8_t preferred = 0; // поза по рецепту: 0 стена, -1 дно, +1 свод
  uint64_t hall_key = 0; // 0 — база графа, иначе зал, где стоит аванпост
};
// Якорь базы графа (узел должен быть базой).
station_anchor anchor_of(const adventure& world, uint32_t node_id);
// Аванпосты зала: на некоторых широких уступах, лодка разворачивается над уступом.
std::vector<station_anchor> hall_station_anchors(uint64_t seed, const hall& h);
// Все якоря окна: базы графа и аванпосты залов окна.
std::vector<station_anchor> station_anchors(const adventure& world);

struct station_room {
  int32_t u0 = 0, v0 = 0, u1 = 0, v1 = 0; // внутренность в клетках плана [u0,u1)x[v0,v1)
  // Срез угла под 45° на столько клеток: углы (u0,v0), (u1,v0), (u1,v1), (u0,v1).
  uint8_t chamfer[4] = {0, 0, 0, 0};
  std::string purpose;
  bool inner = false; // по программе внутренняя часть (склады, койки)
  uint32_t parent = UINT32_MAX;
};
// Проём — клетки стены между двумя помещениями: линия толщиной в одну клетку.
struct station_door {
  uint32_t a = 0, b = 0;
  int32_t u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};
struct station_plan {
  station_site site;
  theme kind = theme::outpost;
  std::vector<station_room> rooms; // 0 — шлюз
  std::vector<station_door> doors;
  // Люк: клетки стены шлюза (боковая поза) или плиты крыши/пола (вертикальные).
  int32_t hatch_u0 = 0, hatch_v0 = 0, hatch_u1 = 0, hatch_v1 = 0;
  // Своя порода: блок в локальных осях станции (u, y, v в метрах), без скругления и неровности —
  // внутри него поле заведомо порода (неровность и шов только добавляют её).
  point rock_low, rock_high;
};

// Четверти клетки плана (i,k) внутри помещения со срезанными углами (0xF — целая).
uint8_t station_room_quarters(const station_room& room, int32_t i, int32_t k);
// Независимая проверка плана по прямоугольникам и по полю пещеры.
// with_sites — проверять ещё и против пещер мест (мягкий счёт вскрытий; станция их не ждёт).
std::string verify_station_plan(const adventure& world, const station_anchor& anchor,
                                const station_plan& plan, bool with_sites = false);
std::string verify_station_plan(const adventure& world, uint32_t node_id,
                                const station_plan& plan);

// Воксели станции: на клетку плана и слой высоты 0.5 м — маска четвертей клетки
// (бит 0: -u, 1: +v, 2: +u, 3: -v; четверть — треугольник от центра к стороне).
struct station_voxels {
  int32_t i0 = 0, k0 = 0, y0 = 0; // y — слой относительно пола: слой 0 = [пол, пол+0.5)
  int32_t width = 0, depth = 0, height = 0;
  std::vector<uint8_t> solid;  // четверти твёрдой постройки
  std::vector<uint8_t> shade;  // материал твёрдых клеток
  uint8_t at(int32_t i, int32_t y, int32_t k) const;
};
// field — поле пещеры до монтажа над коробкой плана (фундамент и подвес доходят до породы, стекло — где за
// скосом вода).
station_voxels build_station_voxels(const std::function<double(point)>& field, const station_plan& plan);

struct station_triangle {
  point a, b, c; // мир
  point normal;
  uint8_t shade = 0;
};
std::vector<station_triangle> station_mesh(const station_plan& plan, const station_voxels& voxels);

// Станция, готовая для мира: план, воксели, сетка и мировая коробка всего, что она
// строит и вырезает. Выбирается один раз в канонической окрестности базы.
struct built_station {
  station_anchor anchor;
  uint32_t node_key_low = 0; // младшие биты ключа узла, для отладки
  station_plan plan;
  station_voxels voxels;
  std::vector<station_triangle> mesh;
  point low, high;
};
// Порода блока станции в точке: +1 внутри коробки блока (локальные оси), -1 снаружи.
double station_rock_core(const station_site& site, point rock_low, point rock_high, point position);
// Станция в мире — экземпляр генератора station цепочки (ключ — сектор якоря и его ключ), решённый в
// канонической окрестности якоря; кэш — кэш цепочки. nullptr — не база или места нет.
devils_engine::originator::instance_key station_key(const station_anchor& anchor);
// Станция из экземпляра генератора station (разобранный вид живёт с экземпляром); nullptr — не построена.
std::shared_ptr<const built_station> station_view(const devils_engine::originator::generator_result& instance);
std::shared_ptr<const devils_engine::originator::generator_result> station_instance(const adventure& world,
                                                                                    const station_anchor& anchor);
std::shared_ptr<const built_station> station_for(const adventure& world, const station_anchor& anchor);
std::shared_ptr<const built_station> station_for(const adventure& world, uint32_t node_id);
// Вся станция (вырез, сетка, путь к причалу) лежит в кубе этого полуразмера вокруг узла.

// Вырез скалы под станцию: внутренности помещений, проёмы и люк с запасом
// carve_margin за внутренней гранью стены, и путь лодки от узла к причалу —
// капсула корпуса с охранным слоем сетки, протащенная параллельно (как маршрут).
// Отрицательно внутри выреза.
inline constexpr double station_carve_margin = 0.75;
double station_carve_field(const built_station& station, point position);
// Только путь лодки к причалу (та же капсула, что в вырезе).
double berth_sweep_field(const station_site& site, point position);
bool station_reaches(const built_station& station, point low, point high);
// Треугольники сетки, обрезанные по коробке [low, high).
void clip_station_mesh(const built_station& station, point low, point high,
                       const std::function<void(const point*, size_t, point normal, uint8_t)>& polygon);
// То же для любого списка треугольников; skip отбрасывает треугольник целиком.
void clip_triangles(const std::vector<station_triangle>& mesh, point low, point high,
                    const std::function<void(const point*, size_t, point normal, uint8_t)>& polygon,
                    const std::function<bool(const station_triangle&)>& skip = {});
// Точка строго внутри помещения станции (план со срезами углов, между полом и потолком).
bool station_interior_contains(const built_station& station, point position);

bool write_station_svg(const adventure& world, uint32_t node_id, const std::string& path);
// Словарь назначений помещений станции (значения генератора station).
const std::vector<std::string>& station_purposes();

} // namespace gn09
