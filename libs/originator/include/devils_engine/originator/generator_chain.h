#ifndef DEVILS_ENGINE_ORIGINATOR_GENERATOR_CHAIN_H
#define DEVILS_ENGINE_ORIGINATOR_GENERATOR_CHAIN_H

#include <array>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "devils_engine/utils/type_traits.h"

#include "buffer.h"
#include "pipeline.h"

// ЦЕПОЧКА ГЕНЕРАТОРОВ: выходные буферы одного генератора — входы следующего.
//
// Генератор в цепочке — ЛОГИЧЕСКАЯ единица (макрограф, место, пещера), а не шаг: у него свой ключ
// экземпляра, свои объявленные выходы и свои связи с другими генераторами. Шаги lua остаются внутри
// генератора; цепочка между генераторами говорит только о ДАННЫХ.
//
// ТРИ ВИДА СВЯЗИ, и все три — способ получить ключ производителя:
//   same     B(k) <- A(k)             тот же ключ;
//   derived  B(k) <- A(g(k))          ключ ВЫВЕДЕН: часть слов своего ключа (`project`) или слова,
//                                     прочитанные из буфера, привезённого более ранней связью (`key`);
//   fan-in   B(k) <- A(k1..kn)        список ключей читается из буфера ранней связи (`keys`, `count`).
// Функций ключа, зарегистрированных хостом, нет: вывод ключа — либо проекция, либо ДАННЫЕ, которые
// посчитал производитель. Поэтому правило «какие пещеры старше этой» живёт там же, где его считают,
// а не в C++ рядом с цепочкой.
//
// КЭШ — ЭТО И ЕСТЬ РАЗРЕШЕНИЕ СВЯЗИ. Экземпляр называется (генератор, зерно мира, ключ) плюс отпечаток
// содержимого генератора и всех его производителей. Кэшируются только объявленные выходы; результат
// неизменяем и разделяется без копии. Одна сборка на ключ: второй запросивший ждёт тот же результат.
// Планировщика нет и не будет: параллельность живёт внутри инструментов и в потоках хоста, которые
// просто просят экземпляры.
//
// РЕКУРСИЯ ОБЯЗАНА СХОДИТЬСЯ ПО ДАННЫМ. Пещера просит старшие пещеры, и что старше — решают данные
// (меньший ключ). Цикл в одном потоке ловится громко; цикл между потоками стал бы взаимным ожиданием
// двух сборок, поэтому правило «связь ведёт только к меньшему» — обещание автора данных, которое
// библиотека проверить не может.
//
// СВЕРКА ПЕРЕСЧЁТОМ (`verify`) собирает экземпляр заново мимо кэша (связи берутся из кэша) и
// сравнивает выходы ПОБАЙТНО. Кэш, который однажды отдал не то, что посчитала бы чистая функция,
// неотличим от правильного ничем, кроме такого пересчёта.

namespace devils_engine {
namespace originator {

constexpr uint32_t max_key_words = 3;

// Ключ экземпляра: до трёх 64-битных слов. Сколько из них значимы, объявляет генератор (`key = N`);
// лишние слова нулевые.
struct instance_key {
  std::array<uint64_t, max_key_words> words{};

  bool operator==(const instance_key& other) const noexcept = default;
  auto operator<=>(const instance_key& other) const noexcept = default;
};

// Слово ключа в буфере — одна компонента `ul` (или `l`), читается целым путём: через double 64-битное
// слово не проходит. Поле `ul1` держит одно слово, `ul2` — два.
uint64_t read_key_word(const const_field_accessor& field, const size_t element, const uint32_t word = 0);
void write_key_word(const field_accessor& field, const size_t element, const uint64_t value, const uint32_t word = 0);

struct chain_link {
  enum class kind { same, project, from_data, fan_in };

  // Имя связи у потребителя. Буферы одиночной связи входят в пайплайн потребителя ПОД СВОИМИ именами
  // (объявленными у производителя); список ответа связи `fan_in` виден телу шага как step.links[имя].
  std::string name;
  std::string from;
  kind type = kind::same;
  // project: номера слов своего ключа, которые составят ключ производителя, по порядку.
  std::vector<uint32_t> project;
  // from_data: одно или несколько полей `буфер.поле` (элемент 0), слова ключа читаются подряд:
  // `key = буфер.поле` или `keys = [ ... ]` без `count`.
  // fan_in: те же поля, элементы 0..count-1, по одному ключу на элемент.
  std::vector<std::string> fields;
  // fan_in: `буфер.поле`, элемент 0 — сколько ключей занято.
  std::string count;
};

// Поле из примитивов у генератора: член — вид формы словаря и связь, из чьих экземпляров он читает.
struct chain_field {
  struct term {
    std::string kind;
    std::string link;
  };
  std::string name;
  std::vector<term> terms;
};

struct chain_generator {
  std::string name;
  uint32_t key_words = 1;
  // Ровно одно из двух: точка входа генератора-пайплайна (demiurg id) или имя НАТИВНОГО тела,
  // зарегистрированного хостом. Нативный генератор объявляет свои буферы документом `buffers`.
  std::string entry;
  std::string native;
  std::string buffers;
  std::vector<std::string> outputs;
  std::vector<chain_link> links;
  // Поля из примитивов, собираемые из связей ДО шагов (primitive_field.h).
  std::vector<chain_field> fields;
  // Выходы, которые уходят в кэш ОБРЕЗАННЫМИ до занятого числа: буфер -> `буфер.поле` (элемент 0,
  // тоже выход). Ёмкость объявлена с запасом на худший случай, а кэш держит сотни экземпляров.
  std::vector<std::pair<std::string, std::string>> trim;
  // Сколько готовых экземпляров держит кэш. Ноль — значение цепочки по умолчанию.
  size_t capacity = 0;
};

// Общий lua-модуль цепочки: функции правил, которые нужны нескольким генераторам (рельеф слоя, ключ
// узла, шум). `require` у генератора нет — он втянул бы demiurg в lua, — поэтому модуль объявляет
// документ цепочки, а хост загружает его в каждый lua-хост и кладёт в `modules.<имя>`.
struct chain_module {
  std::string name;
  std::string path;
};

struct chain_description {
  std::string name;
  std::vector<chain_module> modules;
  std::vector<chain_generator> generators;
};

// Разбор документа цепочки:
//   name = мир
//   modules = [ { name = relief, path = ./scripts/relief.lua } ]
//   generators = [
//     { name = route, native = route, key = 2, buffers = ./route_buffers, outputs = [ nodes ] },
//     { name = cave, entry = ./cave, key = 3, outputs = [ rooms ],
//       links = [ { name = place, from = place, key = same },
//                 { name = elders, from = cave, keys = [ site.sector ], count = site.count } ] }
//   ]
// Пути (`entry`, `buffers`) остаются как написаны: в id их переводит слой demiurg.
chain_description parse_chain(const std::string_view& text, const std::string_view& label);

// Готовый экземпляр: объявленные выходы и, у нативного генератора, объект хоста.
class primitive_field;
class shape_vocabulary;

// Строки одного вида формы из одного экземпляра, стёртые: данные и параллельные им коробки влияния
// (primitive_field.h, `shape_vocabulary::decoded`). Здесь — потому что экземпляр их кэширует.
struct field_box {
  std::array<double, 3> low{};
  std::array<double, 3> high{};

  bool contains(const std::array<double, 3>& p) const noexcept;
  bool overlaps(const std::array<double, 3>& other_low, const std::array<double, 3>& other_high) const noexcept;
  // Коробка отрезка ab, раздутая на radius.
  static field_box around(const std::array<double, 3>& a, const std::array<double, 3>& b, const double radius) noexcept;
};

struct shape_rows {
  std::shared_ptr<const void> rows;
  std::vector<field_box> boxes;
};

class generator_result {
public:
  const std::string& generator() const noexcept;
  uint64_t seed() const noexcept;
  const instance_key& key() const noexcept;

  size_t buffer_count() const noexcept;
  const buffer& buffer_at(const size_t index) const noexcept;
  const buffer* find(const std::string_view& buffer_name) const noexcept;
  // Отсутствие объявленного выхода — ошибка потребителя (опечатка), а не пустой буфер.
  const buffer& at(const std::string_view& buffer_name) const;
  const_field_accessor field(const std::string_view& buffer_name, const std::string_view& field_name) const;

  // ОБЪЕКТ ХОСТА: нативный генератор (или исполнитель пайплайна) может приложить к выходам свой C++
  // объект — например готовую структуру мира для нативных инструментов потребителя. Скрипты его не
  // видят, сверка пересчётом его не сравнивает: это не данные цепочки, а их удобная форма для C++.
  template <typename type_t>
  const type_t* payload() const noexcept {
    return payload_ != nullptr && payload_type_ == utils::type_id<type_t>() ?
             static_cast<const type_t*>(payload_.get()) : nullptr;
  }
  std::shared_ptr<const void> payload_handle() const noexcept;

  // Побайтовое равенство выходов. Пустая строка — равны; иначе первое расхождение словами.
  std::string compare(const generator_result& other) const;

  // Поля из примитивов, объявленные у генератора: остаются с экземпляром после сборки. Нет поля —
  // nullptr (handle — громкий отказ); держатель handle держит поле и все строки его членов.
  const primitive_field* shape_field(const std::string_view& field_name) const noexcept;
  std::shared_ptr<const primitive_field> shape_field_handle(const std::string_view& field_name) const;

  // Строки вида формы из выходов этого экземпляра — разбираются ОДИН раз на (словарь, вид): экземпляр
  // неизменен, а поля многих потребителей (чанки одного окна) читают одни и те же строки.
  shape_rows decoded_rows(const shape_vocabulary& vocabulary, const size_t kind) const;

  // ПРЕДСТАВЛЕНИЕ ХОСТА: C++-объект, собранный из выходов экземпляра (например, станция для нативных
  // потребителей). Собирается один раз на тип и живёт с экземпляром; держатель указателя держит и его.
  // В отличие от payload это не данные генератора, а их удобная форма, выведенная из буферов.
  //
  // Сборка идёт БЕЗ замка: make вправе спросить у того же экземпляра строки или другое представление (вид
  // формы станции собирается из представления станции). Два потока могут собрать одно и то же — остаётся
  // первое, результат один и тот же.
  template <typename type_t>
  std::shared_ptr<const type_t> view(const std::function<std::shared_ptr<const type_t>(const generator_result&)>& make) const {
    const size_t type = utils::type_id<type_t>();
    {
      std::lock_guard guard(views_lock_);
      for (const auto& [known, object] : views_) {
        if (known == type) return std::static_pointer_cast<const type_t>(object);
      }
    }
    std::shared_ptr<const type_t> made = make(*this);
    std::lock_guard guard(views_lock_);
    for (const auto& [known, object] : views_) {
      if (known == type) return std::static_pointer_cast<const type_t>(object);
    }
    views_.emplace_back(type, made);
    return made;
  }

private:
  friend class generator_chain;
  friend class generator_run;

  struct decoded_entry {
    const shape_vocabulary* vocabulary = nullptr;
    size_t kind = 0;
    shape_rows rows;
  };
  std::vector<std::pair<std::string, std::shared_ptr<const primitive_field>>> fields_;
  mutable std::mutex decoded_lock_;
  mutable std::vector<decoded_entry> decoded_;
  mutable std::mutex views_lock_;
  mutable std::vector<std::pair<size_t, std::shared_ptr<const void>>> views_;

  std::string generator_;
  uint64_t seed_ = 0;
  instance_key key_{};
  std::vector<std::unique_ptr<buffer>> buffers_;
  std::shared_ptr<const void> payload_;
  size_t payload_type_ = 0;
};

using result_ptr = std::shared_ptr<const generator_result>;

// Разрешённая связь: одиночная держит один экземпляр, fan-in — сколько ключей насчитали данные.
struct resolved_link {
  std::string name;
  std::vector<result_ptr> results;
};

class generator_chain;
class primitive_field;
class shape_vocabulary;

// Одна сборка экземпляра — то, что видит исполнитель.
class generator_run {
public:
  generator_run();
  ~generator_run();

  const chain_generator& generator() const noexcept;
  uint64_t world_seed() const noexcept;
  // Зерно сборки: hash(зерно мира, имя генератора, слова ключа). Не зависит от того, какие
  // экземпляры собраны раньше.
  uint64_t seed() const noexcept;
  const instance_key& key() const noexcept;
  // Общие значения генератора с наложенными значениями цепочки.
  const parameters& values() const noexcept;

  std::span<const resolved_link> links() const noexcept;
  const resolved_link& link(const std::string_view& link_name) const;
  // Одиночная связь — её единственный экземпляр.
  const generator_result& linked(const std::string_view& link_name) const;

  // Пайплайн генератора-пайплайна: уже построен, связанные буферы подключены. У нативного — nullptr.
  pipeline* line() noexcept;
  // Исполняет шаги пайплайна тем invoker'ом, который передал хост цепочке.
  void run_steps();

  // Выходной буфер для записи (нативный генератор — свои буферы, пайплайн — буферы пайплайна).
  buffer& output(const std::string_view& buffer_name);

  // Поле, объявленное у генератора, уже собранное из связей.
  const primitive_field& field(const std::string_view& field_name) const;

  template <typename type_t>
  void set_payload(std::shared_ptr<const type_t> object) {
    result_->payload_ = std::move(object);
    result_->payload_type_ = utils::type_id<type_t>();
  }

private:
  friend class generator_chain;

  const generator_chain* chain_ = nullptr;
  const chain_generator* generator_ = nullptr;
  uint64_t world_seed_ = 0;
  uint64_t seed_ = 0;
  instance_key key_{};
  const parameters* values_ = nullptr;
  std::vector<resolved_link> links_;
  std::unique_ptr<pipeline> line_;
  std::vector<std::unique_ptr<buffer>> native_buffers_;
  std::vector<std::pair<std::string, std::shared_ptr<const primitive_field>>> fields_;
  std::shared_ptr<generator_result> result_;
};

// Исполнитель генератора-пайплайна по умолчанию — `run.run_steps()`. Хост ставит свой, когда шагам
// нужен контекст вокруг (нативные инструменты, читающие объект производителя), и зовёт run_steps сам.
using generator_executor = std::function<void(generator_run&)>;
// Даёт invoker для пайплайна этого генератора в ТЕКУЩЕМ потоке (обычно lua-хост из пула потока).
using invoker_source = std::function<step_invoker(const chain_generator&)>;

// Текст, который исполнитель генератора-пайплайна загружает в свой хост: тело шага или программа.
struct script_text {
  bool program = false;
  // Имя шага у тела, имя программы у программы.
  std::string name;
  // demiurg id — им же называется кусок lua в сообщениях об ошибке.
  std::string id;
  std::string text;
};

// Всё, из чего собирается цепочка, кроме самого описания.
struct chain_setup {
  // Описания генераторов-пайплайнов по имени генератора (обычно load_generator по `entry`).
  std::vector<std::pair<std::string, pipeline_description>> pipelines;
  // Буферы нативных генераторов по имени генератора (обычно parse_buffers по `buffers`).
  std::vector<std::pair<std::string, std::vector<buffer_description>>> native_buffers;
  // Тела шагов и программы генераторов-пайплайнов по имени генератора.
  std::vector<std::pair<std::string, std::vector<script_text>>> scripts;
  // Общие модули цепочки в порядке объявления (имя модуля, id, текст): модуль видит уже загруженные.
  std::vector<script_text> modules;
  // Тексты, из которых сложен каждый генератор: из них считается отпечаток.
  std::vector<std::pair<std::string, std::string>> fingerprints;
  size_table sizes;
  // Значения цепочки: накладываются на значения КАЖДОГО генератора (параметры мира, общие для всех).
  parameters values;
  size_t default_capacity = 256;
  // Словарь форм для полей генераторов. Не владеет; нужен, если хоть один генератор объявил поле.
  const shape_vocabulary* vocabulary = nullptr;
};

class generator_chain {
  friend class generator_run;

public:
  generator_chain(chain_description description, chain_setup setup);

  const chain_description& description() const noexcept;
  const chain_generator& generator(const std::string_view& name) const;

  // Нативное тело — исполнитель, который САМ пишет выходы: пайплайна у него нет.
  void set_native(const std::string_view& name, generator_executor body);
  void set_executor(const std::string_view& name, generator_executor executor);
  void set_invoker_source(invoker_source source);

  // Экземпляр из кэша или собранный сейчас. Потокобезопасно; одна сборка на ключ.
  result_ptr get(const std::string_view& name, const uint64_t world_seed, const instance_key& key) const;
  // Пересобирает экземпляр мимо кэша и сравнивает с кэшированным. Пусто — совпали.
  std::string verify(const std::string_view& name, const uint64_t world_seed, const instance_key& key) const;

  // Отпечаток генератора вместе со всеми его производителями. Кэш живёт внутри цепочки, а её
  // содержимое после постройки не меняется, поэтому отпечаток называет КЭШ целиком, а не запись: он
  // для отчёта и для будущего кэша, который переживёт процесс.
  uint64_t fingerprint(const std::string_view& name) const;
  std::span<const script_text> scripts(const std::string_view& name) const;
  std::span<const script_text> modules() const noexcept;

  struct statistics {
    uint64_t hits = 0;
    uint64_t builds = 0;
    uint64_t evictions = 0;
  };
  statistics stats(const std::string_view& name) const;

private:
  struct entry {
    std::shared_future<result_ptr> future;
    uint64_t used = 0;
  };
  struct slot {
    chain_generator description;
    bool has_pipeline = false;
    const std::vector<buffer_description>* buffers = nullptr;
    // Описание пайплайна с наложенными значениями цепочки — один раз, а не на каждую сборку.
    pipeline_description merged;
    std::span<const script_text> scripts;
    parameters values;
    uint64_t fingerprint = 0;
    uint64_t name_hash = 0;
    generator_executor native;
    generator_executor executor;
    size_t capacity = 0;
    mutable std::mutex lock;
    mutable std::map<std::pair<uint64_t, instance_key>, entry> cache;
    mutable uint64_t tick = 0;
    mutable statistics stats;
  };

  slot& find_slot(const std::string_view& name);
  const slot& find_slot(const std::string_view& name) const;
  result_ptr build(const slot& at, const uint64_t world_seed, const instance_key& key) const;
  std::vector<resolved_link> resolve(const slot& at, const uint64_t world_seed, const instance_key& key) const;
  uint64_t fingerprint_of(const slot& at, std::vector<const slot*>& visiting) const;
  static void trim_outputs(const chain_generator& g, generator_result& result);

  chain_description description_;
  chain_setup setup_;
  std::vector<std::unique_ptr<slot>> slots_;
  invoker_source invokers_;
};

} // namespace originator
} // namespace devils_engine

#endif
