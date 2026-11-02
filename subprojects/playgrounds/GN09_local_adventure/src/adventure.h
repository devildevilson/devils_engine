#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
  double squash = 1.0;  // вертикальный масштаб камеры: расширенный зал ниже, чем шире
  double stretch = 1.0; // вытянутость камеры вдоль её курса yaw
  double yaw = 0.0;
};

struct room_link {
  uint32_t a = 0;
  uint32_t b = 0;
  // Пещера: уменьшенная галерея вокруг отрезка — полуширина, свод, пол и раздутие
  // в доле bulge_at. width = 0 — только гарантированное ядро.
  double width = 0.0, roof = 0.0, floor = 0.0, bulge = 0.0, bulge_at = 0.5;
};

// Место на узле маршрута. У базы это программа помещений станции; у природного
// места — пещера для ныряльщиков (cave.h): устье в стене полости узла, кишки и
// расширенные залы, тупики с залежами, гнездо за изгибами.
struct site {
  uint32_t node_id = 0;
  theme kind = theme::grotto;
  site_recipe recipe = site_recipe::ore_cave;
  point centre;
  std::vector<portal> portals;
  std::vector<room> rooms;
  std::vector<room_link> links;
  // Природная пещера: комната устья, гнездо, горловины перед залежами (будущие
  // ледяные пробки) и коробка скелета (для рисунка).
  bool has_cave = false;
  uint32_t mouth = UINT32_MAX, nest = UINT32_MAX, lair = UINT32_MAX; // lair — первая встреча с монстрами
  std::vector<uint32_t> entrances; // устье и вторые входы
  uint32_t pillars = 0;            // колонны в крупных нишах (формы cave_pillar)
  std::vector<uint32_t> plugs;
  // N — радиосвязь (для отчёта), вид логова: 0 ниша, 2 тупик; масштаб ходов.
  double radio_target = 0.0, scale = 0.0;
  uint8_t lair_kind = 0;
  // Бюджет заплыва: залежей, сбор одной (с), весь заплыв (с), обход (м) и оценка времени (с).
  uint32_t deposits = 0;
  double collect = 0.0, budget = 0.0, tour = 0.0, estimate = 0.0;
  // Узкая часть горла и её ядро; blocks_sub — ядро уже корпуса лодки.
  point throat_a, throat_b;
  double throat_core = 0.0;
  bool blocks_sub = false;
  point low, high;
};

struct edge_pocket {
  point mouth;
  point centre;
  double radius = 0;
};
// Небольшой декоративный карман привязан к ребру, не создавая новый узел графа.
std::optional<edge_pocket> materialize_edge_pocket(uint64_t seed,
                                                    const node& a, const node& b);

// Большой зал регионального уровня. Он не ребро графа, а объём пещеры на высоте
// слоя: сектора, чья база попала бы в зал, получают вместо базы узел внутри зала,
// и обычные маршруты соседей входят в него через стены. В зале есть гарантированный
// коридор для крупных существ и укрытия у стен. Координаты зала локальные: u вдоль
// forward, y вверх, v вправо.
enum class hall_kind : uint8_t { pillared, lair };
const char* name(hall_kind value);

struct hall_pillar {
  double u = 0, v = 0, radius = 0; // у пола и потолка шире: radius * (1 + 0.35 (y/H)^2)
};

struct hall_ledge {
  point centre; // локальная коробка уступа, частью уходящая в стену
  point half;
};

// Укрытие: ниша с коленом. Вход идёт по нормали в стену, в колене помещается
// поворот лодки, дальше отвод вдоль стены — его дно с коридора существ закрыто
// скалой. У части ниш над входом навес.
struct hall_niche {
  point mouth, elbow, end; // локальные точки оси
  double radius = 0;
  bool overhang = false;
};

// Обитатель зала. Зверь крупный, но его можно одолеть; повелителя глубин убить
// нельзя, только отогнать. В зале без обитателя петля остаётся путём для
// проплывающих мимо крупных существ.
enum class creature_class : uint8_t { none, beast, lord };
const char* name(creature_class value);

struct hall {
  uint64_t key = 0;
  int region_x = 0, region_z = 0;
  hall_kind kind = hall_kind::pillared;
  point centre;
  point forward{1, 0, 0};
  double half_length = 0, half_width = 0, half_height = 0;
  point relevance; // полуразмеры области влияния, рассчитаны генератором halls
  double floor = 0; // локальная высота плоского дна
  std::vector<hall_pillar> pillars;
  std::vector<hall_ledge> ledges;
  std::vector<hall_niche> niches;
  // Петля патруля: замкнутая ломаная в локальных координатах, вырезанная под тело
  // радиуса body_radius с охранным слоем сетки. Обходит колонны и уступы.
  creature_class resident = creature_class::none;
  double body_radius = 0;
  std::vector<point> loop;
};

// Станции вдоль петли (локальные точки, шаг около 20 м): с них проверяется, что дно
// ниши-укрытия не видно существу на петле.
std::vector<point> loop_stations(const hall& h);

// Запись присутствия крупного существа в окрестности зала. Это мета для ИИ и
// режиссуры, а не геометрия: она читает зал и граф маршрутов и ничего не вырезает.
struct creature_presence {
  uint64_t hall_key = 0;
  creature_class resident = creature_class::none;
  double body_radius = 0;
  // Домен — где существу разрешено двигаться: узлы залов домена (сам зал и ярус
  // стопки, не больше трёх залов) и проходы маршрута, в которые тело помещается,
  // не дальше четырёх рёбер от зала. Рёбра к базам в домен не входят: стены станции
  // не входят в поле плотности.
  std::vector<uint64_t> halls;
  std::vector<uint64_t> nodes;
  std::vector<std::pair<uint64_t, uint64_t>> passages;
  // Внешние концы входов в зал: между ними есть путь в обход домена.
  std::vector<uint64_t> refuges;
};

struct adventure {
  uint64_t seed = 0;
  graph route;
  double clearance_radius = 1.0; // радиус гарантированно проходящей сферы (ныряльщик), метры
  // Станции баз не хранятся в окне: station_for (station.h) строит их по
  // канонической окрестности базы при первом запросе, поэтому любое окно получает
  // ту же станцию, а далёкие базы ничего не стоят.
  // Залы всех регионов, чьё влияние может дотянуться до окна. Чистая функция от
  // seed и координат региона, поэтому совпадает в любых окнах.
  std::vector<hall> halls;
  // Ключ экземпляра окна, из которого собран этот мир (world_view): хостовый путь поля берёт из него
  // строки видов, объявленных данными (shapes).
  std::array<uint64_t, 3> window_key{};
  bool from_window = false;
};

inline constexpr uint32_t min_base_intermediates = 2;
inline constexpr uint32_t max_deadend_nodes = 5;

// Параметры мира читаются из generator/constants.tavl после выбора модов.
int sector_size();
int region_size();
int world_y_min();
int world_y_max();
double surface_guard();
double starter_sub_length();
double starter_sub_beam();
double starter_sub_axis_half();
double starter_sub_path_radius();
double starter_sub_turn_radius();
double macro_scale();
double base_cavern_radius();
double cave_extent();
double cave_reach();
double cave_hull_radius();
double station_reach();
double tunnel_partner_reach();
double tunnel_max_slope_deg();
double station_cell();

// Макрограф окна — экземпляр генератора `graph` цепочки generator/world (world_chain.h): правила баз и
// маршрутов живут в скриптах resources/gn09/generator/macro.
graph generate_graph(uint64_t seed, graph::window area = {});
// Место базы: программа помещений (природные места строит cave.h).
site generate_site(uint64_t seed, const node& at, std::vector<portal> ports,
                   double clearance_radius = 1.0);
// Место узла: база — сразу, природное — пещера из общего кэша (cave_for).
site materialize_site(const adventure& world, uint32_t node_id);
adventure generate(uint64_t seed, graph::window area = {}, double clearance_radius = 1.0);
// Абсолютное окружение 3x3 сектора вокруг базы: в нём решения по пещере не
// зависят от окна, а края запрошенного окна могут быть обрезаны. Без плана баз.
adventure canonical_context(uint64_t seed, point centre, double clearance_radius);
// Сектор, в котором лежит точка, и та же окрестность по сектору (её ключ в цепочке генераторов).
std::array<int, 2> sector_of(point centre);
adventure sector_context(uint64_t seed, int sx, int sz, double clearance_radius);
// Возвращает конечный фрагмент бесконечной по X/Z сети с запасом за дальностью видимости.
adventure generate_for_view(uint64_t seed, point observer, double visibility,
                            double clearance_radius = 1.0);

// Небольшой 3D value noise в [-1,1]; ключ — мировой узел решётки.
double value_noise(uint64_t seed, point p, double period);
// Высота пещерной сети над точкой X/Z: крупный рельеф слоя, |h| <= 320 м. Базы,
// узлы маршрутов и залы ставятся относительно него.
double layer_height(uint64_t seed, double x, double z);

// Залы одного региона: форма, коридор существ и горловина в обжитой граф.
// Залы одного региона — чистая функция seed и координат региона.
std::vector<hall> region_halls(uint64_t seed, int region_x, int region_z);
// Залы всех регионов, чьё поле может дотянуться до окна секторов.
std::vector<hall> halls_near(uint64_t seed, graph::window area);
// Точка лежит внутри номинального эллипсоида зала (без рельефа) и выше дна.
bool inside_hall(const hall& h, point position);
// Локальные координаты зала (u вдоль forward, y, v вправо) в мировые и обратно.
point hall_to_world(const hall& h, point local);
point hall_to_local(const hall& h, point world);
// Поле зала вместе с коридором и нишами (без природных галерей вокруг).
double hall_field(uint64_t seed, const hall& h, point position);
// Может ли поле зала в области [low,high] опуститься ниже фоновой скалы.
bool hall_reaches(const hall& h, point low, point high);
// Коробка, вне которой поле зала больше фоновой скалы.
void hall_influence(const hall& h, point& low, point& high);
// Вся область [low,high] заведомо вода: внутри внутреннего эллипсоида, выше дна,
// вдали от колонн и уступов. Точная оценка, без сэмплов поля.
bool hall_contains_air(const hall& h, point low, point high);
// Дальность по вертикали, ниже которой поле зала может быть меньше фоновой скалы.
double hall_vertical_reach(const hall& h);
// Входы маршрута: рёбра графа, у которых один конец внутри зала, другой снаружи
// (концы {снаружи, внутри}). Граф строится по окну, накрывающему зал.
std::vector<std::pair<point, point>> hall_entrances(uint64_t seed, const hall& h);
// Независимая проверка зала: высота мира, входы маршрута, петля, ниши-укрытия.
std::string verify_hall(uint64_t seed, const hall& h, double clearance_radius);
// Присутствие существа у зала: домен и обход (граф окна вокруг зала, кэш на поток).
creature_presence creature_of(uint64_t seed, const hall& h);
// Независимая проверка присутствия: тело помещается в каждом проходе домена, домен
// связан и ограничен, а входы зала связаны обходом вне домена.
std::string verify_creature(uint64_t seed, const hall& h, const creature_presence& presence);

// Порода по виду полости, к которой примыкает поверхность: слагаемое минимума поля,
// давшее значение в точке. Номер уходит оттенком вершины (знаковый байт 0..111;
// от 112 — построенное), просмотрщик красит породу по таблице.
enum class rock_kind : uint8_t {
  background, karst, rift, chimney, base_cavern, hall,
  ore_cave, nest_cave, thermal_cave, pocket, station_cut
};
const char* name(rock_kind value);

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
  uint32_t bases_without_station = 0; // место для станции не нашлось
  uint32_t hall_stations = 0; // построенные аванпосты залов
  uint32_t tunnels = 0;       // сухие туннели между станциями
  uint32_t caves = 0, sites_without_cave = 0; // пещеры природных мест
  // Мягкие нарушения пещер: логово мельче, цель мельче, гнездо видно из лодки, стена
  // вскрыта в чужую воду (см. cave_check).
  std::array<uint32_t, 4> cave_soft{};
  // Станции, чью внутреннюю комнату вскрыла пещера места (мягко: станции пещер не ждут).
  uint32_t stations = 0, stations_breached = 0, tunnels_breached = 0;
};
verification verify(const adventure& world);
struct built_station;
// Станция в канонической окрестности своего якоря: план, вырез помещений, подход лодки.
std::string verify_built_station(const adventure& world, const built_station& station);
bool write_svg(const adventure& world, const std::string& path);
bool write_site_svg(const adventure& world, uint32_t node_id, const std::string& path);

} // namespace gn09
