#ifndef DEVILS_ENGINE_ORIGINATOR_PIPELINE_H
#define DEVILS_ENGINE_ORIGINATOR_PIPELINE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "buffer.h"
#include "tools.h"

// ПАЙПЛАЙН ГЕНЕРАЦИИ: буферы и шаги, объявленные в конфиге, плюс разбор самого конфига
// (`parse_*`) и точка входа генератора.
//
// ИЗ ПРИВЯЗОК ШАГА (`reads`/`writes`) ВЫВОДИТСЯ ВСЁ ОСТАЛЬНОЕ — так же, как painter выводит барьеры
// из биндингов, а не из ручного списка: порядок и зависимости шагов, что доступно потребителю после
// шага N (это ровно `writes`), изменяемость вью у тела шага и громкий отказ до исполнения, если шаг
// читает то, чего никто не писал.
//
// ТЕЛО ШАГА ПАЙПЛАЙНУ НЕИЗВЕСТНО: он отдаёт наружу контекст, а исполняет вызывающий (обычно lua).
// Поэтому библиотека не тянет за собой ни lua, ни devils_script.
//
// ЧАНКИ И ФОН — ДВЕ РАЗНЫЕ ВЕЩИ. Фоновый поток живёт у потребителя: он решает, когда дёрнуть шаг и
// что делать с готовыми буферами. Здесь остаётся только то, чего без библиотеки не сделать —
// ПРАВИЛО НЕЗАВИСИМОСТИ: результат чанка зависит от (зерно мира, ключ чанка) и не зависит от того,
// какие чанки посчитаны раньше. Отсюда и вывод зерна шага хешем, а не потоком случайности, который
// протянулся бы через все чанки в порядке генерации.
//
// ЧТО ОБЪЯВЛЯЕТСЯ ОДИН РАЗ. Размер буфера — ИМЕНЕМ константы, а не числом (генератор обязан уметь
// назвать свою стоимость по памяти до запуска, и одно имя связывает буферы одного масштаба). Число,
// нужное нескольким шагам, — в общих значениях пайплайна: порог, продублированный в двух местах,
// однажды разъедется, и заметить это будет почти нечем.

namespace devils_engine {
namespace thread {
class atomic_pool;
}

namespace originator {

// Ключ чанка. Три координаты покрывают и плоскую сетку (x, y), и объём (x, y, z), и кубосферу
// (грань, i, j).
struct chunk_key {
  int64_t x = 0;
  int64_t y = 0;
  int64_t z = 0;

  bool operator==(const chunk_key& other) const noexcept = default;
};

// Объявление буфера из конфига.
struct buffer_description {
  std::string name;
  buffer_layout layout;
  // Имя константы размера. Пусто, если объявлена ФОРМА: тогда число элементов — произведение осей.
  std::string size_name;
  // Имена констант по осям, 1..3. ФОРМА — это то, чем буфер адресуют, и она объявляется вместо
  // размера, а не вместе с ним. Пусто => буфер линейный, адресуется номером элемента.
  std::vector<std::string> extent_names;
};

struct step_description {
  std::string name;
  std::string body; // demiurg-путь скрипта, тело шага
  std::vector<std::string> reads;
  std::vector<std::string> writes;
  parameters params;
  // Программы devils_script, доступные телу шага по имени. Тело решает, к каким полям их применить;
  // имена входов берутся у самих привязок, поэтому в скрипте поле зовётся ровно так же, как в конфиге.
  std::vector<std::pair<std::string, std::string>> programs;
};

// ТОЧКА ВХОДА: один документ, который называет свои части, — иначе хост обязан знать имена трёх
// файлов, то есть устройство чужого генератора, а мод не может назвать их иначе. Всё, что здесь
// написано, — demiurg-id (путь от корня модуля без расширения, `demiurg::absolute_resource_path`).
//
// Шаги лежат ВНУТРИ документа списком (`steps = [ { ... } { ... } ]`), а не блоками верхнего уровня:
// tavl читает документ либо как одну структуру, либо как список значений, и строка `values = ...`
// рядом с блоком `{ ... }` делает документ структурой, у которой блоки становятся лишними значениями.
struct pipeline_entry {
  std::string name;
  // Документ общих значений; пусто означает «у генератора их нет».
  std::string values;
  // Документ буферов. Обязателен: без буферов пайплайну нечего связывать.
  std::string buffers;
  // Буферы, приходящие ИЗВНЕ: их заполняет хост до запуска. См. pipeline_description::inputs.
  std::vector<std::string> inputs;
  std::vector<step_description> steps;
};

struct pipeline_description {
  std::string name;
  // Общие значения пайплайна. Шаг видит их среди своих параметров и может переопределить своим.
  parameters values;
  std::vector<buffer_description> buffers;
  // ВХОДЫ ПАЙПЛАЙНА: буферы, которые заполняет ХОСТ до запуска, а не какой-то шаг. Понятие пришло из
  // ДВУХМАСШТАБНОЙ генерации: грубый мировой проход отдаёт каркас, а чанковый пайплайн читает из него
  // то, что попадает в его область, — и кто попадает, знает только хост, владеющий резидентностью
  // каркаса. Объявляется АВТОРОМ, а не выводится из «буфер никто не пишет»: именно это библиотека и
  // обязана считать ошибкой, а отличить «привезли извне» от опечатки в имени можно лишь по
  // объявлению.
  //
  // КОНТРАКТ ДЛЯ ХОСТА: заполненный вход обязан быть ФУНКЦИЕЙ ОБЛАСТИ чанка, а не того, что успело
  // подгрузиться. Неполный вход — не «чуть хуже», а другой мир под тем же ключом; поэтому хост либо
  // гарантирует полноту, либо откладывает чанк.
  std::vector<std::string> inputs;
  std::vector<step_description> steps;
};

// Диапазон настройки одного значения: границы и шаг объявлены рядом с самим значением, поэтому хост
// строит настройку, НИЧЕГО не зная о смысле числа. Значение без объявленного диапазона не
// настраивается — правильный ответ по умолчанию: не всякое число мира имеет смысл крутить вслепую.
struct value_range {
  std::string name;
  double minimum = 0.0;
  double maximum = 0.0;
  double step = 0.0;

  double clamp(const double value) const noexcept;
  // Значение, сдвинутое на n шагов и прижатое к границам. Шаг применяется от МИНИМУМА, а не от
  // текущего значения: иначе накопленная дробная часть уводит настройку с сетки шага.
  double advance(const double value, const int64_t steps) const noexcept;
};

// Разбор конфига. Оба принимают текст одного документа tavl; список буферов пайплайна не
// объявляется отдельно — он выводится из шагов и сверяется с объявленными буферами.
std::vector<buffer_description> parse_buffers(const std::string_view& text, const std::string_view& label);
// Список шагов отдельным документом: только блоки, без строк верхнего уровня.
std::vector<step_description> parse_steps(const std::string_view& text, const std::string_view& label);
// Точка входа: имя генератора, ссылки на документы значений и буферов, шаги списком.
pipeline_entry parse_entry(const std::string_view& text, const std::string_view& label);
// Документ общих значений: numbers = { ... }, strings = { ... }.
parameters parse_values(const std::string_view& text, const std::string_view& label);
// Диапазоны настройки из того же документа: ranges = { имя = [минимум, максимум, шаг] }.
std::vector<value_range> parse_value_ranges(const std::string_view& text, const std::string_view& label);

// Именованные размеры, которые host подставляет в буферы.
class size_table {
public:
  void set(const std::string_view& name, const size_t value);
  bool has(const std::string_view& name) const noexcept;
  size_t get(const std::string_view& name, const std::string_view& buffer_name) const;

private:
  std::vector<std::pair<std::string, size_t>> entries_;
};

// Что тело шага видит на входе.
struct step_context {
  std::string_view name;
  size_t index = 0;
  // ДВА зерна с разными контрактами, путать их нельзя (выяснилось на первой же сверке чанкованной
  // генерации с картой целиком):
  //   seed        hash(зерно мира, имя шага) — НЕ зависит от чанка, поэтому поле по мировой позиции
  //               непрерывно через шов. Значение по умолчанию: шум, молча разъехавшийся на швах,
  //               хуже случайно совпавшего джиттера соседей;
  //   chunk_seed  hash(зерно мира, имя шага, ключ) — для того, что обязано быть НЕЗАВИСИМЫМ в каждом
  //               чанке: разброс объектов внутри чанка, локальные вариации.
  uint64_t seed = 0;
  uint64_t chunk_seed = 0;
  chunk_key chunk{};
  // Идёт ли чанкованная генерация. Выставляется вызовом set_chunk — сам ключ этого не показывает,
  // потому что чанк (0,0,0) такой же полноправный, как любой другой.
  bool chunked = false;
  const parameters* params = nullptr;
  std::span<const std::pair<std::string, std::string>> programs;

  // Привязки шага. Буфер в writes доступен и на чтение, буфер в reads — только на чтение.
  std::span<buffer* const> writes;
  std::span<const buffer* const> reads;

  buffer* find_write(const std::string_view& buffer_name) const noexcept;
  const buffer* find_read(const std::string_view& buffer_name) const noexcept;
};

using step_invoker = std::function<void(const step_context&)>;

class pipeline {
public:
  pipeline(pipeline_description description, const size_table& sizes, const uint64_t seed);

  const std::string& name() const noexcept;
  uint64_t seed() const noexcept;

  // Текущий чанк. Смена ключа НЕ перевыделяет буферы: один пайплайн генерирует много чанков подряд,
  // и это единственный способ не платить за аллокацию на каждый чанк.
  const chunk_key& chunk() const noexcept;
  bool chunked() const noexcept;
  void set_chunk(const chunk_key& key) noexcept;

  // Обнуляет все буферы. Правильному пайплайну это не нужно: каждый шаг пишет то, что читает
  // следующий. Метод существует ради доказательства обратного — если результат чанка меняется от
  // того, обнулили буферы или нет, значит какой-то шаг читает то, чего не писал.
  void clear_buffers() noexcept;

  size_t buffer_count() const noexcept;
  buffer& buffer_at(const size_t index) noexcept;
  buffer* find_buffer(const std::string_view& buffer_name) noexcept;
  size_t total_byte_size() const noexcept;

  size_t step_count() const noexcept;
  const step_description& step_at(const size_t index) const noexcept;

  // Буферы, которые стали валидными после исполнения шага index. Это ровно writes шага — отсюда же
  // берётся ответ на вопрос «что уже можно отдать наружу» в фоновом режиме.
  std::span<buffer* const> published_after(const size_t index) const noexcept;

  // Исполняет шаги по порядку, вызывая invoker на каждом.
  void run(const step_invoker& invoker);
  void run_step(const size_t index, const step_invoker& invoker);

private:
  void build(const size_table& sizes);
  void validate() const;

  pipeline_description description_;
  chunk_key chunk_{};
  bool chunked_ = false;
  // Готовые параметры шага: общие значения пайплайна плюс его собственные сверху. Считаются один раз
  // при построении, чтобы тело шага получало один набор, а не разбиралось в приоритетах.
  std::vector<parameters> step_params_;
  uint64_t seed_ = 0;
  std::vector<std::unique_ptr<buffer>> buffers_;
  std::vector<std::vector<buffer*>> step_writes_;
  std::vector<std::vector<const buffer*>> step_reads_;
};

} // namespace originator
} // namespace devils_engine

#endif
