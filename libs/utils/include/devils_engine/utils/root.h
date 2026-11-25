#ifndef DEVILS_ENGINE_UTILS_ROOT_H
#define DEVILS_ENGINE_UTILS_ROOT_H

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

// Корень файловых путей: единственное место, где допустим абсолютный путь.
//
// Конфиги, сохранения и кэш называют файлы ОТНОСИТЕЛЬНО корня, и результат не выходит за его
// пределы. Абсолютный путь задаётся один раз — при открытии корня (папка проекта, временная папка
// теста) — и там же проверяется. Так конфиг, написанный на одной машине, значит то же самое на
// другой, и ни один ключ конфига не может указать в чужой каталог.
//
// Правила resolve одинаковы на всех платформах: путь, который на Windows абсолютный (`C:\x`,
// `C:x`, `\\server`), отвергается и на Linux, хотя там это было бы просто странное имя файла, а `\`
// считается разделителем везде. Иначе один и тот же конфиг вёл бы себя по-разному.
//
// Отказ — это статус, а не исключение: критичен ли он, решает владелец конфига.

namespace devils_engine {
namespace utils {

enum class path_refusal : uint8_t {
  absolute,      // начинается с '/', '\', буквы диска или `//server`
  escapes_root,  // `..` поднимается выше корня
  invalid,       // ':' или нулевой символ: на Windows такое имя не файл
  root_not_absolute,
  root_not_directory,
};

std::string_view to_string(path_refusal refusal) noexcept;

class root {
public:
  // base — абсолютный путь к существующему каталогу.
  static std::expected<root, path_refusal> open(std::string_view base);

  // Путь внутри корня. Пустой путь и `.` — сам корень. Завершающий '/' сохраняется: им вызывающий
  // говорит, что ждёт каталог.
  std::expected<std::string, path_refusal> resolve(std::string_view relative) const;

  // Нормализованный, с '/' в качестве разделителя и с завершающим '/'.
  const std::string& path() const noexcept { return base_; }

private:
  explicit root(std::string base) noexcept : base_(std::move(base)) {}

  std::string base_;
};

} // namespace utils
} // namespace devils_engine

#endif
