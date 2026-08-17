#ifndef DEVILS_ENGINE_PAINTER_SHADER_SPECIALIZATION_H
#define DEVILS_ENGINE_PAINTER_SHADER_SPECIALIZATION_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace devils_engine {
namespace painter {

// Одна specialization constant, найденная в SPIR-V шейдера.
// Имя может быть пустым: OpName необязателен (его нет у компонентов local_size_*_id и он может быть
// снят оптимизацией), поэтому адресация по 'id_<N>' обязана работать всегда.
struct specialization_constant {
  enum class value_kind : uint32_t {
    boolean,
    signed_integer,
    unsigned_integer,
    floating,
  };

  std::string name;
  uint32_t constant_id;
  uint32_t size; // байт: 4 либо 8
  value_kind kind;
};

// Линейный проход по SPIR-V: собирает все OpSpecConstant/OpSpecConstantTrue/False с decoration SpecId.
// OpSpecConstantComposite/OpSpecConstantOp специализировать нельзя — они выводятся из своих операндов,
// поэтому в результат не попадают. Некорректный поток слов приводит к loud error.
std::vector<specialization_constant> reflect_specialization_constants(
  const std::span<const uint32_t> spirv,
  const std::string_view& owner_hint = {});

// Переносит имена из отдельно собранной (с debug info) копии того же шейдера в reflection готового
// модуля: spirv-opt снимает OpName, поэтому в оптимизированном модуле имён обычно нет, а SpecId
// авторские и совпадают. Константы, отсутствующие в источнике имён, остаются безымянными.
void merge_specialization_names(
  std::vector<specialization_constant>& target,
  const std::span<const specialization_constant> named_source);

// true для ключа конфига формы 'id_<N>'.
bool is_explicit_specialization_id(const std::string_view& key) noexcept;

// Готовые данные для VkSpecializationInfo одной стадии.
struct specialization_blob {
  struct entry {
    uint32_t constant_id;
    uint32_t offset;
    uint32_t size;
  };

  std::vector<uint8_t> data;
  std::vector<entry> entries;

  bool empty() const noexcept { return entries.empty(); }
};

// Сопоставляет запрошенные (имя|'id_<N>', текст значения) с reflected константами стадии и парсит
// значения по РЕФЛЕКТИРОВАННОМУ типу. Запрошенные имена, которых в этой стадии нет, молча
// пропускаются (стадии видят разные наборы констант) и НЕ отмечаются в matched; вызывающая сторона
// обязана сама сообщить о константе, не найденной ни в одной стадии.
// Ошибки loud: неразбираемое значение, дубликат constant_id, неизвестный размер.
specialization_blob build_specialization_blob(
  const std::span<const specialization_constant> reflected,
  const std::span<const std::pair<std::string, std::string>> requested,
  const std::string_view& owner_hint,
  std::vector<bool>* matched = nullptr);

// Описание найденных констант для сообщений об ошибках: "pcf_radius(id 0, uint), id 3(float)".
std::string describe_specialization_constants(const std::span<const specialization_constant> reflected);

} // namespace painter
} // namespace devils_engine

#endif
