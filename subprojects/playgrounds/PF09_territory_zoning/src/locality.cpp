#include "locality.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/hash.h"

namespace devils_engine::pf09 {

namespace {

constexpr uint64_t roll_range = 0x10000ull;

[[nodiscard]] uint64_t roll(const zone_id anchor, const uint64_t seed, const uint64_t salt) noexcept {
  return utils::splitmix(utils::hash_combine(seed ^ salt, uint64_t(anchor) + 1ull));
}

[[nodiscard]] bool chance(const uint64_t value, const double probability) noexcept {
  return (value & (roll_range - 1)) < uint64_t(std::clamp(probability, 0.0, 1.0) * double(roll_range));
}

} // namespace

std::string_view locality_kind_name(const locality_kind value) noexcept {
  switch (value) {
    case locality_kind::town: return "town";
    case locality_kind::crypt: return "crypt";
    case locality_kind::castle: return "castle";
    default: return "none";
  }
}

std::string_view zone_role_name(const zone_role value) noexcept {
  switch (value) {
    case zone_role::street: return "street";
    case zone_role::yard: return "yard";
    case zone_role::building: return "building";
    case zone_role::room: return "room";
    default: return "?";
  }
}

locality::locality(const territory& map, const locality_config& config, const zone_id anchor,
                   const locality_kind kind)
  : config_(config), anchor_(anchor), kind_(kind) {
  if (config_.plot_side == 0 || config_.room_side == 0) {
    utils::error{}("PF09 locality: plot and room grids must not be empty");
  }
  if (config_.street_stride < 2) {
    utils::error{}("PF09 locality: street stride {} would pave everything", config_.street_stride);
  }

  room_stride_ = config_.room_side * config_.room_side + 1;
  centre_ = map.node_centre_m(anchor);
  build(map);
}

void locality::connect(const uint32_t a, const uint32_t b) {
  links_[a].push_back(b);
  links_[b].push_back(a);
}

void locality::build(const territory& map) {
  (void)map;

  const uint32_t side = config_.plot_side;
  const uint32_t plots = side * side;
  roles_.assign(size_t(plots) * room_stride_, zone_role::room);
  links_.assign(roles_.size(), {});

  // Улицы кладутся по регулярному шагу и дополняются хешем. Регулярный шаг задаёт КВАРТАЛЫ, а не
  // достижимость: при шаге 3 квартал получается 2x2, и его дальний угол не касается улицы ни одной
  // стороной. Достижимость обеспечивается ниже — тем, что такой угол становится двором и соединяется с
  // соседями по кварталу. Гарантия здесь двухчастная: квартал связен внутри себя, а любой квартал хотя
  // бы одной стороной выходит на улицу, потому что лежит между двумя улицами.
  std::vector<uint8_t> street_row(side, 0);
  std::vector<uint8_t> street_column(side, 0);
  for (uint32_t i = 0; i < side; ++i) {
    const bool regular = i % config_.street_stride == 0;
    street_row[i] = regular || chance(roll(anchor_, 0x5713e7ull, i * 2ull + 1ull), config_.extra_street_chance);
    street_column[i] = regular || chance(roll(anchor_, 0x5713e7ull, i * 2ull + 2ull), config_.extra_street_chance);
  }

  for (uint32_t y = 0; y < side; ++y) {
    for (uint32_t x = 0; x < side; ++x) {
      const uint32_t plot = y * side + x;
      const uint32_t zone = plot_zone(plot);

      if (street_row[y] != 0 || street_column[x] != 0) {
        roles_[zone] = zone_role::street;
        continue;
      }
      const auto h = roll(anchor_, 0xb0d1ull, plot);
      roles_[zone] = chance(h, config_.building_chance) ? zone_role::building : zone_role::yard;
    }
  }

  constexpr int32_t offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  const auto neighbour_plot = [&](const uint32_t x, const uint32_t y, const uint32_t direction) -> int64_t {
    const int64_t nx = int64_t(x) + offsets[direction][0];
    const int64_t ny = int64_t(y) + offsets[direction][1];
    if (nx < 0 || ny < 0 || nx >= int64_t(side) || ny >= int64_t(side)) return -1;
    return ny * int64_t(side) + nx;
  };

  // Участок, не касающийся улицы ни одной стороной, становится двором. Это не заплатка ради связности, а
  // то, чем такой участок и является: внутренность квартала, куда попадают через соседа, а не с улицы.
  for (uint32_t y = 0; y < side; ++y) {
    for (uint32_t x = 0; x < side; ++x) {
      const uint32_t zone = plot_zone(y * side + x);
      if (roles_[zone] == zone_role::street) continue;

      bool touches_street = false;
      for (uint32_t d = 0; d < 4 && !touches_street; ++d) {
        const int64_t other = neighbour_plot(x, y, d);
        touches_street = other >= 0 && roles_[plot_zone(uint32_t(other))] == zone_role::street;
      }
      if (!touches_street) roles_[zone] = zone_role::yard;
    }
  }

  // Смежность. Улица соединяется со всем, к чему примыкает: у постройки может быть больше одного выхода,
  // и запрещать это значило бы описывать лабиринт, а не город. Двор дополнительно соединяется с соседями
  // по кварталу — так внутренность квартала достижима, но пройти НАСКВОЗЬ через чужой дом нельзя.
  for (uint32_t y = 0; y < side; ++y) {
    for (uint32_t x = 0; x < side; ++x) {
      const uint32_t zone = plot_zone(y * side + x);

      for (uint32_t d = 0; d < 4; ++d) {
        const int64_t other = neighbour_plot(x, y, d);
        if (other < 0) continue;

        const uint32_t neighbour = plot_zone(uint32_t(other));
        if (neighbour <= zone) continue; // ребро добавляет только меньший конец: иначе оно удвоится

        const bool here_street = roles_[zone] == zone_role::street;
        const bool there_street = roles_[neighbour] == zone_role::street;
        const bool here_yard = roles_[zone] == zone_role::yard;
        const bool there_yard = roles_[neighbour] == zone_role::yard;

        if (here_street || there_street || here_yard || there_yard) connect(zone, neighbour);
      }
    }
  }

  // Помещения существуют только у зданий; у улиц и дворов их слоты остаются незанятыми. Дерево помещений
  // строится от входной комнаты, и каждая следующая цепляется к уже подключённой — так связность
  // получается по построению, а не проверкой постфактум.
  const uint32_t rooms = config_.room_side * config_.room_side;
  for (uint32_t plot = 0; plot < plots; ++plot) {
    const uint32_t zone = plot_zone(plot);
    if (roles_[zone] != zone_role::building) {
      for (uint32_t r = 0; r < rooms; ++r) {
        roles_[room_zone(plot, r)] = zone_role::count;
      }
      continue;
    }

    for (uint32_t r = 0; r < rooms; ++r) {
      roles_[room_zone(plot, r)] = zone_role::room;
    }
    connect(zone, room_zone(plot, 0));

    for (uint32_t r = 1; r < rooms; ++r) {
      const auto h = roll(anchor_, 0x7007ull + plot * 131ull, r);
      const uint32_t parent = uint32_t(h % r);
      connect(room_zone(plot, parent), room_zone(plot, r));
    }
  }
}

bool locality::contains(const glm::dvec2& point_m) const {
  const double half = config_.extent_m * 0.5;
  return std::abs(point_m.x - centre_.x) <= half && std::abs(point_m.y - centre_.y) <= half;
}

uint32_t locality::zone_at(const glm::dvec2& point_m) const {
  if (!contains(point_m)) return invalid_local;

  const double half = config_.extent_m * 0.5;
  const double cell = config_.extent_m / double(config_.plot_side);
  const auto x = uint32_t(std::clamp((point_m.x - centre_.x + half) / cell, 0.0, double(config_.plot_side) - 1.0));
  const auto y = uint32_t(std::clamp((point_m.y - centre_.y + half) / cell, 0.0, double(config_.plot_side) - 1.0));

  const uint32_t plot = y * config_.plot_side + x;
  const uint32_t zone = plot_zone(plot);
  if (roles_[zone] != zone_role::building) return zone;

  // Внутри здания точка попадает в помещение. Это и есть «где какие комнаты в склепе»: та же координата,
  // но ответ на ярус глубже.
  const double room_cell = cell / double(config_.room_side);
  const double local_x = point_m.x - centre_.x + half - double(x) * cell;
  const double local_y = point_m.y - centre_.y + half - double(y) * cell;
  const auto rx = uint32_t(std::clamp(local_x / room_cell, 0.0, double(config_.room_side) - 1.0));
  const auto ry = uint32_t(std::clamp(local_y / room_cell, 0.0, double(config_.room_side) - 1.0));
  return room_zone(plot, ry * config_.room_side + rx);
}

std::span<const uint32_t> locality::neighbours(const uint32_t local) const {
  return {links_[local].data(), links_[local].size()};
}

uint64_t locality::byte_size() const noexcept {
  uint64_t total = roles_.size() * sizeof(zone_role);
  for (const auto& list : links_) {
    total += sizeof(uint32_t) * list.size() + sizeof(std::vector<uint32_t>);
  }
  return total;
}

// --- поле локальностей ---

locality_field::locality_field(const territory& map, const locality_config& config) : map_(&map), config_(config) {}

locality_kind locality_field::placed_at(const zone_id locale_node) const {
  if (tier_of(locale_node) != tier::locale) return locality_kind::none;

  // Один бросок на три вида, а не три независимых: иначе в одной клетке оказались бы и город, и замок,
  // и склеп одновременно, и «одна локальность на узел» перестало бы быть правдой.
  const auto h = roll(locale_node, map_->config().seed, 0x10ca11ull);
  const double unit = double(h & (roll_range - 1)) / double(roll_range);

  if (unit < config_.town_chance) return locality_kind::town;
  if (unit < config_.town_chance + config_.crypt_chance) return locality_kind::crypt;
  if (unit < config_.town_chance + config_.crypt_chance + config_.castle_chance) return locality_kind::castle;
  return locality_kind::none;
}

uint32_t locality_field::focus(const glm::dvec2& observer_m) {
  const double span = map_->tier_span_m(tier::locale);
  const auto radius = int64_t(std::ceil(config_.residency_radius_m / span));

  std::vector<std::unique_ptr<locality>> kept;
  std::vector<const locality*> live;

  const int64_t cx = int64_t(std::floor(observer_m.x / span));
  const int64_t cy = int64_t(std::floor(observer_m.y / span));

  uint32_t built = 0;
  for (int64_t y = cy - radius; y <= cy + radius && live.size() < config_.residency; ++y) {
    for (int64_t x = cx - radius; x <= cx + radius && live.size() < config_.residency; ++x) {
      const glm::dvec2 probe{(double(x) + 0.5) * span, (double(y) + 0.5) * span};
      const auto node = map_->resolve(probe, tier::locale);
      const auto kind = placed_at(node);
      if (kind == locality_kind::none) continue;

      const auto centre = map_->node_centre_m(node);
      const double dx = centre.x - observer_m.x;
      const double dy = centre.y - observer_m.y;
      if (dx * dx + dy * dy > config_.residency_radius_m * config_.residency_radius_m) continue;

      // Уже построенная локальность переиспользуется, а не строится заново: наблюдатель, ходящий туда-
      // сюда через границу радиуса, иначе перестраивал бы город на каждом шаге.
      auto existing = std::find_if(storage_.begin(), storage_.end(),
                                   [&](const auto& item) { return item != nullptr && item->anchor() == node; });
      if (existing != storage_.end()) {
        live.push_back(existing->get());
        kept.push_back(std::move(*existing));
        continue;
      }

      auto fresh = std::make_unique<locality>(*map_, config_, node, kind);
      live.push_back(fresh.get());
      kept.push_back(std::move(fresh));
      ++built;
    }
  }

  storage_ = std::move(kept);
  resident_ = std::move(live);
  return built;
}

const locality* locality_field::at(const glm::dvec2& point_m) const {
  for (const auto* item : resident_) {
    if (item->contains(point_m)) return item;
  }
  return nullptr;
}

local_address locality_field::resolve_local(const glm::dvec2& point_m) const {
  const auto* item = at(point_m);
  if (item == nullptr) return {};

  const auto local = item->zone_at(point_m);
  if (local == locality::invalid_local) return {};
  return {item->anchor(), local};
}

uint64_t locality_field::resident_bytes() const {
  uint64_t total = 0;
  for (const auto* item : resident_) {
    total += item->byte_size();
  }
  return total;
}

} // namespace devils_engine::pf09
