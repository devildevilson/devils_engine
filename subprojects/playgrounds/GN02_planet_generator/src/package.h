#ifndef DEVILS_ENGINE_GN02_PACKAGE_H
#define DEVILS_ENGINE_GN02_PACKAGE_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "devils_engine/originator/pipeline.h"

// Пакет планеты: результат генерации, лежащий на диске.
//
// Генератор планеты одноразовый. Он считает мир один раз, а играет проект уже с готовых данных,
// поэтому «дамп» здесь не отладочный вывод, а ЕДИНСТВЕННЫЙ выход площадки: всё, что не попало в
// пакет, не существует для потребителя.
//
// Формат намеренно тупой: заголовок, список секций, байты буферов как они лежат в памяти. Причина в
// том, что originator уже описывает раскладку буфера типами полей, и второе описание тех же данных
// было бы местом, где два описания разъезжаются. Секция несёт СВОЮ раскладку, поэтому потребитель
// читает поля по именам и не обязан знать порядок, в котором их объявил генератор.
//
// Чего формат не делает: не сжимает, не переставляет байты под другой порядок и не претендует на
// роль движкового формата ресурсов. Запечатывание пакета — решение проекта, а не движка; здесь
// лежит рабочая форма для площадки и её проверок.

namespace devils_engine::gn02 {

struct package_field {
  std::string name;
  uint8_t base = 0;
  uint8_t components = 0;
};

struct package_section {
  std::string name;
  uint32_t storage = 0;
  uint64_t count = 0;
  std::vector<package_field> fields;
  std::vector<std::byte> bytes;
};

struct package {
  uint32_t version = 0;
  uint64_t seed = 0;
  uint64_t cell_count = 0;
  uint64_t fingerprint = 0;
  std::vector<package_section> sections;

  const package_section* find(const std::string_view& name) const noexcept;
};

// Отпечаток пакета: хеш от раскладок и байтов всех секций. Он и есть ответ на вопрос «тот же это
// мир или другой»: сравнивать два пакета по одному числу дешевле, чем по гигабайту, а зерно на этот
// вопрос не отвечает — при смене формул зерно то же, а мир другой.
uint64_t fingerprint_of(const std::vector<package_section>& sections) noexcept;

// Собирает пакет из буферов пайплайна. Список секций задаётся именами буферов: что попадает в
// пакет, решает конфиг, а не полнота перечисления в коде.
package build_package(originator::pipeline& source, const std::span<const std::string>& section_names,
                      const uint64_t seed, const uint64_t cell_count);

void write_package(const package& value, const std::filesystem::path& path);
package read_package(const std::filesystem::path& path);

} // namespace devils_engine::gn02

#endif
