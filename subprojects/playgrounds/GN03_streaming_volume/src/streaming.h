#ifndef DEVILS_ENGINE_GN03_STREAMING_H
#define DEVILS_ENGINE_GN03_STREAMING_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/vec3.hpp>

#include "devils_engine/originator/pipeline.h"

// Потоковая генерация окрестности наблюдателя и арена готовой геометрии.
//
// Ни одного графического типа здесь нет намеренно: и окно чанков, и арена — это числа, поэтому их
// можно проверить без окна и без устройства. Графика отвечает только на вопрос «нарисовать то, что
// дали», и она же решает, когда безопасно переписать байты.
//
// ЧАНКОВАНИЕ И ФОНОВОЕ ИСПОЛНЕНИЕ — ДВЕ РАЗНЫЕ ВЕЩИ, и библиотека это разделение держит: правило
// независимости чанков живёт в originator, а фоновый поток — здесь, у потребителя. Отсюда и выбор:
// каждый рабочий поток считает СВОЙ чанк целиком, со своим пайплайном и своим хостом скриптов, и
// пул инструментам не отдаётся вовсе. Так параллельность идёт по чанкам, а не внутри чанка, и
// доказанное свойство «результат чанка зависит только от (зерно, ключ)» превращает многопоточность
// из риска в арифметику.

namespace devils_engine::gn03 {

// Вершина, как её видит устройство. Двенадцать байт, и ни одно из полей не является тем, чем было
// сначала.
//
// ПОЗИЦИЯ — ФИКСИРОВАННАЯ ТОЧКА ВНУТРИ ЧАНКА, а не мировой float. Это и есть «всё относительно
// камеры» в своей главной части: геометрия не знает, где она в мире, а смещение своего чанка
// относительно чанка камеры берёт из таблицы на кадр. Отсюда два следствия, каждое лучше прежнего:
// уход камеры из чанка стоит обновления таблицы в килобайты вместо переписывания арены в десятки
// мегабайт, а точность больше не зависит от того, как далеко улетел наблюдатель — шаг решётки
// `span / 65535` это полмиллиметра на чанке в 32 метра, тогда как float32 на удалении сотни
// километров даёт уже сантиметры.
//
// НОРМАЛЬ — три знаковых байта: она единичная, и шаг 1/127 это 0.45 градуса, чего глазу хватает с
// большим запасом.
//
// НОМЕР ЧАНКА — слот в таблице смещений, а не ключ: ключ это три 64-битных числа, то есть больше
// самой вершины. Слот выдаёт арена при вставке, она же его и переиспользует.
struct gpu_vertex {
  uint16_t position[3]{};
  uint16_t chunk = 0;
  int8_t normal[3]{};
  int8_t padding = 0;
};
static_assert(sizeof(gpu_vertex) == 12);

// Сколько слотов чанков может быть в таблице смещений. Число объявлено здесь и в конфиге графа
// одним и тем же смыслом: больше слотов — больше чанков в мире одновременно.
inline constexpr size_t max_chunk_slots = 4096;

// Кодирование позиции в фиксированную точку. Вынесено в функцию, потому что обратное преобразование
// делает шейдер, и эти двое обязаны договориться об одном и том же: ноль это начало чанка, 65535 —
// его дальняя граница.
inline uint16_t encode_local_position(const double value, const double span) noexcept {
  const double normalized = span > 0.0 ? value / span : 0.0;
  const double clamped = normalized < 0.0 ? 0.0 : (normalized > 1.0 ? 1.0 : normalized);
  return uint16_t(clamped * 65535.0 + 0.5);
}

inline double decode_local_position(const uint16_t value, const double span) noexcept {
  return double(value) * span / 65535.0;
}

// СУЩНОСТЬ ЧАНКА, как её видит устройство: веха с местом, наклоном и родом.
//
// Позиция здесь float и УЖЕ приведена к системе чанка камеры, в отличие от вершины, которая лежит в
// координатах своего чанка и ищет смещение в таблице. Разница не в принципе, а в количестве: вершин
// миллионы, поэтому им нужна и упаковка, и таблица; сущностей — десяток на чанк, то есть тысячи на
// весь мир, и приводить их на процессоре каждый кадр дешевле, чем заводить им вторую арену.
//
// Само по себе это следствие того же правила «стоимость называется заранее»: 32 байта на сущность
// при десяти тысячах сущностей — это 320 килобайт на кадр, столько же пишет оверлей.
struct gpu_prop {
  float position[3]{};
  float scale = 1.0f;
  float normal[3]{};
  uint32_t kind = 0;
};
static_assert(sizeof(gpu_prop) == 32);

// Сущность чанка в координатах СВОЕГО чанка — то, что отдаёт генератор.
//
// `origin` — ИДЕНТИЧНОСТЬ: номер попытки генератора, на которой эта сущность появилась. Вместе с
// ключом чанка это её имя, переживающее выгрузку и пересчёт, и именно по нему мир помнит, что с ней
// сделали. Место в выходном массиве на эту роль не годится: оно зависит от того, сколько попыток до
// неё отказалось, поэтому подкрутка любого порога сделала бы память об одной сущности памятью о
// другой.
struct chunk_prop {
  double position[3]{};
  double normal[3]{};
  double size = 1.0;
  uint32_t kind = 0;
  uint32_t origin = 0;
};

// Готовая геометрия одного чанка плюс то, что о нём стоит знать. Позиции вершин здесь уже в
// координатах своего чанка, а номер слота ещё не заполнен: его выдаёт арена при вставке.
struct chunk_mesh {
  originator::chunk_key key{};
  std::vector<gpu_vertex> vertices;
  // Сущности чанка живут вместе с ним: выгрузка чанка — это и их выгрузка. Владелец здесь чанк, а не
  // мир, и это прямое следствие правила «сущность лежит по ключу чанка плюс смещение».
  std::vector<chunk_prop> props;
  double field_milliseconds = 0.0;
  double surface_milliseconds = 0.0;
  // Сколько биомов участвовало в этом чанке. Цена биомов равна их числу В ЧАНКЕ, а не в мире,
  // поэтому число едет вместе с чанком, а не выводится задним числом.
  uint32_t biomes_used = 0;
};

// ПЛАВАЮЩЕЕ НАЧАЛО КООРДИНАТ: мировое место — это целый ключ чанка плюс небольшое смещение внутри
// него. Ни одна большая мировая координата в float не попадает вовсе.
//
// ДВЕ РАЗНЫЕ БОЛЕЗНИ, и лечатся они разным. РАЗРЕШЕНИЕ (что число вообще способно выразить) лечит
// именно эта конструкция: смещение никогда не выходит за размер чанка, поэтому шаг сетки остаётся
// микронным и в миллионе метров от нуля, тогда как у наивной мировой координаты во float32 он там
// 0.06 метра. НАКОПЛЕНИЕ (сумма миллиона округлений) от начала координат не зависит вовсе — оно
// зависит от ШИРИНЫ НАКОПИТЕЛЯ, поэтому смещение здесь хранится в `double`.
//
// Все три величины измерены на 370 километрах пути шагами по 0.37 м:
//   наивно во float32                 — снос 2783.78 м (и выразить шаг в 5 см там уже нечем);
//   плавающее начало + float32        — снос 0.36 м (за пару игровых сессий видимая величина);
//   плавающее начало + double         — снос порядка 10^-9 м, то есть его нет.
//
// Позиция для отрисовки получается ОДНИМ преобразованием в float на кадр: там оно точно, потому что
// величина мала, и в накопитель не возвращается.
struct local_frame {
  originator::chunk_key key{};
  glm::dvec3 position{}; // внутри [0, span) по каждой оси после rebase
};

// Заворачивает позицию внутрь чанка и правит ключ. Деление именно с ОКРУГЛЕНИЕМ ВНИЗ, а не
// усечением к нулю: у усечения позиции -0.5 и +0.5 дают один и тот же ключ, то есть у начала
// координат окно перекошено на чанк, и там появляется полоса, которая то грузится, то нет.
void rebase(local_frame& frame, double span) noexcept;

// Мировое место. В double, а не во float: именно здесь большое число и появляется — в отчёте для
// человека, а не в математике кадра.
glm::dvec3 absolute_position(const local_frame& frame, double span) noexcept;

// Смещение чанка относительно чанка наблюдателя, в метрах. Разность считается в ЦЕЛЫХ ключах и
// только потом умножается на размер чанка, поэтому пока окно не шире нескольких чанков, все числа
// кадра остаются в пределах сотен метров.
glm::vec3 chunk_offset(const originator::chunk_key& key, const originator::chunk_key& origin,
                       double span) noexcept;

// Хеш ключа чанка. Нужен снаружи: потребитель держит по ключу свои данные (например сущности
// чанка), и заводить второе мнение о том, как ключ хешируется, незачем.
struct chunk_key_hash {
  size_t operator()(const originator::chunk_key& key) const noexcept;
};

// Окно чанков вокруг наблюдателя. Радиусы, а не абсолютные границы, и по всем трём осям: мир
// генерируется В ЛЮБОМ НАПРАВЛЕНИИ от наблюдателя, потому что выше уровня земли он не кончается —
// там висят острова, а ниже идут пещеры. Прежние абсолютные границы по вертикали означали «мир
// плоский, и у него есть верх», то есть ровно то, чего у объёма нет.
//
// Вертикальный радиус отдельный от горизонтального не по принципу, а по цене: слой вверх стоит
// столько же, сколько кольцо в стороны, а смотрят чаще вдоль, чем вверх.
struct chunk_window {
  originator::chunk_key centre{};
  int64_t horizontal_radius = 4;
  int64_t vertical_radius = 2;

  bool contains(const originator::chunk_key& key) const noexcept;
  size_t volume() const noexcept;
};

struct streamer_stats {
  size_t pending = 0;   // ждут своей очереди
  size_t in_flight = 0; // считаются прямо сейчас
  size_t present = 0;   // посчитаны и отданы потребителю
  size_t generated = 0; // всего посчитано за сеанс
  double last_milliseconds = 0.0;
  double total_milliseconds = 0.0;
  size_t largest_chunk_vertices = 0;
};

// Тело генерации одного чанка. Создаётся ОДИН раз на рабочий поток: у пайплайна свои буферы, у
// хоста скриптов свой sol::state, и делить их между потоками нельзя. Смена ключа буферы не
// перевыделяет, поэтому поток считает тысячи чанков без единой аллокации в пайплайне.
using chunk_generator = std::function<void(const originator::chunk_key&, chunk_mesh&)>;
using generator_factory = std::function<chunk_generator()>;

class chunk_streamer {
public:
  chunk_streamer(generator_factory factory, size_t worker_count);
  ~chunk_streamer();

  chunk_streamer(const chunk_streamer&) = delete;
  chunk_streamer& operator=(const chunk_streamer&) = delete;

  // Новое окно: чего не хватает — в очередь по близости к центру, что вышло за окно — в список на
  // выброс. Порядок именно по близости, потому что дальний чанк не виден, пока не досчитан ближний.
  void set_window(const chunk_window& window);

  // Готовое забирает потребитель. Пока он не забрал, чанк считается посчитанным, но не
  // присутствующим: присутствие означает «лежит в арене».
  bool pop_ready(chunk_mesh& out);

  std::vector<originator::chunk_key> take_evicted();

  // Чанк не поместился в арену: он забывается, и его посчитают заново, когда место появится.
  // Отдельный вызов, а не тихое «считать присутствующим», потому что арена и окно — разные вещи, и
  // потерянный между ними чанк выглядел бы как дырка в мире без единого сообщения.
  void forget(const originator::chunk_key& key);

  streamer_stats stats() const;

private:
  void worker_loop();
  bool next_request(originator::chunk_key& key);

  struct key_hash {
    size_t operator()(const originator::chunk_key& key) const noexcept;
  };

  generator_factory factory_;
  std::vector<std::thread> workers_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;

  chunk_window window_{};
  std::vector<originator::chunk_key> pending_; // отсортированы: последний — самый близкий
  std::unordered_map<originator::chunk_key, uint8_t, key_hash> state_;
  std::deque<chunk_mesh> ready_;
  std::vector<originator::chunk_key> evicted_;
  size_t in_flight_ = 0;
  size_t present_ = 0;
  streamer_stats totals_{};
};

// Арена вершин: одна непрерывная область, из которой чанки берут свои отрезки.
//
// Рисуется она ОДНИМ вызовом на [0, high_water), поэтому дырки внутри неизбежны — и заполняются
// вырожденными треугольниками (три совпавшие вершины), которые растеризатор отбрасывает. Это
// сознательный выбор против двух других: сдвигать хвост арены при каждой выгрузке значило бы
// переливать десятки мегабайт, а рисовать чанки по отдельности — заводить в painter команду, которой
// там нет.
//
// ОСВОБОЖДЁННЫЙ ОТРЕЗОК ОБНУЛЯЕТСЯ СРАЗУ, А В ОБОРОТ ВОЗВРАЩАЕТСЯ ПОЗЖЕ. Выгруженный чанк исчезает с
// экрана тем же кадром (вырожденный треугольник не рисуется), а ждать приходится только
// ПЕРЕИСПОЛЬЗОВАНИЯ места: пока кадры в полёте читают эти байты, писать туда новую геометрию значило
// бы смесь двух чанков в одном треугольнике.
class vertex_arena {
public:
  struct block {
    size_t first = 0;
    size_t count = 0;
  };

  // Чанк, лежащий в арене: ключ и слот в таблице смещений. Из этого списка графика каждый кадр
  // строит таблицу — по одному вектору на чанк, а не по одному на вершину.
  struct live_chunk {
    originator::chunk_key key{};
    uint32_t slot = 0;
  };

  vertex_arena(size_t capacity, size_t granularity, size_t retirement_frames);

  // Отрезок под геометрию чанка. false означает «места нет» (или кончились слоты) — вызывающий
  // обязан сказать об этом вслух, потому что только он знает, сколько чанков просил.
  //
  // Номер слота проставляется КАЖДОЙ вершине здесь же: копирование всё равно идёт, а знать свой слот
  // до вставки геометрия не может — его выдаёт арена.
  bool insert(const originator::chunk_key& key, std::span<const gpu_vertex> vertices);
  void remove(const originator::chunk_key& key);

  std::span<const live_chunk> live() const noexcept { return live_; }

  // Кадр закончен: отрезки, отжившие свой срок, возвращаются в оборот.
  void advance_frame();

  size_t high_water() const noexcept { return high_water_; }
  size_t capacity() const noexcept { return capacity_; }
  // Выданные вершины: занятое место, а не число вершин геометрии — отрезок занимает арену целиком,
  // включая округление до гранулярности.
  size_t used() const noexcept { return used_; }
  size_t hole_vertices() const noexcept;
  size_t live_blocks() const noexcept { return blocks_.size(); }

  std::span<const gpu_vertex> mirror() const noexcept { return mirror_; }

  // Что изменилось с прошлого раза: графика заливает на устройство только эти отрезки, а не всю
  // арену. Список забирается вместе с очисткой — второй раз те же байты везти незачем.
  std::vector<block> take_dirty();

private:
  size_t rounded(size_t count) const noexcept;
  void fill_degenerate(const block& region);
  void recompute_high_water() noexcept;

  struct key_hash {
    size_t operator()(const originator::chunk_key& key) const noexcept;
  };

  struct retiring {
    block region;
    size_t frames_left = 0;
  };

  size_t capacity_ = 0;
  size_t granularity_ = 1;
  size_t retirement_frames_ = 2;
  size_t high_water_ = 0;
  size_t used_ = 0;
  size_t top_ = 0; // граница ещё ни разу не выданной части арены

  std::vector<gpu_vertex> mirror_;
  struct entry {
    block region{};
    uint32_t slot = 0;
  };
  std::unordered_map<originator::chunk_key, entry, key_hash> blocks_;
  std::vector<live_chunk> live_;
  std::vector<uint32_t> free_slots_;
  std::vector<block> free_list_;
  std::vector<retiring> retiring_;
  std::vector<block> dirty_;
};

} // namespace devils_engine::gn03

#endif
