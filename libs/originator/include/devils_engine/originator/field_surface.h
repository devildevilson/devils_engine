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
