#ifndef GN07_TAVL_READER_H
#define GN07_TAVL_READER_H

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <tavl/parser.h>

// Чтение tavl по событиям, а не отражением. Причина одна: поле-предикат (`where = { ... }`)
// должен разобрать devils_script прямо из этого же парсера, и отражению такое поле не отдать.
// Поэтому архетип и задание читаются одинаково — строка за строкой, с явным потребителем поля.
//
// Контракт потребителя поля: он обязан съесть ровно одно значение. Конец строки он может
// съесть или оставить — чтец пропускает `row_end` в обоих случаях (ds съедает его сам).
// Любое неожиданное событие и любая критическая ошибка tavl — исключение с позицией и файлом.

namespace gn07 {

class tavl_reader {
public:
  using field_fn = std::function<void(std::string_view field)>;

  tavl_reader(tavl::parser& parser, std::string source_name);

  tavl::parser& parser() noexcept { return parser_; }
  const std::string& source_name() const noexcept { return source_; }

  // Корень документа: строки до eof.
  void read_document(const field_fn& field);
  // `{ ... }`: строки до object_end.
  void read_object(const field_fn& field);
  // `[ ... ]`: каждый элемент отдаётся потребителю, который обязан его съесть.
  void read_array(const std::function<void()>& element);

  std::string read_string();
  int64_t read_int();
  std::vector<std::string> read_string_list();
  std::vector<int64_t> read_int_list();
  // `[min, max]` с min <= max.
  void read_range(int64_t& min, int64_t& max);
  // Число с дробью как фиксированная точка: `2.5` при scale 100 — 250. Без double, чтобы
  // данные одинаково читались везде; дробных знаков не больше, чем позволяет scale.
  int64_t read_fixed(int64_t scale);
  void read_fixed_range(int64_t scale, int64_t& min, int64_t& max);
  void skip_value();

  // Позиция следующего события — для сообщений об ошибках.
  std::string where();
  [[noreturn]] void fail(std::string_view message);

private:
  tavl::event next();
  tavl::event peek();
  void read_rows(tavl::event_type terminator, const field_fn& field);

  tavl::parser& parser_;
  std::string source_;
};

} // namespace gn07

#endif
