#include <algorithm>
#include <charconv>

#include "devils_engine/utils/core.h"
#include "shader_specialization.h"

namespace devils_engine {
namespace painter {
namespace {
constexpr uint32_t spirv_magic = 0x07230203;
constexpr size_t spirv_header_words = 5;

// Подмножество SPIR-V, нужное для specialization constants.
constexpr uint32_t op_name = 5;
constexpr uint32_t op_type_bool = 20;
constexpr uint32_t op_type_int = 21;
constexpr uint32_t op_type_float = 22;
constexpr uint32_t op_spec_constant_true = 48;
constexpr uint32_t op_spec_constant_false = 49;
constexpr uint32_t op_spec_constant = 50;
constexpr uint32_t op_decorate = 71;
constexpr uint32_t decoration_spec_id = 1;

constexpr uint32_t no_spec_id = UINT32_MAX;

struct type_info {
  specialization_constant::value_kind kind;
  uint32_t size;
};

std::string_view literal_string(const std::span<const uint32_t> words) {
  const auto* chars = reinterpret_cast<const char*>(words.data());
  const size_t capacity = words.size() * sizeof(uint32_t);
  size_t length = 0;
  while (length < capacity && chars[length] != '\0') {
    ++length;
  }
  return std::string_view(chars, length);
}

// 'id_<N>' => N, иначе no_spec_id. Только эта форма, потому что имя константы тоже может
// начинаться с 'id_' — тогда сначала сработает поиск по имени.
uint32_t parse_explicit_id(const std::string_view& key) {
  constexpr std::string_view prefix = "id_";
  if (!key.starts_with(prefix) || key.size() == prefix.size()) {
    return no_spec_id;
  }
  uint32_t value = 0;
  const auto digits = key.substr(prefix.size());
  const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
    return no_spec_id;
  }
  return value;
}

std::string_view kind_to_string(const specialization_constant::value_kind kind) {
  switch (kind) {
    case specialization_constant::value_kind::boolean: return "bool";
    case specialization_constant::value_kind::signed_integer: return "int";
    case specialization_constant::value_kind::unsigned_integer: return "uint";
    case specialization_constant::value_kind::floating: return "float";
  }
  return "?";
}

template <typename T>
void append_scalar(std::vector<uint8_t>& data, const T value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  data.insert(data.end(), bytes, bytes + sizeof(T));
}

template <typename T>
T parse_number(
  const std::string_view& text,
  const std::string_view& key,
  const std::string_view& owner_hint) {
  auto trimmed = text;
  while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
    trimmed.remove_prefix(1);
  }
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
    trimmed.remove_suffix(1);
  }
  if (trimmed.starts_with('+')) {
    trimmed.remove_prefix(1);
  }

  T value{};
  const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
  if (result.ec != std::errc{} || result.ptr != trimmed.data() + trimmed.size()) {
    utils::error{}(
      "Shader constant '{}' of '{}': could not parse '{}' as {}",
      key,
      owner_hint,
      text,
      kind_to_string(
        std::is_floating_point_v<T>
          ? specialization_constant::value_kind::floating
          : (std::is_signed_v<T>
               ? specialization_constant::value_kind::signed_integer
               : specialization_constant::value_kind::unsigned_integer)));
  }
  return value;
}

void append_value(
  std::vector<uint8_t>& data,
  const specialization_constant& constant,
  const std::string_view& key,
  const std::string_view& text,
  const std::string_view& owner_hint) {
  switch (constant.kind) {
    case specialization_constant::value_kind::boolean: {
      // VkBool32: принимаем и текст, и 0/1 — конфиги пишут и так, и так.
      uint32_t value = 0;
      if (text == "true" || text == "1") {
        value = 1;
      } else if (text == "false" || text == "0") {
        value = 0;
      } else {
        utils::error{}(
          "Shader constant '{}' of '{}': could not parse '{}' as bool (expected true/false/1/0)",
          key,
          owner_hint,
          text);
      }
      append_scalar(data, value);
      return;
    }

    case specialization_constant::value_kind::signed_integer: {
      if (constant.size == sizeof(int32_t)) {
        append_scalar(data, parse_number<int32_t>(text, key, owner_hint));
      } else {
        append_scalar(data, parse_number<int64_t>(text, key, owner_hint));
      }
      return;
    }

    case specialization_constant::value_kind::unsigned_integer: {
      if (constant.size == sizeof(uint32_t)) {
        append_scalar(data, parse_number<uint32_t>(text, key, owner_hint));
      } else {
        append_scalar(data, parse_number<uint64_t>(text, key, owner_hint));
      }
      return;
    }

    case specialization_constant::value_kind::floating: {
      if (constant.size == sizeof(float)) {
        append_scalar(data, parse_number<float>(text, key, owner_hint));
      } else {
        append_scalar(data, parse_number<double>(text, key, owner_hint));
      }
      return;
    }
  }

  utils::error{}("Shader constant '{}' of '{}' has unsupported type", key, owner_hint);
}
} // namespace

std::vector<specialization_constant> reflect_specialization_constants(
  const std::span<const uint32_t> spirv,
  const std::string_view& owner_hint) {
  std::vector<specialization_constant> constants;
  if (spirv.empty()) {
    return constants;
  }

  if (spirv.size() < spirv_header_words || spirv[0] != spirv_magic) {
    utils::error{}("Shader '{}': not a SPIR-V module (bad magic or truncated header)", owner_hint);
  }

  // id -> данные аннотаций/типов. Аннотации и типы в SPIR-V объявлены до констант, которые их
  // используют, поэтому одного линейного прохода достаточно.
  std::vector<std::string_view> names;
  std::vector<uint32_t> spec_ids;
  std::vector<type_info> types;
  std::vector<uint8_t> has_type;

  const auto ensure_size = [](auto& container, const uint32_t id, const auto& fill) {
    if (id >= container.size()) {
      container.resize(size_t(id) + 1, fill);
    }
  };

  size_t offset = spirv_header_words;
  while (offset < spirv.size()) {
    const uint32_t instruction = spirv[offset];
    const uint32_t word_count = instruction >> 16;
    const uint32_t opcode = instruction & 0xffffu;
    if (word_count == 0 || offset + word_count > spirv.size()) {
      utils::error{}("Shader '{}': malformed SPIR-V instruction at word {}", owner_hint, offset);
    }
    const auto operands = spirv.subspan(offset + 1, word_count - 1);

    switch (opcode) {
      case op_name: {
        if (operands.size() >= 2) {
          const uint32_t target = operands[0];
          ensure_size(names, target, std::string_view{});
          names[target] = literal_string(operands.subspan(1));
        }
        break;
      }

      case op_decorate: {
        if (operands.size() >= 3 && operands[1] == decoration_spec_id) {
          const uint32_t target = operands[0];
          ensure_size(spec_ids, target, no_spec_id);
          spec_ids[target] = operands[2];
        }
        break;
      }

      case op_type_bool: {
        if (!operands.empty()) {
          const uint32_t result = operands[0];
          ensure_size(types, result, type_info{});
          ensure_size(has_type, result, uint8_t(0));
          types[result] = type_info{specialization_constant::value_kind::boolean, sizeof(uint32_t)};
          has_type[result] = 1;
        }
        break;
      }

      case op_type_int: {
        if (operands.size() >= 3) {
          const uint32_t result = operands[0];
          const uint32_t width = operands[1];
          const bool is_signed = operands[2] != 0;
          ensure_size(types, result, type_info{});
          ensure_size(has_type, result, uint8_t(0));
          types[result] = type_info{
            is_signed ? specialization_constant::value_kind::signed_integer
                      : specialization_constant::value_kind::unsigned_integer,
            width / 8};
          has_type[result] = 1;
        }
        break;
      }

      case op_type_float: {
        if (operands.size() >= 2) {
          const uint32_t result = operands[0];
          ensure_size(types, result, type_info{});
          ensure_size(has_type, result, uint8_t(0));
          types[result] = type_info{specialization_constant::value_kind::floating, operands[1] / 8};
          has_type[result] = 1;
        }
        break;
      }

      case op_spec_constant_true:
      case op_spec_constant_false:
      case op_spec_constant: {
        if (operands.size() < 2) {
          break;
        }
        const uint32_t type_id = operands[0];
        const uint32_t result = operands[1];
        const uint32_t spec_id = result < spec_ids.size() ? spec_ids[result] : no_spec_id;
        if (spec_id == no_spec_id) {
          break; // константа без SpecId специализации не подлежит
        }
        if (type_id >= has_type.size() || has_type[type_id] == 0) {
          utils::error{}(
            "Shader '{}': specialization constant id {} has unsupported or unknown type",
            owner_hint,
            spec_id);
        }
        const auto& type = types[type_id];
        if (type.size != 4 && type.size != 8) {
          utils::error{}(
            "Shader '{}': specialization constant id {} has unsupported size {}",
            owner_hint,
            spec_id,
            type.size);
        }

        specialization_constant constant{};
        constant.name = result < names.size() ? std::string(names[result]) : std::string();
        constant.constant_id = spec_id;
        constant.size = type.size;
        constant.kind = type.kind;
        constants.push_back(std::move(constant));
        break;
      }

      default: break;
    }

    offset += word_count;
  }

  return constants;
}

void merge_specialization_names(
  std::vector<specialization_constant>& target,
  const std::span<const specialization_constant> named_source) {
  for (auto& constant : target) {
    if (!constant.name.empty()) {
      continue;
    }
    const auto named = std::find_if(named_source.begin(), named_source.end(), [&](const auto& entry) {
      return entry.constant_id == constant.constant_id && !entry.name.empty();
    });
    if (named != named_source.end()) {
      constant.name = named->name;
    }
  }
}

bool is_explicit_specialization_id(const std::string_view& key) noexcept {
  return parse_explicit_id(key) != no_spec_id;
}

specialization_blob build_specialization_blob(
  const std::span<const specialization_constant> reflected,
  const std::span<const std::pair<std::string, std::string>> requested,
  const std::string_view& owner_hint,
  std::vector<bool>* matched) {
  specialization_blob blob;
  if (requested.empty() || reflected.empty()) {
    return blob;
  }

  for (size_t index = 0; index < requested.size(); ++index) {
    const auto& [key, text] = requested[index];
    const auto by_name = std::find_if(reflected.begin(), reflected.end(), [&](const auto& constant) {
      return !constant.name.empty() && constant.name == key;
    });

    auto found = by_name;
    if (found == reflected.end()) {
      const uint32_t explicit_id = parse_explicit_id(key);
      if (explicit_id != no_spec_id) {
        found = std::find_if(reflected.begin(), reflected.end(), [&](const auto& constant) {
          return constant.constant_id == explicit_id;
        });
      }
    }

    if (found == reflected.end()) {
      continue; // константы нет в ЭТОЙ стадии — нормальная ситуация
    }

    const auto duplicate = std::find_if(blob.entries.begin(), blob.entries.end(), [&](const auto& entry) {
      return entry.constant_id == found->constant_id;
    });
    if (duplicate != blob.entries.end()) {
      utils::error{}(
        "Shader constant '{}' of '{}' resolves to specialization id {}, which is already set",
        key,
        owner_hint,
        found->constant_id);
    }

    // Значения размера 8 выравниваем по 8: спека это не требует, но драйверы читают data+offset.
    const uint32_t alignment = found->size;
    const uint32_t offset = uint32_t(utils::align_to(blob.data.size(), size_t(alignment)));
    blob.data.resize(offset, 0);
    append_value(blob.data, *found, key, text, owner_hint);
    blob.entries.push_back(specialization_blob::entry{found->constant_id, offset, found->size});
    if (matched != nullptr && index < matched->size()) {
      (*matched)[index] = true;
    }
  }

  std::sort(blob.entries.begin(), blob.entries.end(), [](const auto& a, const auto& b) {
    return a.constant_id < b.constant_id;
  });

  return blob;
}

std::string describe_specialization_constants(const std::span<const specialization_constant> reflected) {
  std::string out;
  for (const auto& constant : reflected) {
    if (!out.empty()) {
      out += ", ";
    }
    if (constant.name.empty()) {
      out += "id_" + std::to_string(constant.constant_id);
    } else {
      out += constant.name + "(id_" + std::to_string(constant.constant_id) + ")";
    }
    out += ":";
    out += kind_to_string(constant.kind);
    out += std::to_string(constant.size * 8);
  }
  return out.empty() ? std::string("<none>") : out;
}

} // namespace painter
} // namespace devils_engine
