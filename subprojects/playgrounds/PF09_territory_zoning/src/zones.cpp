#include "zones.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/hash.h"

namespace devils_engine::pf09 {

namespace {

constexpr char sector_magic[8] = {'P', 'F', '0', '9', 'Z', 'S', '0', '2'};

struct sector_header {
  char magic[8];
  int32_t x;
  int32_t y;
  uint32_t zone_count;
  uint32_t vertex_count;
  uint32_t portal_count;
  uint32_t name_bytes;
  uint64_t fingerprint;
};

template <typename T>
void append(std::vector<char>& out, const T& value) {
  const auto offset = out.size();
  out.resize(offset + sizeof(T));
  std::memcpy(out.data() + offset, &value, sizeof(T));
}

template <typename T>
bool take(const std::vector<char>& in, size_t& cursor, T& value) {
  if (cursor + sizeof(T) > in.size()) return false;
  std::memcpy(&value, in.data() + cursor, sizeof(T));
  cursor += sizeof(T);
  return true;
}

} // namespace

std::string_view zone_level_name(const zone_level value) noexcept {
  switch (value) {
    case zone_level::interior: return "interior";
    case zone_level::local: return "local";
    case zone_level::regional: return "regional";
    case zone_level::political: return "political";
    default: return "?";
  }
}

std::string_view zone_kind_name(const zone_kind value) noexcept {
  switch (value) {
    case zone_kind::room: return "room";
    case zone_kind::street: return "street";
    case zone_kind::yard: return "yard";
    case zone_kind::building: return "building";
    case zone_kind::settlement: return "settlement";
    case zone_kind::road: return "road";
    case zone_kind::landmark: return "landmark";
    case zone_kind::market: return "market";
    case zone_kind::route: return "route";
    case zone_kind::holding: return "holding";
    default: return "?";
  }
}

std::string_view zone_sector::name_of(const zone_record& record) const {
  if (record.name_offset >= names.size()) return {};
  return std::string_view(names.data() + record.name_offset);
}

std::span<const glm::vec2> zone_sector::outline_of(const zone_record& record) const {
  if (record.vertex_begin + record.vertex_count > vertices.size()) return {};
  return {vertices.data() + record.vertex_begin, record.vertex_count};
}

std::span<const zone_portal> zone_sector::portals_of(const zone_record& record) const {
  if (record.portal_begin + record.portal_count > portals.size()) return {};
  return {portals.data() + record.portal_begin, record.portal_count};
}

uint64_t zone_sector::byte_size() const noexcept {
  return zones.size() * sizeof(zone_record) + vertices.size() * sizeof(glm::vec2) +
         portals.size() * sizeof(zone_portal) + names.size();
}

// Луч вправо и счёт пересечений. Границу считаем принадлежащей зоне через `>=`/`<` на одном конце: точка
// ровно на общем ребре двух зон обязана достаться ровно одной, иначе персонаж, стоящий в дверях, окажется
// то в одной комнате, то в другой в зависимости от порядка перебора.
bool point_in_outline(const std::span<const glm::vec2> outline, const glm::vec2 point) noexcept {
  if (outline.size() < 3) return false;

  bool inside = false;
  for (size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
    const auto& a = outline[i];
    const auto& b = outline[j];
    if ((a.y > point.y) == (b.y > point.y)) continue;
    const float crossing = (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x;
    if (point.x < crossing) inside = !inside;
  }
  return inside;
}

std::filesystem::path sector_path(const std::filesystem::path& root, const int32_t x, const int32_t y) {
  return root / ("sector_" + std::to_string(x) + "_" + std::to_string(y) + ".zsec");
}

// Отпечаток считается по СОДЕРЖИМОМУ, а не по байтам файла: тогда он одинаков у сектора, собранного в
// памяти, и у прочитанного с диска, и сравнение этих двух становится проверкой сериализации, а не
// проверкой того, что файл побайтно равен сам себе.
uint64_t compute_fingerprint(const zone_sector& sector) {
  uint64_t hash = utils::splitmix(uint64_t(uint32_t(sector.x)) ^ (uint64_t(uint32_t(sector.y)) << 32));
  for (const auto& record : sector.zones) {
    hash = utils::hash_combine(hash, record.key);
    hash = utils::hash_combine(hash, record.parent);
    hash = utils::hash_combine(hash, uint32_t(record.level));
    hash = utils::hash_combine(hash, uint32_t(record.kind));
    hash = utils::hash_combine(hash, record.vertex_begin);
    hash = utils::hash_combine(hash, record.vertex_count);
    hash = utils::hash_combine(hash, record.portal_begin);
    hash = utils::hash_combine(hash, record.portal_count);
    hash = utils::hash_combine(hash, record.name_offset);
    hash = utils::hash_combine(hash, record.tags);
    for (uint32_t axis = 0; axis < 3; ++axis) {
      hash = utils::hash_combine(hash, std::bit_cast<uint32_t>(record.bounds.lower[axis]));
      hash = utils::hash_combine(hash, std::bit_cast<uint32_t>(record.bounds.upper[axis]));
    }
  }
  for (const auto& vertex : sector.vertices) {
    hash = utils::hash_combine(hash, std::bit_cast<uint32_t>(vertex.x));
    hash = utils::hash_combine(hash, std::bit_cast<uint32_t>(vertex.y));
  }
  for (const auto& portal : sector.portals) {
    hash = utils::hash_combine(hash, portal.other);
    hash = utils::hash_combine(hash, portal.flags);
    hash = utils::hash_combine(hash, std::bit_cast<uint32_t>(portal.from.x));
    hash = utils::hash_combine(hash, std::bit_cast<uint32_t>(portal.from.y));
    hash = utils::hash_combine(hash, std::bit_cast<uint32_t>(portal.to.x));
    hash = utils::hash_combine(hash, std::bit_cast<uint32_t>(portal.to.y));
  }
  for (const char symbol : sector.names) {
    hash = utils::hash_combine(hash, uint64_t(uint8_t(symbol)));
  }
  return utils::splitmix(hash);
}

void write_sector(const std::filesystem::path& path, const zone_sector& sector) {
  sector_header header{};
  std::memcpy(header.magic, sector_magic, sizeof(sector_magic));
  header.x = sector.x;
  header.y = sector.y;
  header.zone_count = uint32_t(sector.zones.size());
  header.vertex_count = uint32_t(sector.vertices.size());
  header.portal_count = uint32_t(sector.portals.size());
  header.name_bytes = uint32_t(sector.names.size());
  header.fingerprint = compute_fingerprint(sector);

  std::vector<char> bytes;
  bytes.reserve(sizeof(header) + sector.byte_size());
  append(bytes, header);
  for (const auto& record : sector.zones) {
    append(bytes, record);
  }
  for (const auto& vertex : sector.vertices) {
    append(bytes, vertex);
  }
  for (const auto& portal : sector.portals) {
    append(bytes, portal);
  }
  bytes.insert(bytes.end(), sector.names.begin(), sector.names.end());

  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) utils::error{}("PF09 zones: cannot write '{}'", path.string());
  file.write(bytes.data(), std::streamsize(bytes.size()));
}

bool read_sector(const std::filesystem::path& path, zone_sector& out) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return false;

  const auto size = size_t(file.tellg());
  file.seekg(0);
  std::vector<char> bytes(size);
  file.read(bytes.data(), std::streamsize(size));

  size_t cursor = 0;
  sector_header header{};
  if (!take(bytes, cursor, header)) return false;
  if (std::memcmp(header.magic, sector_magic, sizeof(sector_magic)) != 0) {
    utils::error{}("PF09 zones: '{}' is not a sector file", path.string());
  }

  out.x = header.x;
  out.y = header.y;
  out.zones.resize(header.zone_count);
  out.vertices.resize(header.vertex_count);
  out.portals.resize(header.portal_count);
  out.names.resize(header.name_bytes);

  for (auto& record : out.zones) {
    if (!take(bytes, cursor, record)) return false;
  }
  for (auto& vertex : out.vertices) {
    if (!take(bytes, cursor, vertex)) return false;
  }
  for (auto& portal : out.portals) {
    if (!take(bytes, cursor, portal)) return false;
  }
  if (cursor + out.names.size() > bytes.size()) return false;
  std::memcpy(out.names.data(), bytes.data() + cursor, out.names.size());

  out.fingerprint = compute_fingerprint(out);

  // Отпечаток из заголовка сверяется с пересчитанным по содержимому. Порченый файл обязан сообщить о себе
  // здесь, а не проявиться зоной без соседей где-то посреди игры.
  if (out.fingerprint != header.fingerprint) {
    utils::error{}("PF09 zones: '{}' fingerprint {} does not match stored {}", path.string(), out.fingerprint,
                   header.fingerprint);
  }
  return true;
}

zone_store::zone_store(std::filesystem::path root, const double radius_m, const uint32_t budget)
  : root_(std::move(root)), radius_m_(radius_m), budget_(budget) {
  if (budget_ == 0) utils::error{}("PF09 zone store: the budget must hold at least one sector");
}

stream_stats zone_store::focus(const glm::dvec2& observer_m) {
  stream_stats stats{};

  const int32_t cx = sector_of(observer_m.x);
  const int32_t cy = sector_of(observer_m.y);
  const auto reach = int32_t(std::ceil(radius_m_ / sector_span_m));

  // Кандидаты сортируются по расстоянию до наблюдателя, поэтому при нехватке бюджета отсекаются дальние.
  // Это единственный разумный порядок: ближний сектор нужен раньше, а «первый попавшийся» зависел бы от
  // обхода и делал бы поведение непредсказуемым.
  std::vector<std::pair<double, std::pair<int32_t, int32_t>>> wanted;
  for (int32_t y = cy - reach; y <= cy + reach; ++y) {
    for (int32_t x = cx - reach; x <= cx + reach; ++x) {
      const double centre_x = (double(x) + 0.5) * sector_span_m;
      const double centre_y = (double(y) + 0.5) * sector_span_m;
      const double dx = centre_x - observer_m.x;
      const double dy = centre_y - observer_m.y;
      const double distance = dx * dx + dy * dy;
      if (distance > (radius_m_ + sector_span_m) * (radius_m_ + sector_span_m)) continue;
      wanted.emplace_back(distance, std::pair{x, y});
    }
  }
  std::sort(wanted.begin(), wanted.end());
  if (wanted.size() > budget_) wanted.resize(budget_);

  std::vector<entry> kept;
  kept.reserve(wanted.size());

  for (const auto& [distance, coordinate] : wanted) {
    const auto [x, y] = coordinate;

    auto existing = std::find_if(resident_.begin(), resident_.end(),
                                 [&](const entry& item) { return item.x == x && item.y == y; });
    if (existing != resident_.end()) {
      kept.push_back(std::move(*existing));
      existing->x = INT32_MIN; // помечаем перенесённый, чтобы не посчитать его выгруженным
      continue;
    }

    entry fresh{};
    fresh.x = x;
    fresh.y = y;
    if (!read_sector(sector_path(root_, x, y), fresh.data)) continue; // за краем мира секторов нет

    stats.bytes_read += fresh.data.byte_size();
    ++stats.loaded;
    kept.push_back(std::move(fresh));
  }

  for (const auto& item : resident_) {
    if (item.x != INT32_MIN) ++stats.evicted;
  }

  resident_ = std::move(kept);
  stats.resident = uint32_t(resident_.size());
  stats.resident_bytes = resident_bytes();
  last_ = stats;
  return stats;
}

const zone_sector* zone_store::sector(const int32_t x, const int32_t y) const {
  const auto found = std::find_if(resident_.begin(), resident_.end(),
                                  [&](const entry& item) { return item.x == x && item.y == y; });
  return found == resident_.end() ? nullptr : &found->data;
}

const zone_record* zone_store::find(const zone_key key) const {
  if (key == invalid_key) return nullptr;

  // Сектор берётся ИЗ КЛЮЧА, а не поиском по всем резидентным: ссылка через границу обязана разрешаться
  // за один шаг, иначе граф связей начнёт стоить обхода.
  const auto* owner = sector(key_sector_x(key), key_sector_y(key));
  if (owner == nullptr) return nullptr;

  const uint32_t local = key_local(key);
  if (local >= owner->zones.size()) return nullptr;
  return &owner->zones[local];
}

const zone_record* zone_store::pick(const glm::vec3& point_m, const zone_level level) const {
  const auto* owner = sector(sector_of(point_m.x), sector_of(point_m.z));
  if (owner == nullptr) return nullptr;

  const glm::vec2 flat{point_m.x, point_m.z};

  // При попадании в несколько фигур выигрывает МЕНЬШАЯ по габариту: комната внутри здания важнее самого
  // здания, и это единственное правило, дающее один ответ независимо от порядка записей в файле.
  const zone_record* best = nullptr;
  for (const auto& record : owner->zones) {
    if (record.level != level || record.abstract()) continue;
    if (!record.bounds.contains(point_m)) continue; // широкая фаза по габариту
    if (!point_in_outline(owner->outline_of(record), flat)) continue;
    if (best == nullptr || record.bounds.area_xz() < best->bounds.area_xz()) best = &record;
  }
  return best;
}

std::span<const glm::vec2> zone_store::outline_of(const zone_record& record) const {
  const auto* owner = sector(key_sector_x(record.key), key_sector_y(record.key));
  return owner == nullptr ? std::span<const glm::vec2>{} : owner->outline_of(record);
}

std::span<const zone_portal> zone_store::portals_of(const zone_record& record) const {
  const auto* owner = sector(key_sector_x(record.key), key_sector_y(record.key));
  return owner == nullptr ? std::span<const zone_portal>{} : owner->portals_of(record);
}

std::string_view zone_store::name_of(const zone_record& record) const {
  const auto* owner = sector(key_sector_x(record.key), key_sector_y(record.key));
  return owner == nullptr ? std::string_view{} : owner->name_of(record);
}

uint64_t zone_store::resident_bytes() const {
  uint64_t total = 0;
  for (const auto& item : resident_) {
    total += item.data.byte_size();
  }
  return total;
}

} // namespace devils_engine::pf09
