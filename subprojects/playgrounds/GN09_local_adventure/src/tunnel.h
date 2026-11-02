#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "station.h"
#include "devils_engine/originator/tools.h"

namespace gn09 {

// Сухой туннель между двумя станциями через толщу породы. Станции строятся каждая
// сама по себе; туннель выбирается после, по их планам, и ничего в них не меняет:
// он вырезает свой объём в скале, а проём в стене станции — это треугольники её
// сетки внутри ноги туннеля, которые не выводятся при сборке чанка.
//
// Пара — взаимно ближайшие станции в пределах tunnel_partner_reach по абсолютным
// координатам (базы в разных рукавах и аванпосты залов тоже). Каждая сторона решает
// в своей канонической окрестности, поэтому любое окно видит тот же туннель.
// Форма: нога из внутреннего помещения сквозь стену, тамбур 3x3 м, наклонные пролёты
// (не круче 25°, пола-ступени пока нет) с площадками на изломах — трасса может уйти
// от стены и нырнуть под пещеру между станциями, — тамбур и нога второй станции.

// Коробка внутренности куска туннеля: центр и ортонормированная рамка, x — вдоль.
struct tunnel_piece {
  point centre, x, y, z;
  double hx = 0, hy = 0, hz = 0;
  uint64_t station_key = 0; // нога: станция, в стену которой она входит; иначе 0
};

struct built_tunnel {
  uint64_t key = 0;
  uint64_t first_key = 0, second_key = 0; // ключи якорей, first < second
  std::vector<tunnel_piece> pieces;        // нога, тамбур, пролёты с площадками, тамбур, нога
  std::vector<station_triangle> mesh;
  point low, high;
  double span = 0, slope_deg = 0; // длина трассы между тамбурами и самый крутой пролёт
};


// Туннель, который входит в станцию этого якоря, или nullptr: экземпляр генератора tunnel цепочки.
std::shared_ptr<const built_tunnel> tunnel_for(const adventure& world, const station_anchor& anchor);
// Туннель станции по ключу её экземпляра (tunnel_link якоря, затем tunnel первого якоря); nullptr — нет.
std::shared_ptr<const built_tunnel> tunnel_of_station(devils_engine::originator::generator_chain& chain, uint64_t seed,
                                                     const devils_engine::originator::instance_key& station);
// Туннель из экземпляра генератора tunnel (разобранный вид живёт с экземпляром); nullptr — не построен.
std::shared_ptr<const built_tunnel> tunnel_view(const devils_engine::originator::generator_result& instance);
// Все туннели станций окна, без повторов.
std::vector<std::shared_ptr<const built_tunnel>> tunnels_near(const adventure& world, point low, point high);
bool tunnel_reaches(const built_tunnel& tunnel, point low, point high);
// Вырез скалы: внутренность кусков с тем же запасом, что у станции. Отрицательно внутри.
double tunnel_carve_field(const built_tunnel& tunnel, point position);
// Точка строго внутри ноги туннеля, входящей в станцию station_key: треугольники
// станции здесь — проём, их не выводят.
bool in_tunnel_doorway(const built_tunnel& tunnel, uint64_t station_key, point position);
// Независимая проверка по итоговому полю: внутренность вырезана, вокруг на 1.5 м порода.
// with_sites — проверять ещё и против пещер мест (мягкий счёт вскрытий; туннель их не ждёт).
std::string verify_tunnel(const adventure& world, const built_tunnel& tunnel, bool with_sites = false);

} // namespace gn09
