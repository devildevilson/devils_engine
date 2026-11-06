#ifndef DEVILS_ENGINE_GN03_VIEWER_H
#define DEVILS_ENGINE_GN03_VIEWER_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "devils_engine/originator/pipeline.h"

#include "memory.h"
#include "streaming.h"

// Окно площадки: свободная камера и геометрия, которая строится вокруг неё.
//
// Оно существует не «для красоты». Числа отвечают почти на всё, что доказывает эта площадка, но у
// объёма есть вопрос, на который числа отвечают плохо: НЕТ ЛИ ШВА. Шов шириной в один треугольник —
// это ноль в отчёте (вершины совпадают точно) и ясно видимая полоса на экране, если совпадают не
// вершины, а только их окрестность. Второе, что видно только глазами: успевает ли мир появляться
// перед летящей камерой.

namespace devils_engine::gn03 {

// Настраиваемое значение генератора: диапазон из конфига плюс текущее значение. Окно не знает, что
// значение означает, и знать не должно: границы и шаг объявлены рядом с самим числом в values.tavl.
struct tunable_value {
  originator::value_range range;
  double value = 0.0;
};

// Пересборка генератора: другие значения (и другое зерно) означают другой мир, а значит другие
// пайплайны у всех рабочих потоков и пустую арену. Окно умеет попросить об этом, но не умеет это
// сделать — сборка пайплайна принадлежит хосту.
using factory_builder =
  std::function<generator_factory(const std::vector<std::pair<std::string, double>>& overrides, uint64_t seed)>;

// ОТЛАДОЧНАЯ ЛИНИЯ в мировых метрах. Хост отдаёт их списком (например, граф маршрутов), а окно
// каждый кадр приводит их к системе чанка камеры, как сущности: линий тысячи, а не миллионы.
struct debug_line {
  glm::dvec3 a{};
  glm::dvec3 b{};
  uint32_t colour = 0xffffffffu; // RGBA8, младший байт — красный
};

// Источник линий вокруг наблюдателя: зерно и мировая позиция камеры. Вызывается В ФОНОВОМ
// ПОТОКЕ, не чаще, чем камера уходит на `lines_refresh` метров, поэтому может быть дорогим.
using line_provider = std::function<std::vector<debug_line>(uint64_t seed, glm::dvec3 observer)>;

struct viewer_options {
  std::string window_title = "GN03 — streaming volume";
  std::string overlay_title = "GN03 streaming volume";
  std::string overlay_summary =
    "marching cubes over a chunked density field, generated around the camera in worker threads";
  std::string overlay_controls =
    "WASD/QE fly | Shift fast | M view | G chunk grid | Tab/-/= tune | Enter apply | N new seed | U overlay | Esc quit";
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t frames = 0;
  bool validation = false;
  bool uncapped = false;
  // Keep the CPU frame pacer while avoiding a second FIFO/VSync throttle where supported.
  // Opt-in: existing playground presentation behaviour is unchanged.
  bool async_present = false;
  // Представление и сетка чанков задаются и ключом: отладочный вид нужен в автоматическом прогоне,
  // где кнопку нажать некому, а именно по нему проверяется «нет ли шва».
  size_t mode = 0;
  bool grid = false;
  std::string dump_path;
  // ПОЛЁТ ПО ЗАПИСИ: скорость в метрах в секунду, ноль — камера слушается клавиш. Существует ради
  // одного утверждения, которое иначе проверить нечем: «на ускорении кадры просаживаются». Просадка
  // это свойство ПОЛЁТА, а не сцены, поэтому и полёт обязан быть повторяемым, а не наигранным руками.
  double flight_speed = 0.0;
  // Zero keeps the existing chunk-relative free-flight speed.
  double move_speed = 0.0;
  float near_plane = 0.1f;
  // Real elapsed-time motion for streaming acceptance tests; legacy fixed-step replay stays default.
  bool realtime_flight = false;
  // Optional acceptance route: turn 90 degrees after each interval, 0 keeps a straight flight.
  double flight_turn_seconds = 0.0;
  // Гасить ли строки чанков за пирамидой видимости. ПО УМОЛЧАНИЮ НЕТ, и это результат замера, а не
  // забывчивость: отсечение оставляет 30% чанков окна, а пасс от этого не меняется вовсе (4.65 против
  // 4.74 мс) — цена пасса пропорциональна ВЕРШИНАМ В ДИАПАЗОНЕ ОТРИСОВКИ, а не тому, что видно.
  // Ключ оставлен, чтобы этот замер можно было повторить одной командой.
  bool cull = false;
  // Сколько вершин арены оттирать за кадр. Ключ существует ради замера: бюджет размером со всю арену
  // это прежнее поведение — выгрузка платила за себя целиком в том же кадре.
  size_t scrub_budget = 65536;

  // АТМОСФЕРА. По умолчанию — небо и туман цвета горизонта, как в GN03. Подводная — тёмная
  // вода без солнца: рассеянный свет толщи, фонарь у камеры и туман, который ГАРАНТИРОВАННО
  // доходит до полной плотности за чанк до края окна по любой оси, а фон ровно цвета тумана.
  // Тогда ещё не подгруженный чанк неотличим от темноты, а подгрузка не видна вовсе.
  bool underwater = false;
  float water_colour[3] = {0.010f, 0.024f, 0.030f};
  float ambient = 1.0f;          // множитель рассеянного света толщи
  float lamp = 1.8f;             // сила фонаря у камеры, 0 — без фонаря
  float lamp_range = 40.0f;      // метры, на которых фонарь ослабевает вдвое
  float fog_density = 0.012f;    // 1/м: экспоненциальная мутность воды
  float fog_distance = 0.0f;     // метры полной плотности; 0 — край окна минус чанк
  // Палитра породы по номеру вида полости (GN09 rock_kind) вместо ленты биомов GN03:
  // оттенок — дискретный номер, а не смешанное значение. Построенное (>= 112) — как было,
  // плюс стекло 113.
  bool rock_palette = false;

  uint64_t seed = 1;
  size_t chunk_cells = 32;
  double cell_size = 1.0;
  size_t arena_vertices = 2u << 20;
  size_t workers = 4;
  // Опережение по ходу камеры: окно, сдвинутое на 1..prefetch_layers чанков вперёд по направлению
  // движения, минус текущее окно — у этих чанков заранее готовятся зависимости (prefetch). 0 — нет.
  chunk_prefetcher prefetch;
  uint32_t prefetch_layers = 0;
  // ГАРАНТИЯ: камера не входит в чанк, пока куб чанков радиуса guard_radius вокруг него не построен
  // целиком — дыр рядом с камерой нет по построению, а при нехватке генератора полёт замедляется.
  // -1 — выключено.
  int guard_radius = -1;
  chunk_window window{};
  // Чанк, с которого начинает камера. Мир обязан выглядеть одинаково и рядом с нулём, и в миллионе
  // чанков от него — иначе «всё относительно камеры» сделано не до конца.
  originator::chunk_key start{0, 2, 0};
  // Место внутри этого чанка и направление взгляда. Существуют ради посадки НА МАРШРУТ каркаса:
  // коридор — узкая труба, и увидеть её можно только изнутри, а нажать кнопку в автоматическом
  // прогоне некому.
  bool start_offset_valid = false;
  glm::dvec3 start_offset{};
  float start_yaw = 0.0f;
  float start_pitch = 0.0f;

  std::vector<tunable_value> tunables;

  // ПАМЯТЬ МИРА, и она принадлежит ХОСТУ, а не окну: окно умеет её менять (игрок забрал веху), но
  // читает и пишет её на диск тот, кто знает, где лежит сохранение. Пустой указатель означал бы мир
  // без памяти, поэтому его нет: склад существует всегда, просто может быть пустым.
  world_memory* memory = nullptr;

  // Отладочные линии (клавиша L: выкл / с глубиной / сквозь стены). Без источника слоя нет.
  line_provider lines;
  double lines_refresh = 64.0;
  uint32_t lines_mode = 1;
};

int run_viewer(const viewer_options& options, const factory_builder& builder);

} // namespace devils_engine::gn03

#endif
