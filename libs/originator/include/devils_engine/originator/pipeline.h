#ifndef DEVILS_ENGINE_ORIGINATOR_PIPELINE_H
#define DEVILS_ENGINE_ORIGINATOR_PIPELINE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "buffer.h"
#include "tools.h"

// Пайплайн генерации: буферы и шаги, объявленные в конфиге.
//
// Из привязок шага (reads/writes) выводится всё остальное — так же, как painter выводит барьеры из
// биндингов шага, а не из ручного списка:
//   - порядок и зависимости шагов;
//   - что именно доступно потребителю после шага N (это ровно writes);
//   - изменяемость вью, которую получит тело шага;
//   - громкий отказ до исполнения, если шаг читает то, что никто не писал.
//
// Тело шага пайплайну неизвестно: он отдаёт наружу контекст, а вызывающий (обычно lua) исполняет.
// Поэтому библиотека не тянет за собой ни lua, ни devils_script.

namespace devils_engine {
namespace thread {
class atomic_pool;
}

namespace originator {

// Объявление буфера из конфига. Размер задаётся ИМЕНЕМ константы, а не числом: генератор должен
// уметь назвать свою стоимость по памяти до запуска, и это же имя связывает буферы одного масштаба.
struct buffer_description {
  std::string name;
  buffer_layout layout;
  std::string size_name;
};

struct step_description {
  std::string name;
  std::string body; // demiurg-путь скрипта, тело шага
  std::vector<std::string> reads;
  std::vector<std::string> writes;
  parameters params;
  // Программы devils_script, доступные телу шага по имени. Тело решает, к каким полям их применить;
  // имена входов берутся у самих привязок, поэтому в скрипте поле зовётся ровно так же, как в конфиге.
  std::vector<std::pair<std::string, std::string>> programs;
};

struct pipeline_description {
  std::string name;
  std::vector<buffer_description> buffers;
  std::vector<step_description> steps;
};

// Разбор конфига. Оба принимают текст одного документа tavl; список буферов пайплайна не
// объявляется отдельно — он выводится из шагов и сверяется с объявленными буферами.
std::vector<buffer_description> parse_buffers(const std::string_view& text, const std::string_view& label);
std::vector<step_description> parse_steps(const std::string_view& text, const std::string_view& label);

// Именованные размеры, которые host подставляет в буферы.
class size_table {
public:
  void set(const std::string_view& name, const size_t value);
  bool has(const std::string_view& name) const noexcept;
  size_t get(const std::string_view& name, const std::string_view& buffer_name) const;

private:
  std::vector<std::pair<std::string, size_t>> entries_;
};

// Что тело шага видит на входе.
struct step_context {
  std::string_view name;
  size_t index = 0;
  uint64_t seed = 0; // hash(зерно пайплайна, имя шага): не поток, а функция от имени
  const parameters* params = nullptr;
  std::span<const std::pair<std::string, std::string>> programs;

  // Привязки шага. Буфер в writes доступен и на чтение, буфер в reads — только на чтение.
  std::span<buffer* const> writes;
  std::span<const buffer* const> reads;

  buffer* find_write(const std::string_view& buffer_name) const noexcept;
  const buffer* find_read(const std::string_view& buffer_name) const noexcept;
};

using step_invoker = std::function<void(const step_context&)>;

class pipeline {
public:
  pipeline(pipeline_description description, const size_table& sizes, const uint64_t seed);

  const std::string& name() const noexcept;
  uint64_t seed() const noexcept;

  size_t buffer_count() const noexcept;
  buffer& buffer_at(const size_t index) noexcept;
  buffer* find_buffer(const std::string_view& buffer_name) noexcept;
  size_t total_byte_size() const noexcept;

  size_t step_count() const noexcept;
  const step_description& step_at(const size_t index) const noexcept;

  // Буферы, которые стали валидными после исполнения шага index. Это ровно writes шага — отсюда же
  // берётся ответ на вопрос «что уже можно отдать наружу» в фоновом режиме.
  std::span<buffer* const> published_after(const size_t index) const noexcept;

  // Исполняет шаги по порядку, вызывая invoker на каждом.
  void run(const step_invoker& invoker);
  void run_step(const size_t index, const step_invoker& invoker);

private:
  void build(const size_table& sizes);
  void validate() const;

  pipeline_description description_;
  uint64_t seed_ = 0;
  std::vector<std::unique_ptr<buffer>> buffers_;
  std::vector<std::vector<buffer*>> step_writes_;
  std::vector<std::vector<const buffer*>> step_reads_;
};

} // namespace originator
} // namespace devils_engine

#endif
