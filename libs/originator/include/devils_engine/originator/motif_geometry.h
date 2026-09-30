#ifndef DEVILS_ENGINE_ORIGINATOR_MOTIF_GEOMETRY_H
#define DEVILS_ENGINE_ORIGINATOR_MOTIF_GEOMETRY_H

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "devils_engine/originator/motif_path.h"

// Расширение строительного словаря не должно расширять цепочку if в размещателе.
// Здесь только локальная геометрия и её общий контракт; ресурсы темы и стиль у потребителя.
// Проектный builder обязан быть детерминированным по входу; реестр не владеет PRNG или растром.

namespace devils_engine::originator {

struct motif_geometry_parameter {
  std::string name;
  int32_t value = 0;
};

struct motif_geometry_input {
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t seed = 0;
  std::span<const motif_geometry_parameter> parameters;
};

struct motif_geometry_result {
  std::vector<std::vector<motif_path_point>> pieces;
  std::string refusal;
  bool valid() const noexcept { return refusal.empty(); }
};

// Процедура создаёт только локальные выпуклые части. Назначения, тема, стиль,
// выбор соседей и пристыковка принадлежат ресурсу/сборщику, не этой операции.
using motif_geometry_builder = motif_geometry_result (*)(const motif_geometry_input&);

class motif_geometry_registry {
public:
  // Реестр собирается до генерации, затем читается без мутаций и глобального состояния.
  // Пустое имя, nullptr и повторная регистрация — ошибки разработчика (invalid_argument).
  void add(std::string name, const motif_geometry_builder builder);
  bool contains(const std::string_view name) const noexcept;
  // Неизвестная процедура, неверный вход или невалидный результат дают отказ с причиной.
  // Проверка результата общая для штатных и проектных процедур: CCW-выпуклость,
  // непересечение внутренностей и связность через положительные общие отрезки.
  motif_geometry_result build(const std::string_view name, const motif_geometry_input& input) const;
private:
  struct operation {
    std::string name;
    motif_geometry_builder builder = nullptr;
  };
  std::vector<operation> operations_;
};

// Начальный словарь: rectangle, rounded_east, diagonal. Путь остаётся отдельным
// производителем make_motif_path: он дополнительно возвращает ось и семантику портов.
void add_standard_motif_geometry(motif_geometry_registry& registry);

} // namespace devils_engine::originator

#endif
