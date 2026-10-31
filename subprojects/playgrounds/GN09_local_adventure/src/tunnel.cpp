#include "tunnel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <cstdio>
#include <cstdlib>

#include "local_structure.h"
#include "world_chain.h"
#include "devils_engine/originator/tools.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/prng.h"

namespace gn09 {
namespace {

point add(const point a, const point b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
point subtract(const point a, const point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
point scale(const point a, const double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const point a, const point b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
point cross(const point a, const point b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

constexpr point up{0, 1, 0};
constexpr double half_height = 1.25;  // внутренность 2.5 м
constexpr double half_width = 1.0;    // проём 2 м = 4 клетки плана
constexpr double leg_inset = 0.25;    // нога заходит в помещение, чтобы проём был целым
constexpr double leg_out = 3.5;       // от внутренней грани стены наружу, с заходом в тамбур
constexpr double chamber_half = 1.5;  // тамбур 3x3 м
constexpr double chamber_offset = 4.5; // центр тамбура от внутренней грани стены
constexpr double door_zone = 1.25;    // у стены порода принадлежит вырезу станции
constexpr double rock_ring = 1.5;     // порода вокруг внутренности
constexpr double carve_margin = station_carve_margin;

point piece_local(const tunnel_piece& piece, const point p) {
  const point d = subtract(p, piece.centre);
  return {dot(d, piece.x), dot(d, piece.y), dot(d, piece.z)};
}

point piece_world(const tunnel_piece& piece, const double lx, const double ly, const double lz) {
  return add(piece.centre, add(add(scale(piece.x, lx), scale(piece.y, ly)), scale(piece.z, lz)));
}

bool piece_contains(const tunnel_piece& piece, const point p) {
  constexpr double inset = 1e-4;
  const point l = piece_local(piece, p);
  return std::abs(l.x) < piece.hx - inset && std::abs(l.y) < piece.hy - inset &&
         std::abs(l.z) < piece.hz - inset;
}

// Знаковое расстояние до коробки внутренности.
double piece_field(const tunnel_piece& piece, const point p) {
  const point l = piece_local(piece, p);
  const double qx = std::abs(l.x) - piece.hx, qy = std::abs(l.y) - piece.hy, qz = std::abs(l.z) - piece.hz;
  const double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0), oz = std::max(qz, 0.0);
  return std::sqrt(ox * ox + oy * oy + oz * oz) + std::min(std::max({qx, qy, qz}), 0.0);
}

// Расстояние вдоль ноги от внутренней грани стены станции.
double leg_depth(const tunnel_piece& leg, const point p) {
  return piece_local(leg, p).x + leg.hx - leg_inset;
}

// Плитки граней внутренности: шаг до 0.5 м, у ноги — разрез ровно по грани стены.
std::vector<double> breaks(const double lo, const double hi, const double first) {
  std::vector<double> out{lo};
  double at = lo;
  if (first > 0.0) out.push_back(at = lo + first);
  const int count = std::max(1, int(std::ceil((hi - at) / 0.5 - 1e-9)));
  for (int i = 1; i <= count; ++i) out.push_back(at + (hi - at) * i / count);
  return out;
}

std::vector<station_triangle> tunnel_mesh(const std::vector<tunnel_piece>& pieces,
                                          const built_station& first, const built_station& second) {
  const bool derelict = first.plan.kind == theme::derelict || second.plan.kind == theme::derelict;
  const uint8_t wall_shade = derelict ? 115 : 123, slab_shade = derelict ? 112 : 119;
  std::vector<station_triangle> mesh;
  for (size_t k = 0; k < pieces.size(); ++k) {
    const tunnel_piece& piece = pieces[k];
    const double h[3] = {piece.hx, piece.hy, piece.hz};
    const point axes[3] = {piece.x, piece.y, piece.z};
    for (uint32_t a = 0; a < 3; ++a) for (const double side : {-1.0, 1.0}) {
      const uint32_t b = (a + 1) % 3, c = (a + 2) % 3;
      const double first_b = piece.station_key && b == 0 ? leg_inset : 0.0;
      const double first_c = piece.station_key && c == 0 ? leg_inset : 0.0;
      const std::vector<double> bs = breaks(-h[b], h[b], first_b), cs = breaks(-h[c], h[c], first_c);
      const point normal = scale(axes[a], -side); // внутрь туннеля
      const uint8_t shade = a == 1 ? slab_shade : wall_shade;
      for (size_t i = 0; i + 1 < bs.size(); ++i) for (size_t j = 0; j + 1 < cs.size(); ++j) {
        const auto corner = [&](const double vb, const double vc) {
          double l[3];
          l[a] = side * h[a]; l[b] = vb; l[c] = vc;
          return piece_world(piece, l[0], l[1], l[2]);
        };
        const point p0 = corner(bs[i], cs[j]), p1 = corner(bs[i + 1], cs[j]);
        const point p2 = corner(bs[i + 1], cs[j + 1]), p3 = corner(bs[i], cs[j + 1]);
        const point middle = scale(add(add(p0, p1), add(p2, p3)), 0.25);
        bool hidden = station_interior_contains(first, middle) || station_interior_contains(second, middle);
        for (size_t other = 0; other < pieces.size() && !hidden; ++other) {
          hidden = other != k && piece_contains(pieces[other], middle);
        }
        if (hidden) continue;
        const auto emit = [&](point x, point y, point z) {
          if (dot(cross(subtract(y, x), subtract(z, x)), normal) < 0.0) std::swap(y, z);
          mesh.push_back({x, y, z, normal, shade});
        };
        emit(p0, p1, p2);
        emit(p0, p2, p3);
      }
    }
  }
  return mesh;
}

namespace originator = devils_engine::originator;

point read3(const originator::const_field_accessor& f, const size_t i) { return {f.get(i, 0), f.get(i, 1), f.get(i, 2)}; }
void write3(const originator::field_accessor& f, const size_t i, const point p) {
  f.set(i, p.x, 0); f.set(i, p.y, 1); f.set(i, p.z, 2);
}

// Блок генератора tunnel: сетка и коробка готовых кусков. Станции пары — экземпляры генератора station по
// их ключам (вход 6), зерно мира — вход 7: проём в стене — треугольники станции внутри ноги, а грани
// туннеля внутри помещений станций не выводятся.
void tool_tunnel_build(const originator::tool_call& call, const size_t, const size_t) {
  std::vector<tunnel_piece> pieces;
  const auto centre = call.input(0).read(), x = call.input(1).read(), y = call.input(2).read(), z = call.input(3).read(),
             half = call.input(4).read(), station = call.input(5).read();
  for (int64_t i = 0; i < call.params->integer("pieces"); ++i) {
    const size_t at = size_t(i);
    tunnel_piece piece;
    piece.centre = read3(centre, at); piece.x = read3(x, at); piece.y = read3(y, at); piece.z = read3(z, at);
    const point h = read3(half, at);
    piece.hx = h.x; piece.hy = h.y; piece.hz = h.z;
    piece.station_key = originator::read_key_word(station, at);
    pieces.push_back(piece);
  }
  const auto pair = call.input(6).read();
  const uint64_t seed = originator::read_key_word(call.input(7).read(), 0);
  auto& chain = world_chain(call.params->number("clearance"));
  const auto view = [&](const size_t row) {
    const originator::instance_key key{{originator::read_key_word(pair, row, 0), originator::read_key_word(pair, row, 1),
                                        originator::read_key_word(pair, row, 2)}};
    return station_view(*chain.get("station", seed, key));
  };
  const auto a = view(0), b = view(1);
  if (!a || !b) devils_engine::utils::error{}("GN09 tunnel_build: a station of the pair is not built");
  const std::vector<station_triangle> mesh = tunnel_mesh(pieces, *a, *b);
  point low{1e30, 1e30, 1e30}, high{-1e30, -1e30, -1e30};
  for (const tunnel_piece& piece : pieces) {
    for (const double sx : {-1.0, 1.0}) for (const double sy : {-1.0, 1.0}) for (const double sz : {-1.0, 1.0}) {
      const point p = piece_world(piece, sx * piece.hx, sy * piece.hy, sz * piece.hz);
      low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
      high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
  }
  // За коробкой вырез больше фоновой скалы (3 + 0.46): отбросить его — точно.
  constexpr double margin = 5.0;
  const auto ma = call.output(4).write(), mb = call.output(5).write(), mc = call.output(6).write(),
             mn = call.output(7).write(), ms = call.output(8).write();
  if (mesh.size() > ma.count()) devils_engine::utils::error{}("GN09 tunnel_build: {} triangles exceed {}", mesh.size(), ma.count());
  for (size_t i = 0; i < mesh.size(); ++i) {
    write3(ma, i, mesh[i].a); write3(mb, i, mesh[i].b); write3(mc, i, mesh[i].c); write3(mn, i, mesh[i].normal);
    ms.set(i, double(mesh[i].shade));
  }
  call.output(1).write().set(0, double(mesh.size()));
  write3(call.output(2).write(), 0, {low.x - margin, low.y - margin, low.z - margin});
  write3(call.output(3).write(), 0, {high.x + margin, high.y + margin, high.z + margin});
  call.output(0).write().set(0, 1.0);
}

}  // namespace

// Туннель из буферов экземпляра генератора tunnel: представление для нативных потребителей.
std::shared_ptr<const built_tunnel> tunnel_from(const originator::generator_result& instance) {
  const auto meta = [&](const std::string_view name) { return instance.field("tunnel_meta", name); };
  if (meta("built").get(0) == 0.0) return nullptr;
  auto tunnel = std::make_shared<built_tunnel>();
  tunnel->key = originator::read_key_word(meta("key"), 0);
  tunnel->first_key = originator::read_key_word(meta("first"), 0);
  tunnel->second_key = originator::read_key_word(meta("second"), 0);
  tunnel->span = meta("span").get(0);
  tunnel->slope_deg = meta("slope").get(0);
  tunnel->low = read3(meta("low"), 0);
  tunnel->high = read3(meta("high"), 0);
  const auto centre = instance.field("tunnel_pieces", "centre"), x = instance.field("tunnel_pieces", "x"),
             y = instance.field("tunnel_pieces", "y"), z = instance.field("tunnel_pieces", "z"),
             half = instance.field("tunnel_pieces", "half"), station = instance.field("tunnel_pieces", "station");
  for (size_t i = 0; i < centre.count(); ++i) {
    tunnel_piece piece;
    piece.centre = read3(centre, i); piece.x = read3(x, i); piece.y = read3(y, i); piece.z = read3(z, i);
    const point h = read3(half, i);
    piece.hx = h.x; piece.hy = h.y; piece.hz = h.z;
    piece.station_key = originator::read_key_word(station, i);
    tunnel->pieces.push_back(piece);
  }
  const auto ma = instance.field("tunnel_mesh", "a"), mb = instance.field("tunnel_mesh", "b"),
             mc = instance.field("tunnel_mesh", "c"), mn = instance.field("tunnel_mesh", "normal"),
             ms = instance.field("tunnel_mesh", "shade");
  tunnel->mesh.reserve(ma.count());
  for (size_t i = 0; i < ma.count(); ++i) {
    tunnel->mesh.push_back({read3(ma, i), read3(mb, i), read3(mc, i), read3(mn, i), uint8_t(ms.get(i))});
  }
  return tunnel;
}

std::shared_ptr<const built_tunnel> tunnel_view(const originator::generator_result& instance) {
  return instance.view<built_tunnel>(tunnel_from);
}

std::shared_ptr<const built_tunnel> tunnel_of_station(originator::generator_chain& chain, const uint64_t seed,
                                                     const originator::instance_key& station) {
  // Туннель — экземпляр генератора tunnel (generator/tunnel), ключ — станция первого якоря пары; пару и
  // её взаимность решает tunnel_link якоря. Кэш — кэш цепочки.
  const auto link = chain.get("tunnel_link", seed, station);
  if (link->field("tunnel_link", "mutual").get(0) == 0.0) return nullptr;
  const auto first = link->field("tunnel_link", "first_station");
  const originator::instance_key key{{originator::read_key_word(first, 0, 0), originator::read_key_word(first, 0, 1),
                                      originator::read_key_word(first, 0, 2)}};
  return tunnel_view(*chain.get("tunnel", seed, key));
}

std::shared_ptr<const built_tunnel> tunnel_for(const adventure& world, const station_anchor& anchor) {
  return tunnel_of_station(world_chain(world.clearance_radius), world.seed, station_key(anchor));
}

void add_tunnel_tools(originator::tool_registry& tools) {
  tools.add(originator::tool_description{.name = "tunnel_build", .shape = originator::aperture::sequential,
                                         .input_count = 8, .output_count = 9, .body = tool_tunnel_build,
                                         .footprint = originator::no_temporary_memory});
}

std::vector<std::shared_ptr<const built_tunnel>> tunnels_near(const adventure& world, const point low,
                                                              const point high) {
  std::vector<std::shared_ptr<const built_tunnel>> out;
  for (const station_anchor& anchor : station_anchors(world)) {
    if (anchor.centre.x < low.x - station_reach || anchor.centre.x > high.x + station_reach ||
        anchor.centre.y < low.y - station_reach || anchor.centre.y > high.y + station_reach ||
        anchor.centre.z < low.z - station_reach || anchor.centre.z > high.z + station_reach) continue;
    auto tunnel = tunnel_for(world, anchor);
    if (!tunnel || !tunnel_reaches(*tunnel, low, high)) continue;
    if (std::any_of(out.begin(), out.end(), [&](const auto& t) { return t->key == tunnel->key; })) continue;
    out.push_back(std::move(tunnel));
  }
  return out;
}

bool tunnel_reaches(const built_tunnel& tunnel, const point low, const point high) {
  return tunnel.low.x <= high.x && tunnel.high.x >= low.x && tunnel.low.y <= high.y &&
         tunnel.high.y >= low.y && tunnel.low.z <= high.z && tunnel.high.z >= low.z;
}

double tunnel_carve_field(const built_tunnel& tunnel, const point position) {
  double field = 1e30;
  for (const tunnel_piece& piece : tunnel.pieces) field = std::min(field, piece_field(piece, position));
  return field - carve_margin;
}

bool in_tunnel_doorway(const built_tunnel& tunnel, const uint64_t station_key, const point position) {
  for (const tunnel_piece& piece : tunnel.pieces) {
    if (piece.station_key == station_key && piece_contains(piece, position)) return true;
  }
  return false;
}

std::string verify_tunnel(const adventure& world, const built_tunnel& tunnel) {
  if (tunnel.pieces.size() < 5 || tunnel.pieces.size() % 2 == 0 || !tunnel.pieces.front().station_key ||
      !tunnel.pieces.back().station_key) return "tunnel is not leg, chamber, spans, chamber, leg";
  if (tunnel.slope_deg > tunnel_max_slope_deg + 1e-9) return "tunnel span is too steep";
  const point midpoint = tunnel.pieces[tunnel.pieces.size() / 2].centre;
  const adventure context = canonical_context(world.seed, midpoint, world.clearance_radius);
  field_cache final_field(context, true, 24.0);
  field_cache natural(context, false, 24.0);
  for (const tunnel_piece& piece : tunnel.pieces) {
    // Другой шаг и другие точки сечения, чем у генератора: середина и углы внутренности
    // вырезаны, кольцо на 1.5 м — порода и до, и после постройки.
    const int steps = int(std::ceil(2.0 * piece.hx / 0.4));
    for (int s = 0; s <= steps; ++s) {
      const double lx = -piece.hx + 2.0 * piece.hx * s / steps;
      for (const double ly : {-piece.hy + 0.2, 0.0, piece.hy - 0.2}) {
        for (const double lz : {-piece.hz + 0.2, 0.0, piece.hz - 0.2}) {
          if (final_field(piece_world(piece, lx, ly, lz)) >= 0.0) return "tunnel interior is not carved";
        }
      }
      for (const auto& [ly, lz] : std::array<std::pair<double, double>, 4>{{
             {-piece.hy - rock_ring, 0.0}, {piece.hy + rock_ring, 0.0},
             {0.0, -piece.hz - rock_ring}, {0.0, piece.hz + rock_ring}}}) {
        const point p = piece_world(piece, lx, ly, lz);
        if (piece.station_key && leg_depth(piece, p) < door_zone) continue;
        // На стыке кольцо одного куска лежит внутри соседнего: кольцо всего туннеля —
        // только точки не ближе 1.5 м ко всей его внутренности.
        if (tunnel_carve_field(tunnel, p) + carve_margin < rock_ring - 1e-6) continue;
        if (natural(p) <= 0.0 || final_field(p) <= 0.0) return "tunnel is not enclosed in rock";
      }
    }
  }
  // Ноги входят во внутренние помещения своих станций.
  for (const tunnel_piece& piece : {tunnel.pieces.front(), tunnel.pieces.back()}) {
    const point inside = piece_world(piece, -piece.hx + 0.1, 0.0, 0.0);
    bool found = false;
    for (const station_anchor& anchor : station_anchors(context)) {
      if (anchor.key != piece.station_key) continue;
      const auto station = station_for(context, anchor);
      found = station && station_interior_contains(*station, inside);
    }
    if (!found) return "tunnel leg does not open into its station";
  }
  return {};
}

} // namespace gn09
