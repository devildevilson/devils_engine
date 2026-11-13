#ifndef DEVILS_ENGINE_ORIGINATOR_FIELD_SURFACE_H
#define DEVILS_ENGINE_ORIGINATOR_FIELD_SURFACE_H
#include "primitive_field.h"
// Локальная детализация поля: области уточнения задаёт вызывающий генератор. Грани
// и рёбра согласуются с соседями; коробки должны покрывать пояс в одну грубую клетку.
// Пределы внутренних таблиц — явные ёмкости, чтобы случайный большой запрос не
// развернул миллиарды тетраэдров до проверки выходного буфера.
namespace devils_engine::originator {
struct field_surface_options {
  field_vector origin{};
  double cell_size = 1;
  double normal_step = 0.05;
  double detail_normal_step = 0;
  size_t vertex_capacity = 262144;
  size_t point_capacity = 2097152;
  size_t tetrahedron_capacity = 8388608;
  uint32_t cells = 16;
  uint32_t refinement = 4;
  // Клетка без уточнения ни в ней, ни у соседей по грани и ребру: false — центр и центры граней
  // (24 тетраэдра, 15 точек), true — 6 тетраэдров Куна по углам решётки. Диагональ Куна на каждой грани
  // идёт от меньшего угла к большему, и переходная клетка режет такую грань той же диагональю.
  bool kuhn_plain = false;
  // Пропуск пустых блоков (нужен kuhn_plain): блок skip_block^3 простых клеток, у которого все 8 углов
  // одного знака и дальше от нуля, чем наибольший объявленный наклон поля в блоке (region::slopes),
  // умноженный на половину диагонали блока, поверхности не содержит — его внутренние точки не считаются.
  // 0 — выключено. Сетка при честных наклонах та же бит в бит; пара соседних посчитанных точек, чья
  // разность больше наклона на шаг, — громкий отказ (наклон формы занижен). Число делит cells.
  uint32_t skip_block = 0;
  // Полоса непрерывности: коробка заливки или вставки обрывает форму только ниже -band, коробка выреза —
  // только выше band (так выбран их reach); природные формы за коробкой на поле не влияют. В блоке поле
  // обрезается снизу, если его задевает заливка, и сверху, если вырез: обрезанное поле непрерывно, и
  // пропуск и проверка наклона ведутся по нему.
  double band = std::numeric_limits<double>::infinity();
  // Нормали (нужен kuhn_plain): у вершины на ребре между углами решётки дальше normal_step от всех
  // коробок — центральные разности значений решётки (шаг — клетка) в концах ребра, интерполированные по
  // ребру; выборка в самой вершине одна, для метки. false — семь выборок: метка и разности с шагом
  // normal_step в самой вершине.
  bool lattice_normals = false;
};
struct field_surface_vertex {
  field_vector position{}, normal{};
  uint32_t tag = 0;
};
std::vector<field_surface_vertex> make_field_surface(const primitive_field& field,
                                                     const field_surface_options& options, std::span<const field_box> refinement_boxes);
size_t field_surface_footprint(const tool_call& call);
void tool_field_surface(const tool_call& call, size_t begin, size_t end);
} // namespace devils_engine::originator
#endif
