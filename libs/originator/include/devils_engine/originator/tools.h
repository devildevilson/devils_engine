#ifndef DEVILS_ENGINE_ORIGINATOR_TOOLS_H
#define DEVILS_ENGINE_ORIGINATOR_TOOLS_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "buffer.h"
#include "common.h"

// РЕЕСТР НАТИВНЫХ ИНСТРУМЕНТОВ, их проверка и запуск: `tool_description` (объявление), `dispatch`
// (исполнение), `tool_registry` (наборы, поставляемые движком).
//
// ПАРАЛЛЕЛЬНОСТЬ НЕ СООБЩАЕТСЯ, А ВЫВОДИТСЯ. Инструмент объявляет апертуру — как он адресуется
// относительно обрабатываемого элемента, — а движок из неё и из фактических привязок решает, можно
// ли разбивать работу. Скрипт не может соврать про то, чего он не знает, а нарушающий вызов не
// собирается: у привязки на чтение нет операции записи, а gather с совпадающими источником и
// приёмником отклоняется ДО исполнения. Тело получает диапазон элементов и обязано вести себя так,
// будто соседние диапазоны исполняются одновременно.
//
// ИНСТРУМЕНТ ОБЪЯВЛЯЕТ И СВОЮ УСТРОЙСТВЕННУЮ ФОРМУ — тело вычислительного шейдера, а не весь его
// текст. Привязки собирает `build_device_shader` (см. `device_form.h`), уже зная ВЫВЕДЕННЫЙ род
// каждого поля, поэтому тело пишется против аксессоров `in_<i>_at` / `out_<j>_set`, а не против
// `layout(...) buffer`. Соглашение о вызове одно на все инструменты и все переводы:
//
//   биндинги `0..input_count-1` — входы в порядке привязок, дальше выходы;
//   push-константа начинается ОБЩЕЙ ШАПКОЙ (`device_call_header`), за ней объявленные параметры по
//   одному `float` в объявленном порядке;
//   индекс элемента — `index`, охранник по `count` уже поставлен.
//
// Один способ выложить байты нужен затем, чтобы два способа не разъехались: шейдер, прочитавший
// чужие числа, не жалуется. По той же причине шапка ОДНА и для нативного тела, и для перевода `ds`.
//
// ЧЕГО ЗДЕСЬ ЖДАТЬ ПОДВОХА. Значение параметра по умолчанию названо ДВАЖДЫ: в `device_param` и в
// самом теле инструмента (`params->number("scale", 1.0)`). Разъехавшийся default означает, что вызов
// без параметра считает на двух путях разное, поэтому тест гоняет вызовы без необязательных
// параметров на обоих путях и сравнивает. Убрать дублирование можно, только если тело начнёт читать
// значения отсюда — это правка каждого тела, и делать её стоит со вторым потребителем.

namespace devils_engine {
namespace thread {
class atomic_pool;
}

namespace originator {

// Ссылка на конкретное поле конкретного буфера. Изменяемость наследуется от привязки шага:
// получить изменяемую ссылку на буфер, объявленный в reads, невозможно.
struct field_ref {
  const buffer* source = nullptr;
  buffer* target = nullptr; // nullptr => только чтение
  size_t field_index = 0;

  bool valid() const noexcept;
  bool writable() const noexcept;
  size_t count() const noexcept;
  field_type type() const noexcept;
  std::string_view buffer_name() const noexcept;
  std::string_view field_name() const noexcept;

  // Форма буфера, к которому относится поле. Не объявлена => буфер линейный.
  const buffer_extent& extent() const noexcept;

  const_field_accessor read() const noexcept;
  field_accessor write() const noexcept; // пустой аксессор, если ссылка не изменяемая

  // Один и тот же (буфер, поле). Поля НИКОГДА не делят байты, поэтому этого достаточно и для aos.
  bool same_field_as(const field_ref& other) const noexcept;
};

// Небольшой набор именованных параметров вызова. Числа и строки — всё, что нужно инструменту;
// тяжёлые данные лежат в буферах и приходят привязками.
class parameters {
public:
  void set_number(const std::string_view& name, const double value);
  void set_string(const std::string_view& name, std::string value);

  bool has(const std::string_view& name) const noexcept;

  // Перечисление — чтобы хост мог отдать параметры конфига дальше (например в таблицу lua),
  // не заводя второго представления тех же значений.
  size_t size() const noexcept;
  std::string_view name_at(const size_t index) const noexcept;
  bool is_string_at(const size_t index) const noexcept;
  double number_at(const size_t index) const noexcept;
  std::string_view string_at(const size_t index) const noexcept;

  // Накладывает other поверх текущего набора: то, что задано в other, побеждает. Нужно для общих
  // значений пайплайна, которые шаг может переопределить своим параметром.
  void overlay(const parameters& other);

  double number(const std::string_view& name, const double fallback = 0.0) const noexcept;
  int64_t integer(const std::string_view& name, const int64_t fallback = 0) const noexcept;
  std::string_view string(const std::string_view& name, const std::string_view& fallback = {}) const noexcept;

private:
  struct entry {
    std::string name;
    std::string text;
    double value = 0.0;
    bool is_string = false;
  };

  const entry* find(const std::string_view& name) const noexcept;

  std::vector<entry> entries_;
};

// Что инструмент получает в теле.
struct tool_call {
  std::span<const field_ref> inputs;
  std::span<const field_ref> outputs;
  const parameters* params = nullptr;
  uint64_t seed = 0;
  size_t range_begin = 0;
  size_t range_end = 0;
  std::string_view step_name;
  std::string_view tool_name;

  // Состояние, построенное подготовкой инструмента один раз до разбиения работы: пространственный
  // индекс, диаграмма, таблица — то, что нельзя строить в каждом чанке заново.
  const void* shared = nullptr;

  // Пул доступен ТОЛЬКО телам с апертурой scatter и sequential: их движок не разбивает, потому что
  // корректное разбиение зависит от алгоритма, и они организуют свои фазы сами.
  thread::atomic_pool* pool = nullptr;

  size_t range_count() const noexcept;
  const field_ref& input(const size_t index) const;
  const field_ref& output(const size_t index) const;
  // Привязан ли необязательный вход/выход. Тело обязано спрашивать, а не полагаться на счёт: не
  // привязанный выход это не пустой аксессор, а отсутствующий элемент списка.
  bool has_input(const size_t index) const noexcept;
  bool has_output(const size_t index) const noexcept;
};

// СЧЁТЧИК: поле, из которого число обрабатываемых элементов читается ПЕРЕД вызовом, а не задаётся
// числом при объявлении. Существует ради условного вычисления БЕЗ ВЫХОДА ИЗ ОЧЕРЕДИ: сколько
// элементов даст предыдущий проход, до исполнения не знает никто, а возврат в дирижёра за этим
// числом разрывает очередь ровно там, где она должна быть длинной. На устройстве тот же буфер и есть
// аргумент `dispatchIndirect`, поэтому объявление ОДНО, а путей два.
//
// Поле обязано быть однокомпонентным целым: дробное число элементов означало бы тихое усечение.
// Значение берётся из элемента 0 — счётчик это буфер на один элемент, а не массив.
bool valid_count_field(const field_ref& counter) noexcept;

// Читает счётчик и ЗАЖИМАЕТ его по ёмкости. Зажим, а не отказ, потому что на устройстве бросить
// нечем: если бы CPU отказывал, а GPU зажимал, у одного объявления было бы два поведения. Факт
// зажима возвращается вызывающему — тихо обрезанный проход не отследить по результату.
size_t read_count_field(const field_ref& counter, const size_t capacity, bool& clamped);

// ФОРМА, по которой инструмент адресует свой буфер. Берётся У ПРИВЯЗКИ, если буфер объявил `extent`,
// и только иначе — из параметров, по прежнему написанию (`width`, `size_x`/`size_y`). Одновременно и
// то, и другое — ГРОМКАЯ ошибка: два источника одной истины однажды разъедутся. Оси, которые прежнее
// написание не называет, выводятся из числа элементов: у двумерного растра `y = count / x`.
buffer_extent resolve_extent(const tool_call& call,
                             const field_ref& binding,
                             const char* legacy_x,
                             const char* legacy_y = nullptr,
                             const char* legacy_z = nullptr);

// Тело инструмента исполняется над ПОДДИАПАЗОНОМ. Для параллельных апертур поддиапазонов много и
// они идут одновременно, для sequential он ровно один и равен полному диапазону.
using tool_body = std::function<void(const tool_call& call, const size_t chunk_begin, const size_t chunk_end)>;

// Свёртка разбита на две части намеренно: частичный результат считается по чанку, а объединение
// идёт СТРОГО в порядке чанков. Поэтому сумма плавающих чисел не зависит от числа потоков.
using reduce_body = std::function<double(const tool_call& call, const size_t chunk_begin, const size_t chunk_end)>;
using reduce_combine = std::function<double(const double accumulated, const double partial)>;

// Подготовка: вызывается ОДИН раз перед разбиением работы, результат живёт до конца вызова и
// приходит в тело через tool_call::shared. Нужна инструментам, которым требуется общая структура.
using tool_prepare = std::function<std::shared_ptr<void>(const tool_call& call)>;

// Параметр устройственной формы: имя и значение ПО УМОЛЧАНИЮ (о дублировании default см. шапку).
struct device_param {
  std::string name;
  double fallback = 0.0;
  // Имя ПОЛЯ push-константы, если оно отличается от имени параметра. Различие нужно там, где имя
  // параметра занято в GLSL: `remap` принимает `min`/`max`, а это встроенные функции языка.
  std::string symbol;

  const std::string& shader_name() const noexcept { return symbol.empty() ? name : symbol; }
};

struct tool_description {
  std::string name;
  aperture::values shape = aperture::pointwise;
  uint32_t input_count = 0;
  // Сколько ПОСЛЕДНИХ входов можно не привязывать. Нужно там, где вход задаёт НЕОБЯЗАТЕЛЬНОЕ условие
  // (заранее занятые клетки решателя): буфер нулей ради отсутствия условия — плата памятью ни за что.
  uint32_t optional_inputs = 0;
  uint32_t output_count = 0;
  // Сколько ПОСЛЕДНИХ выходов можно не привязывать. Нужно там, где инструмент за тот же обход знает
  // попутную величину (`voronoi_label` — метку и близость к границе), которая нужна не всем.
  uint32_t optional_outputs = 0;
  // Инициализаторы по умолчанию нужны не для красоты: описание заполняется именованной
  // инициализацией с опущенными полями.
  tool_body body = nullptr;
  tool_prepare prepare = nullptr;

  // Заполняются только у апертуры reduce; тогда body остаётся пустым.
  reduce_body partial = nullptr;
  reduce_combine combine = nullptr;
  double initial = 0.0;

  // ТЕЛО вычислительного шейдера и порядок параметров в push-константе (соглашение — в шапке файла).
  // Пусто => инструмент на устройство не переносится, и очередь с ним считается на CPU. Объявляется
  // рядом с апертурой потому, что это утверждение того же рода: ни одно из двух не выводится из тела,
  // ведь тело у нативного инструмента — это C++.
  std::string device_body;
  // ФУНКЦИИ, которые тело зовёт: объявить их внутри `main` нельзя, а инструменту, не умещающемуся в
  // выражение (шум — это хеш решётки, интерполяция и октавы), они необходимы.
  std::string device_prelude;
  std::vector<device_param> device_params;

  // Номера входов (по позиции в привязках), которые устройственная форма читает ФИЛЬТРОМ — то есть
  // между элементами. Единственное, из чего выводится род ресурса: поле, которое кто-то читает так,
  // обязано стать картинкой, а всё остальное остаётся буфером и выигрывает по всем пунктам.
  std::vector<uint32_t> device_filtered_inputs;

  // ТЕЛО ГОДИТСЯ ДЛЯ ЛЮБОГО ДОПУЩЕННОГО РОДА. По умолчанию устройственный план отклоняет нативный
  // инструмент над целым полем: тело написано против `float` один раз на все будущие привязки. Флаг —
  // объявление автора, что тело кинд-агностично; тогда сборщик добавляет переводящие перегрузки
  // аксессоров. Цена названа: перевод идёт через `float32`, точный до 2^24, а на хосте то же поле
  // читается через double. Ставить стоит там, где значение мало или приходит параметром (`fill`).
  bool device_integer_ready = false;

  // ЗАПИСИ НЕ ЗАВИСЯТ ОТ ПОРЯДКА — объявление, которое пускает `scatter` в очередь. Апертура
  // отклоняется не за запись по чужим индексам, а за то, что порядок этих записей задаёт алгоритм;
  // целочисленное накопление — случай, где порядка нет вовсе. Свойство принадлежит КОНКРЕТНОМУ
  // инструменту, поэтому он его и объявляет. Плавающее накопление сюда не годится.
  bool order_free_writes = false;
};

// Общая шапка push-константы устройственного вызова. Порядок полей — часть соглашения из шапки файла.
// `seed` лежит здесь, а не в параметрах инструмента, потому что случайность нужна и переводу, и
// нативному телу, а второй способ передать зерно однажды разъехался бы с первым.
struct device_call_header {
  uint32_t count = 0;
  uint32_t begin = 0;
  uint32_t extent_x = 0;
  uint32_t extent_y = 0;
  uint32_t seed = 0;
};

// Свёртка 64-битного зерна вызова в 32 бита шапки: ширина хеша на устройстве 32-битная
// (`utils::shared::prng2`), поэтому старшая половина обязана участвовать, а не пропадать.
constexpr uint32_t fold_seed(const uint64_t seed) noexcept {
  return uint32_t(seed) ^ uint32_t(seed >> 32);
}

// Свёртка: много элементов -> одно значение. Разбиение на чанки ФИКСИРОВАНО и не зависит от числа
// потоков, иначе порядок сложения плавающих чисел менял бы результат.
constexpr size_t reduce_chunk_size = 65536;

class tool_registry {
public:
  void add(tool_description description);
  const tool_description* find(const std::string_view& name) const noexcept;
  const tool_description& at(const size_t index) const noexcept;
  size_t size() const noexcept;

  // Наборы, поставляемые движком. Разделены не по темам, а по СПОСОБУ адресоваться и отвечать:
  //   graph      — соседство вместо растра: у сферы нет ни окна, ни края карты;
  //   volume     — первый выход ПЕРЕМЕННОЙ длины (ёмкость объявлена, занятая длина приезжает буфером);
  //   constraint — не проход по полю, а РЕШЕНИЕ ЗАДАЧИ: может не найти ответа и попросить попытку;
  //   scatter    — произвольный параллельный разброс НЕ БЫВАЕТ детерминированным, детерминизм даёт
  //                конкретный алгоритм, поэтому апертура доступна только готовым инструментам.
  void add_standard_tools();
  void add_graph_tools();
  void add_volume_tools();
  void add_constraint_tools();
  void add_scatter_tools();

private:
  std::vector<tool_description> tools_;
};

// СОБСТВЕННЫЙ ШУМ ДВИЖКА: значение решётки, градиентный и ячеистый, с октавами. Ставится вместе со
// стандартным набором, потому что внешних зависимостей у него нет, и умеет ОБА пути — в отличие от
// обвязки FastNoise2, чьё дерево вычисляет её собственная библиотека. Совпадения с ней не обещается.
void add_noise_field_tools(tool_registry& registry);

// Инструмент, ПОДГОТОВЛЕННЫЙ к исполнению по частям: tool_call собран, prepare выполнен один раз,
// и дальше тело можно звать на любом поддиапазоне сколько угодно раз.
//
// Существует ради слияния проходов в очереди вычислений: обычный dispatch обходит данные «вызов,
// потом чанк», слияние переставляет обход на «плитка, потом вызов». Проверок здесь НЕТ намеренно —
// исполняющий по частям проверяет свой набор вызовов целиком и до начала работы (`check_queue`).
class prepared_call {
public:
  prepared_call(const tool_description& tool,
                const std::span<const field_ref>& inputs,
                const std::span<const field_ref>& outputs,
                const parameters& params,
                const uint64_t seed,
                const size_t range_begin,
                const size_t range_end,
                const std::string_view& step_name,
                thread::atomic_pool* pool);

  // Тело над поддиапазоном. Границы обязаны лежать внутри объявленного диапазона.
  void run(const size_t begin, const size_t end) const;

private:
  const tool_description* tool_ = nullptr;
  std::shared_ptr<void> shared_;
  tool_call call_;
};

// Результат проверки вызова. Пустое сообщение => вызов допустим.
struct dispatch_check {
  bool allowed = true;
  bool parallel = false;
  std::string message;
};

// Проверяет вызов, не исполняя его: число привязок, изменяемость выходов, диапазон и — для gather —
// непересечение источника с приёмником.
dispatch_check check_dispatch(const tool_description& tool,
                              const std::span<const field_ref>& inputs,
                              const std::span<const field_ref>& outputs,
                              const size_t range_begin,
                              const size_t range_end,
                              const std::string_view& step_name);

// Проверяет, допустим ли scatter-вызов при активном чанке; для остальных апертур разрешает всегда.
// Глобальный scatter в чанковом проходе — не «неточный», а бессмысленный: ни одна группа не
// заканчивается, и результат зависит от того, какие чанки успели посчитаться.
dispatch_check check_key_support(const tool_description& tool,
                                 const key_support::values support,
                                 const bool chunk_active,
                                 const std::string_view& step_name);

// Запускает инструмент. pool == nullptr => всё исполняется в вызывающем потоке; результат обязан
// совпадать бит в бит с параллельным исполнением, и это проверяется тестом.
// Недопустимый вызов — критическая ошибка конфига/скрипта: бросает utils::error с именем шага.
void dispatch(const tool_description& tool,
              const std::span<const field_ref>& inputs,
              const std::span<const field_ref>& outputs,
              const parameters& params,
              const uint64_t seed,
              const size_t range_begin,
              const size_t range_end,
              const std::string_view& step_name,
              thread::atomic_pool* pool);

// Свёртка. Возвращает значение, одинаковое при любом числе потоков и при pool == nullptr.
double dispatch_reduce(const tool_description& tool,
                       const std::span<const field_ref>& inputs,
                       const parameters& params,
                       const uint64_t seed,
                       const size_t range_begin,
                       const size_t range_end,
                       const std::string_view& step_name,
                       thread::atomic_pool* pool);

} // namespace originator
} // namespace devils_engine

#endif
