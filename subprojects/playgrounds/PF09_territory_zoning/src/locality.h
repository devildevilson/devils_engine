#ifndef DEVILS_ENGINE_PF09_LOCALITY_H
#define DEVILS_ENGINE_PF09_LOCALITY_H

// Локальность: набор зон нижнего масштаба, привязанный к одному узлу глобального дерева. Город, склеп,
// замок — то, внутри чего действует партия и по чему прокладывает маршрут локальный ИИ.
//
// Это ТРЕТИЙ способ существования узла, и он появился не из вкуса, а из арифметики. Верх дерева
// (`world`..`district`) материализован таблицей; середина (`locale`, `parcel`) выводится арифметикой из
// координаты. Продолжить арифметику вниз нельзя: застройка по `26 м` — это `1.5` миллиарда ячеек, а
// помещения по `4.4 м` — `53` миллиарда, и ни то ни другое не влезает в 29 бит индекса. Но главное даже
// не это: таких зон НЕ СУЩЕСТВУЕТ на большей части мира. Комната есть там, где есть склеп; улица есть
// там, где есть город. Равномерное разбиение описывало бы пустоту с той же подробностью, что и город.
//
// Отсюда три свойства, которых нет у верхних ярусов:
//   - разрежённость: локальности стоят там, где размещены, и больше нигде;
//   - собственное пространство идентификаторов: зона адресуется парой (якорь, локальный индекс);
//   - смежность как ДАННЫЕ: наверху соседство выводится из координат, здесь оно и есть содержание —
//     улица соединяет дворы, лестница соединяет этажи, и маршрут ИИ строится по этому графу.
//
// Локальность ПЕРЕКРЫВАЕТ процедурное разбиение внутри своего пятна. Это не компромисс, а семантика:
// у города один владелец, даже если геометрически он расползся на соседние процедурные ячейки.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>

#include "territory.h"

namespace devils_engine::pf09 {

enum class locality_kind : uint32_t { none, town, crypt, castle, count };

// Роль зоны в локальности. Она нужна не картинке, а поведению: «найди работу в городе» — это обход
// публичных зон, а не всех подряд, и вход в частное владение требует повода.
enum class zone_role : uint32_t { street, yard, building, room, count };

std::string_view locality_kind_name(const locality_kind value) noexcept;
std::string_view zone_role_name(const zone_role value) noexcept;

struct locality_config {
  double extent_m = 640.0;       // сторона пятна локальности
  uint32_t plot_side = 12;       // сетка участков застройки внутри пятна
  uint32_t room_side = 3;        // сетка помещений внутри здания
  uint32_t street_stride = 3;    // каждая N-я линия сетки — улица; задаёт гарантию выхода к улице
  double extra_street_chance = 0.25;
  double building_chance = 0.75;

  // Плотность размещения по ярусу `locale`. Держится высокой для фикстуры: на настоящей карте городов
  // сильно меньше, но тогда стенд пришлось бы искать глазами.
  double town_chance = 0.08;
  double crypt_chance = 0.03;
  double castle_chance = 0.015;

  uint32_t residency = 16;          // сколько локальностей материализуем одновременно
  double residency_radius_m = 4000.0;
};

// Адрес зоны нижнего масштаба. Пара, а не одно число: локальный индекс осмыслен только внутри своего
// якоря, и склеивать их в одно 32-битное слово значило бы вернуться к переполнению, из-за которого весь
// этот способ существования и появился.
struct local_address {
  zone_id anchor = invalid_zone;
  uint32_t local = 0xffffffffu;

  bool valid() const noexcept { return anchor != invalid_zone; }
  bool operator==(const local_address&) const noexcept = default;
};

class locality {
public:
  locality(const territory& map, const locality_config& config, const zone_id anchor, const locality_kind kind);

  zone_id anchor() const noexcept { return anchor_; }
  locality_kind kind() const noexcept { return kind_; }
  glm::dvec2 centre_m() const noexcept { return centre_; }
  double extent_m() const noexcept { return config_.extent_m; }

  uint32_t zone_count() const noexcept { return uint32_t(roles_.size()); }
  zone_role role(const uint32_t local) const noexcept { return roles_[local]; }

  // Зона в точке. `local` вне пятна не существует — снаружи действует обычное процедурное разбиение.
  uint32_t zone_at(const glm::dvec2& point_m) const;
  bool contains(const glm::dvec2& point_m) const;

  std::span<const uint32_t> neighbours(const uint32_t local) const;

  uint64_t byte_size() const noexcept;

  static constexpr uint32_t invalid_local = 0xffffffffu;

private:
  uint32_t plot_of(const uint32_t local) const noexcept { return local / (room_stride_); }
  uint32_t room_of(const uint32_t local) const noexcept { return local % room_stride_; }
  uint32_t plot_zone(const uint32_t plot) const noexcept { return plot * room_stride_; }
  uint32_t room_zone(const uint32_t plot, const uint32_t room) const noexcept { return plot * room_stride_ + 1 + room; }

  void build(const territory& map);
  void connect(const uint32_t a, const uint32_t b);

  locality_config config_;
  zone_id anchor_ = invalid_zone;
  locality_kind kind_ = locality_kind::none;
  glm::dvec2 centre_{};
  uint32_t room_stride_ = 1;

  std::vector<zone_role> roles_;
  std::vector<std::vector<uint32_t>> links_;
};

// Реестр локальностей: где они стоят и какие сейчас материализованы. Размещение детерминировано и
// вычисляется, а содержимое строится только для тех, что рядом с наблюдателем.
class locality_field {
public:
  locality_field(const territory& map, const locality_config& config);

  locality_kind placed_at(const zone_id locale_node) const;

  // Материализовать всё в радиусе вокруг наблюдателя и выгрузить остальное. Возвращает, сколько
  // локальностей построено на этом вызове.
  uint32_t focus(const glm::dvec2& observer_m);

  std::span<const locality* const> resident() const noexcept { return {resident_.data(), resident_.size()}; }
  const locality* at(const glm::dvec2& point_m) const;

  local_address resolve_local(const glm::dvec2& point_m) const;
  uint64_t resident_bytes() const;

private:
  const territory* map_ = nullptr;
  locality_config config_;
  std::vector<std::unique_ptr<locality>> storage_;
  std::vector<const locality*> resident_;
};

} // namespace devils_engine::pf09

#endif
