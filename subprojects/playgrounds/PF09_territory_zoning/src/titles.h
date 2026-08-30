#ifndef DEVILS_ENGINE_PF09_TITLES_H
#define DEVILS_ENGINE_PF09_TITLES_H

// Титулы: кому земля принадлежит ПО ПРАВУ и кто держит её НА ДЕЛЕ.
//
// Это отдельная от зон структура, и разделение здесь несущее. У зоны есть пространственная вложенность
// (комната в здании, здание в квартале, квартал в поселении) — она отвечает на вопрос «где это». У земли
// есть ЮРИДИЧЕСКАЯ принадлежность — она отвечает на вопрос «чьё это», и совпадать они не обязаны: дом
// стоит на улице, но принадлежит не улице, а своему владельцу, который признаёт власть квартала. Две
// иерархии параллельны, и попытка обойтись одной кончается тем, что «переехал» и «сменил хозяина»
// становятся одним событием.
//
// Титулы НЕ СТРИМЯТСЯ. Земля подгружается по мере движения игрока, а «кто правит этим городом» обязано
// отвечаться всегда — иначе право собственности зависело бы от того, где сейчас стоит партия. Дерево
// титулов мало (единицы килобайт на область) и живёт целиком в памяти.
//
// Модель прямо повторяет систему титулов CK: у каждого титула есть де-юре родитель (меняется редко, это
// карта права) и де-факто держатель (меняется в игре, это карта силы). Революционеры заняли площадь —
// площадь де-факто их, а квартал ещё под прежней властью. Отсюда же вырастает ЗАКОН: держатель признаёт
// власть уровня выше, только если он законен сам или совпадает с тем, кто держит уровень выше; цепочка
// признаний обрывается на узурпаторе, и дальше его закон никуда не распространяется.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "devils_engine/utils/inherited.h"

#include "zones.h"

namespace devils_engine::pf09 {

using title_id = uint32_t;
constexpr title_id invalid_title = 0xffffffffu;

// Ранг определяет, что титул значит, а не насколько он «важен». Собственность — это то, на что
// распространяется право входа; квартал, город и держава — то, что издаёт законы.
enum class title_rank : uint32_t {
  property,   // дом с двором: частная территория
  district,   // квартал
  city,       // город
  realm,      // держава
  count
};

std::string_view title_rank_name(const title_rank value) noexcept;

struct title_record {
  title_id de_jure_parent = invalid_title;  // карта права: меняется редко
  uint32_t de_jure_holder = 0;              // законный владелец
  // Начальное положение карты силы. В файле лежит именно НАЧАЛЬНОЕ: дальше держателем распоряжается
  // рантайм, ровно как состоянием двери. Ноль означает «держит законный владелец».
  uint32_t de_facto_holder = 0;
  uint32_t law = 0;                         // свод законов, который титул издаёт; ноль — не издаёт
  title_rank rank = title_rank::property;
  uint32_t name_offset = 0;
};

// Почему во вход отказано. Отказ без причины бесполезен: игре надо знать, чью границу нарушают и чей
// закон это преследует, — иначе она не сможет ни предупредить, ни позвать стражу, ни решить, что здесь
// стражи нет вовсе.
struct entry_verdict {
  bool allowed = true;
  title_id owner = invalid_title;   // чья собственность, если частная
  title_id law_source = invalid_title; // чей закон здесь в силе; `invalid_title` — ничей
};

class title_book {
public:
  void load(const std::filesystem::path& path);
  void save(const std::filesystem::path& path) const;

  title_id add(const title_record& record, const std::string_view name);
  uint32_t size() const noexcept { return uint32_t(records_.size()); }
  const title_record* find(const title_id id) const;
  std::string_view name_of(const title_id id) const;

  // Де-факто держатель. По умолчанию — законный; подмена живёт в рантайме и не трогает файл, потому что
  // «занял» и «владеет по праву» — разные утверждения, и терять второе при первом нельзя.
  void set_holder(const title_id id, const uint32_t holder);
  uint32_t holder(const title_id id) const;
  uint32_t de_jure_holder(const title_id id) const;
  bool usurped(const title_id id) const;

  // Признаёт ли титул власть уровня выше. Законный держатель признаёт; тот, кто держит и родителя, тоже
  // (это одна и та же сила); узурпатор — нет.
  bool recognises(const title_id id) const;

  // Чей закон здесь в силе. Поднимаемся, пока признают, и останавливаемся на первом, кто не признал.
  // `invalid_title` означает, что закона тут нет вовсе, — и это законный ответ, а не ошибка.
  title_id law_source(const title_id id) const;

  std::vector<title_id> chain(const title_id id) const;

private:
  std::vector<title_record> records_;
  std::vector<char> names_;

  // Четвёртая по счёту таблица подмен в этой площадке — и последняя, написанная вручную. Все четыре
  // делали одно и то же, и теперь делают это одним кодом.
  utils::override_table<title_id, uint32_t> holders_;
};

// Земля и право вместе. Раздельные структуры, общий вопрос: связывать их внутри `zone_store` значило бы
// потянуть нестримящееся дерево титулов в стримящееся хранилище.
struct realm_view {
  const zone_store* zones = nullptr;
  const title_book* titles = nullptr;
};

// Кто де-факто держит ЭТО МЕСТО. Не то же, что держатель титула: площадь можно занять, не взяв квартал, —
// и первая редакция, спрашивавшая только титул, не умела выразить ровно то, ради чего всё это заводится.
uint32_t de_facto_holder(const realm_view& view, const zone_key place);

// Чей закон в силе на этом месте.
title_id law_over(const realm_view& view, const zone_key place);

// Можно ли сюда входить этому актору.
entry_verdict may_enter(const realm_view& view, const zone_key place, const uint32_t actor);

std::filesystem::path titles_path(const std::filesystem::path& root);

} // namespace devils_engine::pf09

#endif
