#ifndef DEVILS_ENGINE_SIMUL_FRAME_CAPTURE_H
#define DEVILS_ENGINE_SIMUL_FRAME_CAPTURE_H

#include <chrono>
#include <cstdint>
#include <format>
#include <string>

#include <devils_engine/painter/image_file.h>
#include <devils_engine/utils/platform.h>

// Снимок экрана со стороны главного потока: путь по умолчанию для клавиши пользователя. Сам съём —
// messages.h (command_capture_frame / command_frame_captured), game_host::request_frame_capture.
// Клиентский заголовок: тянет painter (кодировщики), в headless не включается.

namespace devils_engine {
namespace simul {

// `<проект>/screenshots/ГГГГММДД-ЧЧММСС-мс.avif`; без кодировщика AVIF в сборке — `.png`. Путь
// внутри папки проекта: абсолютный корень — только utils::project_folder.
inline std::string default_screenshot_path() {
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
  const char* ext = painter::image_file_format_available(painter::image_file_format::avif) ? "avif" : "png";
  return std::format("{}screenshots/{:%Y%m%d-%H%M%S}-{:03}.{}", utils::project_folder(), seconds, ms, ext);
}

} // namespace simul
} // namespace devils_engine

#endif
