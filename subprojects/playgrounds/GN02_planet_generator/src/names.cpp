#include "names.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "devils_engine/utils/core.h"

namespace devils_engine::gn02 {
namespace {

originator::const_field_accessor field_of(originator::pipeline& line, const std::string_view& buffer_name,
                                          const std::string_view& field_name) {
  auto* buffer = line.find_buffer(buffer_name);
  if (buffer == nullptr) {
    utils::error{}("GN02 names: no buffer '{}'", buffer_name);
  }
  const size_t index = buffer->find_field(field_name);
  if (index == originator::buffer_layout::npos) {
    utils::error{}("GN02 names: buffer '{}' has no field '{}'", buffer_name, field_name);
  }
  return buffer->field(index);
}

double state_value(originator::pipeline& line, const std::string_view& field_name) {
  const auto field = field_of(line, "state", field_name);
  return field.count() == 0 ? 0.0 : field.get(0);
}

// НАИМЕНУЮЩАЯ ТРАДИЦИЯ. Слоги разбиты на наборы, и набор выбирается КУЛЬТУРОЙ, а не случайно: у
// названий одного края должно быть общее звучание, иначе карта читается как список из генератора
// случайных строк. Это то же соображение, по которому в игре у одной культуры один набор имён.
struct tradition {
  std::span<const char* const> onsets;
  std::span<const char* const> nuclei;
  std::span<const char* const> codas;
  std::span<const char* const> endings;
};

constexpr const char* onsets_north[] = {"br", "d", "f", "g", "h", "k", "sk", "st", "th", "v", "n", "r"};
constexpr const char* nuclei_north[] = {"a", "e", "i", "o", "u", "au", "ei", "ja"};
constexpr const char* codas_north[] = {"rd", "ng", "lm", "st", "nd", "rk", "ll", "n"};
constexpr const char* endings_north[] = {"heim", "mark", "vik", "land", "gard", "fell"};

constexpr const char* onsets_south[] = {"b", "c", "l", "m", "p", "s", "t", "v", "gr", "tr", "fl"};
constexpr const char* nuclei_south[] = {"a", "e", "i", "o", "ia", "ae", "au", "ua"};
constexpr const char* codas_south[] = {"nt", "rr", "ll", "mb", "st", "ss", "n", "r"};
constexpr const char* endings_south[] = {"ia", "ora", "ena", "ala", "ium", "esa"};

constexpr const char* onsets_east[] = {"ch", "sh", "z", "j", "k", "t", "n", "y", "h", "m", "s"};
constexpr const char* nuclei_east[] = {"a", "e", "i", "o", "u", "ai", "ao", "ie"};
constexpr const char* codas_east[] = {"n", "ng", "k", "t", "s", "r", "m", "y"};
constexpr const char* endings_east[] = {"shima", "zan", "kai", "do", "hara", "ryo"};

constexpr const char* onsets_desert[] = {"b", "d", "h", "kh", "m", "n", "q", "r", "s", "z", "gh", "t"};
constexpr const char* nuclei_desert[] = {"a", "e", "i", "u", "ai", "ay", "ua", "o"};
constexpr const char* codas_desert[] = {"r", "d", "l", "m", "n", "sh", "kh", "b"};
constexpr const char* endings_desert[] = {"an", "ar", "at", "iyah", "abad", "een"};

const std::array<tradition, 4> traditions{{
  {onsets_north, nuclei_north, codas_north, endings_north},
  {onsets_south, nuclei_south, codas_south, endings_south},
  {onsets_east, nuclei_east, codas_east, endings_east},
  {onsets_desert, nuclei_desert, codas_desert, endings_desert},
}};

// Тот же хеш, что в скриптах генератора: одна и та же пара (затравка, номер) обязана давать одно и
// то же значение, иначе название места менялось бы от того, в каком порядке его спросили.
uint64_t mix(const uint64_t a, const uint64_t b) noexcept {
  uint64_t x = (a * 0x9E3779B97F4A7C15ull) ^ (b * 0xBF58476D1CE4E5B9ull);
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

template <typename T>
const char* pick(const std::span<const T>& list, const uint64_t value) {
  return list[size_t(value % list.size())];
}

std::string capitalise(std::string text) {
  if (!text.empty() && text.front() >= 'a' && text.front() <= 'z') {
    text.front() = char(text.front() - 'a' + 'A');
  }
  return text;
}

// Слово: приступ + ядро [+ кода + ядро] + окончание. Длина зависит от затравки, потому что на карте
// не должно быть двадцати названий одинакового ритма.
std::string word_of(const uint64_t seed, const size_t culture) {
  // Традиция от культуры, а у безымянного места (океан ничей) — от собственной затравки. Без этого
  // все океаны получали культуру 0 и, значит, ОДНУ традицию: на карте выходили «Neiheim Ocean»,
  // «Haungeigard Ocean», «Stellufell Ocean» — четыре океана одним и тем же скандинавским складом.
  const auto style_index = culture != 0 ? culture : size_t(mix(seed, 17));
  const auto& style = traditions[style_index % traditions.size()];
  const uint64_t a = mix(seed, 1);
  const uint64_t b = mix(seed, 2);
  const uint64_t c = mix(seed, 3);

  std::string text = pick(style.onsets, a);
  text += pick(style.nuclei, a >> 8);
  // Средний слог только у части названий, и не тогда, когда слово и без него уже длинное: на карте
  // не должно быть ни двадцати названий одного ритма, ни «Shiaitoaishima» в четырнадцать букв.
  if ((b & 3) != 0 && text.size() <= 3) {
    text += pick(style.codas, b >> 8);
    text += pick(style.nuclei, b >> 16);
  }
  text += pick(style.endings, c);
  return capitalise(std::move(text));
}

// Сторона света области ВНУТРИ её материка. Считается по направлению от центра материка к центру
// области, разложенному по локальному северу и востоку: «северная» — это про положение внутри
// материка, а не про широту, поэтому глобальные координаты здесь не годятся.
const char* compass_of(const std::array<double, 3>& region, const std::array<double, 3>& continent) {
  const std::array<double, 3> north{0.0, 1.0, 0.0};
  std::array<double, 3> east{continent[2] * north[1] - continent[1] * north[2],
                             continent[0] * north[2] - continent[2] * north[0],
                             continent[1] * north[0] - continent[0] * north[1]};
  const double east_length = std::sqrt(east[0] * east[0] + east[1] * east[1] + east[2] * east[2]);
  if (east_length < 1e-9) {
    return "Central";
  }
  for (auto& value : east) {
    value /= east_length;
  }
  const std::array<double, 3> up{continent[1] * east[2] - continent[2] * east[1],
                                 continent[2] * east[0] - continent[0] * east[2],
                                 continent[0] * east[1] - continent[1] * east[0]};

  const std::array<double, 3> offset{region[0] - continent[0], region[1] - continent[1],
                                     region[2] - continent[2]};
  const double along_east = offset[0] * east[0] + offset[1] * east[1] + offset[2] * east[2];
  const double along_up = offset[0] * up[0] + offset[1] * up[1] + offset[2] * up[2];
  const double distance = std::sqrt(along_east * along_east + along_up * along_up);

  // Порог не в радианах, а в долях: «центральная» область — та, что лежит близко к центру материка
  // по сравнению с его собственным размером, и размер этот у материков разный.
  if (distance < 0.12) {
    return "Central";
  }
  if (std::abs(along_up) > std::abs(along_east)) {
    return along_up > 0.0 ? "Northern" : "Southern";
  }
  return along_east > 0.0 ? "Eastern" : "Western";
}

} // namespace

place_names build_place_names(originator::pipeline& line, const size_t continent_min_provinces,
                              const size_t ocean_zones) {
  const auto land_mass_count = size_t(state_value(line, "land_mass_count"));
  const auto continent_count = size_t(state_value(line, "continent_count"));
  const auto historical_count = size_t(state_value(line, "historical_count"));
  const auto ocean_count = size_t(state_value(line, "ocean_count"));
  const auto continent_min = std::max<size_t>(continent_min_provinces, 1);

  place_names result;
  result.land_masses.assign(land_mass_count + 1, std::string{});
  result.continents.assign(continent_count + 1, std::string{});
  result.historical_regions.assign(historical_count + 1, std::string{});
  result.oceans.assign(ocean_count + 1, std::string{});

  const auto mass_name_seed = field_of(line, "land_masses", "name_seed");
  const auto mass_culture = field_of(line, "land_masses", "culture");
  const auto mass_provinces = field_of(line, "land_masses", "provinces");
  for (size_t i = 1; i <= land_mass_count && i < mass_name_seed.count(); ++i) {
    auto word = word_of(uint64_t(mass_name_seed.get(i)), size_t(mass_culture.get(i)));
    // Массив, не дотянувший до своего материка, — это остров, и называется он островом: «Isle of X»
    // читается как место, а «X» на карте рядом с материком того же ранга сбивает с толку.
    result.land_masses[i] = size_t(mass_provinces.get(i)) >= continent_min
                              ? std::move(word)
                              : std::format("Isle of {}", word);
  }

  // Имена материков РАЗЛИЧИМЫ ПО ПОСТРОЕНИЮ, а не по удаче. Затравок столько же, сколько мест,
  // поэтому два материка могут получить одно слово: на карте это читается как ошибка данных, хотя
  // данные верны. Проверка на пяти зёрнах и нашла такое столкновение на зерне 1.
  //
  // При столкновении затравка ПРОДВИГАЕТСЯ тем же перемешиванием, что и внутри синтеза, и слово
  // берётся заново. Контракт «имя полностью определено данными» при этом держится: порядок обхода
  // один и тот же у любого потребителя пакета, значит и продвижение то же. Тот же приём уже стоит у
  // исторических областей, где сторон света пять, а областей в материке бывает двадцать.
  const auto continent_name_seed = field_of(line, "continents", "name_seed");
  const auto continent_culture = field_of(line, "continents", "culture");
  std::unordered_set<std::string> continent_taken;
  for (size_t i = 1; i <= continent_count && i < continent_name_seed.count(); ++i) {
    auto seed = uint64_t(continent_name_seed.get(i));
    const auto culture = size_t(continent_culture.get(i));
    auto word = word_of(seed, culture);
    // Восемь попыток: складов в традиции хватает на тысячи слов, поэтому восемь подряд занятых
    // означало бы не столкновение, а исчерпание традиции, и молчать об этом нельзя.
    for (uint32_t attempt = 0; attempt < 8 && continent_taken.count(word) != 0; ++attempt) {
      seed = mix(seed, 0x9e3779b9ull);
      word = word_of(seed, culture);
    }
    continent_taken.insert(word);
    result.continents[i] = std::move(word);
  }

  // Историческая область называется СТОРОНОЙ СВЕТА СВОЕГО МАТЕРИКА — «Северная Европа», — ровно как
  // и просили. Но не всегда: сторон света пять, а областей в материке бывает двадцать, поэтому при
  // столкновении область получает собственное имя. Приоритет у стороны света, потому что она несёт
  // больше смысла: по ней сразу понятно, где это.
  const auto region_name_seed = field_of(line, "historical_regions", "name_seed");
  const auto region_culture = field_of(line, "historical_regions", "culture");
  const auto region_center = field_of(line, "historical_regions", "center");
  const auto region_continent = field_of(line, "historical_regions", "continent");
  const auto continent_center = field_of(line, "continents", "center");

  // Занятые имена держатся ПО МАТЕРИКУ: различимость нужна там, где по имени ориентируются, а два
  // «Северных» в разных материках друг другу не мешают. Имя стороны света уже содержит имя материка,
  // поэтому для него достаточно одного множества, а вот СОБСТВЕННОЕ слово области надо проверять
  // именно внутри её материка.
  std::unordered_set<std::string> taken;
  std::unordered_map<size_t, std::unordered_set<std::string>> taken_in_continent;
  for (size_t i = 1; i <= historical_count && i < region_name_seed.count(); ++i) {
    const auto owner = size_t(region_continent.get(i));
    auto seed = uint64_t(region_name_seed.get(i));
    const auto culture = size_t(region_culture.get(i));
    auto word = word_of(seed, culture);
    if (owner == 0 || owner >= result.continents.size() || result.continents[owner].empty()) {
      result.historical_regions[i] = std::move(word);
      continue;
    }

    const std::array<double, 3> region{region_center.get(i, 0), region_center.get(i, 1),
                                       region_center.get(i, 2)};
    const std::array<double, 3> parent{continent_center.get(owner, 0), continent_center.get(owner, 1),
                                       continent_center.get(owner, 2)};
    auto candidate = std::format("{} {}", compass_of(region, parent), result.continents[owner]);
    auto& used = taken_in_continent[owner];
    if (taken.insert(candidate).second) {
      used.insert(candidate);
      result.historical_regions[i] = std::move(candidate);
      continue;
    }

    // Сторона света занята — область получает собственное имя, и оно тоже обязано быть различимо
    // ВНУТРИ материка. Первая версия этой проверки не делала, и проверка на пяти зёрнах нашла
    // столкновение: сторон света пять, а областей в материке бывает двадцать, поэтому в собственные
    // имена уходит большинство областей крупного материка, и совпадение там — вопрос времени.
    for (uint32_t attempt = 0; attempt < 8 && used.count(word) != 0; ++attempt) {
      seed = mix(seed, 0x9e3779b9ull);
      word = word_of(seed, culture);
    }
    used.insert(word);
    result.historical_regions[i] = std::move(word);
  }

  const auto ocean_name_seed = field_of(line, "oceans", "name_seed");
  const auto ocean_culture = field_of(line, "oceans", "culture");
  const auto ocean_zone_count = field_of(line, "oceans", "zones");
  for (size_t i = 1; i <= ocean_count && i < ocean_name_seed.count(); ++i) {
    const auto word = word_of(uint64_t(ocean_name_seed.get(i)), size_t(ocean_culture.get(i)));
    const auto zones = size_t(ocean_zone_count.get(i));
    result.oceans[i] = std::format("{} {}", word, zones * 2 >= ocean_zones ? "Ocean" : "Sea");
  }

  // ---------------------------------------------------------------- титулы
  const auto province_count = size_t(state_value(line, "province_count"));
  const auto duchy_count = size_t(state_value(line, "duchy_count"));
  const auto empire_count = size_t(state_value(line, "empire_count"));
  const auto realm_count = size_t(state_value(line, "realm_count"));

  result.provinces.assign(province_count + 1, std::string{});
  result.duchies.assign(duchy_count + 1, std::string{});
  result.empires.assign(empire_count + 1, std::string{});
  result.realms.assign(realm_count + 1, std::string{});

  const auto province_name_seed = field_of(line, "provinces", "name_seed");
  const auto province_culture = field_of(line, "provinces", "culture");
  for (size_t i = 1; i <= province_count && i < province_name_seed.count(); ++i) {
    result.provinces[i] = word_of(uint64_t(province_name_seed.get(i)), size_t(province_culture.get(i)));
  }

  const auto duchy_name_seed = field_of(line, "duchies", "name_seed");
  const auto duchy_culture = field_of(line, "duchies", "culture");
  for (size_t i = 1; i <= duchy_count && i < duchy_name_seed.count(); ++i) {
    result.duchies[i] = word_of(uint64_t(duchy_name_seed.get(i)), size_t(duchy_culture.get(i)));
  }

  const auto empire_name_seed = field_of(line, "empires", "name_seed");
  const auto empire_culture = field_of(line, "empires", "culture");
  for (size_t i = 1; i <= empire_count && i < empire_name_seed.count(); ++i) {
    result.empires[i] = word_of(uint64_t(empire_name_seed.get(i)), size_t(empire_culture.get(i)));
  }

  // Ранг державы по числу её графств, как в CK: сорок и больше — империя, четырнадцать — королевство,
  // четыре — герцогство, меньше — графство. Порог королевства взят от размера исторической области,
  // потому что де-юре королевство и есть она.
  const auto realm_name_seed = field_of(line, "realms", "name_seed");
  const auto realm_culture = field_of(line, "realms", "culture");
  const auto realm_provinces = field_of(line, "realms", "provinces");
  for (size_t i = 1; i <= realm_count && i < realm_name_seed.count(); ++i) {
    const auto word = word_of(uint64_t(realm_name_seed.get(i)), size_t(realm_culture.get(i)));
    const auto held = size_t(realm_provinces.get(i));
    const char* rank = held >= 40 ? "Empire" : held >= 14 ? "Kingdom" : held >= 4 ? "Duchy" : "County";
    result.realms[i] = std::format("{} of {}", rank, word);
  }

  return result;
}

} // namespace devils_engine::gn02
