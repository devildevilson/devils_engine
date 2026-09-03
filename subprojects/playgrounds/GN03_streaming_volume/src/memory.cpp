#include "memory.h"

#include <algorithm>
#include <cstring>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"

namespace devils_engine::gn03 {

namespace {

uint64_t mix(uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}

// Запись склада на диске. Фиксированного размера и без выравнивающих дыр, потому что файл читается
// сравнением байтов в проверке: запись с неинициализированным хвостом сравнивалась бы случайно.
struct file_record {
  int64_t x = 0;
  int64_t y = 0;
  int64_t z = 0;
  uint32_t origin = 0;
  uint32_t flags = 0;
  uint32_t touches = 0;
  uint32_t padding = 0;
};
static_assert(sizeof(file_record) == 40);

constexpr uint32_t file_magic = 0x334e4747u;  // "GGN3"
constexpr uint32_t file_version = 1;

struct file_header {
  uint32_t magic = file_magic;
  uint32_t version = file_version;
  uint64_t count = 0;
};
static_assert(sizeof(file_header) == 16);

constexpr uint32_t flag_taken = 1u << 0;
constexpr uint32_t flag_marked = 1u << 1;

} // namespace

size_t prop_id_hash::operator()(const prop_id& id) const noexcept {
  return size_t(mix(uint64_t(id.chunk.x) ^ mix(uint64_t(id.chunk.y) ^ mix(uint64_t(id.chunk.z) ^ mix(id.origin)))));
}

const prop_delta* world_memory::find(const prop_id& id) const noexcept {
  const auto found = entries_.find(id);
  return found == entries_.end() ? nullptr : &found->second;
}

prop_delta& world_memory::entry(const prop_id& id) {
  return entries_[id];
}

void world_memory::drop_if_empty(const prop_id& id) {
  const auto found = entries_.find(id);
  if (found != entries_.end() && found->second.empty()) {
    entries_.erase(found);
  }
}

void world_memory::take(const prop_id& id) {
  auto& delta = entry(id);
  delta.taken = true;
  delta.touches += 1;
}

void world_memory::mark(const prop_id& id) {
  auto& delta = entry(id);
  // Пометка ПЕРЕКЛЮЧАЕТСЯ, а не ставится: иначе «снять пометку» пришлось бы делать отдельным
  // действием. Снятая пометка возвращает мир к выводимому, поэтому запись ИСЧЕЗАЕТ — вместе со
  // счётчиком касаний, и это правильно: от числа касаний в этом мире ничего не зависит, значит оно
  // статистика, а не отличие, и держать из-за него запись в сохранении незачем.
  delta.marked = !delta.marked;
  delta.touches += 1;
  drop_if_empty(id);
}

void world_memory::forget(const prop_id& id) {
  entries_.erase(id);
}

size_t world_memory::join(const originator::chunk_key& chunk, const std::span<const chunk_prop> derived,
                          std::vector<joined_prop>& visible) const {
  visible.clear();
  visible.reserve(derived.size());
  for (const auto& prop : derived) {
    const auto* delta = find(prop_id{chunk, prop.origin});
    if (delta != nullptr && delta->taken) {
      continue; // мир помнит, что её забрали
    }
    visible.push_back(joined_prop{&prop, delta != nullptr ? *delta : prop_delta{}});
  }
  return visible.size();
}

size_t world_memory::unmatched(const originator::chunk_key& chunk,
                               const std::span<const chunk_prop> derived) const {
  size_t missing = 0;
  for (const auto& [id, delta] : entries_) {
    if (!(id.chunk == chunk)) {
      continue;
    }
    const bool present = std::any_of(derived.begin(), derived.end(),
                                     [&id](const chunk_prop& prop) { return prop.origin == id.origin; });
    missing += present ? 0 : 1;
  }
  return missing;
}

bool world_memory::save(const std::string& path) const {
  // Порядок записей ФИКСИРОВАН сортировкой, а не порядком обхода таблицы: иначе один и тот же склад
  // давал бы разные файлы, и сравнить два сохранения побайтово было бы нельзя.
  std::vector<file_record> records;
  records.reserve(entries_.size());
  for (const auto& [id, delta] : entries_) {
    file_record record;
    record.x = id.chunk.x;
    record.y = id.chunk.y;
    record.z = id.chunk.z;
    record.origin = id.origin;
    record.flags = (delta.taken ? flag_taken : 0u) | (delta.marked ? flag_marked : 0u);
    record.touches = delta.touches;
    records.push_back(record);
  }
  std::sort(records.begin(), records.end(), [](const file_record& a, const file_record& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    if (a.z != b.z) return a.z < b.z;
    return a.origin < b.origin;
  });

  file_header header;
  header.count = records.size();

  std::vector<char> bytes(sizeof(header) + records.size() * sizeof(file_record));
  std::memcpy(bytes.data(), &header, sizeof(header));
  if (!records.empty()) {
    std::memcpy(bytes.data() + sizeof(header), records.data(), records.size() * sizeof(file_record));
  }
  return file_io::write(std::span<const char>(bytes.data(), bytes.size()), path, file_io::type::binary);
}

bool world_memory::load(const std::string& path) {
  const auto bytes = file_io::read<char>(path, file_io::type::binary);
  if (bytes.empty()) {
    return false;
  }
  if (bytes.size() < sizeof(file_header)) {
    utils::error{}("GN03 memory '{}' is {} bytes, which is smaller than its own header", path, bytes.size());
  }

  file_header header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (header.magic != file_magic) {
    utils::error{}("GN03 memory '{}' does not start with the expected mark", path);
  }
  if (header.version != file_version) {
    utils::error{}("GN03 memory '{}' is version {}, and this build reads version {}", path, header.version,
                   file_version);
  }
  const size_t expected = sizeof(file_header) + size_t(header.count) * sizeof(file_record);
  if (bytes.size() != expected) {
    utils::error{}("GN03 memory '{}' claims {} records, which needs {} bytes, but the file is {}", path,
                   header.count, expected, bytes.size());
  }

  entries_.clear();
  for (size_t i = 0; i < size_t(header.count); ++i) {
    file_record record;
    std::memcpy(&record, bytes.data() + sizeof(file_header) + i * sizeof(file_record), sizeof(record));
    prop_delta delta;
    delta.taken = (record.flags & flag_taken) != 0;
    delta.marked = (record.flags & flag_marked) != 0;
    delta.touches = record.touches;
    // Пустая запись в файле не имеет смысла и в склад не попадает: разреженность — свойство склада,
    // а не файла, и восстанавливать её надо на чтении тоже.
    if (!delta.empty()) {
      entries_[prop_id{originator::chunk_key{record.x, record.y, record.z}, record.origin}] = delta;
    }
  }
  return true;
}

} // namespace devils_engine::gn03
