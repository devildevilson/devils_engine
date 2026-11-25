#include "image_file.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <memory>
#include <system_error>

#include <png.h>

#ifdef DEVILS_ENGINE_PAINTER_HAS_AVIF
#  include <avif/avif.h>
#endif

#include "devils_engine/utils/fileio.h"

namespace devils_engine {
namespace painter {

namespace {

bool ends_with_ignore_case(const std::string_view text, const std::string_view suffix) noexcept {
  if (text.size() < suffix.size()) {
    return false;
  }
  const auto tail = text.substr(text.size() - suffix.size());
  return std::equal(tail.begin(), tail.end(), suffix.begin(), [](const char a, const char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  });
}

std::expected<void, std::string> check_view(const image_view& image) {
  if (image.width == 0 || image.height == 0) {
    return std::unexpected(std::format("empty image {}x{}", image.width, image.height));
  }
  const size_t need = size_t(image.width) * image.height * 4;
  if (image.pixels.size() < need) {
    return std::unexpected(std::format("{}x{} image needs {} bytes, got {}", image.width, image.height, need,
                                       image.pixels.size()));
  }
  return {};
}

// RGB8 без альфы, в порядке R, G, B.
std::vector<uint8_t> to_rgb(const image_view& image) {
  const size_t count = size_t(image.width) * image.height;
  std::vector<uint8_t> rgb(count * 3);
  const size_t r = image.bgra ? 2 : 0;
  const size_t b = image.bgra ? 0 : 2;
  for (size_t i = 0; i < count; ++i) {
    rgb[i * 3 + 0] = image.pixels[i * 4 + r];
    rgb[i * 3 + 1] = image.pixels[i * 4 + 1];
    rgb[i * 3 + 2] = image.pixels[i * 4 + b];
  }
  return rgb;
}

std::expected<std::vector<uint8_t>, std::string> encode_png(const image_view& image) {
  const auto rgb = to_rgb(image);
  png_image png{};
  png.version = PNG_IMAGE_VERSION;
  png.width = image.width;
  png.height = image.height;
  png.format = PNG_FORMAT_RGB;
  const png_int_32 stride = png_int_32(image.width * 3);

  // Упрощённый API libpng: первый вызов узнаёт размер, второй пишет.
  png_alloc_size_t size = 0;
  if (png_image_write_to_memory(&png, nullptr, &size, 0, rgb.data(), stride, nullptr) == 0 || size == 0) {
    std::string reason = png.message;
    png_image_free(&png);
    return std::unexpected("png: " + reason);
  }
  std::vector<uint8_t> out(size);
  if (png_image_write_to_memory(&png, out.data(), &size, 0, rgb.data(), stride, nullptr) == 0) {
    std::string reason = png.message;
    png_image_free(&png);
    return std::unexpected("png: " + reason);
  }
  out.resize(size);
  png_image_free(&png);
  return out;
}

// P6: заголовок и RGB8 подряд.
std::vector<uint8_t> encode_ppm(const image_view& image) {
  const std::string header = std::format("P6\n{} {}\n255\n", image.width, image.height);
  const auto rgb = to_rgb(image);
  std::vector<uint8_t> out;
  out.reserve(header.size() + rgb.size());
  out.insert(out.end(), header.begin(), header.end());
  out.insert(out.end(), rgb.begin(), rgb.end());
  return out;
}

#ifdef DEVILS_ENGINE_PAINTER_HAS_AVIF
std::expected<std::vector<uint8_t>, std::string> encode_avif(const image_view& image, const image_encode_options& options) {
  struct image_deleter {
    void operator()(avifImage* p) const noexcept { avifImageDestroy(p); }
  };
  struct encoder_deleter {
    void operator()(avifEncoder* p) const noexcept { avifEncoderDestroy(p); }
  };

  std::unique_ptr<avifImage, image_deleter> avif(avifImageCreate(image.width, image.height, 8, AVIF_PIXEL_FORMAT_YUV444));
  if (avif == nullptr) {
    return std::unexpected("avif: out of memory creating the image");
  }
  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, avif.get());
  rgb.format = image.bgra ? AVIF_RGB_FORMAT_BGRA : AVIF_RGB_FORMAT_RGBA;
  rgb.depth = 8;
  rgb.ignoreAlpha = AVIF_TRUE; // альфа цели кадра — служебные данные, в снимок не идёт
  rgb.pixels = const_cast<uint8_t*>(image.pixels.data());
  rgb.rowBytes = image.width * 4;
  if (const avifResult r = avifImageRGBToYUV(avif.get(), &rgb); r != AVIF_RESULT_OK) {
    return std::unexpected(std::string("avif: RGB to YUV failed: ") + avifResultToString(r));
  }

  std::unique_ptr<avifEncoder, encoder_deleter> encoder(avifEncoderCreate());
  if (encoder == nullptr) {
    return std::unexpected("avif: out of memory creating the encoder");
  }
  encoder->quality = std::clamp(options.avif_quality, 0, 100);
  encoder->speed = std::clamp(options.avif_speed, 0, 10);
  encoder->maxThreads = 1; // поток вызывающего, своих потоков кодировщик не заводит

  avifRWData output = AVIF_DATA_EMPTY;
  if (const avifResult r = avifEncoderWrite(encoder.get(), avif.get(), &output); r != AVIF_RESULT_OK) {
    avifRWDataFree(&output);
    return std::unexpected(std::string("avif: encoding failed: ") + avifResultToString(r));
  }
  std::vector<uint8_t> out(output.data, output.data + output.size);
  avifRWDataFree(&output);
  return out;
}
#endif

} // namespace

std::optional<image_file_format> image_file_format_from_path(const std::string_view path) noexcept {
  if (ends_with_ignore_case(path, ".png")) {
    return image_file_format::png;
  }
  if (ends_with_ignore_case(path, ".avif")) {
    return image_file_format::avif;
  }
  if (ends_with_ignore_case(path, ".ppm")) {
    return image_file_format::ppm;
  }
  return std::nullopt;
}

bool image_file_format_available(const image_file_format format) noexcept {
  switch (format) {
    case image_file_format::png: return true;
    case image_file_format::ppm: return true;
#ifdef DEVILS_ENGINE_PAINTER_HAS_AVIF
    case image_file_format::avif: return true;
#else
    case image_file_format::avif: return false;
#endif
  }
  return false;
}

std::string_view to_string(const image_file_format format) noexcept {
  switch (format) {
    case image_file_format::png: return "png";
    case image_file_format::avif: return "avif";
    case image_file_format::ppm: return "ppm";
  }
  return "?";
}

std::expected<std::vector<uint8_t>, std::string>
encode_image(const image_file_format format, const image_view& image, const image_encode_options& options) {
  if (auto valid = check_view(image); !valid) {
    return std::unexpected(valid.error());
  }
  switch (format) {
    case image_file_format::png: return encode_png(image);
    case image_file_format::ppm: return encode_ppm(image);
    case image_file_format::avif:
#ifdef DEVILS_ENGINE_PAINTER_HAS_AVIF
      return encode_avif(image, options);
#else
      (void)options;
      return std::unexpected("avif: this build has no AVIF encoder (DEVILS_ENGINE_PAINTER_AVIF=OFF)");
#endif
  }
  return std::unexpected("unknown image format");
}

std::expected<void, std::string>
write_image_file(const std::string& path, const image_view& image, const image_encode_options& options) {
  const auto format = image_file_format_from_path(path);
  if (!format) {
    return std::unexpected(std::format("'{}': unknown image extension, expected .avif, .png or .ppm", path));
  }
  auto bytes = encode_image(*format, image, options);
  if (!bytes) {
    return std::unexpected(std::format("'{}': {}", path, bytes.error()));
  }
  const std::filesystem::path target(path);
  if (target.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
      return std::unexpected(std::format("'{}': cannot create directory: {}", path, ec.message()));
    }
  }
  if (!file_io::write(std::span<const uint8_t>(bytes->data(), bytes->size()), path, file_io::type::binary)) {
    return std::unexpected(std::format("'{}': write failed", path));
  }
  return {};
}

} // namespace painter
} // namespace devils_engine
