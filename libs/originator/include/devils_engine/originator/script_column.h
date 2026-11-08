#ifndef DEVILS_ENGINE_ORIGINATOR_SCRIPT_COLUMN_H
#define DEVILS_ENGINE_ORIGINATOR_SCRIPT_COLUMN_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "workspace.h"

// КОЛОНОЧНЫЙ ИСПОЛНИТЕЛЬ `devils_script`: та же программа, но не элемент за элементом через
// виртуальную машину, а СТОЛБЦАМИ — каждая операция проходит сразу по пакету значений.
//
// Это третий потребитель одного разбора (после ВМ ds и перевода в GLSL): обход идёт по AST, который
// отдаёт сам ds (`make_script_ast`), и подмножество то же, что у перевода, — арифметика, сравнения,
// математика ds, `value_or`, ветвления `select` / `sequence` / `switch`, локали `ctx_save`. Считает в
// double, как ds. Ветвления считают ОБЕ ветви и выбирают по маске: программа чистая (ни записи, ни
// случайности), поэтому ответ тот же, а столбец остаётся без ветвлений внутри цикла.
//
// Имена делятся на два рода, и это главное отличие от ВМ: СТОЛБЕЦ (своё значение у каждой точки
// пакета — координата, шум) и СКАЛЯР (одно значение на весь пакет — поле строки формы). Скаляр не
// разворачивается в столбец заранее и поднимается из цикла: ради этого пакет и существует.
//
// Не переводится: случайность (`chance`, `random` — у пакета нет зерна элемента), списки,
// диагностика, `execute`. `ctx:arg:name` допускается только как явно объявленный scalar.
// try_compile возвращает причину отказа; strict compile поднимает utils::error.

namespace devils_engine {
namespace originator {

class column_program {
public:
  // Сколько значений в пакете за один проход.
  static constexpr size_t lanes = column_workspace::lanes;

  ~column_program() noexcept;
  column_program(const column_program&) = delete;
  column_program& operator=(const column_program&) = delete;

  // Компилирует текст против имён столбцов и скаляров. Неизвестное имя — отказ.
  static std::expected<std::unique_ptr<column_program>, std::string> try_compile(
                                                 const std::string_view& name, const std::string_view& source,
                                                 const std::span<const std::string> columns,
                                                 const std::span<const std::string> scalars);
  static std::unique_ptr<column_program> compile(const std::string_view& name, const std::string_view& source,
                                                 const std::span<const std::string> columns,
                                                 const std::span<const std::string> scalars);

  const std::string& name() const noexcept;
  // Какие столбцы программа читает: остальные вызывающий может не готовить.
  const std::vector<uint8_t>& used_columns() const noexcept;
  size_t workspace_bytes() const noexcept;

  // n <= lanes значений; columns[i] — n значений столбца i (неиспользуемые могут быть nullptr),
  // scalars[i] — скаляр i; out — n результатов.
  void run(const size_t n, const std::span<const double* const> columns, const std::span<const double> scalars,
           double* out, column_workspace& workspace) const;

  // One-shot convenience: no capacity survives this call. Repeated calls use the explicit workspace.
  void run_once(const size_t n, const std::span<const double* const> columns, const std::span<const double> scalars,
           double* out) const;

  struct implementation;

private:
  explicit column_program(std::unique_ptr<implementation> state) noexcept;
  std::unique_ptr<implementation> state_;
};

} // namespace originator
} // namespace devils_engine

#endif
