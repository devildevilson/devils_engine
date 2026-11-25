#ifndef FRONTIER_ONLINE_CORE_LAUNCH_OPTIONS_H
#define FRONTIER_ONLINE_CORE_LAUNCH_OPTIONS_H

#include <cstdint>
#include <string>

#include <devils_engine/simul/launch_arguments.h>

#include "core/net_client.h"

// Опции графического клиента из аргументов запуска. Живут в bootstrap процесса: main проверяет
// аргументы до старта, init_bootstrap переводит их сюда, а системы получают свою часть при создании
// (съём кадра — рендер и главный поток, `--join` — сцена). Синглтонов «запрос процесса» нет.

namespace frontier_online {
namespace core {

// Снимок по команде запуска: `--dump=ПУТЬ` (.avif, .png или .ppm — отладка) на рисованном кадре `--frames=N`
// (по умолчанию первый). Тот же механизм, что у клавиши `screenshot`: game_host::request_frame_capture.
// Без `--stay` процесс выходит, когда снимок записан (код 0) или не удался (код 1): стенд запускается
// из скрипта, и висящее окно означало бы, что скрипт ждёт вечно.
struct capture_option {
  uint64_t at_frame = 0;
  std::string path;
  bool exit_after = true;

  bool wanted() const noexcept {
    return at_frame != 0 && !path.empty();
  }
};

struct launch_options {
  capture_option capture;
  join_request_t join;
};

// Перевести аргументы в опции. Неопознанный или испорченный аргумент — отказ с объяснением в problem:
// молча запущенный не тот режим потом объясняют часами.
[[nodiscard]] bool parse_launch_options(const devils_engine::simul::launch_arguments& arguments,
                                        launch_options& out, std::string& problem);

} // namespace core
} // namespace frontier_online

#endif
