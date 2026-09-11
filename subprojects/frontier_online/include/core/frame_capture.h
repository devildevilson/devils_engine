#ifndef FRONTIER_ONLINE_CORE_FRAME_CAPTURE_H
#define FRONTIER_ONLINE_CORE_FRAME_CAPTURE_H

#include <atomic>
#include <cstdint>
#include <string>

// СЪЁМ КАДРА — инструмент разработчика, тот же по смыслу, что `--frames/--dump` у площадок.
//
// Нужен ровно затем, чтобы на результат можно было ПОСМОТРЕТЬ, не имея перед собой экрана:
// гистограмма классов рельефа говорит, что мир не выродился, но не говорит, что он выглядит как
// мир. Кадр снимается с цели `albedo_res` (то, во что рисует граф) и пишется в PPM — формат
// выбран за то, что его пишет десяток строк без единой зависимости.
//
// Запрос заполняется ОДИН РАЗ в main до старта потоков и дальше только читается; меняется после
// старта единственное поле — `done`, и оно атомарно, потому что ставит его поток рендера, а
// читает главный, решая, пора ли выходить.

namespace devils_engine {
namespace painter {
class graphics_base;
}
} // namespace devils_engine

namespace frontier_online {
namespace core {

struct frame_capture_request {
  // Номер кадра, на котором снимать (считая с первого нарисованного). Ноль = не снимать.
  uint64_t at_frame = 0;
  std::string path;
  // Выйти сразу после съёмки. Без этого стенд остался бы висеть окном, а он запускается из
  // скрипта.
  bool exit_after = true;

  std::atomic<uint64_t> drawn{0};
  std::atomic<bool> done{false};

  bool wanted() const noexcept {
    return at_frame != 0 && !path.empty();
  }
  bool finished() const noexcept {
    return wanted() && exit_after && done.load(std::memory_order_acquire);
  }
};

// Единственный запрос процесса. Инструмент разработчика, а не часть игры, поэтому он и живёт
// здесь, а не протянут через конфиг, брокер и три конструктора.
frame_capture_request& capture_request() noexcept;

// Снять текущее содержимое цели `resource_name` и записать PPM. Зовётся из ПОТОКА РЕНДЕРА сразу
// после отправки кадра.
void capture_frame(devils_engine::painter::graphics_base& base, const std::string& resource_name,
                   const std::string& path);

} // namespace core
} // namespace frontier_online

#endif
