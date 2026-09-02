#ifndef DEVILS_ENGINE_GN02_VIEWER_H
#define DEVILS_ENGINE_GN02_VIEWER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "devils_engine/originator/pipeline.h"

// Просмотр результата генерации.
//
// Площадка headless по своей природе: её результат — данные. Но у генератора есть вопрос, на который
// числа отвечают плохо: «похоже ли это на планету». Поэтому просмотрщик существует, и он намеренно
// простой — сфера, цвет клетки по выбранному полю, смещение по высоте.
//
// Второе, ради чего он нужен: ПРОСМОТР ПО ШАГАМ. Мир считается вниз по причинам, и увидеть, что
// добавил каждый шаг, — это отладка, которую иначе приходится делать по диапазонам полей. Смена шага
// пересчитывает мир до этого шага и только: следующие поля остаются нулевыми, и это видно.

namespace devils_engine::gn02 {

struct viewer_options {
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t frames = 0;
  uint64_t seed = 0;
  // Подразделений икосферы: 20 * 4^n треугольников. Ноль означает «подобрать по числу клеток»:
  // сетка должна быть ЗАМЕТНО мельче клеток, иначе берег превращается в лестницу из треугольников,
  // и видно разрешение сетки, а не форму материка.
  uint32_t subdivisions = 0;
  float camera_distance = 2.6f;
  // Преувеличение рельефа в разах. Планета с точностью до процента гладкая, поэтому без
  // преувеличения рельеф на глобусе не виден; выше шести начинают выпирать зубцы на берегу, где
  // соседние клетки различаются на четыре километра.
  float relief_scale = 6.0f;
  bool validation = false;
  bool fixed_rotation = false;
  // Частота кадров ограничена шестьюдесятью: просмотрщик планеты — не бенчмарк, и рисовать
  // две с половиной тысячи кадров в секунду означает греть машину ради кадров, которых никто не
  // увидит. Снимается флагом --uncapped, когда частоту как раз и мерят.
  bool uncapped = false;
  size_t mode = 0;
  size_t step_limit = 0; // 0 = все шаги
  std::string dump_path;
};

// Мир, пересчитанный до указанного шага.
struct generated_world {
  std::unique_ptr<originator::pipeline> line;
  double milliseconds = 0.0;
  std::vector<std::string> step_names;
  size_t executed_steps = 0;
  uint64_t seed = 0;
};

// Настраиваемое значение: диапазон из конфига плюс текущее значение. Просмотрщик не знает, что оно
// означает, и знать не должен: границы и шаг объявлены рядом с самим значением в values.tavl.
struct tunable_value {
  originator::value_range range;
  double value = 0.0;
};

struct generation_request {
  size_t step_limit = 0;
  uint64_t seed = 0;
  // Значения, которыми надо накрыть конфиг. Отдаются ВСЕ настраиваемые, а не только изменённые:
  // список короткий, а «применилось не то, что показано» — ошибка, которую потом не найдёшь.
  std::vector<std::pair<std::string, double>> overrides;
};

using regenerate_fn = std::function<generated_world(const generation_request&)>;

int run_viewer(const viewer_options& options, std::vector<tunable_value> tunables,
               const regenerate_fn& regenerate);

} // namespace devils_engine::gn02

#endif
