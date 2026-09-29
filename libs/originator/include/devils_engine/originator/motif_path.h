#ifndef DEVILS_ENGINE_ORIGINATOR_MOTIF_PATH_H
#define DEVILS_ENGINE_ORIGINATOR_MOTIF_PATH_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "devils_engine/originator/motif_layout.h"

// Полоса одной ширины вокруг осевой линии, а не сборка независимо заданных комнат.
// Выпуклые части делят одну кромку на повороте; дробление не меняет проход.
// Координаты плана целые: нормали считаются в fixed point, округление даёт погрешность
// ширины до одной меры. Прямоугольники — предварительные резервы, не точные границы.

namespace devils_engine::originator {

struct motif_path_point {
  int32_t x = 0;
  int32_t y = 0;
  bool operator==(const motif_path_point&) const noexcept = default;
};

enum class motif_path_mode : uint8_t { offset, corner };

struct motif_path_parameters {
  uint32_t length = 0;
  uint32_t width = 0;
  uint32_t l1_per_mille = 550;
  uint32_t l2_per_mille = 250;
  int32_t bend = 0; // offset: смещение в мерах; corner: только знак поворота
  motif_path_mode mode = motif_path_mode::offset;
  uint32_t min_bend_length = 1; // ограничение автора, а не скрытое ограничение шириной
};

enum class motif_path_port_kind : uint8_t { start, end, wall };

struct motif_path_port {
  uint32_t piece = 0;
  uint32_t edge = 0;
  motif_path_port_kind kind = motif_path_port_kind::wall;
};

struct motif_path_piece {
  std::vector<motif_path_point> outline;
  motif_rect reservation;
  uint8_t side = 0;
  motif_attach_align align = motif_attach_align::start;
};

struct motif_path_shape {
  std::vector<motif_path_point> axis2; // ось в УДВОЕННЫХ мерах: нечётная ширина имеет полуцелый центр
  std::vector<motif_path_piece> pieces;
  std::vector<motif_path_port> ports; // внутренние miter-сечения здесь отсутствуют
  std::string refusal;
  motif_path_parameters parameters;
  uint32_t width = 0;
  bool valid() const noexcept { return refusal.empty(); }
};

// offset: length — продольный размах по X (ломаная ось при M != 0 длиннее).
// L1/L2 — доли прямых начала/конца этого размаха, остаток отдан диагонали; сумма >= 1000
// или M == 0 даёт прямую. corner: L2 выводится как остаток после L1, знак M задаёт 90°.
// Начало направлено по +X; +M — вправо от него (+Y). Поворот всего места принадлежит потребителю.
motif_path_shape make_motif_path(const motif_path_parameters& parameters);

// SAT над выпуклыми CCW-полигонами. Касание ребра/вершины НЕ является пересечением внутренностей.
bool convex_interiors_overlap(const std::span<const motif_path_point> a,
                              const std::span<const motif_path_point> b) noexcept;

// Порты работают в единицах переданных координат: потребитель может выбрать мелкую
// целочисленную сетку независимо от растра. Концы лежат ТОЧНО на ребре (его целочисленная
// решётка), а не на округлённой параллели. position — доля свободного участка после margin.
// Для портов |координаты|, размеры и margin ограничены 1'048'576; отказ несёт причину.
struct motif_edge_port {
  motif_path_point a;
  motif_path_point b;
  std::string refusal;
  bool valid() const noexcept { return refusal.empty(); }
};

motif_edge_port make_motif_edge_port(const std::span<const motif_path_point> outline,
                                    const uint32_t edge, const uint32_t width,
                                    const uint32_t position_per_mille = 500,
                                    const uint32_t margin = 0);

struct motif_port_attachment {
  std::vector<motif_path_point> passage; // пусто у открытого стыка
  std::vector<motif_path_point> body;
  std::string refusal;
  bool valid() const noexcept { return refusal.empty(); }
};

// Прямоугольный блок в системе кромки: его входная стена параллельна стене родителя,
// проём центрирован на ней. Глубина прохода == 0 означает прямой стык без зоны-двери.
motif_port_attachment attach_motif_rect(const motif_edge_port& port, const uint32_t frontage,
                                       const uint32_t depth, const uint32_t passage_depth);

// Общий отрезок положительной длины. Касания точкой и соседних клеток недостаточно.
bool share_motif_boundary(const std::span<const motif_path_point> a,
                          const std::span<const motif_path_point> b) noexcept;

struct motif_boundary_segment {
  motif_path_point a;
  motif_path_point b;
};

// Точные общие отрезки, не целые рёбра соседей. При обрезке пути защищать надо именно
// их: короткий открытый стык может лежать посреди длинной боковины.
std::vector<motif_boundary_segment> shared_motif_boundary(
  const std::span<const motif_path_point> a, const std::span<const motif_path_point> b);

struct motif_path_trim {
  std::string refusal;
  std::vector<motif_path_point> outline;
  uint32_t removed = 0; // в единицах переданных координат
  bool valid() const noexcept { return refusal.empty(); }
};

// Сдвигает только внешний осевой торец четырёхугольной части, не miter и не соседние части.
// protected_points — концы всех используемых общих отрезков; clearance сохраняет отступ
// после них, minimum_length — целый остаток части после её внутреннего сечения.
// Занятый торец не сокращается. Размеры и |координаты| <= 1'048'576, minimum_length > 0.
motif_path_trim trim_motif_path_end(const std::span<const motif_path_point> outline,
                                   const uint32_t cap_edge,
                                   const std::span<const motif_path_point> protected_points,
                                   const uint32_t clearance, const uint32_t minimum_length);

} // namespace devils_engine::originator

#endif
