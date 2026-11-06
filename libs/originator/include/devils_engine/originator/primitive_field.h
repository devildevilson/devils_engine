#ifndef DEVILS_ENGINE_ORIGINATOR_PRIMITIVE_FIELD_H
#define DEVILS_ENGINE_ORIGINATOR_PRIMITIVE_FIELD_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "generator_chain.h"
#include "tools.h"

// ПОЛЕ ИЗ ПРИМИТИВОВ: «что в точке — порода или вода» как минимум форм, которые генераторы цепочки
// отдали БУФЕРАМИ. Генератор ребра пишет галереи, генератор пещеры — ниши и ходы; поле собирается из
// их выходов и спрашивается общими запросами.
//
// ЧТО ЧЬЁ. Проект регистрирует СЛОВАРЬ ФОРМ (`shape_vocabulary`): как вид формы читает свои строки из
// выходов экземпляра, его КОРОБКУ ВЛИЯНИЯ и значение в точке; общие величины точки (шумы, сдвиг) —
// ленивыми слотами; фон и его верхнюю оценку. Библиотека не знает, как выглядит галерея: она знает,
// как собрать поле — индекс коробок, отсечение, минимум, вид формы, давшей минимум, — и как его
// спрашивать: значение, луч до породы или воды.
//
// КОРОБКА ВЛИЯНИЯ — обещание вида формы: вне коробки значение формы не ниже верхней оценки фона.
// На нём стоит точность отсечения: минимум не зависит от порядка и от того, какие формы ВНЕ коробки
// не посчитаны, поэтому подготовленная область и поле целиком дают одно и то же значение БИТ В БИТ.
// Ничья в минимуме оставляет форму, встреченную раньше (порядок — порядок объявления членов поля и
// строк внутри вида), как у std::min.
//
// ФОН может сказать «дальше не считать» (`stop`): точка вне мира — это порода, какие бы формы её ни
// касались.
//
// Поле объявляется у генератора в документе цепочки и строится из его разрешённых связей ДО шагов:
//   fields = [ { name = natural, terms = [ { kind = gallery, link = window }, { kind = hall, link = window } ] } ]
// Шаги спрашивают его инструментами `field_sample` и `field_march` (`add_field_tools`) по имени поля;
// нативный исполнитель — через `generator_run::field`. Хост, у которого строки уже есть объектом,
// собирает поле из `field_term_rows`: формулы остаются в словаре, сборка и запросы — здесь.

namespace devils_engine {
namespace originator {

// Коробка влияния (field_box) и стёртые строки вида (shape_rows) объявлены в generator_chain.h:
// экземпляр цепочки кэширует свои разобранные строки.
using field_vector = std::array<double, 3>;

class shape_vocabulary;

// Точка поля со своими слотами: каждый слот считается при первом запросе и один раз на точку.
class field_point {
public:
  field_point(const shape_vocabulary& vocabulary, const field_vector& position, const uint64_t seed = 0) noexcept;

  const field_vector& position() const noexcept;
  // Зерно мира поля: шумы точки — функции зерна и позиции.
  uint64_t seed() const noexcept;
  double slot(const size_t index);

  static constexpr size_t max_slots = 16;

private:
  const shape_vocabulary* vocabulary_ = nullptr;
  field_vector position_{};
  uint64_t seed_ = 0;
  std::array<double, max_slots> values_{};
  uint32_t ready_ = 0;
};

// Значение формы и метка того, ЧТО это (у мира — вид породы для палитры).
struct shape_value {
  double value = 0.0;
  uint32_t tag = 0;
};

// Пакет точек для вида, который считает СТОЛБЦОМ (колоночный ds): позиции по осям, точки со слотами
// (шумы считаются лениво и один раз на точку), выходы — значение и метка на точку.
struct field_batch {
  size_t count = 0;
  const double* x = nullptr;
  const double* y = nullptr;
  const double* z = nullptr;
  field_point* const* points = nullptr;
  double* values = nullptr;
  uint32_t* tags = nullptr;
};

// Роль вида в поле. Поле считается в четыре стадии: ПРИРОДНОЕ (минимум фона и форм — формы вынимают
// породу), ЗАЛИВКА (сглаженный максимум с природным — форма приносит свою породу, например блок
// постройки; радиус смешения blend заполняет швы, а не оставляет складку), ВЫРЕЗ (минимум поверх
// заливки — помещения построек внутри своей же породы), ВСТАВКА (сглаженный максимум поверх выреза —
// порода внутри вырезанного: колонны, сталагмиты; без неё вырез стёр бы их). Порядок слагаемых внутри
// стадии при blend = 0 важен только для метки при равных значениях. С blend > 0 природное и вырез
// используют сглаженный минимум; смешение нескольких форм последовательно и зависит от порядка.
enum class shape_role : uint8_t { natural, fill, cut, insert };

class shape_vocabulary {
public:
  using slot_function = std::function<double(field_point&)>;
  // Значение фона; `stop` = точка решена фоном, формы не считаются.
  using background_function = std::function<shape_value(field_point&, bool& stop)>;

  // Строки вида формы из выходов одного экземпляра: данные и коробки, параллельно (generator_chain.h).
  using decoded = shape_rows;
  using decode_function = std::function<decoded(const generator_result&)>;
  using value_function = std::function<shape_value(const void* rows, size_t row, field_point&)>;

  size_t add_slot(std::string name, slot_function compute);
  size_t slot(const std::string_view& name) const;
  const slot_function& slot_at(const size_t index) const noexcept;

  // upper_bound — верхняя оценка фона везде: коробка влияния формы — там, где форма может быть ниже.
  // lower_bound — нижняя оценка фона: если она выше порога поверхности, область без форм — сплошная
  // порода, и её можно не сэмплировать (field_bound).
  void set_background(background_function function, const double upper_bound,
                      const double lower_bound = -std::numeric_limits<double>::infinity());
  const background_function& background() const noexcept;
  double background_bound() const noexcept;
  double background_lower() const noexcept;

  // Строки своего C++-типа в стёртый вид: так их отдаёт decode, и так же их собирает хост, у которого
  // строки уже есть объектом (см. field_term_rows).
  template <typename row_t>
  static decoded pack(std::vector<std::pair<row_t, field_box>> rows) {
    decoded out;
    auto data = std::make_shared<std::vector<row_t>>();
    data->reserve(rows.size());
    out.boxes.reserve(rows.size());
    for (auto& [row, box] : rows) {
      data->push_back(std::move(row));
      out.boxes.push_back(box);
    }
    out.rows = std::move(data);
    return out;
  }

  // Вид формы через свой C++-тип строки: decode отдаёт строки и их коробки, value считает одну.
  template <typename row_t>
  void add_kind(std::string name, std::function<std::vector<std::pair<row_t, field_box>>(const generator_result&)> decode,
                std::function<shape_value(const row_t&, field_point&)> value) {
    add_kind_erased(
      std::move(name),
      [decode = std::move(decode)](const generator_result& instance) { return pack(decode(instance)); },
      [value = std::move(value)](const void* rows, const size_t row, field_point& point) {
        return value((*static_cast<const std::vector<row_t>*>(rows))[row], point);
      });
  }

  // Вид, который считает пакетом точек (одна строка формы — много точек): столбцом считать дешевле,
  // постоянные строки поднимаются из цикла. Поточечный вызов идёт пакетом из одной точки.
  using batch_function = std::function<void(const void* rows, size_t row, field_batch& batch)>;
  void add_kind_batch(std::string name, decode_function decode, batch_function batch);

  void add_kind_erased(std::string name, decode_function decode, value_function value);
  // Роль вида (по умолчанию природная); blend — радиус сглаженного min/max данной стадии, метры.
  void set_role(const std::string_view& name, shape_role role, double blend = 0.0);
  shape_role role(const size_t kind) const noexcept;
  double blend(const size_t kind) const noexcept;
  size_t kind(const std::string_view& name) const;
  const decode_function& decoder(const size_t kind) const noexcept;
  const value_function& evaluator(const size_t kind) const noexcept;
  // Пакетная форма вида; пусто — вид считает по точке.
  const batch_function& batcher(const size_t kind) const noexcept;
  size_t slot_count() const noexcept { return slots_.size(); }
  std::string_view slot_name(const size_t index) const noexcept { return slots_[index].name; }

private:
  struct named_slot {
    std::string name;
    slot_function compute;
  };
  struct named_kind {
    std::string name;
    decode_function decode;
    value_function value;
    batch_function batch;
    shape_role role = shape_role::natural;
    double blend = 0.0;
  };
  std::vector<named_slot> slots_;
  std::vector<named_kind> kinds_;
  background_function background_;
  double background_bound_ = 0.0;
  double background_lower_ = -std::numeric_limits<double>::infinity();
};

// Член поля: вид формы и экземпляры, из выходов которых он читает строки.
struct field_term_source {
  std::string kind;
  std::vector<result_ptr> instances;
};

// Член поля, чьи строки хост уже держит объектом (а не буферами экземпляра): вид и строки,
// упакованные `shape_vocabulary::pack`. Значение считает тот же вид словаря — формула одна.
struct field_term_rows {
  std::string kind;
  shape_vocabulary::decoded rows;
};

class primitive_field {
public:
  primitive_field(const shape_vocabulary& vocabulary, std::span<const field_term_source> terms,
                  const uint64_t seed = 0);
  primitive_field(const shape_vocabulary& vocabulary, std::span<const field_term_rows> terms,
                  const uint64_t seed = 0);

  const shape_vocabulary& vocabulary() const noexcept;
  uint64_t seed() const noexcept;
  size_t shape_count() const noexcept;

  // Формы, чьи коробки задевают область. Значение в точке ВНЕ области — значение без отсечения
  // (как у поля целиком), поэтому область — только ускорение.
  class region {
  public:
    shape_value value(const field_vector& p) const;
    const field_vector& low() const noexcept;
    const field_vector& high() const noexcept;
    bool contains(const field_vector& p) const noexcept;
    size_t shape_count() const noexcept;

  private:
    friend class primitive_field;
    struct shape_ref;
    const primitive_field* field_ = nullptr;
    field_vector low_{}, high_{};
    std::vector<const void*> shapes_;
  };

  region prepare(const field_vector& low, const field_vector& high) const;
  shape_value value(const field_vector& p) const;

  // Значения во многих точках сразу: точки бьются на клетки со стороной 2 * half, у клетки — своя
  // область, и формы считаются ФОРМА ЗА ФОРМОЙ над столбцом точек клетки (пакетные виды — пакетом).
  // Порядок форм в каждой точке тот же, что у поточечного значения, поэтому и ответ тот же.
  void sample_points(std::span<const field_vector> points, std::span<double> values, std::span<uint32_t> tags,
                     const double half) const;

  // Значения по точкам подряд: одна подготовленная область (куб со стороной 2 * half) служит
  // соседним точкам; точка вне неё пересобирает область вокруг себя.
  class sampler {
  public:
    sampler(const primitive_field& field, const double half);
    double operator()(const field_vector& p);
    shape_value sample(const field_vector& p);

  private:
    const primitive_field* field_;
    double half_;
    std::optional<region> region_;
  };

  // Первое расстояние s в [from, to] с шагом step, где поле в origin + direction * s — порода
  // (value > 0) или вода (value <= 0); -1, если такого нет. refine > 0 уточняет границу делением пополам
  // между последним промахом и попаданием и отдаёт сторону попадания.
  double march(sampler& samples, const field_vector& origin, const field_vector& direction, const double from,
               const double to, const double step, const bool want_rock, const uint32_t refine = 0) const;

private:
  struct shape {
    uint32_t kind = 0;
    uint32_t row = 0;
    const void* rows = nullptr;
    field_box box;
  };
  shape_value evaluate(std::span<const void* const> candidates, const field_vector& p) const;
  void add_rows(const size_t kind, shape_vocabulary::decoded decoded);
  void collect(const field_vector& low, const field_vector& high, std::vector<const void*>& out) const;

  const shape_vocabulary* vocabulary_ = nullptr;
  uint64_t seed_ = 0;
  std::vector<std::shared_ptr<const void>> sources_;
  std::vector<shape> shapes_;
};

// Поле, объявленное у текущего исполняемого генератора цепочки: инструменты зовут его по имени.
// Вне сборки экземпляра или с неизвестным именем — громкий отказ.
const primitive_field& current_field(const std::string_view& name);
// То же с владением: блок, который держит подготовленную выборку между вызовами, держит и поле — по
// указателю тогда видно, что это то же самое поле, а не новое по тому же адресу.
const std::shared_ptr<const primitive_field>& current_field_handle(const std::string_view& name);

} // namespace originator
} // namespace devils_engine

#endif
