#include "package.h"

#include <cstring>
#include <fstream>

#include "devils_engine/utils/core.h"

namespace devils_engine::gn02 {

namespace {

constexpr char package_magic[8] = {'G', 'N', '0', '2', 'P', 'L', 'N', 'T'};
constexpr uint32_t package_version = 1;

uint64_t mix64(uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

uint64_t hash_bytes(uint64_t accumulated, const void* data, const size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  size_t i = 0;
  // Хеш идёт по восемь байт, потому что иначе отпечаток гигабайтного пакета стоит дороже самой
  // генерации; хвост добирается побайтово.
  for (; i + 8 <= size; i += 8) {
    uint64_t word = 0;
    std::memcpy(&word, bytes + i, sizeof(word));
    accumulated = mix64(accumulated ^ word);
  }
  uint64_t tail = 0;
  for (; i < size; ++i) {
    tail = (tail << 8) | bytes[i];
  }
  return mix64(accumulated ^ tail);
}

template <typename type_t>
void write_value(std::ostream& stream, const type_t value) {
  stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename type_t>
type_t read_value(std::istream& stream, const std::filesystem::path& path) {
  type_t value{};
  stream.read(reinterpret_cast<char*>(&value), sizeof(value));
  if (!stream) {
    utils::error{}("GN02 package '{}': file ends in the middle of a value", path.string());
  }
  return value;
}

void write_string(std::ostream& stream, const std::string& value) {
  write_value<uint32_t>(stream, uint32_t(value.size()));
  stream.write(value.data(), std::streamsize(value.size()));
}

std::string read_string(std::istream& stream, const std::filesystem::path& path) {
  const auto size = read_value<uint32_t>(stream, path);
  if (size > (1u << 16)) {
    utils::error{}("GN02 package '{}': a name of {} bytes cannot be right", path.string(), size);
  }
  std::string value(size, '\0');
  stream.read(value.data(), std::streamsize(size));
  if (!stream) {
    utils::error{}("GN02 package '{}': file ends in the middle of a name", path.string());
  }
  return value;
}

} // namespace

const package_section* package::find(const std::string_view& name) const noexcept {
  for (const auto& section : sections) {
    if (section.name == name) {
      return &section;
    }
  }
  return nullptr;
}

uint64_t fingerprint_of(const std::vector<package_section>& sections) noexcept {
  uint64_t value = 0x5f2a1c0d7b3e9146ull;
  for (const auto& section : sections) {
    // В отпечаток идут и МЕТАДАННЫЕ, а не только байты: переименованное поле или сменившая род
    // раскладка — это другой пакет, даже если числа в памяти совпали.
    value = hash_bytes(value, section.name.data(), section.name.size());
    value = mix64(value ^ uint64_t(section.storage));
    value = mix64(value ^ section.count);
    for (const auto& field : section.fields) {
      value = hash_bytes(value, field.name.data(), field.name.size());
      value = mix64(value ^ (uint64_t(field.base) << 8 | uint64_t(field.components)));
    }
    value = hash_bytes(value, section.bytes.data(), section.bytes.size());
  }
  return value;
}

package build_package(originator::pipeline& source, const std::span<const std::string>& section_names,
                      const uint64_t seed, const uint64_t cell_count) {
  package result;
  result.version = package_version;
  result.seed = seed;
  result.cell_count = cell_count;

  for (const auto& name : section_names) {
    auto* buffer = source.find_buffer(name);
    if (buffer == nullptr) {
      utils::error{}("GN02: package section '{}' names a buffer that the pipeline does not declare", name);
    }

    package_section section;
    section.name = name;
    section.storage = uint32_t(buffer->layout().storage);
    section.count = buffer->count();
    for (const auto& field : buffer->layout().fields) {
      section.fields.push_back(package_field{field.name, uint8_t(field.type.base), uint8_t(field.type.components)});
    }

    section.bytes.resize(buffer->byte_size());
    std::memcpy(section.bytes.data(), buffer->base_pointer(), section.bytes.size());
    result.sections.push_back(std::move(section));
  }

  result.fingerprint = fingerprint_of(result.sections);
  return result;
}

void write_package(const package& value, const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }

  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    utils::error{}("GN02: could not open '{}' for writing", path.string());
  }

  stream.write(package_magic, sizeof(package_magic));
  write_value<uint32_t>(stream, value.version);
  write_value<uint32_t>(stream, uint32_t(value.sections.size()));
  write_value<uint64_t>(stream, value.seed);
  write_value<uint64_t>(stream, value.cell_count);
  write_value<uint64_t>(stream, value.fingerprint);

  for (const auto& section : value.sections) {
    write_string(stream, section.name);
    write_value<uint32_t>(stream, section.storage);
    write_value<uint32_t>(stream, uint32_t(section.fields.size()));
    for (const auto& field : section.fields) {
      write_string(stream, field.name);
      write_value<uint8_t>(stream, field.base);
      write_value<uint8_t>(stream, field.components);
    }
    write_value<uint64_t>(stream, section.count);
    write_value<uint64_t>(stream, uint64_t(section.bytes.size()));
    stream.write(reinterpret_cast<const char*>(section.bytes.data()), std::streamsize(section.bytes.size()));
  }

  if (!stream) {
    utils::error{}("GN02: writing '{}' failed", path.string());
  }
}

package read_package(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    utils::error{}("GN02: could not open '{}' for reading", path.string());
  }

  char magic[sizeof(package_magic)]{};
  stream.read(magic, sizeof(magic));
  if (!stream || std::memcmp(magic, package_magic, sizeof(magic)) != 0) {
    utils::error{}("GN02: '{}' is not a planet package", path.string());
  }

  package result;
  result.version = read_value<uint32_t>(stream, path);
  if (result.version != package_version) {
    utils::error{}("GN02 package '{}': version {}, this build reads {}", path.string(), result.version, package_version);
  }

  const auto section_count = read_value<uint32_t>(stream, path);
  result.seed = read_value<uint64_t>(stream, path);
  result.cell_count = read_value<uint64_t>(stream, path);
  const auto stored_fingerprint = read_value<uint64_t>(stream, path);

  result.sections.reserve(section_count);
  for (uint32_t i = 0; i < section_count; ++i) {
    package_section section;
    section.name = read_string(stream, path);
    section.storage = read_value<uint32_t>(stream, path);
    const auto field_count = read_value<uint32_t>(stream, path);
    for (uint32_t f = 0; f < field_count; ++f) {
      package_field field;
      field.name = read_string(stream, path);
      field.base = read_value<uint8_t>(stream, path);
      field.components = read_value<uint8_t>(stream, path);
      section.fields.push_back(std::move(field));
    }
    section.count = read_value<uint64_t>(stream, path);
    const auto byte_size = read_value<uint64_t>(stream, path);
    section.bytes.resize(size_t(byte_size));
    stream.read(reinterpret_cast<char*>(section.bytes.data()), std::streamsize(byte_size));
    if (!stream) {
      utils::error{}("GN02 package '{}': section '{}' is truncated", path.string(), section.name);
    }
    result.sections.push_back(std::move(section));
  }

  // Отпечаток пересчитывается при чтении, а не принимается на слово: битый файл обязан падать
  // здесь, а не через три шага в виде странного мира.
  result.fingerprint = fingerprint_of(result.sections);
  if (result.fingerprint != stored_fingerprint) {
    utils::error{}("GN02 package '{}': fingerprint {:#x} does not match the stored {:#x}",
                   path.string(), result.fingerprint, stored_fingerprint);
  }

  return result;
}

} // namespace devils_engine::gn02
