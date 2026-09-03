#ifndef DEVILS_ENGINE_GN03_VIEWER_H
#define DEVILS_ENGINE_GN03_VIEWER_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "devils_engine/originator/pipeline.h"

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

struct viewer_options {
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t frames = 0;
  bool validation = false;
  bool uncapped = false;
  // Представление и сетка чанков задаются и ключом: отладочный вид нужен в автоматическом прогоне,
  // где кнопку нажать некому, а именно по нему проверяется «нет ли шва».
  size_t mode = 0;
  bool grid = false;
  std::string dump_path;

  uint64_t seed = 1;
  size_t chunk_cells = 32;
  double cell_size = 1.0;
  size_t arena_vertices = 2u << 20;
  size_t workers = 4;
  chunk_window window{};
  // Чанк, с которого начинает камера. Мир обязан выглядеть одинаково и рядом с нулём, и в миллионе
  // чанков от него — иначе «всё относительно камеры» сделано не до конца.
  originator::chunk_key start{0, 2, 0};

  std::vector<tunable_value> tunables;
};

int run_viewer(const viewer_options& options, const factory_builder& builder);

} // namespace devils_engine::gn03

#endif
