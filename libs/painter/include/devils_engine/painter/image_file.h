#ifndef DEVILS_ENGINE_PAINTER_IMAGE_FILE_H
#define DEVILS_ENGINE_PAINTER_IMAGE_FILE_H

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ЗАПИСЬ СНИМКА В ФАЙЛ: AVIF (основной), PNG (запасной) и PPM (отладка: несжатый RGB, читается чем
// угодно, без единой зависимости — для сверки пикселей скриптом). Без Vulkan: зовётся с потока, который
// пишет файлы (поток ассетов), а не с потока рендера.
//
// AVIF — системная зависимость (libavif из пакетного менеджера), есть только у клиентских целей:
// painter в headless-сервер не линкуется. Сборка без неё (`DEVILS_ENGINE_PAINTER_AVIF=OFF`) отвечает
// на `.avif` отказом-статусом, а не падением: `image_file_format_available` позволяет выбрать PNG
// заранее. PNG — системная libpng.
//
// Альфа отбрасывается: снимок — то, что видно на экране, а альфа цели кадра — служебные данные
// отрисовки. Ошибка кодирования или записи — статус с причиной: снимок — действие пользователя,
// процесс из-за него не останавливается.

namespace devils_engine {
namespace painter {

enum class image_file_format { png, avif, ppm };

// Формат по расширению (`.png`, `.avif`, `.ppm`, без учёта регистра); nullopt — другое расширение.
[[nodiscard]] std::optional<image_file_format> image_file_format_from_path(std::string_view path) noexcept;
// Есть ли кодировщик в этой сборке.
[[nodiscard]] bool image_file_format_available(image_file_format format) noexcept;
[[nodiscard]] std::string_view to_string(image_file_format format) noexcept;

// width*height*4 байт, строки сверху вниз, 8 бит на канал.
struct image_view {
  uint32_t width = 0;
  uint32_t height = 0;
  bool bgra = false;
  std::span<const uint8_t> pixels;
};

struct image_encode_options {
  int avif_quality = 90; // 0..100, 100 — без потерь яркости/цвета в пределах YUV 4:4:4
  int avif_speed = 8;    // 0..10, больше — быстрее и крупнее файл
};

[[nodiscard]] std::expected<std::vector<uint8_t>, std::string>
encode_image(image_file_format format, const image_view& image, const image_encode_options& options = {});

// Кодирует по расширению path и пишет файл (каталог создаётся).
[[nodiscard]] std::expected<void, std::string>
write_image_file(const std::string& path, const image_view& image, const image_encode_options& options = {});

} // namespace painter
} // namespace devils_engine

#endif
