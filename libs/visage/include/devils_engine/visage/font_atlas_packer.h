#ifndef DEVILS_ENGINE_VISAGE_FONT_ATLAS_PACKER_H
#define DEVILS_ENGINE_VISAGE_FONT_ATLAS_PACKER_H

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include <string>
#include <memory>
#include "utils/locale-utils.hpp"
#include "font.h"

namespace devils_engine {
namespace visage {

// передаем сюда список шрифтов так и что на выходе?
// я так понимаю лучше перекопирование сделать в другом месте
// а здесь вернуть память и количество каналов
// нет, по итогу наверное один пакер на одну картинку иначе какой то бред получается

// надо интерфейс переделать: я думаю что 
// создать данные для шрифта мы по идее можем без упаковки всех глифов в картинку
// но нет похоже что это не так, судя по msdf нам сначала нужно собрать все глифы
// потом мы их пакуем
class font_atlas_packer {
public:
  using charset_range_t = std::pair<uint32_t, uint32_t>;

  struct config {
    utils::locale main_locale; // а как из локали чарсет получить? просто как будто самому задать
    std::initializer_list<charset_range_t> charsets;
    double max_corner_angle;
    double minimum_scale;
    double pixel_range;
    double mitter_limit;
    uint32_t color_channels; // 3 or 4
    uint32_t thread_count;
    bool save_png;

    // тут еще в будущем потребуется конфиг коррекции ошибок задать
    // там кажется есть что то полезное
  };

  struct font_image_t {
    std::vector<uint8_t> bytes;
    uint32_t width, height, channels;
  };

  void setup_font(std::string path);
  void setup_font(std::vector<uint8_t> data, std::string hint);

  std::tuple<std::vector<std::unique_ptr<font_t>>, font_image_t> load_fonts(const config &cfg);
private:
  std::vector<std::pair<std::vector<uint8_t>, std::string>> fonts_data;
};
}
}

#endif