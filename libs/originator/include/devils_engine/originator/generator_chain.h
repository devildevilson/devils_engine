#ifndef DEVILS_ENGINE_ORIGINATOR_GENERATOR_CHAIN_H
#define DEVILS_ENGINE_ORIGINATOR_GENERATOR_CHAIN_H

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <tuple>

#include "devils_engine/utils/type_traits.h"

#include "buffer.h"
#include "pipeline.h"
#include "workspace.h"

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

class primitive_field;

// Explicit execution state. It may move between workers between calls, never be used
// simultaneously. release() drops reusable capacity and Lua hosts while workers stay alive.
struct generation_context {
  std::vector<std::tuple<const void*, uint64_t, instance_key>> building;
  int step_depth = 0;
  std::shared_ptr<const void> current_node;
  const std::vector<std::pair<std::string, std::shared_ptr<const primitive_field>>>* fields = nullptr;
  field_workspace field_scratch;
  std::map<std::pair<const void*, std::string>, std::shared_ptr<void>> hosts;
  void release();
  generation_context fork_task() const;
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
  // Поля живут только во время сборки: поле держит строки ВСЕХ связей (станции, пещеры, окно), и кэш из
  // сотен экземпляров удерживал бы их сверх ёмкости их собственных кэшей. true — поля остаются с
  // экземпляром (shape_field); иначе их пересобирает generator_chain::field из связей по требованию.
  bool keep_fields = false;
};

// Общий lua-модуль цепочки: функции правил, которые нужны нескольким генераторам (рельеф слоя, ключ
// узла, шум). `require` у генератора нет — он втянул бы demiurg в lua, — поэтому модуль объявляет
// документ цепочки, а хост загружает его в каждый lua-хост и кладёт в `modules.<имя>`.
struct chain_module {
  std::string name;
  std::string path;
};

// Вид формы поля, объявленный данными: строки — элементы буфера `buffer` экземпляра (занято — поле
// `count`, пусто — все), коробка — точка `box_a` (и второй конец `box_b` у капсулы) ± `reach` (поле
// строки или число), метка — `tag` (поле или число), значение — программа ds `path` над столбцами
// x, y, z и слотами словаря; поля строки видны ей скалярами (многокомпонентные — имя_x/_y/_z/_w).
// `box = aabb` трактует box_a/box_b как противоположные углы ящика, расширенного на reach.
struct chain_shape {
  std::string name;
  std::string buffer;
  std::string count;
  std::string box_a;
  std::string box_b;
  std::string box;
  std::string reach;
  std::string tag;
  std::string path;
  std::string text; // текст программы (заполняет загрузка)
  // Роль вида в поле: natural (по умолчанию), fill (своя порода, сглаженный максимум радиуса blend),
  // cut (вырез поверх заливки) — см. shape_role.
  std::string role;
  double blend = 0.0;
  // Составная форма: каждая дочерняя строка относится к основной через диапазон first/count
  // в её буфере. Операции идут по порядку: max добавляет твёрдые детали, min вырезает полости.
  struct part {
    std::string buffer;
    std::string first;
    std::string count;
    std::string op;
    std::string path;
    std::string text;
    std::string box_a;
    std::string box_b;
  };
  std::vector<part> parts;
  // Необязательная рамка капсулы (field_box::framed): поля строки с осями right/up и три выражения по
  // полям строки — сколько форма влияет поперёк по right, по up и за концом отрезка. Всё или ничего.
  std::string frame_right;
  std::string frame_up;
  std::string reach_right;
  std::string reach_up;
  std::string reach_end;
  // Необязательный НАКЛОН (field_box::slope): число или выражение по полям строки и `<слот>_slope`
  // (граница градиента слота словаря). Пусто — не объявлен, и поверхность возле формы не пропускает
  // пустых блоков.
  std::string slope;
};

// Общие шумовые слоты и фон поля задаются документом цепочки. Программа формы DS видит слоты
// по именам; веса фона и границы мира остаются данными, а не правилами хоста.
struct chain_noise_slot {
  std::string name;
  uint64_t seed_add = 0;
  double period = 1.0;
};

struct chain_background {
  struct term { std::string slot; double weight = 0.0; };
  double base = 0.0;
  double outside = 0.0;
  double y_min = -1e30;
  double y_max = 1e30;
  double upper = 0.0;
  double lower = 0.0;
  uint32_t tag = 0;
  std::vector<term> terms;
  std::string y_min_constant;
  std::string y_max_constant;
};

struct chain_description {
  std::string name;
  std::string constants;
  std::vector<chain_module> modules;
  std::vector<chain_noise_slot> noise_slots;
  chain_background background;
  bool has_background = false;
  std::vector<chain_shape> shapes;
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
  // Необязательная КАПСУЛА внутри коробки: отрезок ab ± capsule_radius (меньше нуля — нет). Точка вне
  // капсулы считается вне коробки: у длинной диагональной формы коробка-AABB огромна, а капсула — нет,
  // и это и есть ранний выход по расстоянию до оси, вынесенный из формулы в общее отсечение.
  std::array<double, 3> a{};
  std::array<double, 3> b{};
  double capsule_radius = -1.0;
  // Необязательная РАМКА отрезка (frame_right <= 0 — нет): точка вне, если её смещение от оси по right
  // или up больше frame_right/frame_up или если она дальше frame_end за концом отрезка. Это обещание
  // формулы в её собственных осях: у плоского широкого хода капсула радиуса reach во много раз больше.
  std::array<double, 3> right{};
  std::array<double, 3> up{};
  double frame_right = -1.0;
  double frame_up = -1.0;
  double frame_end = -1.0;
  // НАКЛОН: верхняя граница модуля градиента значения формы (единиц поля на метр) везде, где форма может
  // изменить поле. Обещание данных, как reach; бесконечность — не объявлен. По нему поверхность пропускает
  // блоки, в которых знак поля измениться не может (field_surface, параметр skip).
  double slope = std::numeric_limits<double>::infinity();

  bool contains(const std::array<double, 3>& p) const noexcept;
  bool overlaps(const std::array<double, 3>& other_low, const std::array<double, 3>& other_high) const noexcept;
  // Коробка отрезка ab, раздутая на radius.
  static field_box around(const std::array<double, 3>& a, const std::array<double, 3>& b, const double radius) noexcept;
  // То же с капсулой: точка дальше radius от отрезка — вне.
  static field_box capsule(const std::array<double, 3>& a, const std::array<double, 3>& b, const double radius) noexcept;
  // Капсула, дополнительно ограниченная рамкой отрезка. Коробка сужается до рамки, только если right, up
  // и ось ортонормированы: иначе проверка рамки верна, а её коробка — нет.
  static field_box framed(const std::array<double, 3>& a, const std::array<double, 3>& b, const double radius,
                          const std::array<double, 3>& right, const std::array<double, 3>& up, const double across_right,
                          const double across_up, const double beyond_end) noexcept;
};

struct shape_rows {
  std::shared_ptr<const void> rows;
  std::vector<field_box> boxes;
};

// Одна форма поля: вид, номер строки, стёртые строки вида и коробка влияния.
struct field_shape {
  uint32_t kind = 0;
  uint32_t row = 0;
  const void* rows = nullptr;
  field_box box;
};

// Формы одного вида из одного источника, разобранные ОДИН раз: поле чанка ссылается на них, а не
// копирует. low/high — общая коробка всех форм набора (пустой набор — low > high).
struct shape_set {
  shape_rows rows;
  std::vector<field_shape> shapes;
  std::array<double, 3> low{};
  std::array<double, 3> high{};
};
// Набор форм из строк вида; коробки без строк — громкий отказ.
shape_set make_shape_set(const size_t kind, shape_rows rows);

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
  // То же набором форм поля; держатель указателя держит и строки.
  std::shared_ptr<const shape_set> shapes(const shape_vocabulary& vocabulary, const size_t kind) const;

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
    std::shared_ptr<const shape_set> set;
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
  generation_context& context() const noexcept { return *context_; }
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
  generation_context* context_ = nullptr;
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
// Даёт invoker генератора в эксклюзивном execution context (Lua host принадлежит context).
using invoker_source = std::function<step_invoker(const chain_generator&, generation_context&)>;

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
  // Константы из документа цепочки: доступны в Lua как constants и step.constants (read-only),
  // также входят в общие параметры генераторов. values хоста накладываются поверх для входов запуска.
  parameters constants;
  size_t default_capacity = 256;
  // Словарь форм для полей генераторов. Не владеет; нужен, если хоть один генератор объявил поле.
  const shape_vocabulary* vocabulary = nullptr;
};

class generator_chain {
  friend class generator_run;

public:
  generator_chain(chain_description description, chain_setup setup);
  ~generator_chain();

  const chain_description& description() const noexcept;
  const chain_generator& generator(const std::string_view& name) const;

  // Нативное тело — исполнитель, который САМ пишет выходы: пайплайна у него нет.
  void set_native(const std::string_view& name, generator_executor body);
  void set_executor(const std::string_view& name, generator_executor executor);
  void set_invoker_source(invoker_source source);
  void set_invoker_source(std::function<step_invoker(const chain_generator&)> source);

  // Экземпляр из кэша или собранный сейчас. Потокобезопасно; одна сборка на ключ.
  result_ptr get(const std::string_view& name, const uint64_t world_seed, const instance_key& key) const;
  result_ptr get(generation_context& context, const std::string_view& name, const uint64_t world_seed, const instance_key& key) const;
  // Строит всё, от чего зависит экземпляр (разрешает его связи), но не сам экземпляр: опережающая
  // подготовка тяжёлых производителей для чанков впереди по ходу.
  void prefetch(const std::string_view& name, const uint64_t world_seed, const instance_key& key) const;
  void prefetch(generation_context& context, const std::string_view& name, const uint64_t world_seed, const instance_key& key) const;
  // Пересобирает экземпляр мимо кэша и сравнивает с кэшированным. Пусто — совпали.
  std::string verify(const std::string_view& name, const uint64_t world_seed, const instance_key& key) const;
  std::string verify(generation_context& context, const std::string_view& name, const uint64_t world_seed, const instance_key& key) const;

  // Отпечаток генератора вместе со всеми его производителями. Кэш живёт внутри цепочки, а её
  // содержимое после постройки не меняется, поэтому отпечаток называет КЭШ целиком, а не запись: он
  // для отчёта и для будущего кэша, который переживёт процесс.
  uint64_t fingerprint(const std::string_view& name) const;
  std::span<const script_text> scripts(const std::string_view& name) const;
  std::span<const script_text> modules() const noexcept;
  const parameters& constants() const noexcept;

  struct statistics {
    uint64_t hits = 0;
    uint64_t builds = 0;
    uint64_t evictions = 0;
    // Время: сборка целиком (со связями), своя часть (без разрешения связей) и ожидание чужой
    // сборки того же экземпляра другим потоком, в наносекундах.
    uint64_t build_ns = 0;
    uint64_t self_ns = 0;
    uint64_t wait_ns = 0;
    uint64_t waits = 0;
  };
  statistics stats(const std::string_view& name) const;
  // Статистика всех генераторов в порядке документа.
  std::vector<std::pair<std::string, statistics>> all_stats() const;
  // Поле генератора, собранное заново из связей экземпляра (связи — из кэша; шаги не исполняются). Для
  // генераторов с keep_fields = false, когда поле всё же нужно после сборки (сверка, отладка).
  std::shared_ptr<const primitive_field> field(const std::string_view& name, uint64_t world_seed,
                                               const instance_key& key, const std::string_view& field_name) const;
  std::shared_ptr<const primitive_field> field(generation_context& context, const std::string_view& name, uint64_t world_seed,
                                               const instance_key& key, const std::string_view& field_name) const;
  // Что держит кэш генератора сейчас: готовые экземпляры и байты их буферов (без полей и представлений).
  struct footprint {
    size_t instances = 0;
    size_t buffer_bytes = 0;
  };
  std::vector<std::pair<std::string, footprint>> cache_footprint() const;
  void reset_stats() const;

  // Convenience root calls lease chain-owned contexts. Nested/native calls pass run.context().
  // Only idle contexts can be discarded; cached generator results have independent lifetime.
  void release_contexts() const;
  size_t context_count() const;

private:
  struct context_slot;
  struct context_lease;
  context_lease acquire_context() const;
  mutable std::mutex contexts_lock_;
  mutable std::vector<std::unique_ptr<context_slot>> contexts_;
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

  // Сборка, захваченная разрешением fan-in и ждущая исполнителя: её берёт сам разрешающий поток по
  // порядку или поток, который ждёт экземпляр, в поддереве которого она лежит (parent — путь
  // зависимостей до неё). Другой выбор мог бы взять сборку, зависящую от недостроенного ниже по стеку
  // самого помощника, и это была бы взаимная блокировка.
  struct build_node;
  struct pending_build;
  struct claimed {
    std::shared_future<result_ptr> future;
    std::shared_ptr<pending_build> job; // не пусто — сборка за этим потоком
  };

  slot& find_slot(const std::string_view& name);
  const slot& find_slot(const std::string_view& name) const;
  claimed claim(generation_context& context, const slot& at, const uint64_t world_seed, const instance_key& key) const;
  void run_claim(generation_context& context, pending_build& job) const;
  void wait_helping(generation_context& context, const slot& at, const uint64_t world_seed, const instance_key& key,
                    const std::shared_future<result_ptr>& future) const;
  result_ptr build(generation_context& context, const slot& at, const uint64_t world_seed, const instance_key& key) const;
  std::vector<resolved_link> resolve(generation_context& context, const slot& at, const uint64_t world_seed, const instance_key& key) const;
  uint64_t fingerprint_of(const slot& at, std::vector<const slot*>& visiting) const;
  static void trim_outputs(const chain_generator& g, generator_result& result);

  mutable std::mutex pending_lock_;
  mutable std::deque<std::shared_ptr<pending_build>> pending_;

  chain_description description_;
  chain_setup setup_;
  std::vector<std::unique_ptr<slot>> slots_;
  invoker_source invokers_;
};

} // namespace originator
} // namespace devils_engine

#endif
