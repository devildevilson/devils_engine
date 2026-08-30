#ifndef DEVILS_ENGINE_PF09_ZONES_H
#define DEVILS_ENGINE_PF09_ZONES_H

// Игровые территории как данные: зона — это бокс, набор ссылок на соседей и немного полей. Ничего больше.
//
// Это перестроенная площадка. Прежняя строила единое иерархическое разбиение плоскости и растр по нему;
// оказалось, что игре нужно не это. Три вывода, которые отсюда следуют и которые стоит держать в голове,
// читая код:
//
//   - зоны НЕ ЗАМОЩАЮТ пространство. Между ними бывают пропуски, и это не дефект: зона — это МЕСТО
//     (комната, отрезок улицы, площадь, участок дороги, владение), а не клетка разбиения. Точка в
//     пропуске не принадлежит никакой зоне, и честный ответ на вопрос «где я» — «нигде», а не
//     ближайшая клетка.
//   - форма зоны — ПРОИЗВОЛЬНЫЙ МНОГОУГОЛЬНИК, а не бокс. Бокс годился для комнаты и не годился ни для
//     чего сложнее: владение не квадратное, улица не квадратная, и подгонка под прямоугольник либо
//     наложит зоны друг на друга, либо оставит между ними щели.
//   - проход — это ОБЩЕЕ РЕБРО двух фигур, а не запись в списке соседей. Отсюда связность не авторская,
//     а выводимая: дверь — это маленькая фигура, чьё ребро лежит и на здании, и на улице. Портал хранит
//     сам отрезок, поэтому по нему можно не только пройти, но и прицелиться — как в navmesh.
//   - уровни НЕ ОБРАЗУЮТ дерева. На каждом уровне своя карта взаимодействий и свой граф связей; ссылка
//     вверх есть, но она справочная, а не структурная. Комната знает свой город, но город не обязан
//     быть разбит на комнаты.
//   - связность — ЭТО и есть содержание. В разбиении соседство выводится из координат; здесь оно
//     авторское, и именно по нему ходит и рассуждает игра.
//
// Хранение — секторные файлы, подгружаемые по близости к наблюдателю. Сектор выводится ИЗ
// идентификатора зоны, поэтому ссылка через границу сектора разрешается без глобального индекса: по id
// видно, какой файл открывать.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace devils_engine::pf09 {

// Уровни — это игровые задачи, а не степени детализации одной карты. Игрок переключается между ними,
// решая разные вопросы, и на каждом видит свой набор зон и свои связи.
enum class zone_level : uint32_t {
  interior,  // комнаты, отрезки улиц, дворы: где действует партия
  local,     // поселения, дороги, приметные места: где раздаются поручения
  regional,  // города и торговые пути: где живёт компания
  political, // владения: где идёт политика
  count
};

enum class zone_kind : uint32_t {
  room, street, yard, building,
  settlement, road, landmark,
  market, route,
  holding,
  count
};

std::string_view zone_level_name(const zone_level value) noexcept;
std::string_view zone_kind_name(const zone_kind value) noexcept;

using zone_key = uint64_t;
constexpr zone_key invalid_key = 0;

// Секторная сетка мира. Сторона сектора — единица подгрузки: слишком мелкая множит файлы и швы, слишком
// крупная тянет с диска лишнее.
constexpr double sector_span_m = 8192.0;
constexpr int32_t sector_bias = 0x800; // сдвиг, чтобы отрицательные координаты сектора влезли в поле id

// Идентификатор несёт свой сектор. Это не оптимизация, а условие того, чтобы ссылка через границу
// разрешалась без глобальной таблицы: по ключу видно, какой файл надо открыть.
[[nodiscard]] constexpr zone_key make_key(const int32_t sector_x, const int32_t sector_y, const uint32_t local) noexcept {
  const uint64_t x = uint64_t(uint32_t(sector_x + sector_bias)) & 0xfffull;
  const uint64_t y = uint64_t(uint32_t(sector_y + sector_bias)) & 0xfffull;
  return (x << 52) | (y << 40) | (uint64_t(local) & 0xffffffffffull);
}

[[nodiscard]] constexpr int32_t key_sector_x(const zone_key key) noexcept {
  return int32_t((key >> 52) & 0xfffull) - sector_bias;
}

[[nodiscard]] constexpr int32_t key_sector_y(const zone_key key) noexcept {
  return int32_t((key >> 40) & 0xfffull) - sector_bias;
}

[[nodiscard]] constexpr uint32_t key_local(const zone_key key) noexcept {
  return uint32_t(key & 0xffffffffffull);
}

// Габарит нужен только для широкой фазы: отсечь заведомо далёкое до точной проверки по многоугольнику.
struct zone_bounds {
  glm::vec3 lower{};
  glm::vec3 upper{};

  bool contains(const glm::vec3& point) const noexcept {
    return point.x >= lower.x && point.y >= lower.y && point.z >= lower.z && point.x <= upper.x &&
           point.y <= upper.y && point.z <= upper.z;
  }
  bool overlaps_xz(const zone_bounds& other) const noexcept {
    return lower.x < other.upper.x && other.lower.x < upper.x && lower.z < other.upper.z && other.lower.z < upper.z;
  }
  float area_xz() const noexcept { return (upper.x - lower.x) * (upper.z - lower.z); }
};

// Свойство прохода — это МЕТАИНФОРМАЦИЯ НА РЕБРЕ, а не на зоне. Заперта не комната, заперта дверь; и
// именно поэтому флаг живёт здесь, а не в записи зоны.
enum class portal_flags : uint32_t {
  open = 0,
  door = 1u << 0,
  locked = 1u << 1,
  climb = 1u << 2,

  // Связь БЕЗ геометрии: дорога между поселениями, ребро политического графа. Отрезка у неё нет, и это
  // не вырождение, а другая природа связи — «переместиться», а не «пройти». Флаг нужен, чтобы отличать
  // такую связь от геометрической, у которой отсутствие отрезка было бы ошибкой сборки.
  graph = 1u << 3,
};

[[nodiscard]] constexpr uint32_t operator|(const portal_flags a, const portal_flags b) noexcept {
  return uint32_t(a) | uint32_t(b);
}

struct zone_portal {
  zone_key other = invalid_key;
  glm::vec2 from{};   // отрезок общего ребра: по нему и проходят
  glm::vec2 to{};
  uint32_t flags = 0;

  bool passable() const noexcept { return (flags & uint32_t(portal_flags::locked)) == 0; }
  bool geometric() const noexcept { return (flags & uint32_t(portal_flags::graph)) == 0; }
  glm::vec2 middle() const noexcept { return {(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f}; }
};

struct zone_record {
  zone_key key = invalid_key;
  zone_key parent = invalid_key;   // справочная ссылка на уровень выше, не структурная
  zone_bounds bounds{};
  zone_level level = zone_level::interior;
  zone_kind kind = zone_kind::room;
  uint32_t vertex_begin = 0;
  uint32_t vertex_count = 0;       // ноль означает АБСТРАКТНУЮ зону: узел графа без формы
  uint32_t portal_begin = 0;
  uint32_t portal_count = 0;
  uint32_t name_offset = 0;
  uint32_t tags = 0;

  bool abstract() const noexcept { return vertex_count == 0; }
};

// Один файл. Читается и пишется целиком: единица подгрузки должна совпадать с единицей файла, иначе
// частично прочитанный сектор станет отдельным состоянием, которое придётся отдельно проверять.
struct zone_sector {
  int32_t x = 0;
  int32_t y = 0;
  std::vector<zone_record> zones;
  std::vector<glm::vec2> vertices;
  std::vector<zone_portal> portals;
  std::vector<char> names;
  uint64_t fingerprint = 0;

  std::string_view name_of(const zone_record& record) const;
  std::span<const glm::vec2> outline_of(const zone_record& record) const;
  std::span<const zone_portal> portals_of(const zone_record& record) const;
  uint64_t byte_size() const noexcept;
};

// Точка внутри многоугольника, обычный алгоритм пересечений луча. Вынесена наружу, потому что ей
// пользуются и пикинг, и проверка, что персонаж не вышел из своей зоны.
bool point_in_outline(const std::span<const glm::vec2> outline, const glm::vec2 point) noexcept;

std::filesystem::path sector_path(const std::filesystem::path& root, const int32_t x, const int32_t y);
uint64_t compute_fingerprint(const zone_sector& sector);

void write_sector(const std::filesystem::path& path, const zone_sector& sector);
bool read_sector(const std::filesystem::path& path, zone_sector& out);

[[nodiscard]] constexpr int32_t sector_of(const double coordinate) noexcept {
  return int32_t(coordinate >= 0.0 ? coordinate / sector_span_m : (coordinate / sector_span_m) - 1.0);
}

struct stream_stats {
  uint32_t loaded = 0;
  uint32_t evicted = 0;
  uint32_t resident = 0;
  uint64_t resident_bytes = 0;
  uint64_t bytes_read = 0;
};

// Хранилище с подгрузкой по близости. Держит только сектора в радиусе; ответы не должны зависеть от
// того, в каком порядке они появлялись.
class zone_store {
public:
  explicit zone_store(std::filesystem::path root, const double radius_m = 12000.0, const uint32_t budget = 32);

  stream_stats focus(const glm::dvec2& observer_m);

  const zone_sector* sector(const int32_t x, const int32_t y) const;
  const zone_record* find(const zone_key key) const;

  // Зона в точке на заданном уровне. Пропуски — законный ответ `nullptr`: между зонами бывает пустота.
  const zone_record* pick(const glm::vec3& point_m, const zone_level level) const;

  std::span<const glm::vec2> outline_of(const zone_record& record) const;
  std::span<const zone_portal> portals_of(const zone_record& record) const;
  std::string_view name_of(const zone_record& record) const;

  uint32_t resident_sectors() const noexcept { return uint32_t(resident_.size()); }
  uint64_t resident_bytes() const;
  const stream_stats& last() const noexcept { return last_; }

private:
  struct entry {
    int32_t x = 0;
    int32_t y = 0;
    zone_sector data;
  };

  std::filesystem::path root_;
  double radius_m_ = 0.0;
  uint32_t budget_ = 0;
  std::vector<entry> resident_;
  stream_stats last_{};
};

} // namespace devils_engine::pf09

#endif
