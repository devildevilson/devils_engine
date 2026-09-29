#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "devils_engine/originator/tools.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/shared.h"

// ПОСТРОЙКИ: то, что СДЕЛАНО, а не выросло.
//
// Весь остальной генератор отвечает на вопрос «какое здесь значение»: шум даёт плавность, вороной —
// области, заливка по графу — достижимость по стоимости, решатель ограничений — жёсткий локальный
// запрет. Все они ПОЭЛЕМЕНТНЫ по обещанию: что бы инструмент ни сказал про клетку, он говорит это
// про клетку и её окрестность.
//
// У постройки обещание ДРУГОГО РОДА, и оно ГЛОБАЛЬНОЕ: из любой комнаты можно дойти до любой другой.
// Это свойство всей карты сразу, и никакое локальное правило его не даёт — раскладка, у которой
// каждая пара соседей законна, спокойно распадается на два отрезанных друг от друга куска. Ровно
// поэтому набор отдельный: он не про способ адресоваться, а про то, что инструмент ОБЕЩАЕТ.
//
// ЗДЕСЬ ДВА СЕМЕЙСТВА, И ОНИ ОТВЕЧАЮТ НА РАЗНЫЕ ВОПРОСЫ.
//
// НОРА — то, что ВЫРЕЗАЮТ в камне: места разбросаны, между ними порода, связь это прорытый ход.
//
//   place_rooms      ПЛАН: непересекающиеся прямоугольники в объявленных границах;
//   link_rooms       ПЛАН: остов над комнатами — из него и берётся обещание достижимости;
//   link_path        ПЛАН -> ГЕОМЕТРИЯ: связь превращается в отрезок между центрами комнат;
//   carve_corridors  ГЕОМЕТРИЯ: отрезок в клетки растра коленом из двух осевых пробегов.
//
// ЗДАНИЕ — то, что ДЕЛЯТ: пятно режется на места, стены общие, связь это дверь в стене.
//
//   slice_zones      ПЛАН: пятно -> места; циркуляция кладётся ПЕРВОЙ, места нарезаются под неё;
//   zone_joins       ПЛАН: стыки мест выводятся из ПЛАНА ровно один раз;
//   zone_graph       ПЛАН: стыки -> каноническое соседство (CSR) для инструментов графа;
//   open_doors       ПЛАН: какие стыки становятся дверьми, а дверь — это ЗОНА;
//   place_columns    ПЛАН: колоннада внутри места (колонна — это кусок стены, а не зона);
//   project_zones    ПЛАН -> КАРТИНКА: окно в мере, показанное клетками. Последний шаг.
//
// ПЛАН ЖИВЁТ В МЕРЕ, А РАСТР — ЭТО ВИД, и у здания это не удобство, а условие. Пока стыки выводились
// из растра, план был привязан к картинке трижды: пятно не могло быть крупнее изображения, мера
// длины была пикселем, а цена разбора стыков росла вместе с числом клеток — хотя стыки есть свойство
// десятков мест. Теперь ни один инструмент постройки клетки не читает, `project_zones` стоит
// последним, и его выход не читает никто. Отсюда обещание, которое проверяется прямо: ОДИН И ТОТ ЖЕ
// ПЛАН В ЛЮБОМ ОКНЕ И ЛЮБОМ МАСШТАБЕ ОСТАЁТСЯ ТЕМ ЖЕ ПЛАНОМ.
//
// Общий у семейств ровно один инструмент — `paint_rects` (прямоугольники И ИХ ФОРМЫ в клетки, номер
// элемента плюс один), и он остался у НОРЫ: у неё растр и есть предмет — в камне вырезают клетки, а
// не меры. Разница с проектором ровно в этом и названа: `paint_rects` пишет ИСТИНУ и отказывает,
// когда её теряют; `project_zones` делает КАРТИНКУ и числом сообщает, чего на ней не видно.
// Всё остальное различается, потому что нора и здание отличаются не параметрами, а тем, что одно
// вырезают, а другое делят.
//
// РАЗДЕЛЕНИЕ ПЛАНА И ГЕОМЕТРИИ НЕ УКРАШЕНИЕ. План — это список из десятков элементов, и он целиком
// помещается в голове (и в дампе); геометрия — это миллион клеток. Пока они разделены, «дверь не
// туда» видно в плане, а не в растре, и усложнять план (BSP, этажи, ключи и замки) можно, не трогая
// вырезание. И проверять их надо ПО ОТДЕЛЬНОСТИ: остов над комнатами и связность растра — РАЗНЫЕ
// утверждения, и второе не следует из первого само (коридор, уехавший за край карты, обрезается, и
// план остаётся верным при разорванном подземелье).
//
// ШОВ МЕЖДУ НИМИ — `link_path`, и он существует отдельным инструментом не от любви к дроблению.
// Вырезанию нужны КООРДИНАТЫ концов, а связь знает только номера комнат; сложить их можно двумя
// способами. Первый — дописать координаты в список связей прямо при построении остова: дёшево, но
// тогда одни и те же центры лежат в двух местах и однажды разъедутся (комнату подвинули, связь
// осталась со старым концом, и по карте это выглядит как коридор в стену). Второй — ВЫВОДИТЬ концы
// из плана отдельным проходом, который можно повторить. Здесь второй: `link_path` это чистая функция
// от (связи, комнаты), а `carve_corridors` про комнаты уже ничего не знает — он вырезает колено
// между двумя точками, и тем же инструментом режется дорога или канал.
//
// ЁМКОСТЬ — ЭТО ЖЕЛАНИЕ, А НЕ ФАКТ. Сколько комнат поместится, до запуска не знает никто: это зависит
// от размеров, зазора и того, куда легли предыдущие. Поэтому объявляется ЁМКОСТЬ буфера, а сколько
// занято — приезжает счётчиком, ровно как у поверхности из `marching_cubes`. Читать список до
// ёмкости вместо счётчика значит читать мусор, оставшийся от прошлого прогона.
//
// ЦЕЛОЧИСЛЕННОСТЬ. Ни одно решение здесь не принимается сравнением плавающих чисел: размеры и
// координаты — целые, расстояние между комнатами — манхэттенское (оно же длина колена), выбор —
// остаток от деления хеша. Тот же урок, что у решателя ограничений с энтропией: случайность обязана
// быть воспроизводимой, а сравнение `double` на разных машинах — не обязано.
//
// ТЕЛ ДЛЯ УСТРОЙСТВА ЗДЕСЬ НЕТ, И ПРИЧИН У ЭТОГО ТРИ РАЗНЫХ.
//
//   place_rooms, link_rooms, slice_zones, zone_joins, zone_graph, open_doors, place_columns,
//   project_zones             `sequential`, то есть `refused` АПЕРТУРОЙ, как решатель ограничений:
//                             принятая комната меняет судьбу следующей попытки, следующее ребро
//                             остова зависит от уже построенного дерева, а открытая дверь — от того,
//                             что уже связано. У колоннады и проекции причина своя: обе ПРИРАЩИВАЮТ
//                             (список опор, счёт потерянного), а у приращения есть порядок;
//   paint_rects, carve_corridors  пишут по чужим индексам, и порядок записей становится безразличен
//                             только потому, что прямоугольники НЕ ПЕРЕСЕКАЮТСЯ — а это обещание
//                             СОСЕДНЕГО инструмента, не их собственное. Объявить `order_free_writes`
//                             по чужому обещанию нельзя: список прямоугольников можно собрать руками;
//   link_path                 параллелится и телу не противоречит — но это ЦЕНА ВХОДА: диапазон у
//                             него десятки элементов, и передача плана на устройство дороже всей
//                             работы. Писать тело стоит тогда, когда план начнут строить на
//                             устройстве целиком, и не раньше.

namespace devils_engine {
namespace originator {

namespace {

// Прямоугольник плана: левый верхний угол и размеры В КЛЕТКАХ. Поле у него ОДНО, четырёхкомпонентное
// (`rect = ui4`), а не четыре однокомпонентных: комната это один элемент списка, и разложенная на
// четыре параллельных массива она пережила бы ровно один рефакторинг.
struct plan_rect {
  int64_t x = 0;
  int64_t y = 0;
  int64_t w = 0;
  int64_t h = 0;

  int64_t centre_x() const noexcept { return x + w / 2; }
  int64_t centre_y() const noexcept { return y + h / 2; }
};

plan_rect read_rect(const const_field_accessor& field, const size_t index) noexcept {
  return plan_rect{int64_t(field.get(index, 0)), int64_t(field.get(index, 1)),
                   int64_t(field.get(index, 2)), int64_t(field.get(index, 3))};
}

void write_rect(const field_accessor& field, const size_t index, const plan_rect& value) noexcept {
  field.set(index, double(value.x), 0);
  field.set(index, double(value.y), 1);
  field.set(index, double(value.w), 2);
  field.set(index, double(value.h), 3);
}

// Пересекаются ли прямоугольники, РАЗДВИНУТЫЕ на зазор. Зазор нужен не для красоты: две комнаты,
// касающиеся стенами, на растре сливаются в одну комнату странной формы, и план начинает описывать
// не то, что вырезано.
bool overlaps(const plan_rect& a, const plan_rect& b, const int64_t gap) noexcept {
  return a.x < b.x + b.w + gap && b.x < a.x + a.w + gap &&
         a.y < b.y + b.h + gap && b.y < a.y + a.h + gap;
}

int64_t manhattan(const plan_rect& a, const plan_rect& b) noexcept {
  return std::abs(a.centre_x() - b.centre_x()) + std::abs(a.centre_y() - b.centre_y());
}

// Четыре числа из одного номера попытки. Зерно попытки берётся от зерна вызова, а дальше хеш
// ВЫЗЫВАЕТСЯ С НОМЕРОМ, а не наматывается на состояние: тогда попытка номер N не зависит от того,
// сколько чисел взяли предыдущие, и отладка одной попытки не двигает остальные.
struct attempt_dice {
  uint32_t seed = 0;

  uint32_t roll(const uint32_t index) const noexcept { return utils::shared::prng2(seed, index + 1u); }

  // Остаток от деления хеша. Смещение у такого выбора есть (диапазон не делит 2^32 нацело), и оно
  // здесь не имеет значения: речь про размер комнаты в клетках, а не про статистику. Зато выбор
  // ЦЕЛОЧИСЛЕННЫЙ и повторяется всюду одинаково.
  int64_t range(const uint32_t index, const int64_t low, const int64_t high) const noexcept {
    if (high <= low) return low;
    return low + int64_t(roll(index) % uint32_t(high - low + 1));
  }
};

uint32_t call_seed(const tool_call& call) noexcept {
  return fold_seed(call.seed);
}

// place_rooms: непересекающиеся прямоугольники в объявленных границах.
//
// Отбор с отказом (rejection sampling), и ничего умнее здесь намеренно нет: BSP-разбиение даёт
// плотную упаковку, но и вид у него узнаваемо «расчерченный», а отбор с отказом честно показывает,
// чего стоит плотность — числом потраченных попыток. Комнату принимают, если она не задевает ни одну
// уже принятую с учётом зазора; иначе попытка выбрасывается целиком.
//
// ВРЕМЕННЫХ ТАБЛИЦ НЕТ: принятые комнаты живут в выходном буфере, и проверка пересечения читает его
// же. Цена — `попытки x принятые` сравнений, и это названо в README, а не спрятано.
void tool_place_rooms(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': place_rooms fills the WHOLE room list, so its range must start at 0, "
                   "got [{}, {})", call.step_name, begin, end);
  }

  const auto rects = call.output(0).write();
  // Тот же буфер на чтение: уже принятые комнаты и есть таблица, по которой проверяется пересечение.
  const auto placed = call.output(0).read();
  const auto counter = call.output(1).write();
  const size_t capacity = end > begin ? end - begin : 0;

  const int64_t width = call.params->integer("width", 0);
  const int64_t height = call.params->integer("height", 0);
  if (width <= 0 || height <= 0) {
    utils::error{}("originator step '{}': place_rooms needs the raster bounds — pass width and height from the "
                   "extent of the grid the rooms are planned for, got {}x{}", call.step_name, width, height);
  }

  const int64_t min_size = std::max<int64_t>(call.params->integer("min_size", 3), 1);
  const int64_t max_size = std::max<int64_t>(call.params->integer("max_size", min_size), min_size);
  const int64_t gap = std::max<int64_t>(call.params->integer("gap", 1), 0);
  // Отступ от края растра. Единица по умолчанию потому, что у подземелья есть внешняя стена: комната,
  // прижатая к краю карты, стеной наружу не обнесена.
  const int64_t border = std::max<int64_t>(call.params->integer("border", 1), 0);

  if (min_size + 2 * border > width || min_size + 2 * border > height) {
    utils::error{}("originator step '{}': place_rooms cannot fit even the smallest room ({} cells plus {} of border "
                   "on each side) into a {}x{} raster", call.step_name, min_size, border, width, height);
  }

  // Ноль означает «посчитай сам»: тридцать две попытки на объявленную комнату — то число, при котором
  // на обычных плотностях ёмкость выбирается целиком, а на невозможной плотности отбор всё-таки
  // заканчивается. Названо здесь, а не угадывается конфигом.
  const int64_t declared_attempts = call.params->integer("attempts", 0);
  const size_t attempts = declared_attempts > 0 ? size_t(declared_attempts) : capacity * 32;

  size_t accepted = 0;
  size_t spent = 0;
  const attempt_dice dice{call_seed(call)};
  for (size_t attempt = 0; attempt < attempts && accepted < capacity; ++attempt) {
    spent = attempt + 1;
    const uint32_t base = uint32_t(attempt) * 4u;
    plan_rect candidate;
    candidate.w = dice.range(base + 0u, min_size, max_size);
    candidate.h = dice.range(base + 1u, min_size, max_size);
    if (candidate.w + 2 * border > width || candidate.h + 2 * border > height) {
      continue;
    }
    candidate.x = dice.range(base + 2u, border, width - border - candidate.w);
    candidate.y = dice.range(base + 3u, border, height - border - candidate.h);

    bool clear = true;
    for (size_t i = 0; i < accepted && clear; ++i) {
      clear = !overlaps(candidate, read_rect(placed, i), gap);
    }
    if (!clear) {
      continue;
    }

    write_rect(rects, accepted, candidate);
    ++accepted;
  }

  counter.set(0, double(accepted));
  // Потраченные попытки — необязательный выход, и он здесь ровно за тем же, зачем у решателя число
  // попыток: по результату не видно, взяли ёмкость легко или еле-еле, а это единственное, что
  // отличает разумную плотность от невозможной.
  if (call.has_output(2)) {
    call.output(2).write().set(0, double(spent));
  }
}

// link_rooms: остов над комнатами и необязательные петли поверх него.
//
// ЗДЕСЬ И ЖИВЁТ ОБЕЩАНИЕ ДОСТИЖИМОСТИ. Остов (minimum spanning tree) по построению соединяет все
// вершины и делает это без циклов — значит из любой комнаты есть путь в любую другую, и ровно
// столько связей, сколько нужно. Всё остальное (петли) добавляется ПОВЕРХ уже полного ответа:
// подземелье без циклов это дерево коридоров, по которому ходят туда и обратно одной дорогой, и
// пара лишних связей меняет его до неузнаваемости, ничего не ломая.
//
// Расстояние МАНХЭТТЕНСКОЕ, а не евклидово, и это не вкус: коридор вырезается коленом из двух
// осевых отрезков, и его длина — это и есть манхэттенское расстояние. Остов по евклидову расстоянию
// минимизировал бы не то, что потом строится.
//
// Прим за O(M^2) вместо Крускала с сортировкой рёбер: комнат десятки, а полный перебор пар не
// требует ни сортировки, ни структуры непересекающихся множеств — и порядок обхода у него один и
// тот же всегда.
void tool_link_rooms(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': link_rooms fills the WHOLE link list, so its range must start at 0, "
                   "got [{}, {})", call.step_name, begin, end);
  }

  const auto rects = call.input(0).read();
  bool clamped = false;
  const size_t rooms = read_count_field(call.input(1), rects.count(), clamped);
  const auto links = call.output(0).write();
  const auto counter = call.output(1).write();
  const size_t capacity = end > begin ? end - begin : 0;

  counter.set(0, 0.0);
  if (rooms < 2) {
    return;
  }

  // ГРОМКИЙ ОТКАЗ, А НЕ ОБРЕЗАНИЕ. Не поместившийся в ёмкость остов означает отрезанные комнаты, и
  // по растру этого не видно: карта выглядит как обычная карта, просто в неё нельзя попасть целиком.
  if (capacity + 1 < rooms) {
    utils::error{}("originator step '{}': link_rooms got room for {} links, and a spanning tree over {} rooms needs "
                   "{} — a truncated tree means rooms nobody can reach", call.step_name, capacity, rooms, rooms - 1);
  }

  std::vector<plan_rect> plan(rooms);
  for (size_t i = 0; i < rooms; ++i) {
    plan[i] = read_rect(rects, i);
  }

  std::vector<int64_t> best(rooms, 0);
  std::vector<uint32_t> parent(rooms, 0);
  std::vector<uint8_t> taken(rooms, 0);
  for (size_t i = 0; i < rooms; ++i) {
    best[i] = manhattan(plan[0], plan[i]);
  }
  taken[0] = 1;

  size_t written = 0;
  for (size_t step = 1; step < rooms; ++step) {
    size_t chosen = rooms;
    for (size_t i = 0; i < rooms; ++i) {
      if (taken[i] != 0) continue;
      // Ничья ломается МЕНЬШИМ номером комнаты, а не порядком обхода: порядок обхода здесь тот же
      // всегда, но правило должно быть названо — иначе первая же перестановка цикла сменила бы карту.
      if (chosen == rooms || best[i] < best[chosen]) {
        chosen = i;
      }
    }

    taken[chosen] = 1;
    links.set(written, double(parent[chosen]), 0);
    links.set(written, double(chosen), 1);
    ++written;

    for (size_t i = 0; i < rooms; ++i) {
      if (taken[i] != 0) continue;
      const auto distance = manhattan(plan[chosen], plan[i]);
      if (distance < best[i]) {
        best[i] = distance;
        parent[i] = uint32_t(chosen);
      }
    }
  }

  // ПЕТЛИ. Берутся самые короткие из пар, которых нет в остове; порядок отбора — (расстояние, первая
  // комната, вторая комната), и он полон, поэтому список не зависит ни от чего, кроме плана.
  //
  // ПЕТЛИ ЗАЖИМАЮТСЯ ПО ЁМКОСТИ, А ОСТОВ ОТКАЗЫВАЕТ — разница не в строгости, а в том, что обещано.
  // Без остова подземелье РАЗОРВАНО, и молчать об этом нельзя; петля же ничего не обещает, она
  // просто улучшает карту, и сколько их поместилось, видно по счётчику связей.
  const auto wanted = size_t(std::max<int64_t>(call.params->integer("extra_links", 0), 0));
  const size_t room_left = capacity > written ? capacity - written : 0;
  const size_t extra = std::min(wanted, room_left);
  if (extra == 0) {
    counter.set(0, double(written));
    return;
  }

  struct candidate {
    int64_t distance = 0;
    uint32_t a = 0;
    uint32_t b = 0;

    bool operator<(const candidate& other) const noexcept {
      if (distance != other.distance) return distance < other.distance;
      if (a != other.a) return a < other.a;
      return b < other.b;
    }
  };

  const auto in_tree = [&](const size_t a, const size_t b) {
    for (size_t k = 0; k < written; ++k) {
      const auto from = size_t(links.get(k, 0));
      const auto to = size_t(links.get(k, 1));
      if ((from == a && to == b) || (from == b && to == a)) return true;
    }
    return false;
  };

  // Список держится РОВНО НА `extra` элементов: полный список пар это квадрат от числа комнат, а
  // нужны из него единицы. Вставка идёт на место, потому что `extra` — единицы же.
  std::vector<candidate> shortlist;
  shortlist.reserve(extra);
  for (size_t a = 0; a < rooms; ++a) {
    for (size_t b = a + 1; b < rooms; ++b) {
      const candidate entry{manhattan(plan[a], plan[b]), uint32_t(a), uint32_t(b)};
      if (shortlist.size() == extra && !(entry < shortlist.back())) continue;
      if (in_tree(a, b)) continue;
      const auto place = std::lower_bound(shortlist.begin(), shortlist.end(), entry);
      shortlist.insert(place, entry);
      if (shortlist.size() > extra) {
        shortlist.pop_back();
      }
    }
  }

  for (const auto& entry : shortlist) {
    links.set(written, double(entry.a), 0);
    links.set(written, double(entry.b), 1);
    ++written;
  }
  counter.set(0, double(written));
}

// link_path: связь (пара номеров комнат) -> отрезок между их центрами.
//
// Апертура `gather`: читает произвольные элементы (комнаты по номеру), пишет свой (концы связи). Это
// единственный инструмент набора, который параллелится сам по себе — и единственный, который ничего
// не решает: он ПЕРЕВОДИТ план в координаты, а не выбирает.
//
// Центр считается целочисленным делением, ровно как у `link_rooms`: это одна и та же точка, и второй
// способ её посчитать означал бы, что остов минимизировал расстояние не между теми точками, которые
// потом соединяют.
void tool_link_path(const tool_call& call, const size_t begin, const size_t end) {
  const auto links = call.input(0).read();
  const auto rects = call.input(1).read();
  const auto target = call.output(0).write();

  for (size_t i = begin; i < end; ++i) {
    const auto from = size_t(links.get(i, 0));
    const auto to = size_t(links.get(i, 1));
    if (from >= rects.count() || to >= rects.count()) {
      utils::error{}("originator step '{}': link_path got link {} between rooms {} and {}, and the plan holds {} "
                     "rooms", call.step_name, i, from, to, rects.count());
    }

    const auto a = read_rect(rects, from);
    const auto b = read_rect(rects, to);
    target.set(i, double(a.centre_x()), 0);
    target.set(i, double(a.centre_y()), 1);
    target.set(i, double(b.centre_x()), 2);
    target.set(i, double(b.centre_y()), 3);
  }
}

// Общая часть вырезания: форма растра и запись клетки с проверкой границ.
struct carve_target {
  field_accessor cells;
  int64_t width = 0;
  int64_t height = 0;

  bool inside(const int64_t x, const int64_t y) const noexcept {
    return x >= 0 && y >= 0 && x < width && y < height;
  }

  void set(const int64_t x, const int64_t y, const double value) const noexcept {
    if (inside(x, y)) {
      cells.set(size_t(y * width + x), value);
    }
  }
};

carve_target make_target(const tool_call& call) {
  const auto extent = resolve_extent(call, call.output(0), "width", "height");
  return carve_target{call.output(0).write(), int64_t(extent.x), int64_t(extent.y)};
}

// ФОРМА МЕСТА. Раздел даёт прямоугольник, но постройка прямоугольниками не исчерпывается: угол
// срезают или скругляют, зал делают округлым, а на срезе угла как раз и встаёт колонна, в которую
// упираются стены. Форма — ДАННЫЕ, лежащие рядом с габаритом, и меняет она ровно одно: какие клетки
// прямоугольника принадлежат месту.
//
// ВСЁ ОСТАЛЬНОЕ ОТ ЭТОГО НЕ ЗАВИСИТ, и это главный довод в пользу зональной модели: стыки выводятся
// из РАСТРА, поэтому у округлого зала они находятся тем же обходом, что у прямоугольной каморки, а
// двери, связность и проверки не замечают разницы вовсе.
//
// Все расчёты ЦЕЛОЧИСЛЕННЫЕ: клетка берётся по своему центру, а центр кладётся в удвоенные
// координаты, чтобы не делить пополам. Форма, посчитанная сравнением плавающих, на другой машине
// могла бы отличаться на клетку — а клетка здесь это дверной проём.
struct zone_shape {
  enum values {
    rectangle,  // прямоугольник целиком
    chamfer,    // срезанные углы: на срезе встаёт колонна
    rounded,    // скруглённые углы объявленного радиуса
    ellipse,    // вписанный эллипс: округлый зал
    count
  };
};

// Принадлежит ли клетка месту такой формы. Координаты — относительные, от левого верхнего угла.
bool inside_shape(const int64_t dx, const int64_t dy, const int64_t w, const int64_t h,
                  const int64_t shape, const int64_t parameter) noexcept {
  switch (shape) {
    case zone_shape::chamfer: {
      const int64_t cut = parameter;
      const int64_t left = dx;
      const int64_t right = w - 1 - dx;
      const int64_t top = dy;
      const int64_t bottom = h - 1 - dy;
      return left + top >= cut && right + top >= cut && left + bottom >= cut && right + bottom >= cut;
    }
    case zone_shape::rounded: {
      const int64_t radius = parameter;
      const int64_t left = dx;
      const int64_t right = w - 1 - dx;
      const int64_t top = dy;
      const int64_t bottom = h - 1 - dy;
      const auto corner = [&](const int64_t a, const int64_t b) {
        if (a >= radius || b >= radius) {
          return true;
        }
        const int64_t ax = radius - a;
        const int64_t by = radius - b;
        return ax * ax + by * by <= radius * radius;
      };
      return corner(left, top) && corner(right, top) && corner(left, bottom) && corner(right, bottom);
    }
    case zone_shape::ellipse: {
      // Центр клетки в удвоенных координатах: 2*d + 1 - сторона. Тогда деления пополам нет нигде.
      const int64_t x = 2 * dx + 1 - w;
      const int64_t y = 2 * dy + 1 - h;
      return x * x * h * h + y * y * w * w <= w * w * h * h;
    }
    default: return true;
  }
}

// ГЕОМЕТРИЯ ЗОНЫ: габарит и форма вместе. Раньше такой пары не существовало — принадлежность точки
// спрашивали у РАСТРА, — и это было верно ровно до тех пор, пока растр был планом. Теперь растр это
// вид, и спрашивать у него значило бы спрашивать у картинки.
//
// ЭКЗЕМПЛЯР ГЕОМЕТРИИ ПО-ПРЕЖНЕМУ ОДИН: `inside_shape` не переписан и не продублирован, у него
// просто стало три вызывающих вместо одного. Разойтись двум отпечаткам формы негде, потому что
// отпечаток теперь не хранится вовсе — форма ВЫЧИСЛЯЕТСЯ там, где нужна.
struct zone_geometry {
  plan_rect rect;
  int64_t shape = 0;
  int64_t parameter = 0;

  bool holds(const int64_t x, const int64_t y) const noexcept {
    if (x < rect.x || y < rect.y || x >= rect.x + rect.w || y >= rect.y + rect.h) {
      return false;
    }
    return inside_shape(x - rect.x, y - rect.y, rect.w, rect.h, shape, parameter);
  }
};

// Читает план в таблицу геометрии и ВСЛУХ отказывает на неизвестной форме: молча принятая форма
// означала бы прямоугольник там, где автор просил эллипс, и увидеть это можно было бы только глазом.
std::vector<zone_geometry> read_geometry(const tool_call& call, const const_field_accessor& rects,
                                         const const_field_accessor& shapes, const bool has_shape,
                                         const size_t count, const char* tool) {
  std::vector<zone_geometry> plan(count);
  for (size_t i = 0; i < count; ++i) {
    plan[i].rect = read_rect(rects, i);
    plan[i].shape = has_shape ? int64_t(shapes.get(i, 0)) : int64_t(zone_shape::rectangle);
    plan[i].parameter = has_shape ? int64_t(shapes.get(i, 1)) : 0;
    if (plan[i].shape < 0 || plan[i].shape >= zone_shape::count) {
      utils::error{}("originator step '{}': {} got shape {} for zone {}, and the library knows {}",
                     call.step_name, tool, plan[i].shape, i, int64_t(zone_shape::count));
    }
  }
  return plan;
}

// Деление с округлением ВНИЗ. Обычное целое деление в C++ округляет к нулю, и окно, начатое левее
// начала координат, получило бы на отрицательной стороне клетку двойной ширины — то есть картинка
// врала бы ровно там, где её труднее всего проверить.
int64_t floor_div(const int64_t value, const int64_t divisor) noexcept {
  const int64_t quotient = value / divisor;
  const int64_t remainder = value % divisor;
  return remainder != 0 && ((remainder < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

// ГРУБАЯ СЕТКА ПО УЧАСТКУ: «кто ещё лежит вот в этом куске». Нужна ровно одному вопросу — стоит ли в
// стене между двумя местами третье, — и нужна именно потому, что ответ почти везде «никого»: перебор
// списка платил бы числом мест за каждый стык, то есть квадрат за пустоту.
//
// СЕТКА ГРУБАЯ НАРОЧНО. Точность ей ни к чему: она только сужает круг, а решает всё равно проверка
// прямоугольников. Сторона берётся корнем из числа мест — тогда в клетке сетки их единицы, сколько бы
// мест ни было, — и зажимается сверху, чтобы на пустом участке не заводить таблицу больше самого
// списка.
class zone_index {
public:
  explicit zone_index(const std::vector<zone_geometry>& plan) : plan_(&plan) {
    if (plan.empty()) {
      return;
    }
    int64_t max_x = plan[0].rect.x + plan[0].rect.w;
    int64_t max_y = plan[0].rect.y + plan[0].rect.h;
    min_x_ = plan[0].rect.x;
    min_y_ = plan[0].rect.y;
    for (const auto& zone : plan) {
      min_x_ = std::min(min_x_, zone.rect.x);
      min_y_ = std::min(min_y_, zone.rect.y);
      max_x = std::max(max_x, zone.rect.x + zone.rect.w);
      max_y = std::max(max_y, zone.rect.y + zone.rect.h);
    }

    side_ = std::clamp<int64_t>(int64_t(std::sqrt(double(plan.size()))) + 1, 1, 64);
    step_x_ = std::max<int64_t>((max_x - min_x_ + side_ - 1) / side_, 1);
    step_y_ = std::max<int64_t>((max_y - min_y_ + side_ - 1) / side_, 1);
    buckets_.resize(size_t(side_ * side_));
    stamp_.assign(plan.size(), 0);

    for (size_t i = 0; i < plan.size(); ++i) {
      const auto& rect = plan[i].rect;
      for (int64_t by = row(rect.y); by <= row(rect.y + rect.h - 1); ++by) {
        for (int64_t bx = column(rect.x); bx <= column(rect.x + rect.w - 1); ++bx) {
          buckets_[size_t(by * side_ + bx)].push_back(uint32_t(i));
        }
      }
    }
  }

  // Кто пересекает этот кусок, кроме двух названных. Список ВЫДАЁТСЯ без повторов: крупная зона лежит
  // сразу в нескольких клетках сетки, и посчитать её дважды значило бы проверять её дважды.
  void gather(const plan_rect& area, const size_t skip_a, const size_t skip_b, std::vector<uint32_t>& out) const {
    out.clear();
    if (buckets_.empty() || area.w <= 0 || area.h <= 0) {
      return;
    }
    ++visit_;
    for (int64_t by = row(area.y); by <= row(area.y + area.h - 1); ++by) {
      for (int64_t bx = column(area.x); bx <= column(area.x + area.w - 1); ++bx) {
        for (const auto candidate : buckets_[size_t(by * side_ + bx)]) {
          if (stamp_[candidate] == visit_ || candidate == skip_a || candidate == skip_b) {
            continue;
          }
          stamp_[candidate] = visit_;
          const auto& rect = (*plan_)[candidate].rect;
          if (rect.x < area.x + area.w && area.x < rect.x + rect.w &&
              rect.y < area.y + area.h && area.y < rect.y + rect.h) {
            out.push_back(candidate);
          }
        }
      }
    }
  }

private:
  int64_t column(const int64_t x) const noexcept {
    return std::clamp<int64_t>(floor_div(x - min_x_, step_x_), 0, side_ - 1);
  }
  int64_t row(const int64_t y) const noexcept {
    return std::clamp<int64_t>(floor_div(y - min_y_, step_y_), 0, side_ - 1);
  }

  const std::vector<zone_geometry>* plan_ = nullptr;
  std::vector<std::vector<uint32_t>> buckets_;
  mutable std::vector<uint32_t> stamp_;
  mutable uint32_t visit_ = 0;
  int64_t min_x_ = 0;
  int64_t min_y_ = 0;
  int64_t step_x_ = 1;
  int64_t step_y_ = 1;
  int64_t side_ = 1;
};

// paint_rects: список прямоугольников (и их ФОРМ) в клетки растра.
//
// Пишется НОМЕР ЭЛЕМЕНТА ПЛЮС ОДИН, а не «занято»: ноль остаётся признаком «ничьё», как у всех
// инструментов с ключом, а по номеру видно, какой клетке какое место принадлежит — без этого нельзя
// ни проверить достижимость по местам, ни поставить дверь. Инструмент назван по деянию, а не по
// комнате: тем же вызовом размечаются зоны здания, палубы корабля и клумбы во дворе.
//
// ПЕРЕЗАПИСЬ ЧУЖОЙ КЛЕТКИ — ОТКАЗ. Прежде перекрытие двух мест было на растре невидимо (второе
// просто затирало первое), и ловила его проверка площадки сравнением площадей — которая перестаёт
// работать, как только у мест появилась форма. Отказывать обязан тот, кто теряет данные, а не тот,
// кто потом считает; здесь это стоит одного чтения на записанную клетку.
void tool_paint_rects(const tool_call& call, const size_t begin, const size_t end) {
  const auto rects = call.input(0).read();
  const bool has_shape = call.has_input(1);
  const auto shapes = has_shape ? call.input(1).read() : const_field_accessor{};
  const auto target = make_target(call);
  const auto painted = call.output(0).read();

  for (size_t i = begin; i < end; ++i) {
    const auto room = read_rect(rects, i);
    const int64_t shape = has_shape ? int64_t(shapes.get(i, 0)) : int64_t(zone_shape::rectangle);
    const int64_t parameter = has_shape ? int64_t(shapes.get(i, 1)) : 0;
    if (shape < 0 || shape >= zone_shape::count) {
      utils::error{}("originator step '{}': paint_rects got shape {} for element {}, and the library knows {}",
                     call.step_name, shape, i, int64_t(zone_shape::count));
    }
    // Срез или скругление БОЛЬШЕ ПОЛОВИНЫ стороны съедает место целиком либо разрывает его надвое, и
    // по растру это выглядит как «комнату почему-то не вырезали».
    if (shape != zone_shape::rectangle && shape != zone_shape::ellipse &&
        2 * parameter > std::min(room.w, room.h)) {
      utils::error{}("originator step '{}': paint_rects got a corner of {} cells for element {} sized {}x{} — a "
                     "corner past half the side eats the place instead of shaping it",
                     call.step_name, parameter, i, room.w, room.h);
    }
    // ОТКАЗ, А НЕ ОБРЕЗАНИЕ: комната за краем растра означает, что план считали в одних границах, а
    // вырезают в других, и тихо обрезанная комната выглядит просто маленькой.
    if (room.w <= 0 || room.h <= 0 || !target.inside(room.x, room.y) ||
        !target.inside(room.x + room.w - 1, room.y + room.h - 1)) {
      utils::error{}("originator step '{}': paint_rects got room {} at ({}, {}) sized {}x{}, which does not fit the "
                     "{}x{} raster — the plan and the carving disagree about the bounds",
                     call.step_name, i, room.x, room.y, room.w, room.h, target.width, target.height);
    }

    for (int64_t y = room.y; y < room.y + room.h; ++y) {
      for (int64_t x = room.x; x < room.x + room.w; ++x) {
        if (!inside_shape(x - room.x, y - room.y, room.w, room.h, shape, parameter)) {
          continue;
        }
        const auto standing = painted.get(size_t(y * target.width + x));
        if (standing != 0.0 && standing != double(i + 1)) {
          utils::error{}("originator step '{}': paint_rects would write element {} over element {} at ({}, {}) — two "
                         "places cannot share a cell, and a silent overwrite looks like a smaller place",
                         call.step_name, i, int64_t(standing) - 1, x, y);
        }
        target.set(x, y, double(i + 1));
      }
    }
  }
}


// place_columns: КОЛОННАДА внутри места.
//
// Колонна — это кусок стены, стоящий посреди зала, и потому она НЕ ЗОНА: с точки зрения прохода это
// препятствие, с точки зрения модели — та же стена, только не у края. Ровно так же срез угла
// оставляет стену на месте схождения стен.
//
// ВЫХОД — СПИСОК ПРЯМОУГОЛЬНИКОВ, А НЕ КЛЕТКИ РАСТРА. Прежде колонна писалась прямо в растр, и это
// была последняя ниточка, которой план держался за картинку: колоннада, посчитанная на картинке,
// поехала бы при смене масштаба, а посчитанная в мере — не поедет никогда. Кто хочет её увидеть,
// отдаёт этот список проектору вторым входом.
//
// СЕТКА ОПОР ВЫРАВНЕНА ПО ЦЕНТРУ МЕСТА, и это не украшение: несимметричная колоннада читается как
// ошибка разметки, а не как замысел. Позиции считаются шагами от центра в обе стороны, поэтому
// симметрия следует из построения, а не из удачного деления.
//
// ОТСТУП МЕРЯЕТСЯ ФОРМОЙ МЕСТА: опора ставится, только если квадрат «колонна плюс отступ со всех
// сторон» целиком принадлежит этому месту. Отсюда сразу три обещания — колонна не прилипает к стене
// (иначе это не колонна, а выступ), не срастается с соседней и не вылезает за скруглённый угол.
//
// ПОСЛЕДОВАТЕЛЬНЫЙ, А НЕ РАЗБРАСЫВАЮЩИЙ, и причина в выходе: список колонн — это ПРИРАЩЕНИЕ, у
// приращения есть порядок, а у разбрасывания его нет. Пока колонны писались в растр по своим
// координатам, порядок был безразличен; у счётчика он безразличным быть перестал.
void tool_place_columns(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': place_columns fills the WHOLE column list, so its range must start at 0, "
                   "got [{}, {})", call.step_name, begin, end);
  }

  const auto rects = call.input(0).read();
  bool clamped = false;
  const size_t zones = read_count_field(call.input(1), rects.count(), clamped);
  const bool has_shape = call.has_input(2);
  const auto shapes = has_shape ? call.input(2).read() : const_field_accessor{};
  const bool has_mask = call.has_input(3);
  const auto mask = has_mask ? call.input(3).read() : const_field_accessor{};

  const auto column = call.output(0).write();
  const auto counter = call.output(1).write();
  const size_t capacity = end > begin ? end - begin : 0;

  const int64_t spacing = std::max<int64_t>(call.params->integer("spacing", 4), 1);
  const int64_t size = std::max<int64_t>(call.params->integer("size", 1), 1);
  const int64_t margin = std::max<int64_t>(call.params->integer("margin", 1), 0);

  const auto plan = read_geometry(call, rects, shapes, has_shape, zones, "place_columns");

  size_t placed = 0;
  for (size_t i = 0; i < zones; ++i) {
    if (has_mask && mask.get(i) == 0.0) {
      continue;
    }

    const auto& room = plan[i].rect;
    const int64_t centre_x = room.x + room.w / 2;
    const int64_t centre_y = room.y + room.h / 2;
    const int64_t steps_x = room.w / (2 * spacing) + 1;
    const int64_t steps_y = room.h / (2 * spacing) + 1;

    for (int64_t ky = -steps_y; ky <= steps_y; ++ky) {
      for (int64_t kx = -steps_x; kx <= steps_x; ++kx) {
        const int64_t x0 = centre_x + kx * spacing;
        const int64_t y0 = centre_y + ky * spacing;

        bool clear = true;
        for (int64_t y = y0 - margin; y < y0 + size + margin && clear; ++y) {
          for (int64_t x = x0 - margin; x < x0 + size + margin && clear; ++x) {
            clear = plan[i].holds(x, y);
          }
        }
        if (!clear) {
          continue;
        }

        if (placed >= capacity) {
          utils::error{}("originator step '{}': place_columns ran out of the declared column capacity ({}) — a hall "
                         "with half its colonnade is not the hall that was planned", call.step_name, capacity);
        }
        write_rect(column, placed, plan_rect{x0, y0, size, size});
        ++placed;
      }
    }
  }

  counter.set(0, double(placed));
}

// project_zones: ПЛАН -> КАРТИНКА. Последний шаг, и единственный, который знает про клетки.
//
// Здесь проходит водораздел всей постройки: до этого места нет ни растра, ни пикселя, ни масштаба —
// есть план в МЕРЕ, нарезанный по правилам, и окно, которое просят показать. Проекция ничего не
// решает и ни на что не влияет: её выход не читает ни один инструмент, и выбросить её значит
// потерять ровно картинку.
//
// ПРОЕКЦИЯ — ЭТО ВЫБОРКА В ЦЕНТРЕ КЛЕТКИ, а не покрытие площадью, и отсюда следует самое полезное её
// свойство: у клетки один центр, значит и зона у неё одна, и спорить за клетку некому. Порядок
// записи от этого безразличен, а перезапись остаётся ровно одним — ПЕРЕКРЫТИЕМ ПЛАНА, и она отказ.
//
// При `view == raster` выборка ТОЧНА: центр клетки попадает в ту же меру, что и её номер, и картинка
// становится планом один в один. Ровно поэтому проверки площадки смотрят именно такую проекцию.
//
// КАРТИНКА ОБЯЗАНА СКАЗАТЬ, ЧЕГО НА НЕЙ НЕ ВИДНО, и это два РАЗНЫХ числа, потому что грубеет она
// двумя разными способами:
//
//   ПОТЕРЯНО  место, попавшее в окно, но не поймавшее ни одного центра клетки: дверь в одну меру при
//             сжатии вчетверо проваливается между выборками;
//   СЛИПЛОСЬ  границ клеток, где рядом оказались места, между которыми в мере стена. Это опаснее
//             потери: заливка по такой картинке нашла бы проход, которого в плане нет.
//
// Пока оба нуля, картинка — это план, и по ней законно проверять связность. Как только они не нули,
// картинка остаётся картинкой, и об этом сказано числом, а не умолчанием.
void tool_project_zones(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': project_zones fills the WHOLE raster, so its range must start at 0, "
                   "got [{}, {})", call.step_name, begin, end);
  }

  const auto rects = call.input(0).read();
  bool clamped = false;
  const size_t zones = read_count_field(call.input(1), rects.count(), clamped);
  const bool has_shape = call.has_input(2);
  const auto shapes = has_shape ? call.input(2).read() : const_field_accessor{};
  // СПИСОК БЕЗ СЧЁТЧИКА — ОТКАЗ, а не тихо пропущенный вход: у списка с ёмкостью занятая длина
  // приезжает счётчиком, и без него нарисовать колоннаду значило бы нарисовать мусор прошлого
  // прогона. Молча же пропущенная колоннада выглядит как зал, которому её не объявляли.
  if (call.has_input(3) != call.has_input(4)) {
    utils::error{}("originator step '{}': project_zones got a list of holes without its counter (or the other way "
                   "round) — capacity is a wish, and the occupied length is the fact", call.step_name);
  }
  const bool has_holes = call.has_input(3) && call.has_input(4);
  const auto hole_rects = has_holes ? call.input(3).read() : const_field_accessor{};
  const size_t holes = has_holes ? read_count_field(call.input(4), hole_rects.count(), clamped) : size_t(0);

  const auto extent = resolve_extent(call, call.output(0), "width", "height");
  const auto target = carve_target{call.output(0).write(), int64_t(extent.x), int64_t(extent.y)};
  const auto lost_counter = call.output(1).write();
  const auto merged_counter = call.output(2).write();

  // ОКНО В МЕРЕ. По умолчанию оно равно растру один к одному — то есть прежнее поведение, когда
  // клетка и мера были одним и тем же.
  const int64_t view_x = call.params->integer("view_x", 0);
  const int64_t view_y = call.params->integer("view_y", 0);
  const int64_t view_w = std::max<int64_t>(call.params->integer("view_w", target.width), 1);
  const int64_t view_h = std::max<int64_t>(call.params->integer("view_h", target.height), 1);

  const auto plan = read_geometry(call, rects, shapes, has_shape, zones, "project_zones");

  // Мера, попадающая в центр клетки. Умножение стоит ПЕРЕД делением, поэтому масштаб не накапливает
  // ошибку, а деление пополам не появляется вовсе: центр кладётся в удвоенные координаты.
  const auto sample_x = [&](const int64_t cell) { return view_x + floor_div((2 * cell + 1) * view_w, 2 * target.width); };
  const auto sample_y = [&](const int64_t cell) { return view_y + floor_div((2 * cell + 1) * view_h, 2 * target.height); };

  for (int64_t y = 0; y < target.height; ++y) {
    for (int64_t x = 0; x < target.width; ++x) {
      target.set(x, y, 0.0);
    }
  }

  const auto cell_span = [](const int64_t low, const int64_t size, const int64_t origin, const int64_t span,
                            const int64_t cells, int64_t& first, int64_t& last) {
    // Оценка берётся С ЗАПАСОМ в клетку в обе стороны, а решает всё равно выборка центра: точное
    // обращение отображения потребовало бы тех же делений ещё раз, а ошибка в нём была бы не видна.
    first = std::max<int64_t>(floor_div((low - origin) * cells, span) - 1, 0);
    last = std::min<int64_t>(floor_div((low + size - origin) * cells, span) + 1, cells - 1);
  };

  size_t lost = 0;
  for (size_t i = 0; i < zones; ++i) {
    const auto& room = plan[i].rect;
    int64_t x0 = 0;
    int64_t x1 = 0;
    int64_t y0 = 0;
    int64_t y1 = 0;
    cell_span(room.x, room.w, view_x, view_w, target.width, x0, x1);
    cell_span(room.y, room.h, view_y, view_h, target.height, y0, y1);

    size_t drawn = 0;
    for (int64_t y = y0; y <= y1; ++y) {
      const int64_t uy = sample_y(y);
      for (int64_t x = x0; x <= x1; ++x) {
        if (!plan[i].holds(sample_x(x), uy)) {
          continue;
        }
        const auto standing = target.cells.get(size_t(y * target.width + x));
        if (standing != 0.0 && standing != double(i + 1)) {
          utils::error{}("originator step '{}': project_zones found zone {} over zone {} at cell ({}, {}) — one "
                         "point of the plan belongs to one zone, so this is an overlap in the PLAN, not in the view",
                         call.step_name, i, int64_t(standing) - 1, x, y);
        }
        target.set(x, y, double(i + 1));
        ++drawn;
      }
    }

    // Место ВНЕ ОКНА не потеряно, а просто не показано: потерей считается только то, что в окно
    // попало и всё равно не нарисовалось. Иначе число говорило бы про окно, а не про масштаб.
    const bool in_view = room.x < view_x + view_w && view_x < room.x + room.w &&
                         room.y < view_y + view_h && view_y < room.y + room.h;
    lost += size_t(in_view && drawn == 0);
  }

  for (size_t i = 0; i < holes; ++i) {
    const auto hole = read_rect(hole_rects, i);
    int64_t x0 = 0;
    int64_t x1 = 0;
    int64_t y0 = 0;
    int64_t y1 = 0;
    cell_span(hole.x, hole.w, view_x, view_w, target.width, x0, x1);
    cell_span(hole.y, hole.h, view_y, view_h, target.height, y0, y1);
    for (int64_t y = y0; y <= y1; ++y) {
      const int64_t uy = sample_y(y);
      for (int64_t x = x0; x <= x1; ++x) {
        const int64_t ux = sample_x(x);
        if (ux < hole.x || uy < hole.y || ux >= hole.x + hole.w || uy >= hole.y + hole.h) {
          continue;
        }
        target.set(x, y, 0.0);
      }
    }
  }

  // СЛИПШИЕСЯ ГРАНИЦЫ. Считаются по картинке, а сверяются с ПЛАНОМ: соседние клетки разных мест
  // законны только тогда, когда места и в мере соприкасаются. Всё прочее — стена, которую съел
  // масштаб.
  size_t merged = 0;
  for (int64_t y = 0; y < target.height; ++y) {
    for (int64_t x = 0; x < target.width; ++x) {
      const auto here = size_t(target.cells.get(size_t(y * target.width + x)));
      if (here == 0) {
        continue;
      }
      const int64_t steps[2][2] = {{1, 0}, {0, 1}};
      for (const auto& step : steps) {
        const int64_t nx = x + step[0];
        const int64_t ny = y + step[1];
        if (!target.inside(nx, ny)) {
          continue;
        }
        const auto there = size_t(target.cells.get(size_t(ny * target.width + nx)));
        if (there == 0 || there == here) {
          continue;
        }
        const auto& first = plan[here - 1].rect;
        const auto& second = plan[there - 1].rect;
        const int64_t gap_x = std::max(first.x - (second.x + second.w), second.x - (first.x + first.w));
        const int64_t gap_y = std::max(first.y - (second.y + second.h), second.y - (first.y + first.h));
        merged += size_t(std::max(gap_x, gap_y) > 0);
      }
    }
  }

  lost_counter.set(0, double(lost));
  merged_counter.set(0, double(merged));
}

// carve_corridors: ОТРЕЗОК в клетки растра КОЛЕНОМ из двух осевых пробегов.
//
// Колено, а не прямая линия: подземелье — постройка, у неё стены по осям. Какое из двух колен
// (сначала по x или сначала по y) — решает хеш от номера отрезка, потому что оба одинаково законны,
// а выбор одного из них навсегда даёт узнаваемо расчерченную карту.
//
// Про комнаты этот инструмент НЕ ЗНАЕТ НИЧЕГО, и это не упущение: концы приходят координатами
// (`link_path`), поэтому тем же вызовом режется дорога между городами или канал между озёрами. Заодно
// исчезает связь между ёмкостями двух списков: у `scatter` диапазон относится ко ВХОДАМ, и список
// комнат на входе ограничивал бы число связей числом комнат.
//
// ТОЛЩИНА ЗАЖИМАЕТСЯ ПО КРАЮ РАСТРА, и это не то же самое, что обрезанная комната: у комнаты
// обрезание меняет ПЛАН (комната стала другой), а у коридора толщина — свойство геометрии, и
// прижатый к краю коридор остаётся тем же коридором между теми же точками. Связность от этого не
// страдает: центральная линия колена лежит между концами, а они внутри растра.
void tool_carve_corridors(const tool_call& call, const size_t begin, const size_t end) {
  const auto path = call.input(0).read();
  const auto target = make_target(call);

  const auto width = std::max<int64_t>(call.params->integer("corridor_width", 1), 1);
  const double value = call.params->number("value", 1.0);
  const int64_t back = (width - 1) / 2;
  const int64_t forward = width / 2;
  const uint32_t seed = call_seed(call);

  const auto run_x = [&](const int64_t from, const int64_t to, const int64_t y) {
    const int64_t low = std::min(from, to);
    const int64_t high = std::max(from, to);
    for (int64_t x = low; x <= high; ++x) {
      for (int64_t offset = -back; offset <= forward; ++offset) {
        target.set(x, y + offset, value);
      }
    }
  };
  const auto run_y = [&](const int64_t from, const int64_t to, const int64_t x) {
    const int64_t low = std::min(from, to);
    const int64_t high = std::max(from, to);
    for (int64_t y = low; y <= high; ++y) {
      for (int64_t offset = -back; offset <= forward; ++offset) {
        target.set(x + offset, y, value);
      }
    }
  };

  for (size_t i = begin; i < end; ++i) {
    const int64_t x0 = int64_t(path.get(i, 0));
    const int64_t y0 = int64_t(path.get(i, 1));
    const int64_t x1 = int64_t(path.get(i, 2));
    const int64_t y1 = int64_t(path.get(i, 3));
    // ОТКАЗ, А НЕ ЗАЖИМ: конец за краем растра означает, что план и вырезание считали в разных
    // границах, и тихо подвинутый конец соединил бы не то, что сказано в плане.
    if (!target.inside(x0, y0) || !target.inside(x1, y1)) {
      utils::error{}("originator step '{}': carve_corridors got a corridor from ({}, {}) to ({}, {}), and the raster "
                     "is {}x{} — the plan and the carving disagree about the bounds",
                     call.step_name, x0, y0, x1, y1, target.width, target.height);
    }

    if (utils::shared::prng2(seed, uint32_t(i) + 1u) % 2u == 0u) {
      run_x(x0, x1, y0);
      run_y(y0, y1, x1);
    } else {
      run_y(y0, y1, x0);
      run_x(x0, x1, y1);
    }
  }
}
// ============================== ЗОНАЛЬНАЯ МОДЕЛЬ ==============================
//
// Деления на «комнаты» и «коридоры» здесь нет вовсе, и это не упрощение, а другая постановка,
// пришедшая из PF09. Есть ЗОНА — место с видом; вид несёт проходимость, предел числа связей и то, с
// какими другими видами связь допустима. Коридор перестаёт быть родом геометрии и становится видом,
// у которого предела связей нет; кухня — видом, которому дверь разрешена в обеденный зал и в
// служебный ход, но не в покои. Что было структурой, стало ДАННЫМИ.
//
// ТРИ УТВЕРЖДЕНИЯ PF09, которые здесь повторены буквально:
//
//   1. СВЯЗНОСТЬ — ДАННЫЕ, а не функция геометрии. Геометрия работает РОВНО ОДИН РАЗ, в сборщике:
//      `zone_joins` выводит стыки из плана, и дальше все решения принимаются по списку стыков.
//      Игра (и проверка) читает готовый граф и никогда его не перевыводит;
//   2. ДВЕРЬ — ЭТО ЗОНА, а не свойство ребра. У двери две стороны, и запертость у МЕСТА одна, а у
//      ребра её пришлось бы держать согласованной с обеих сторон;
//   3. ШИРОКИЙ ПРОЁМ ОСТАЁТСЯ РЕБРОМ. Две зоны, которые касаются без стены (коридорная лента и
//      хребет), связаны и без двери — и `zone_joins` отличает такой стык от стыка через стену.
//
// СМЕЖНОСТЬ И СВЯЗЬ — РАЗНЫЕ ВЕЩИ, и это главное следствие. У кухни с покоями вполне может быть
// общая стена; двери между ними быть не должно. Поэтому и таблиц ДВЕ: одна говорит, какие виды
// вправе ГРАНИЧИТЬ (её читает решатель, когда раскладывает виды), другая — между какими видами
// допустима ДВЕРЬ (её читает `open_doors`). Одной таблицей это не выражается.
//
// СТЕНА — ДОПОЛНЕНИЕ, А НЕ СПИСОК. Зоны покрывают пятно не целиком: мера, не попавшая ни в одну
// зону, и есть стена. Держать стены отдельными прямоугольниками пришлось бы вдобавок к тому же
// растру, а стеновая масса здания СВЯЗНА — то есть это одно место, а не сотня. Проверка площадки
// требует именно связности: как только раздел начнёт её ломать, станет видно сразу.


// Куда `slice_zones` складывает готовые места. Ёмкость ОБЪЯВЛЕНА, и переполнение — громкий отказ:
// урезанная раскладка это здание, в котором молча нет комнат.
struct zone_sink {
  field_accessor rect;
  field_accessor kind;
  const tool_call* call = nullptr;
  size_t capacity = 0;
  size_t count = 0;
  // НАЧАЛО ПЯТНА на участке. Раздел считает в своих координатах, а кладёт в мировые: здание стоит на
  // участке, а не в углу мира, и снаружи у него есть место — улица, двор, соседний дом.
  int64_t origin_x = 0;
  int64_t origin_y = 0;

  void push(const plan_rect& local, const double role) {
    const plan_rect value{local.x + origin_x, local.y + origin_y, local.w, local.h};
    if (count >= capacity) {
      utils::error{}("originator step '{}': slice_zones ran out of the declared zone capacity ({}) — a truncated "
                     "layout is a building whose missing rooms nobody declared",
                     call->step_name, capacity);
    }
    write_rect(rect, count, value);
    kind.set(count, role);
    ++count;
  }
};

// Режет отрезок на места объявленной ширины. Число мест выбирается так, чтобы ширины попали в
// объявленные границы, а остаток раздаётся первым: раздел не оставляет обрезков, иначе «лишние» три
// клетки превратились бы в стену неизвестного происхождения.
std::vector<int64_t> cut_span(const tool_call& call, const int64_t length, const int64_t wall,
                              const int64_t min_size, const int64_t max_size, const int64_t jitter,
                              const uint32_t salt) {
  if (length < min_size) {
    utils::error{}("originator step '{}': slice_zones got a {}-unit strip to cut into places of at least {} — the "
                   "declared plan does not fit the footprint", call.step_name, length, min_size);
  }

  const int64_t average = (min_size + max_size) / 2;
  int64_t places = std::max<int64_t>((length + wall) / (average + wall), 1);
  while (places > 1 && (length - (places - 1) * wall) / places < min_size) {
    --places;
  }
  while ((length - (places - 1) * wall) / places > max_size) {
    ++places;
  }

  const int64_t usable = length - (places - 1) * wall;
  if (usable < places * min_size) {
    utils::error{}("originator step '{}': slice_zones cannot cut {} units into {} places of at least {} with {} of "
                   "wall between them", call.step_name, length, places, min_size, wall);
  }

  std::vector<int64_t> widths(size_t(places), usable / places);
  for (int64_t i = 0; i < usable % places; ++i) {
    widths[size_t(i)] += 1;
  }

  // ДЖИТТЕР: внутренние стены сдвигаются хешем в пределах объявленных границ. Без него ряд мест
  // выходит расчерченным по линейке, а с ним — рядом комнат разной ширины, как в настоящем плане.
  for (size_t i = 0; jitter > 0 && i + 1 < widths.size(); ++i) {
    const auto roll = utils::shared::prng2(salt, uint32_t(i) + 1u) % uint32_t(2 * jitter + 1);
    const int64_t shift = int64_t(roll) - jitter;
    const int64_t left = widths[i] + shift;
    const int64_t right = widths[i + 1] - shift;
    if (left >= min_size && left <= max_size && right >= min_size && right <= max_size) {
      widths[i] = left;
      widths[i + 1] = right;
    }
  }
  return widths;
}

// slice_zones: пятно застройки -> места.
//
// Раздел ГИЛЬОТИННЫЙ и в два приёма: сначала по короткой оси кладутся ленты циркуляции и полосы
// между ними, потом каждая полоса режется поперёк на места. Отсюда и берётся вид настоящего этажа —
// длинный коридор, вдоль которого рядами стоят комнаты одной глубины.
//
// ЦИРКУЛЯЦИЯ КЛАДЁТСЯ ПЕРВОЙ, и это переворот по сравнению с пещерной схемой, где коридор был
// СЛЕДСТВИЕМ связей. В здании наоборот: сначала известно, как по нему ходят, и уже под это нарезаны
// места. Число лент выводится из глубины пятна и объявленных границ глубины полосы — не задаётся
// числом, потому что «сколько лент» это ответ, а не вопрос.
//
// ХРЕБЕТ (поперечная лента) появляется РОВНО ТОГДА, когда лент больше одной: одна лента связна сама
// по себе, а две несвязны, и без хребта связность пришлось бы добывать дверьми через комнаты —
// планировка, в которой из коридора в коридор ходят через чужую спальню.
//
// Зоны ложатся ВПЛОТНУЮ к хребту (без стены между коридором и коридором) — это и есть широкий
// проём, который останется ребром и двери не потребует.
void tool_slice_zones(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': slice_zones fills the WHOLE zone list, so its range must start at 0, "
                   "got [{}, {})", call.step_name, begin, end);
  }

  const auto counter = call.output(2).write();

  const int64_t width = call.params->integer("width", 0);
  const int64_t height = call.params->integer("height", 0);
  // Пятно застройки внутри растра. Ноль по обеим осям — прежнее поведение, когда здание занимало
  // растр целиком; отличное от нуля значит, что снаружи есть чему быть.
  const int64_t origin_x = call.params->integer("origin_x", 0);
  const int64_t origin_y = call.params->integer("origin_y", 0);

  zone_sink sink{call.output(0).write(), call.output(1).write(), &call, end > begin ? end - begin : 0, 0,
                 origin_x, origin_y};
  const int64_t wall = std::max<int64_t>(call.params->integer("wall", 1), 1);
  const int64_t corridor = std::max<int64_t>(call.params->integer("corridor", 2), 1);
  const int64_t min_band = std::max<int64_t>(call.params->integer("min_band", 4), 1);
  const int64_t max_band = std::max<int64_t>(call.params->integer("max_band", min_band), min_band);
  const int64_t min_room = std::max<int64_t>(call.params->integer("min_room", 3), 1);
  const int64_t max_room = std::max<int64_t>(call.params->integer("max_room", min_room), min_room);
  const int64_t jitter = std::max<int64_t>(call.params->integer("jitter", 0), 0);
  // СКОЛЬКО ЧАСТЕЙ ОСТАВИТЬ ЦЕЛЫМИ, В ДОЛЯХ ОТ ИХ ЧИСЛА. Пятно не обязано резаться до последней
  // каморки: крупный нерасчленённый кусок — это парадный зал, атриум или двор, и получить его
  // нарезкой нельзя, потому что резка стремится к среднему. Поэтому он не «большая комната», а НЕ
  // РАЗРЕЗАННАЯ часть.
  //
  // ДОЛЯ, А НЕ ЧИСЛО, и это не вкус: у маленького пятна частей всего две, и объявленная тройка
  // оставила бы целым ВСЁ — здание, в котором ничего не разрезано, перестаёт зависеть от зерна
  // вовсе. Доля же значит одно и то же на любом пятне, а сотня означает «не резать ничего» и
  // остаётся законным ответом.
  const int64_t reserve_percent = std::clamp<int64_t>(call.params->integer("reserve_percent", 0), 0, 100);
  const double corridor_kind = call.params->number("corridor_kind", 1.0);
  const double room_kind = call.params->number("room_kind", 2.0);

  const int64_t inner_w = width - 2 * wall;
  const int64_t inner_h = height - 2 * wall;
  if (inner_w < min_room || inner_h < min_band) {
    utils::error{}("originator step '{}': slice_zones got a {}x{} footprint with {} of outer wall, which leaves "
                   "{}x{} inside — too little for a {}-unit place", call.step_name, width, height, wall, inner_w,
                   inner_h, min_room);
  }

  // Число лент: наименьшее, при котором полосы не глубже объявленного. Больше лент — теснее полосы,
  // меньше — глубже; предел глубины и решает.
  int64_t levels = 1;
  while ((levels + 1) * max_band + levels * (corridor + 2 * wall) < inner_h) {
    ++levels;
  }
  const int64_t floor_depth = (levels + 1) * min_band + levels * (corridor + 2 * wall);
  if (floor_depth > inner_h) {
    utils::error{}("originator step '{}': slice_zones needs {} units of depth for {} corridor bands with strips of "
                   "at least {}, and the footprint leaves {}", call.step_name, floor_depth, levels, min_band, inner_h);
  }
  const int64_t extra = inner_h - floor_depth;

  // ХРЕБЕТ. Нужен только когда лент больше одной — иначе связывать нечего.
  const bool has_spine = levels >= 2;
  const int64_t spine_x = wall + (inner_w - corridor) / 2;
  if (has_spine && (spine_x - 2 * wall < min_room || width - wall - (spine_x + corridor + wall) < min_room)) {
    utils::error{}("originator step '{}': slice_zones cannot fit places on both sides of a {}-unit spine in a {}-unit "
                   "wide footprint", call.step_name, corridor, width);
  }
  if (has_spine) {
    sink.push(plan_rect{spine_x, wall, corridor, inner_h}, corridor_kind);
  }

  const uint32_t seed = call_seed(call);

  // КАКИЕ ЧАСТИ ОСТАЮТСЯ ЦЕЛЫМИ. Выбор идёт по хешу от номера части, а не по «первым N»: иначе
  // крупные места всегда стояли бы в одном углу. Берутся `reserve` частей с наименьшим хешем — то
  // есть выбор полный и повторяемый, как и всё остальное здесь.
  const int64_t sides = has_spine ? 2 : 1;
  const int64_t parts = (levels + 1) * sides;
  std::vector<std::pair<uint32_t, int64_t>> draw(static_cast<size_t>(parts));
  for (int64_t part = 0; part < parts; ++part) {
    draw[size_t(part)] = {utils::shared::prng2(seed, uint32_t(part) + 1u), part};
  }
  std::sort(draw.begin(), draw.end());
  std::vector<uint8_t> whole(size_t(parts), 0);
  const int64_t reserved = parts * reserve_percent / 100;
  for (int64_t taken = 0; taken < reserved; ++taken) {
    whole[size_t(draw[size_t(taken)].second)] = 1;
  }

  int64_t v = wall;
  for (int64_t level = 0; level <= levels; ++level) {
    const int64_t depth = min_band + extra / (levels + 1) + (level < extra % (levels + 1) ? 1 : 0);

    const auto cut_strip = [&](const int64_t x0, const int64_t length, const int64_t side) {
      if (whole[size_t(level * sides + side)] != 0) {
        sink.push(plan_rect{x0, v, length, depth}, room_kind);
        return;
      }
      const auto salt = utils::shared::prng2(seed, uint32_t(level * sides + side) + 1001u);
      int64_t x = x0;
      for (const auto room_width : cut_span(call, length, wall, min_room, max_room, jitter, salt)) {
        sink.push(plan_rect{x, v, room_width, depth}, room_kind);
        x += room_width + wall;
      }
    };

    if (has_spine) {
      cut_strip(wall, spine_x - 2 * wall, 0);
      cut_strip(spine_x + corridor + wall, width - wall - (spine_x + corridor + wall), 1);
    } else {
      cut_strip(wall, inner_w, 0);
    }
    v += depth;

    if (level < levels) {
      v += wall;
      if (has_spine) {
        // Два куска, и оба ПРИМЫКАЮТ к хребту без стены: широкий проём, а не дверь.
        sink.push(plan_rect{wall, v, spine_x - wall, corridor}, corridor_kind);
        sink.push(plan_rect{spine_x + corridor, v, width - wall - (spine_x + corridor), corridor}, corridor_kind);
      } else {
        sink.push(plan_rect{wall, v, inner_w, corridor}, corridor_kind);
      }
      v += corridor + wall;
    }
  }

  counter.set(0, double(sink.count));
}

// zone_joins: СТЫКИ ВЫВОДЯТСЯ ИЗ ПЛАНА, и делается это ровно один раз.
//
// Прежде они выводились из РАСТРА, и это была скрытая привязка плана к картинке: пятно не могло быть
// крупнее растра, мера длины была клеткой, а цена поиска стыков росла вместе с размером изображения,
// хотя стыки — свойство десятков мест, а не миллиона клеток. Теперь геометрия работает над списком
// зон, а растр не участвует вовсе и появляется последним шагом.
//
// ЧТО ОТ ЭТОГО ИЗМЕНИЛОСЬ ПО СУЩЕСТВУ: стык стал утверждением О ПЛАНЕ («между этими местами ровно
// стена, и вот какой длины»), а не наблюдением над клетками. План теперь можно нарезать в мере,
// которой на картинке нет вовсе, и показать его в любом окне и любом масштабе — картинка не
// участвует в решениях, поэтому и испортить их не может.
//
// Стык бывает двух родов, и разница не косметическая:
//
//   ПРЯМОЙ        зоны касаются без стены. Это уже связь — широкий проём, и дверь ему не нужна;
//   ЧЕРЕЗ СТЕНУ   между зонами ровно `wall` меры ничьей стены. Только такой стык вправе стать
//                 дверью, потому что дверь — это кусок стены.
//
// Стык — это МАКСИМАЛЬНЫЙ ПРОБЕГ вдоль границы, а не отдельная точка: дверь ставится в середину
// пробега, а короткий пробег (угол в угол) дверью быть не может и отсекается по `min_span`. Две
// зоны вправе иметь НЕСКОЛЬКО стыков — это не вырождение, а угловое место на повороте коридора, и
// именно отсюда берутся кратные связи.
//
// ФОРМА УЧАСТВУЕТ, И ИНАЧЕ БЫЛО БЫ НЕВЕРНО. Пробег считается не по габаритам, а по тому, какие меры
// крайней линии места ему ДЕЙСТВИТЕЛЬНО принадлежат: у комнаты со срезанными углами прямой кромки
// меньше, чем сторона, и дверь в срез не ставится. Раньше это выходило само собой (растр знал форму),
// и потерять это при переезде значило бы получить двери, висящие в срезанном углу.
//
// ЗАСЛОНЁННЫЙ СТЫК НЕ СТЫК. Если в стене между двумя местами стоит третье (дверь прошлого прогона,
// ниша, столб), пробег рвётся на этом месте. Проверка идёт по списку зон, а не по растру, и почти
// всегда обходится ничем: заслонять стену в пядь шириной обычно нечему. Полагаться на это в коде
// нельзя — обещание «раздел не кладёт ничего в стену» принадлежит СОСЕДНЕМУ инструменту.
void tool_zone_joins(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': zone_joins fills the WHOLE join list, so its range must start at 0, "
                   "got [{}, {})", call.step_name, begin, end);
  }

  const auto rects = call.input(0).read();
  bool clamped = false;
  const size_t zones = read_count_field(call.input(1), rects.count(), clamped);
  const bool has_shape = call.has_input(2);
  const auto shapes = has_shape ? call.input(2).read() : const_field_accessor{};

  const auto pair = call.output(0).write();
  const auto span = call.output(1).write();
  const auto counter = call.output(2).write();

  const int64_t wall = std::max<int64_t>(call.params->integer("wall", 1), 1);
  const int64_t min_span = std::max<int64_t>(call.params->integer("min_span", 1), 1);
  const size_t capacity = end > begin ? end - begin : 0;

  const auto plan = read_geometry(call, rects, shapes, has_shape, zones, "zone_joins");
  const zone_index index(plan);

  size_t written = 0;
  const auto emit = [&](const size_t a, const size_t b, const bool through, const int64_t x, const int64_t y,
                        const int64_t length, const int64_t axis) {
    // `min_span` — это «сколько стены нужно, чтобы поставить дверь», и относится он поэтому ТОЛЬКО к
    // стыкам через стену. Прямое касание дверью не становится: узкое оно или широкое, зоны и так
    // связаны, и отбросить его значило бы потерять связь, которая существует.
    if (through && length < min_span) {
      return;
    }
    if (written >= capacity) {
      utils::error{}("originator step '{}': zone_joins ran out of the declared join capacity ({}) — a truncated list "
                     "of joins is a building whose doors were chosen from half the plan", call.step_name, capacity);
    }
    pair.set(written, double(a), 0);
    pair.set(written, double(b), 1);
    pair.set(written, through ? 1.0 : 0.0, 2);
    span.set(written, double(x), 0);
    span.set(written, double(y), 1);
    span.set(written, double(length), 2);
    span.set(written, double(axis), 3);
    ++written;
  };

  // Один разбор пары: ось (0 — граница вертикальная, пробег идёт по y), зазор (0 — прямое касание,
  // `wall` — стык через стену). Стороны переставляются так, чтобы `first` стояла перед `second`
  // вдоль оси: иначе стык той же пары нашёлся бы дважды и с разным началом пробега.
  const auto look = [&](const size_t a, const size_t b, const int64_t axis, const int64_t gap) {
    const auto& ra = plan[a].rect;
    const auto& rb = plan[b].rect;
    const int64_t a_low = axis == 0 ? ra.x : ra.y;
    const int64_t a_size = axis == 0 ? ra.w : ra.h;
    const int64_t b_low = axis == 0 ? rb.x : rb.y;
    const int64_t b_size = axis == 0 ? rb.w : rb.h;

    size_t first = a;
    size_t second = b;
    int64_t boundary = 0;
    if (a_low + a_size + gap == b_low) {
      boundary = a_low + a_size;
    } else if (b_low + b_size + gap == a_low) {
      first = b;
      second = a;
      boundary = b_low + b_size;
    } else {
      return;
    }

    const auto& rf = plan[first].rect;
    const auto& rs = plan[second].rect;
    const int64_t from = std::max(axis == 0 ? rf.y : rf.x, axis == 0 ? rs.y : rs.x);
    const int64_t to = std::min(axis == 0 ? rf.y + rf.h : rf.x + rf.w, axis == 0 ? rs.y + rs.h : rs.x + rs.w);
    if (from >= to) {
      return;
    }

    // ТРЕТЬИ ЗОНЫ, которым есть чем заслонить стену. Спрашиваются у сетки, а не перебором списка:
    // перебор платил бы числом зон за ответ, который почти везде «никого».
    std::vector<uint32_t> blockers;
    if (gap > 0) {
      const plan_rect strip = axis == 0 ? plan_rect{boundary, from, gap, to - from}
                                        : plan_rect{from, boundary, to - from, gap};
      index.gather(strip, first, second, blockers);
    }

    const auto clear = [&](const int64_t along) {
      for (int64_t step = 0; step < gap; ++step) {
        const int64_t x = axis == 0 ? boundary + step : along;
        const int64_t y = axis == 0 ? along : boundary + step;
        for (const auto c : blockers) {
          if (plan[c].holds(x, y)) {
            return false;
          }
        }
      }
      return true;
    };

    // Крайние линии обеих сторон — с формой, а не с габаритом.
    const auto touching = [&](const int64_t along) {
      const int64_t fx = axis == 0 ? boundary - 1 : along;
      const int64_t fy = axis == 0 ? along : boundary - 1;
      const int64_t sx = axis == 0 ? boundary + gap : along;
      const int64_t sy = axis == 0 ? along : boundary + gap;
      return plan[first].holds(fx, fy) && plan[second].holds(sx, sy) && clear(along);
    };

    int64_t run_start = from;
    int64_t run_length = 0;
    for (int64_t along = from; along <= to; ++along) {
      const bool joined = along < to && touching(along);
      if (joined) {
        if (run_length == 0) {
          run_start = along;
        }
        ++run_length;
        continue;
      }
      if (run_length > 0) {
        const int64_t x = axis == 0 ? boundary : run_start;
        const int64_t y = axis == 0 ? run_start : boundary;
        emit(first, second, gap > 0, x, y, run_length, axis);
      }
      run_length = 0;
    }
  };

  // ПАРЫ ИЩУТСЯ ПО НАЧАЛАМ СТОРОН, А НЕ ПЕРЕБОРОМ ВСЕХ ПАР. Стык требует ТОЧНОГО совпадения: одна
  // сторона кончается там, где начинается другая (плюс стена). Значит достаточно сложить зоны в
  // таблицу по началу стороны и спросить у неё; перебор пар платил бы квадрат от числа мест за ответ,
  // который почти везде «нет», и на участке в тысячу мест это было бы дороже всего остального шага.
  std::vector<std::pair<int64_t, uint32_t>> by_x(zones);
  std::vector<std::pair<int64_t, uint32_t>> by_y(zones);
  for (size_t i = 0; i < zones; ++i) {
    by_x[i] = {plan[i].rect.x, uint32_t(i)};
    by_y[i] = {plan[i].rect.y, uint32_t(i)};
  }
  std::sort(by_x.begin(), by_x.end());
  std::sort(by_y.begin(), by_y.end());

  // ПОРЯДОК ПЕРЕБОРА ОБЪЯВЛЕН: пары по возрастанию номеров, роды стыка следом. От порядка зависит
  // нумерация стыков, а от неё — какая дверь откроется при равной длине пробега; молчаливый порядок
  // сделал бы здание заложником порядка обхода в чужом контейнере. Поэтому найденное сначала
  // СОБИРАЕТСЯ, потом упорядочивается, и только потом разбирается.
  struct meeting {
    uint32_t low = 0;
    uint32_t high = 0;
    uint32_t rank = 0; // род стыка: прямое касание раньше стыка через стену, ось 0 раньше оси 1

    bool operator<(const meeting& other) const noexcept {
      if (low != other.low) return low < other.low;
      if (high != other.high) return high < other.high;
      return rank < other.rank;
    }

    bool operator==(const meeting& other) const noexcept = default;
  };

  std::vector<meeting> meetings;
  const auto collect = [&](const std::vector<std::pair<int64_t, uint32_t>>& table, const int64_t start,
                           const size_t a, const uint32_t rank) {
    auto it = std::lower_bound(table.begin(), table.end(), std::pair<int64_t, uint32_t>{start, 0});
    for (; it != table.end() && it->first == start; ++it) {
      if (size_t(it->second) == a) {
        continue;
      }
      meetings.push_back(meeting{uint32_t(std::min<size_t>(a, it->second)),
                                 uint32_t(std::max<size_t>(a, it->second)), rank});
    }
  };

  for (size_t a = 0; a < zones; ++a) {
    const auto& ra = plan[a].rect;
    for (uint32_t through = 0; through < 2; ++through) {
      const int64_t gap = through == 0 ? 0 : wall;
      collect(by_x, ra.x + ra.w + gap, a, through * 2 + 0);
      collect(by_y, ra.y + ra.h + gap, a, through * 2 + 1);
    }
  }
  std::sort(meetings.begin(), meetings.end());
  // Одна встреча может найтись дважды только у вырожденных габаритов, и тогда пара получила бы две
  // связи вместо одной. Уплотнение стоит прохода по списку и снимает вопрос совсем.
  meetings.erase(std::unique(meetings.begin(), meetings.end()), meetings.end());

  for (const auto& found : meetings) {
    look(found.low, found.high, found.rank % 2, found.rank < 2 ? 0 : wall);
  }
  counter.set(0, double(written));
}

// zone_graph: список стыков -> каноническое соседство (CSR).
//
// Нужен ровно затем, чтобы над зонами заработали инструменты графа — и первым делом решатель
// ограничений, который раскладывает ВИДЫ зон. Дуги симметричны и без повторов: у смежности нет
// направления, а два стыка одной пары (угловое место на повороте) говорят решателю то же самое, что
// один, — повтор сделал бы кратность видимой там, где её смысла нет.
void tool_zone_graph(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': zone_graph fills the WHOLE offset table, so its range must start at 0, "
                   "got [{}, {})", call.step_name, begin, end);
  }

  const auto pairs = call.input(0).read();
  bool clamped = false;
  const size_t joins = read_count_field(call.input(1), pairs.count(), clamped);
  const auto offsets = call.output(0).write();
  const auto arcs = call.output(1).write();
  const size_t zones = read_count_field(call.input(2), end > begin ? end - begin : 0, clamped);

  std::vector<std::pair<uint32_t, uint32_t>> edges;
  edges.reserve(joins * 2);
  for (size_t i = 0; i < joins; ++i) {
    const auto a = uint32_t(pairs.get(i, 0));
    const auto b = uint32_t(pairs.get(i, 1));
    if (a >= zones || b >= zones || a == b) {
      continue;
    }
    edges.emplace_back(a, b);
    edges.emplace_back(b, a);
  }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

  if (edges.size() > arcs.count()) {
    utils::error{}("originator step '{}': zone_graph needs {} arcs for {} joins and the buffer holds {}",
                   call.step_name, edges.size(), joins, arcs.count());
  }

  size_t written = 0;
  for (size_t zone = 0; zone < zones; ++zone) {
    offsets.set(zone, double(written));
    while (written < edges.size() && edges[written].first == zone) {
      arcs.set(written, double(edges[written].second));
      ++written;
    }
  }
  offsets.set(zones, double(written));
}

// open_doors: какие стыки становятся ДВЕРЬМИ.
//
// Здесь сходятся все три слоя, и держать их порознь важнее, чем кажется:
//
//   ТРЕБОВАНИЕ объявленные пары видов, между которыми дверь ОБЯЗАНА быть: каждый их стык становится
//              дверью, и открываются они ПЕРВЫМИ, до остова. Существует потому, что запрет и
//              требование — разные утверждения, и вторым запрет не выражается: «служебный вход в
//              кухню со двора» никаким запретом не получить, его надо потребовать;
//   ГАРАНТИЯ   остов по графу стыков — из любой зоны можно дойти до любой. Это обещание, и оно
//              либо выполнено, либо инструмент ОТКАЗЫВАЕТ вслух;
//   ПРАВИЛА    вид зоны говорит, с какими видами дверь допустима и сколько дверей зона терпит.
//              Запрет здесь жёсткий: дверь из кухни в покои не появится ни ради связности, ни ради
//              щедрости — если из-за запретов связать всё нельзя, это отказ, а не тихая дверь;
//   ЩЕДРОСТЬ   лишние двери поверх остова. Они ничего не обещают и потому ЗАЖИМАЮТСЯ ёмкостью и
//              пределами, а не отказывают.
//
// ТРЕБОВАНИЕ ОБЯЗАНО БЫТЬ ПОДМНОЖЕСТВОМ ДОПУСТИМОГО, и это проверяется до работы: пара, которая
// одновременно запрещена и обязательна, — противоречие в объявлении, а не задача для инструмента.
//
// Прямой стык (широкий проём) связью становится ВСЕГДА и двери не получает: дверь — это кусок
// стены, а между двумя коридорами стены нет. Поэтому остов строится уже поверх компонент, склеенных
// проёмами, и дверей открывается ровно столько, сколько осталось.
//
// Порядок перебора — (длина стыка по убыванию, номер стыка). Длина, а не вид: предпочтение по видам
// означало бы, что инструмент знает, какой вид «главный», а он не знает и знать не должен — это
// говорят ПРАВИЛА, и они уже сказали.
//
// ЛИШНЯЯ ДВЕРЬ ПОМЕЧАЕТСЯ (`spare`). Пометка не украшение: запирать можно только ту дверь, без
// которой связность остаётся, и это ровно двери вне остова. Дверь в тупиковую кладовую запереть
// нельзя — за ней кусок здания, куда игра больше не попадёт.
void tool_open_doors(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': open_doors appends to the WHOLE zone list, so its range must start at 0, "
                   "got [{}, {})", call.step_name, begin, end);
  }

  const auto pairs = call.input(0).read();
  const auto spans = call.input(1).read();
  bool clamped = false;
  const size_t joins = read_count_field(call.input(2), pairs.count(), clamped);
  const auto kinds = call.input(3).read();
  const size_t zones = read_count_field(call.input(4), kinds.count(), clamped);
  const auto allowed = call.input(5).read();
  const auto limits = call.input(6).read();
  const bool has_required = call.has_input(7);
  const auto required = has_required ? call.input(7).read() : const_field_accessor{};

  const auto zone_rect = call.output(0).write();
  const auto zone_kind = call.output(1).write();
  const auto edges = call.output(2).write();
  const auto places_counter = call.output(3).write();
  const auto links_counter = call.output(4).write();

  const size_t capacity = end > begin ? end - begin : 0;
  const size_t kind_count = limits.count();
  if (allowed.count() < kind_count * kind_count) {
    utils::error{}("originator step '{}': open_doors needs a {}x{} door matrix for {} kinds, and '{}.{}' holds {}",
                   call.step_name, kind_count, kind_count, kind_count, call.input(5).buffer_name(),
                   call.input(5).field_name(), allowed.count());
  }

  const int64_t wall = std::max<int64_t>(call.params->integer("wall", 1), 1);
  const int64_t door_width = std::max<int64_t>(call.params->integer("door_width", 1), 1);
  const auto spare_doors = size_t(std::max<int64_t>(call.params->integer("extra_doors", 0), 0));
  const double door_kind = call.params->number("door_kind", 0.0);

  std::vector<uint32_t> parent(zones);
  for (size_t i = 0; i < zones; ++i) {
    parent[i] = uint32_t(i);
  }
  const auto root = [&](uint32_t node) {
    while (parent[node] != node) {
      parent[node] = parent[parent[node]];
      node = parent[node];
    }
    return node;
  };

  std::vector<uint32_t> used(zones, 0);
  size_t places = zones;
  size_t links = 0;

  const auto kind_of = [&](const size_t zone) { return size_t(kinds.get(zone)); };
  const auto limit_of = [&](const size_t zone) {
    const auto kind = kind_of(zone);
    return kind < kind_count ? size_t(limits.get(kind)) : size_t(0);
  };
  const auto has_room_for_link = [&](const size_t zone) {
    const auto limit = limit_of(zone);
    return limit == 0 || used[zone] < limit;
  };
  const auto door_allowed = [&](const size_t a, const size_t b) {
    const auto ka = kind_of(a);
    const auto kb = kind_of(b);
    return ka < kind_count && kb < kind_count && allowed.get(ka * kind_count + kb) != 0.0;
  };
  const auto door_required = [&](const size_t a, const size_t b) {
    if (!has_required) {
      return false;
    }
    const auto ka = kind_of(a);
    const auto kb = kind_of(b);
    return ka < kind_count && kb < kind_count && required.get(ka * kind_count + kb) != 0.0;
  };

  // ПРОТИВОРЕЧИЕ В ОБЪЯВЛЕНИИ ловится до работы: пара, которая обязана быть и при этом запрещена,
  // означает, что автор сказал две вещи сразу, и выбирать за него нельзя.
  if (has_required) {
    if (required.count() < kind_count * kind_count) {
      utils::error{}("originator step '{}': open_doors needs a {}x{} matrix of required doors for {} kinds, and "
                     "'{}.{}' holds {}", call.step_name, kind_count, kind_count, kind_count,
                     call.input(7).buffer_name(), call.input(7).field_name(), required.count());
    }
    for (size_t a = 0; a < kind_count; ++a) {
      for (size_t b = 0; b < kind_count; ++b) {
        if (required.get(a * kind_count + b) != 0.0 && allowed.get(a * kind_count + b) == 0.0) {
          utils::error{}("originator step '{}': open_doors got kinds {} and {} declared both REQUIRED and forbidden "
                         "to share a door", call.step_name, a, b);
        }
      }
    }
  }

  const auto write_link = [&](const size_t a, const size_t b, const size_t door, const bool spare) {
    if (links >= edges.count()) {
      utils::error{}("originator step '{}': open_doors ran out of the declared link capacity ({})",
                     call.step_name, edges.count());
    }
    edges.set(links, double(a), 0);
    edges.set(links, double(b), 1);
    edges.set(links, double(door), 2);
    edges.set(links, spare ? 1.0 : 0.0, 3);
    ++links;
    used[a] += 1;
    used[b] += 1;
  };

  // ПРЯМЫЕ СТЫКИ — связи без двери, и они не выбираются: стены между зонами нет, закрывать нечего.
  // Записываются ВСЕ, даже второй проём между той же парой: широкий проём это ФАКТ ГЕОМЕТРИИ, а не
  // выбор, и список связей обязан описывать мир, а не только его связность.
  for (size_t i = 0; i < joins; ++i) {
    if (pairs.get(i, 2) != 0.0) {
      continue;
    }
    const auto a = size_t(pairs.get(i, 0));
    const auto b = size_t(pairs.get(i, 1));
    if (a >= zones || b >= zones) {
      continue;
    }
    const bool spare = root(uint32_t(a)) == root(uint32_t(b));
    parent[root(uint32_t(a))] = root(uint32_t(b));
    write_link(a, b, 0, spare);
  }

  struct candidate {
    int64_t length = 0;
    uint32_t join = 0;

    bool operator<(const candidate& other) const noexcept {
      if (length != other.length) return length > other.length; // длинный стык — удобная дверь
      return join < other.join;
    }
  };

  std::vector<candidate> order;
  order.reserve(joins);
  for (size_t i = 0; i < joins; ++i) {
    if (pairs.get(i, 2) == 0.0) {
      continue;
    }
    order.push_back(candidate{int64_t(spans.get(i, 2)), uint32_t(i)});
  }
  std::sort(order.begin(), order.end());

  // ЛИШНЯЯ ЛИ ДВЕРЬ — решает не тот, кто её открывает, а состояние связности В МОМЕНТ открытия: если
  // стороны уже связаны, без этой двери связность останется, значит её можно запереть. Передавать
  // признак снаружи означало бы, что обязательная дверь в цикле однажды окажется незапираемой по
  // ошибке вызывающего.
  const auto open_door = [&](const size_t join) {
    const auto a = size_t(pairs.get(join, 0));
    const auto b = size_t(pairs.get(join, 1));
    const bool spare = root(uint32_t(a)) == root(uint32_t(b));
    const int64_t x = int64_t(spans.get(join, 0));
    const int64_t y = int64_t(spans.get(join, 1));
    const int64_t length = int64_t(spans.get(join, 2));
    const int64_t axis = int64_t(spans.get(join, 3));
    const int64_t leaf = std::min(door_width, length);

    if (places >= capacity) {
      utils::error{}("originator step '{}': open_doors ran out of the declared zone capacity ({}) while adding "
                     "doors — a door is a zone, and a building missing one is not the building that was planned",
                     call.step_name, capacity);
    }

    const auto door = axis == 0 ? plan_rect{x, y + (length - leaf) / 2, wall, leaf}
                                : plan_rect{x + (length - leaf) / 2, y, leaf, wall};
    // ДВЕРЬ ДОПИСЫВАЕТСЯ В СПИСОК МЕСТ, И БОЛЬШЕ НИКУДА. Прежде она вдобавок рисовалась в растре, и
    // правда о двери лежала в двух местах сразу: зона говорила одно, клетки — другое, и разойтись им
    // мешало только то, что писал их один и тот же цикл. Растр теперь рисует проектор, из этого же
    // списка.
    write_rect(zone_rect, places, door);
    zone_kind.set(places, door_kind);
    ++places;
    parent[root(uint32_t(a))] = root(uint32_t(b));
    write_link(a, b, places, spare); // номер зоны-двери плюс один: ноль значит «широкий проём»
  };

  std::vector<uint8_t> taken(order.size(), 0);

  // ТРЕБОВАНИЯ ИДУТ ПЕРВЫМИ, и именно поэтому предел числа связей их не съедает: то, что объявлено
  // обязательным, не должно проигрывать гонку за места в пределе случайному стыку.
  for (size_t i = 0; i < order.size(); ++i) {
    const auto join = order[i].join;
    const auto a = size_t(pairs.get(join, 0));
    const auto b = size_t(pairs.get(join, 1));
    if (a >= zones || b >= zones || !door_required(a, b)) {
      continue;
    }
    if (!has_room_for_link(a) || !has_room_for_link(b)) {
      utils::error{}("originator step '{}': open_doors must put a door between zones {} and {} (kinds {} and {} are "
                     "declared required), and the link limit of one of them is already spent — two declarations "
                     "contradict each other", call.step_name, a, b, kind_of(a), kind_of(b));
    }
    open_door(join);
    taken[i] = 1;
  }

  // ОСТОВ РАСТЁТ ОТ ЦИРКУЛЯЦИИ, А НЕ СОБИРАЕТСЯ ИЗ САМЫХ ДЛИННЫХ СТЫКОВ, и разница не в красоте.
  //
  // Жадный набор по длине (Краскал) связывает места ДРУГ С ДРУГОМ: кухня успевает потратить свой
  // предел связей на две кладовые прежде, чем доберётся до коридора, и куст «кухня + кладовые +
  // двор» остаётся отрезанным от дома. Формально это верный остов на графе без пределов — и
  // бесполезное здание.
  //
  // Рост от уже связанного (Прим) даёт ровно то, чем здание и является: места ВИСЯТ НА ЦИРКУЛЯЦИИ.
  // Начинается он с САМОЙ КРУПНОЙ уже связанной части, и это не эвристика про «коридор»: после
  // широких проёмов самой крупной оказывается именно сеть циркуляции, потому что её куски соединены
  // без дверей. Инструмент про виды по-прежнему не знает ничего.
  std::vector<size_t> component_size(zones, 0);
  for (size_t zone = 0; zone < zones; ++zone) {
    component_size[root(uint32_t(zone))] += 1;
  }
  uint32_t core = 0;
  for (size_t zone = 0; zone < zones; ++zone) {
    if (component_size[zone] > component_size[core]) {
      core = uint32_t(zone);
    }
  }

  // Растёт он ПРОХОДАМИ по отсортированному списку, а не выбором лучшего стыка на каждом шаге, и это
  // не приближение ради скорости: выбор лучшего требует заново просмотреть все стыки после КАЖДОЙ
  // двери, то есть квадрат от их числа (замер: `256x256` собиралось 26.8 мс против 6.0 у прежнего
  // жадного набора). Проход же видит рост по ходу — компонента расширяется прямо посреди него, — и
  // проходов нужно единицы, потому что места висят на циркуляции, а не цепочкой друг за другом.
  bool growing = true;
  while (growing) {
    growing = false;
    for (size_t i = 0; i < order.size(); ++i) {
      if (taken[i] != 0) {
        continue;
      }
      const auto join = order[i].join;
      const auto a = size_t(pairs.get(join, 0));
      const auto b = size_t(pairs.get(join, 1));
      if (a >= zones || b >= zones || !door_allowed(a, b) || !has_room_for_link(a) || !has_room_for_link(b)) {
        continue;
      }
      // Ровно одна сторона снаружи: иначе дверь либо ничего не присоединяет, либо соединяет две
      // чужие друг другу части в обход циркуляции. Корень ядра читается ЗДЕСЬ, а не запоминается: он
      // меняется, как только к ядру присоединяют очередное место.
      const auto inside = root(core);
      if ((root(uint32_t(a)) == inside) == (root(uint32_t(b)) == inside)) {
        continue;
      }
      open_door(join);
      taken[i] = 1;
      growing = true;
    }
  }

  // ОТКАЗ ВМЕСТО ОТРЕЗАННОЙ ЗОНЫ. Причина всегда одна из двух — правила видов или предел числа
  // дверей, — и обе названы автором, поэтому назвать зону достаточно, чтобы стало понятно.
  for (size_t zone = 1; zone < zones; ++zone) {
    if (root(uint32_t(zone)) == root(0)) {
      continue;
    }

    // ОТКАЗ НЕСЁТ ПРИЧИНУ, а причин ровно три, и они разные по смыслу: у зоны может не быть стыков
    // вовсе (раздел поставил её так), стыки могут быть запрещены правилами видов, или их съел
    // предел числа связей. Без этого разбора автору пришлось бы гадать, что именно он объявил не так.
    size_t total = 0;
    size_t forbidden = 0;
    size_t blocked = 0;
    for (size_t i = 0; i < joins; ++i) {
      const auto a = size_t(pairs.get(i, 0));
      const auto b = size_t(pairs.get(i, 1));
      if (a != zone && b != zone) {
        continue;
      }
      ++total;
      const auto other = a == zone ? b : a;
      if (!door_allowed(a, b)) {
        ++forbidden;
      } else if (!has_room_for_link(zone) || !has_room_for_link(other)) {
        ++blocked;
      }
    }

    // Соседи названы поимённо, но не все: четырёх хватает, чтобы понять, какое объявление мешает, а
    // список на сотню записей читать всё равно никто не станет.
    std::string neighbours;
    size_t shown = 0;
    for (size_t i = 0; i < joins && shown < 4; ++i) {
      const auto a = size_t(pairs.get(i, 0));
      const auto b = size_t(pairs.get(i, 1));
      if (a != zone && b != zone) {
        continue;
      }
      const auto other = a == zone ? b : a;
      neighbours += std::format(" [zone {} kind {} links {}/{}]", other, kind_of(other), used[other], limit_of(other));
      ++shown;
    }

    utils::error{}("originator step '{}': open_doors cannot reach zone {} (kind {}, {} links of {}) — it has {} "
                   "joins, {} forbidden by the door rules and {} blocked by a link limit, neighbours:{}",
                   call.step_name, zone, kind_of(zone), used[zone], limit_of(zone), total, forbidden, blocked,
                   neighbours);
  }

  // ЩЕДРОСТЬ. Лишние двери ничего не обещают, поэтому здесь зажим, а не отказ: сколько влезло в
  // пределы и ёмкость, столько и открылось, и это видно по счётчику.
  size_t spare_opened = 0;
  for (size_t i = 0; i < order.size() && spare_opened < spare_doors; ++i) {
    if (taken[i] != 0) {
      continue;
    }
    const auto join = order[i].join;
    const auto a = size_t(pairs.get(join, 0));
    const auto b = size_t(pairs.get(join, 1));
    if (a >= zones || b >= zones || !door_allowed(a, b) || !has_room_for_link(a) || !has_room_for_link(b)) {
      continue;
    }
    open_door(join);
    ++spare_opened;
  }

  places_counter.set(0, double(places));
  links_counter.set(0, double(links));
}

} // namespace

void tool_registry::add_structure_tools() {
  add(tool_description{.name = "place_rooms", .shape = aperture::sequential, .input_count = 0, .output_count = 3,
                       .optional_outputs = 1, .body = tool_place_rooms,
                       // Принятые комнаты живут в ВЫХОДНОМ буфере, и проверка пересечения читает его
                       // же: временных таблиц у отбора с отказом нет вовсе.
                       .footprint = no_temporary_memory});
  add(tool_description{.name = "link_rooms", .shape = aperture::sequential, .input_count = 2, .output_count = 2,
                       .body = tool_link_rooms,
                       // План комнат, три массива Прима и короткий список петель. Верхняя оценка
                       // берётся по ЁМКОСТИ списка комнат: сколько их окажется, до чтения счётчика
                       // неизвестно, а занижать стоимость памяти нельзя.
                       .footprint = [](const tool_call& call) {
                         const size_t rooms = call.input(0).count();
                         const size_t extra = size_t(std::max<int64_t>(call.params->integer("extra_links", 0), 0));
                         return rooms * (sizeof(plan_rect) + sizeof(int64_t) + sizeof(uint32_t) + sizeof(uint8_t)) +
                                extra * (sizeof(int64_t) + 2 * sizeof(uint32_t));
                       }});
  add(tool_description{.name = "link_path", .shape = aperture::gather, .input_count = 2, .output_count = 1,
                       .body = tool_link_path, .footprint = no_temporary_memory});
  add(tool_description{.name = "slice_zones", .shape = aperture::sequential, .input_count = 0, .output_count = 3,
                       .body = tool_slice_zones,
                       // Ширины одной полосы — единственная таблица, и она не длиннее числа мест в
                       // полосе; верхняя оценка берётся по ёмкости списка зон.
                       .footprint = [](const tool_call& call) { return call.range_count() * sizeof(int64_t); }});
  add(tool_description{.name = "zone_joins", .shape = aperture::sequential, .input_count = 3,
                       .optional_inputs = 1, .output_count = 3, .body = tool_zone_joins,
                       // Габарит и форма каждой зоны плюс короткий список заслоняющих третьих зон;
                       // верхняя оценка берётся по ЁМКОСТИ списка зон, потому что счётчик читается
                       // уже внутри.
                       .footprint = [](const tool_call& call) {
                         return call.input(0).count() * (sizeof(zone_geometry) + sizeof(uint32_t));
                       }});
  add(tool_description{.name = "zone_graph", .shape = aperture::sequential, .input_count = 3, .output_count = 2,
                       .body = tool_zone_graph,
                       // Дуги в обе стороны до уплотнения: два конца на стык.
                       .footprint = [](const tool_call& call) {
                         return call.input(0).count() * 2 * sizeof(std::pair<uint32_t, uint32_t>);
                       }});
  add(tool_description{.name = "open_doors", .shape = aperture::sequential, .input_count = 8,
                       .optional_inputs = 1, .output_count = 5, .body = tool_open_doors,
                       // Непересекающиеся множества по зонам, счётчик связей и порядок перебора
                       // стыков; стыков не больше объявленной ёмкости их списка.
                       .footprint = [](const tool_call& call) {
                         return call.range_count() * (2 * sizeof(uint32_t)) +
                                call.input(0).count() * (sizeof(int64_t) + sizeof(uint32_t) + 1);
                       }});
  add(tool_description{.name = "paint_rects", .shape = aperture::scatter, .input_count = 2, .optional_inputs = 1,
                       .output_count = 1, .body = tool_paint_rects, .footprint = no_temporary_memory});
  add(tool_description{.name = "place_columns", .shape = aperture::sequential, .input_count = 4,
                       .optional_inputs = 2, .output_count = 2, .body = tool_place_columns,
                       .footprint = [](const tool_call& call) {
                         return call.input(0).count() * sizeof(zone_geometry);
                       }});
  add(tool_description{.name = "project_zones", .shape = aperture::sequential, .input_count = 5,
                       .optional_inputs = 3, .output_count = 3, .body = tool_project_zones,
                       .footprint = [](const tool_call& call) {
                         return call.input(0).count() * sizeof(zone_geometry);
                       }});
  add(tool_description{.name = "carve_corridors", .shape = aperture::scatter, .input_count = 1, .output_count = 1,
                       .body = tool_carve_corridors, .footprint = no_temporary_memory});
}

} // namespace originator
} // namespace devils_engine
