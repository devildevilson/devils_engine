#ifndef DEVILS_ENGINE_VISAGE_FONT_H
#define DEVILS_ENGINE_VISAGE_FONT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct nk_user_font_glyph;
typedef uint32_t nk_rune;
struct nk_user_font;

namespace devils_engine {
namespace visage {
// из чего состоит шрифт? шрифт это атлас в котором все элементы имет миллиард данных по расположению
// да и в общем то все, если я хочу сделать наконец то дистанс шрифты
// то по большому счету мне только нужно указать где хранится юзер дата 
// + функции ширина/высота шрифта и получить глиф (?)
struct font_t {
  // нужен ли мне конфиг?
  struct config_t {
    double size;
    uint32_t coord_type;
    struct { double x, y; } spacing;
    const uint32_t *range;
    // font
    uint32_t fallback_glyph;
  };

  struct glyph_t {
    uint32_t codepoint;
    double advance;
    //double x0, y0, x1, y1, w, h;
    //double u0, v0, u1, v1;
    double scale;
    double gscale;
    int index;
    int x,y,w,h; // rect
    double al,ab,ar,at; // atlas
    double pl,pb,pr,pt; // plane
  };

  struct metrics_t {
    // The size of one EM.
    double em_size;
    // The vertical position of the ascender and descender relative to the baseline.
    double ascender_y, descender_y;
    // The vertical difference between consecutive baselines.
    double line_height;
    // The vertical position and thickness of the underline.
    double underline_y, underline_thickness;
  };

  double scale;
  struct metrics_t metrics;
  std::vector<glyph_t> glyphs;
  const struct glyph_t *fallback;
  uint32_t fallback_codepoint;
  int32_t width, height;
  void* texture;
  struct config *config; // конфиг тут хранить?
  std::unique_ptr<nk_user_font> nkfont; // базовый (дефолтный размер)

  // деструктор объявлен и определён в font.cpp: nkfont — unique_ptr<nk_user_font> (неполный
  // тип здесь), поэтому уничтожение должно происходить там, где nk_user_font полный.
  font_t() = default;
  ~font_t();

  const glyph_t *find_glyph(const uint32_t codepoint) const;
  void query_font_glyph(float font_height, struct nk_user_font_glyph *glyph, nk_rune codepoint, nk_rune next_codepoint) const;
  double text_width(double height, const std::string_view &txt) const;

  // индекс GPU-слота атласа этого шрифта: nuklear зашивает его в texture.id draw-команд текста,
  // а шейдер UI по нему сэмплит нужную текстуру. Определён в font.cpp (nk_user_font там полный).
  // Варианты под разные размеры держит visage::system и освежает texture.id при push_font.
  void set_texture_id(uint32_t id);
};

// Загрузка/генерация атласа шрифта: см. font_atlas_packer (заменил легаси load_font).
}
}

#endif