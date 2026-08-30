#include "titles.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "devils_engine/utils/core.h"

namespace devils_engine::pf09 {

namespace {

constexpr char title_magic[8] = {'P', 'F', '0', '9', 'T', 'T', '0', '1'};

// Подъём по дереву титулов ограничен так же, как по вложенности зон: цепочка коротка по построению, а
// неограниченный обход означал бы готовность крутиться в кольце, если файл окажется порченым.
constexpr uint32_t max_hops = 8;

struct title_header {
  char magic[8];
  uint32_t count;
  uint32_t name_bytes;
};

} // namespace

std::string_view title_rank_name(const title_rank value) noexcept {
  switch (value) {
    case title_rank::property: return "property";
    case title_rank::district: return "district";
    case title_rank::city: return "city";
    case title_rank::realm: return "realm";
    default: return "?";
  }
}

std::filesystem::path titles_path(const std::filesystem::path& root) { return root / "titles.tit"; }

title_id title_book::add(const title_record& record, const std::string_view name) {
  auto copy = record;
  copy.name_offset = uint32_t(names_.size());
  names_.insert(names_.end(), name.begin(), name.end());
  names_.push_back('\0');

  records_.push_back(copy);
  return title_id(records_.size() - 1);
}

const title_record* title_book::find(const title_id id) const {
  return id < records_.size() ? &records_[id] : nullptr;
}

std::string_view title_book::name_of(const title_id id) const {
  const auto* record = find(id);
  if (record == nullptr || record->name_offset >= names_.size()) return {};
  return std::string_view(names_.data() + record->name_offset);
}

void title_book::set_holder(const title_id id, const uint32_t owner) {
  if (id < records_.size()) holders_.set(id, owner);
}

uint32_t title_book::holder(const title_id id) const {
  if (const auto* value = holders_.find(id); value != nullptr) return *value;

  const auto* record = find(id);
  if (record == nullptr) return 0;
  return record->de_facto_holder != 0 ? record->de_facto_holder : record->de_jure_holder;
}

uint32_t title_book::de_jure_holder(const title_id id) const {
  const auto* record = find(id);
  return record == nullptr ? 0 : record->de_jure_holder;
}

bool title_book::usurped(const title_id id) const {
  const auto* record = find(id);
  return record != nullptr && holder(id) != record->de_jure_holder;
}

bool title_book::recognises(const title_id id) const {
  const auto* record = find(id);
  if (record == nullptr) return false;
  if (record->de_jure_parent == invalid_title) return false; // выше некого признавать

  // Законный держатель признаёт власть над собой по определению: его право оттуда и происходит.
  if (!usurped(id)) return true;

  // И тот, кто держит уровень выше, признаёт «сам себя»: это одна и та же сила, а не два лагеря.
  return holder(id) == holder(record->de_jure_parent);
}

title_id title_book::law_source(const title_id id) const {
  auto current = id;
  for (uint32_t hop = 0; hop < max_hops; ++hop) {
    const auto* record = find(current);
    if (record == nullptr) return invalid_title;
    if (!recognises(current)) break;
    current = record->de_jure_parent;
  }

  // Остановились на том, кто не признал старших. Его закон и в силе — если он его вообще издаёт: у
  // собственности законов нет, и узурпированный дом посреди законопослушного квартала оказывается местом
  // БЕЗ закона. Это не дефект модели, а её содержание.
  const auto* record = find(current);
  if (record == nullptr || record->law == 0) return invalid_title;
  return current;
}

std::vector<title_id> title_book::chain(const title_id id) const {
  std::vector<title_id> out;
  auto current = id;
  for (uint32_t hop = 0; hop < max_hops; ++hop) {
    const auto* record = find(current);
    if (record == nullptr) break;
    out.push_back(current);
    if (record->de_jure_parent == invalid_title) break;
    current = record->de_jure_parent;
  }
  return out;
}

void title_book::save(const std::filesystem::path& path) const {
  title_header header{};
  std::memcpy(header.magic, title_magic, sizeof(title_magic));
  header.count = uint32_t(records_.size());
  header.name_bytes = uint32_t(names_.size());

  std::vector<char> bytes(sizeof(header));
  std::memcpy(bytes.data(), &header, sizeof(header));
  const auto offset = bytes.size();
  bytes.resize(offset + records_.size() * sizeof(title_record));
  if (!records_.empty()) {
    std::memcpy(bytes.data() + offset, records_.data(), records_.size() * sizeof(title_record));
  }
  bytes.insert(bytes.end(), names_.begin(), names_.end());

  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) utils::error{}("PF09 titles: cannot write '{}'", path.string());
  file.write(bytes.data(), std::streamsize(bytes.size()));
}

void title_book::load(const std::filesystem::path& path) {
  records_.clear();
  names_.clear();
  holders_.clear();

  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return; // титулов может не быть: мир, собранный старой версией, просто без права

  const auto size = size_t(file.tellg());
  file.seekg(0);
  std::vector<char> bytes(size);
  file.read(bytes.data(), std::streamsize(size));
  if (bytes.size() < sizeof(title_header)) return;

  title_header header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (std::memcmp(header.magic, title_magic, sizeof(title_magic)) != 0) {
    utils::error{}("PF09 titles: '{}' is not a title file", path.string());
  }

  const size_t body = sizeof(header) + size_t(header.count) * sizeof(title_record);
  if (body + header.name_bytes > bytes.size()) {
    utils::error{}("PF09 titles: '{}' is truncated", path.string());
  }

  records_.resize(header.count);
  if (header.count != 0) {
    std::memcpy(records_.data(), bytes.data() + sizeof(header), size_t(header.count) * sizeof(title_record));
  }
  names_.assign(bytes.begin() + std::ptrdiff_t(body), bytes.begin() + std::ptrdiff_t(body + header.name_bytes));
}

uint32_t de_facto_holder(const realm_view& view, const zone_key place) {
  if (view.zones == nullptr || view.titles == nullptr) return 0;

  // Сначала — держатель САМОГО МЕСТА, если его занимали отдельно. Площадь можно взять, не взяв квартала,
  // и это ровно то различие, ради которого де-факто отделено от де-юре. Спрашивается именно
  // `place_holder`, а не `control_at`: второй поднимается до квартала и на вопрос про площадь отвечает
  // про район — то есть стирает различие, которое здесь и проверяется.
  const auto seized = view.zones->place_holder(place);
  if (seized != 0) return seized;

  const auto* record = view.zones->find(place);
  return record == nullptr ? 0 : view.titles->holder(record->title);
}

title_id law_over(const realm_view& view, const zone_key place) {
  if (view.zones == nullptr || view.titles == nullptr) return invalid_title;
  const auto* record = view.zones->find(place);
  if (record == nullptr || record->title == invalid_title) return invalid_title;
  return view.titles->law_source(record->title);
}

entry_verdict may_enter(const realm_view& view, const zone_key place, const uint32_t actor) {
  entry_verdict verdict{};
  if (view.zones == nullptr || view.titles == nullptr) return verdict;

  const auto* record = view.zones->find(place);
  if (record == nullptr || record->title == invalid_title) return verdict;

  verdict.law_source = view.titles->law_source(record->title);

  // Частная территория — ближайший титул ранга `property` в цепочке права. Улица его не имеет, и это не
  // «нет ответа», а «территория общая»: заходить можно.
  for (const auto id : view.titles->chain(record->title)) {
    const auto* title = view.titles->find(id);
    if (title == nullptr || title->rank != title_rank::property) continue;

    verdict.owner = id;
    // Пускает ДЕ-ФАКТО держатель, а не законный владелец. Дом, занятый революционером, закрыт для своего
    // хозяина ровно так же, как чужой дом закрыт для постороннего: право не открывает дверь само по себе.
    verdict.allowed = view.titles->holder(id) == actor;
    break;
  }
  return verdict;
}

} // namespace devils_engine::pf09
