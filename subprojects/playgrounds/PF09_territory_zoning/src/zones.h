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
//   - форма зоны — набор ВЫПУКЛЫХ ЧАСТЕЙ, а не один многоугольник и тем более не бокс. Зона
//     произвольной формы собирается из частей, и это не уступка растеризатору: по невыпуклой фигуре
//     нельзя идти по прямой к проёму, не выйдя наружу, поэтому разбиение на выпуклое нужно движению
//     раньше, чем рисованию. Часть — единица геометрии и навигации, зона — единица смысла.
//   - проход — это ЗАПИСЬ В ФАЙЛЕ, а общее ребро — лишь один из способов её получить. Совпадение рёбер
//     ненадёжно: две фигуры, которые «должны» граничить, расходятся на округление, и связность,
//     вычисляемая на лету, начинает мигать. Поэтому геометрия работает ОДИН РАЗ, в сборщике, а игра
//     читает готовый граф и никогда его не перевыводит. Отрезок в портале хранится затем, чтобы по
//     нему можно было не только пройти, но и прицелиться, — как в navmesh; связь БЕЗ отрезка
//     (лестница на другой этаж, дорога между поселениями) законна и помечена флагом `graph`.
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

// Вид ЗОНЫ-МЕСТА, а не отдельной выпуклой фигуры. Выпуклая фигура мельче осмысленного места: бар — это
// несколько фигур внутри здания, площадь — несколько уличных клеток, задний двор — пара. Поэтому вид
// описывает место целиком, а части — только его геометрию.
enum class zone_kind : uint32_t {
  hall,        // помещения одного здания: то, что игра назовёт баром, кузницей, залом
  wall,        // стены здания: НЕПРОХОДИМОЕ место, а не отсутствие места
  door,        // проём в стене: место, проходимость которого переключается в рантайме
  stair,       // лестница: место, связанное с другим этажом СВЯЗЬЮ, а не общим ребром
  street,      // отрезок улицы между перекрёстками
  crossroad,
  square,
  yard,
  building,    // абстрактная группа: здание как совокупность мест
  district,    // квартал: абстрактная группа мест
  settlement, road, landmark,
  market, route,
  holding,
  count
};

// Свойства места. Проходимость — свойство ЗОНЫ, а не ребра: стена непроходима целиком, а запертая дверь
// закрывает конкретный проход. Смешивать их значит терять оба различия.
enum class zone_flags : uint32_t {
  none = 0,
  impassable = 1u << 0,
  road = 1u << 1,     // предпочтительно для движения: дорога тянет маршрут на себя
  indoor = 1u << 2,

  // Начальное состояние переключаемого места. Обычная дверь — это не особый вид связи, а МЕСТО, у
  // которого проходимость меняется по ходу игры: заперли, выбили, забаррикадировали. Флаг в файле задаёт
  // только начальное положение, дальше состоянием владеет рантайм (`zone_store::set_closed`).
  closed = 1u << 3,
};

[[nodiscard]] constexpr uint32_t operator|(const zone_flags a, const zone_flags b) noexcept {
  return uint32_t(a) | uint32_t(b);
}

std::string_view zone_level_name(const zone_level value) noexcept;
std::string_view zone_kind_name(const zone_kind value) noexcept;

using zone_key = uint64_t;
constexpr zone_key invalid_key = 0;

// Секторная сетка мира. Сторона сектора — единица подгрузки: слишком мелкая множит файлы и швы, слишком
// крупная тянет с диска лишнее.
constexpr double sector_span_m = 8192.0;

// Шаг этажа: высота помещения плюс перекрытие. Нужен и сборщику, и рисованию, и пикингу — «на каком
// уровне искать» это одна величина, и держать её в трёх местах значило бы ждать, когда они разойдутся.
constexpr float storey_height_m = 3.0f;
constexpr float storey_gap_m = 0.4f;
constexpr float storey_pitch_m = storey_height_m + storey_gap_m;
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
  zone_key other = invalid_key;   // зона по ту сторону
  uint32_t other_part = 0;        // и её часть: соседство живёт между ЧАСТЯМИ, а не между зонами
  glm::vec2 from{};               // отрезок общего ребра: по нему и проходят
  glm::vec2 to{};
  uint32_t flags = 0;

  bool passable() const noexcept { return (flags & uint32_t(portal_flags::locked)) == 0; }
  bool geometric() const noexcept { return (flags & uint32_t(portal_flags::graph)) == 0; }
  glm::vec2 middle() const noexcept { return {(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f}; }
};

// Выпуклая часть зоны. Собственный габарит нужен широкой фазе: зона из десятка частей иначе
// проверялась бы целиком там, где хватает одной.
struct zone_part {
  zone_bounds bounds{};
  uint32_t vertex_begin = 0;
  uint32_t vertex_count = 0;
  uint32_t portal_begin = 0;
  uint32_t portal_count = 0;
};

struct zone_record {
  zone_key key = invalid_key;
  zone_key parent = invalid_key;   // справочная ссылка на уровень выше, не структурная
  zone_bounds bounds{};            // объединение габаритов частей
  zone_level level = zone_level::interior;
  zone_kind kind = zone_kind::hall;
  uint32_t part_begin = 0;
  uint32_t part_count = 0;         // ноль означает АБСТРАКТНУЮ зону: узел графа без формы
  uint32_t name_offset = 0;
  uint32_t tags = 0;

  // Рёбра графа у зоны БЕЗ формы. Порталы живут на частях, потому что у прохода есть отрезок; у зоны без
  // частей вешать их некуда, и раньше они молча терялись — политический уровень уезжал на диск как набор
  // изолированных узлов. Узел графа обязан носить свои рёбра сам.
  uint32_t link_begin = 0;
  uint32_t link_count = 0;

  // Предметы места. Лежат у ЗОНЫ, а не у части: «что стоит в этой комнате» — вопрос про место, а часть
  // лишь уточняет, где именно, и хранится в самом предмете.
  uint32_t prop_begin = 0;
  uint32_t prop_count = 0;

  // Этаж — ОТДЕЛЬНОЕ поле, а не вывод из высоты. Высоту читает рисование, а «какой уровень здания» —
  // вопрос игры: подвал, первый, второй. Выводить его из `bounds.lower.y` значило бы требовать, чтобы у
  // всех зданий этажи были одинаковой высоты, а это неправда уже про подвал.
  int32_t floor = 0;

  bool abstract() const noexcept { return part_count == 0; }
  bool impassable() const noexcept { return (tags & uint32_t(zone_flags::impassable)) != 0; }
  bool road() const noexcept { return (tags & uint32_t(zone_flags::road)) != 0; }
  bool closed() const noexcept { return (tags & uint32_t(zone_flags::closed)) != 0; }
};

// Предмет внутри места: стул, стол, бочка, камень. Размещается СВОБОДНО — не по клеткам и не по сетке, —
// но с полным знанием того, в каком месте и в какой его части он лежит.
//
// Он ломает то, на чём держалась вся модель движения: «часть выпукла, значит внутри неё можно идти по
// прямой к проёму». Как только посреди комнаты стоит стол, это неправда. Поэтому предмет — НЕ зона:
// зонами он превратил бы каждую комнату в лоскутное одеяло из десятка фигур, а связность — в граф,
// который надо пересобирать всякий раз, когда стул подвинули. Предмет остаётся препятствием ВНУТРИ
// части, а часть остаётся единицей связности; обход предмета — забота шага, а не маршрута.
enum class prop_flags : uint32_t {
  none = 0,
  blocks_move = 1u << 0,   // актор не проходит сквозь
  blocks_sight = 1u << 1,  // за ним не видно — из этого и получается укрытие
  climbable = 1u << 2,     // через стол можно перелезть: движению мешает, взгляду нет
};

[[nodiscard]] constexpr uint32_t operator|(const prop_flags a, const prop_flags b) noexcept {
  return uint32_t(a) | uint32_t(b);
}

struct zone_prop {
  glm::vec2 position{};
  float radius = 0.0f;
  float height = 0.0f;
  uint32_t part = 0;      // часть, внутри которой он лежит: сужает и запрос, и проверку
  uint32_t flags = 0;

  bool blocks_move() const noexcept { return (flags & uint32_t(prop_flags::blocks_move)) != 0; }
  bool blocks_sight() const noexcept { return (flags & uint32_t(prop_flags::blocks_sight)) != 0; }
  bool climbable() const noexcept { return (flags & uint32_t(prop_flags::climbable)) != 0; }
};

// Ссылка на часть. Навигация и выборка работают с частями, поэтому адрес у них парный: зона говорит, ЧТО
// это, часть — ГДЕ именно.
struct part_ref {
  zone_key zone = invalid_key;
  uint32_t part = 0;

  bool valid() const noexcept { return zone != invalid_key; }
  bool operator==(const part_ref&) const noexcept = default;
  bool operator<(const part_ref& other) const noexcept {
    return zone != other.zone ? zone < other.zone : part < other.part;
  }
};

// Один файл. Читается и пишется целиком: единица подгрузки должна совпадать с единицей файла, иначе
// частично прочитанный сектор станет отдельным состоянием, которое придётся отдельно проверять.
struct zone_sector {
  int32_t x = 0;
  int32_t y = 0;
  std::vector<zone_record> zones;
  std::vector<zone_part> parts;
  std::vector<glm::vec2> vertices;
  std::vector<zone_portal> portals;
  std::vector<zone_prop> props;
  std::vector<char> names;
  uint64_t fingerprint = 0;

  std::string_view name_of(const zone_record& record) const;
  std::span<const zone_part> parts_of(const zone_record& record) const;
  std::span<const glm::vec2> outline_of(const zone_part& part) const;
  std::span<const zone_portal> portals_of(const zone_part& part) const;
  std::span<const zone_portal> links_of(const zone_record& record) const;
  std::span<const zone_prop> props_of(const zone_record& record) const;
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

  // Часть в точке на заданном уровне. Пропуски — законный ответ: между зонами бывает пустота.
  part_ref pick(const glm::vec3& point_m, const zone_level level) const;

  // Предок заданного вида. Иерархия здесь — ВЛОЖЕННОСТЬ, а не масштаб: «эта выпуклая фигура — часть
  // площади, площадь в таком-то квартале, квартал в таком-то городе». На ней и держатся вопросы вроде
  // «кто держит район» — район обязан быть чем-то, на что можно показать.
  const zone_record* containing(const zone_key key, const zone_kind kind) const;

  // Внешние рёбра места: те проходы, что ведут НЕ в его собственные части. По ним ставят баррикады,
  // считают периметр и решают, где место граничит с чужим.
  std::vector<zone_portal> perimeter(const zone_key key) const;

  // Состояние переключаемых мест. Двери НЕ хранятся как особый вид связи: дверь — зона, и «заперта» это
  // её проходимость, а не свойство ребра. Поэтому одно место закрывает СРАЗУ ВСЕ проходы через себя, и
  // не приходится следить за тем, чтобы у двери с четырьмя рёбрами состояние совпало на всех четырёх.
  void set_closed(const zone_key key, const bool value);
  bool closed(const zone_key key) const;
  bool passable(const zone_record& record) const;
  uint32_t closed_count() const noexcept { return uint32_t(overrides_.size()); }

  const zone_part* part_of(const part_ref& reference) const;
  std::span<const zone_part> parts_of(const zone_record& record) const;
  std::span<const glm::vec2> outline_of(const part_ref& reference) const;
  std::span<const zone_portal> portals_of(const part_ref& reference) const;
  std::span<const zone_portal> links_of(const zone_key key) const;
  std::span<const zone_prop> props_of(const zone_key key) const;
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
  struct door_state {
    zone_key key = invalid_key;
    bool closed = false;

    bool operator<(const door_state& other) const noexcept { return key < other.key; }
  };

  std::vector<entry> resident_;
  // Рантайм-состояние переживает выгрузку сектора: закрытая дверь обязана остаться закрытой, когда
  // партия отошла на километр и вернулась. Поэтому оно живёт здесь, а не в записи зоны.
  std::vector<door_state> overrides_;
  stream_stats last_{};
};

} // namespace devils_engine::pf09

#endif
